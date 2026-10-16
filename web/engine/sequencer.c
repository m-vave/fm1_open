/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#include "internal.h"
#include <string.h>

static struct groove_step pattern[GROOVE_TRACKS][GROOVE_STEPS];
static uint8_t lengths[GROOVE_TRACKS];
static uint64_t active[GROOVE_TRACKS], drum_active[GROOVE_TRACKS][GROOVE_PADS];
static unsigned tempo = 120, clock_mode;
static struct groove_sequence_options options;
static struct clock_state {
    uint32_t phase, age, intervals, random;
    uint32_t gate[GROOVE_TRACKS];
    uint16_t measured_tempo, click;
    uint8_t playing, first, seen, lost, measured, tick, pulse, countdown;
    uint8_t next[GROOVE_TRACKS], current[GROOVE_TRACKS], length[GROOVE_TRACKS];
    uint8_t tied[GROOVE_TRACKS], held[GROOVE_TRACKS][8];
} sequence_clock, saved_clock;
static struct {
    uint32_t word;
    uint8_t positions[GROOVE_TRACKS];
} published;
enum { PERIOD = FM1_SAMPLE_RATE * 60U };
static const uint8_t common_chords[][8] = {
    {3, 4, 7}, {3, 3, 7}, {4, 4, 7, 11}, {4, 3, 7, 10},
    {4, 4, 7, 10}, {3, 2, 7}, {3, 5, 7}, {3, 3, 6},
    {3, 4, 8}, {2, 7}, {2, 12}, {4, 4, 7, 14}
};

__attribute__((cold, minsize)) const char *groove_sequence_chord_name(unsigned id)
{
    static const char *const names[] = {
        "MAJOR", "MINOR", "MAJ7", "MIN7", "DOM7", "SUS2",
        "SUS4", "DIM", "AUG", "FIFTH", "OCTAVE", "ADD9"
    };
    if (!id || id > GROOVE_CHORDS || !options.chord[id - 1][0]) return "SINGLE";
    for (unsigned i = 0; i < sizeof common_chords / sizeof common_chords[0]; ++i)
        if (!memcmp(options.chord[id - 1], common_chords[i], 8)) return names[i];
    return "CUSTOM";
}

void groove_sequence_defaults(struct groove_sequence_options *o)
{
    memset(o, 0, sizeof *o);
    o->version = 4;
    memcpy(o->chord, common_chords, sizeof common_chords);
    memset(o->lock, 255, sizeof o->lock);
}
__attribute__((cold, minsize)) int groove_sequence_valid(const struct groove_project *p)
{
    const struct groove_sequence_options *o = &p->sequence;
    if (o->version != 4 || o->swing > 50 || o->metronome > 2 || o->count_in > 1)
        return 0;
    for (unsigned c = 0; c < GROOVE_CHORDS; ++c) {
        unsigned n = o->chord[c][0];
        if (n == 1 || n > 8)
            return 0;
        for (unsigned i = 1; i < 8; ++i) {
            if (i < n && (!o->chord[c][i] || o->chord[c][i] > 127 ||
                          (i > 1 && o->chord[c][i] <= o->chord[c][i - 1])))
                return 0;
            if (i >= n && o->chord[c][i])
                return 0;
        }
    }
    for (unsigned t = 0; t < 8; ++t)
        for (unsigned s = 0; s < 64; ++s) {
            const struct groove_step *v = &p->step[t][s];
            if (v->note > 127 || v->velocity > 127)
                return 0;
            if (p->preset[t].type == GROOVE_808 || p->preset[t].type == GROOVE_SAMPLE) {
                if (v->note & 15U)
                    return 0; /* Low nibble is reserved for drums. */
                if (p->preset[t].type == GROOVE_SAMPLE && (v->drums >> SAMPLE_DRUM_PADS))
                    return 0; /* Sampler patterns address only the eight active pads. */
            } else {
                unsigned chord = v->drums & 31U;
                if ((v->drums & 0xc000U /* bits 14..15 reserved; bit 13 = alt (GROOVE_STEP_ALT) */) || chord > 16 || (chord && !o->chord[chord - 1][0]))
                    return 0;
                if (chord && v->note + o->chord[chord - 1][o->chord[chord - 1][0] - 1] > 127)
                    return 0;
            }
        }
    for (unsigned i = 0; i < GROOVE_LOCKS; ++i) {
        const struct groove_parameter_lock *l = &o->lock[i];
        if (l->track == 255)
            continue;
        if (l->track >= 8 || l->step >= 64 || l->value > 127)
            return 0;
        unsigned type = p->preset[l->track].type, limit = type == GROOVE_DX7      ? 145
                                                          : type == GROOVE_303    ? 12
                                                          : type == GROOVE_SAMPLE ? SAMPLE_DRUM_PADS * SAMPLE_DRUM_PARAMS
                                                          : type == GROOVE_808    ? 1
                                                                                  : 4;
        if (l->parameter >= limit)
            return 0;
        for (unsigned j = 0; j < i; ++j)
            if (o->lock[j].track == l->track && o->lock[j].step == l->step)
                return 0;
    }
    return 1;
}
static __attribute__((noinline, minsize)) void masks(unsigned track)
{
    active[track] = 0;
    memset(drum_active[track], 0, sizeof drum_active[track]);
    unsigned pads = groove_pad_count(track);
    uint64_t bit = 1;
    for (unsigned s = 0; s < 64; ++s, bit <<= 1) {
        if (pads ? pattern[track][s].drums != 0 : pattern[track][s].velocity != 0)
            active[track] |= bit;
        if (pads)
            for (unsigned d = 0; d < pads; ++d)
                if (pattern[track][s].drums & (1U << d))
                    drum_active[track][d] |= bit;
    }
}
static FM1_DSP_CODE void release_track(struct clock_state *c, unsigned i)
{
    unsigned track = i;
    for (unsigned n = 0; n < 8; ++n)
        if (c->held[i][n] != 255) {
            groove_note(track, c->held[i][n], 0, 1);
            c->held[i][n] = 255;
        }
    c->gate[i] = 0;
    c->tied[i] = 0;
}
static FM1_DSP_CODE void release(struct clock_state *c)
{
    for (unsigned i = 0; i < GROOVE_TRACKS; ++i) {
        release_track(c, i);
        groove_lock_restore(i);
    }
}
static FM1_DSP_CODE void clock_publish(void)
{
    const struct clock_state *c = &sequence_clock;
    for (unsigned i = 0; i < GROOVE_TRACKS; ++i)
        published.positions[i] = (uint8_t)(c->current[i] | ((c->length[i] / 16U - 1U) << 6));
    published.word = c->tick | (c->playing ? 128U : 0U) | (c->seen ? 256U : 0U) |
                            (c->lost ? 512U : 0U) | ((uint32_t)c->countdown << 10) |
                            (c->first && !c->tick ? 32768U : 0U) |
                            ((uint32_t)c->measured_tempo << 16);
}
static FM1_DSP_CODE unsigned step_period(const struct clock_state *c)
{
    return PERIOD / 100U *
           (unsigned)(100 + (c->tick & 1U ? -(int)options.swing : (int)options.swing));
}
static FM1_DSP_CODE unsigned step_pulses(const struct clock_state *c)
{
    unsigned shift = (options.swing * 3U + 25U) / 50U;
    return c->tick & 1U ? 6U - shift : 6U + shift;
}
static FM1_DSP_CODE unsigned gate_value(const struct groove_step *v)
{
    unsigned g = (v->drums >> 5) & 15U;
    return g ? g == 15 ? 16 : g : 8;
}
static FM1_DSP_CODE void sequence_event(void)
{
    struct clock_state *c = &sequence_clock;
    if (!(c->tick & 3U) &&
        (c->countdown || options.metronome == 2 ||
         (options.metronome == 1 && groove_edit_recording())))
        c->click = 384;
    if (c->countdown && --c->countdown) {
        clock_publish();
        return;
    }
    for (unsigned i = 0; i < GROOVE_TRACKS; ++i) {
        unsigned track = i, s = c->next[i];
        if (!s)
            c->length[i] = lengths[track];
        c->current[i] = (uint8_t)s;
        c->next[i] = (uint8_t)((s + 1U) % c->length[i]);
        const struct groove_step *v = &pattern[track][s];
        unsigned pads = groove_pad_count(track);
        unsigned probability = pads ? v->note >> 4 : (v->drums >> 9) & 7U;
        c->random ^= c->random << 13;
        c->random ^= c->random >> 17;
        c->random ^= c->random << 5;
        groove_lock_restore(track);
        for (unsigned l = 0; l < GROOVE_LOCKS; ++l)
            if (options.lock[l].track == track && options.lock[l].step == s)
                groove_lock_apply(track, options.lock[l].parameter, options.lock[l].value);
        if (probability && (c->random & 7U) >= probability) {
            release_track(c, i);
            continue;
        }
        if (pads) {
            release_track(c, i);
            for (unsigned d = 0; d < pads; ++d)
                if (v->drums & (1U << d))
                    groove_note(track, groove_pad_note(track, d), v->velocity ? v->velocity : 100,
                                1);
            continue;
        }
        unsigned chord = v->drums & 31U, count = chord ? options.chord[chord - 1][0] : 1;
        if (!v->velocity) {
            if (!c->tied[i])
                release_track(c, i);
            continue;
        }
        uint8_t notes[8];
        memset(notes, 255, sizeof notes);
        for (unsigned n = 0; n < count; ++n) {
            unsigned note = v->note + (n && chord ? options.chord[chord - 1][n] : 0);
            if (note < 128)
                notes[n] = (uint8_t)note;
        }
        for (unsigned n = 0; n < 8; ++n)
            if (c->held[i][n] != 255) {
                unsigned keep = 0;
                if (c->tied[i])
                    for (unsigned k = 0; k < 8; ++k)
                        keep |= notes[k] == c->held[i][n];
                if (!keep) {
                    groove_note(track, c->held[i][n], 0, 1);
                    c->held[i][n] = 255;
                }
            }
        for (unsigned n = 0; n < count; ++n)
            if (notes[n] != 255) {
                unsigned held = 0;
                for (unsigned k = 0; k < 8; ++k)
                    held |= c->held[i][k] == notes[n];
                if (!held)
                    groove_note_ex(track, notes[n], v->velocity, 1, (v->drums >> 13) & 1U);
            }
        memcpy(c->held[i], notes, sizeof notes);
        c->tied[i] = (v->drums >> 12) & 1U;
        c->gate[i] = clock_mode == GROOVE_CLOCK_MIDI ? (step_pulses(c) * gate_value(v) + 15U) / 16U
                                                     : step_period(c) / 16U * gate_value(v);
    }
    clock_publish();
}
__attribute__((cold, minsize)) void groove_sequence_init(void)
{
    memset(pattern, 0, sizeof pattern);
    memset(active, 0, sizeof active);
    memset(drum_active, 0, sizeof drum_active);
    memset(&sequence_clock, 0, sizeof sequence_clock);
    memset(lengths, 64, sizeof lengths);
    tempo = 120;
    groove_sequence_defaults(&options);
    sequence_clock.first = 1;
    sequence_clock.random = 0x6d2b79f5U;
    memset(sequence_clock.held, 255, sizeof sequence_clock.held);
    memset(sequence_clock.length, 64, sizeof sequence_clock.length);
    clock_publish();
    for (unsigned t = 0; t < 8; ++t)
        for (unsigned s = 0; s < 64; ++s)
            pattern[t][s].note = groove_pad_count(t) ? 0 : 60;
}
unsigned groove_clock_mode(void)
{
    return clock_mode;
}
__attribute__((cold, minsize)) void groove_clock_set(unsigned mode)
{
    if (mode > GROOVE_CLOCK_MIDI || mode == clock_mode)
        return;
    fm1_web_pause();
    release(&sequence_clock);
    groove_arp_reset(1);
    memset(&sequence_clock, 0, sizeof sequence_clock);
    sequence_clock.first = 1;
    sequence_clock.random = 0x6d2b79f5U;
    memset(sequence_clock.held, 255, sizeof sequence_clock.held);
    for (unsigned i = 0; i < GROOVE_TRACKS; ++i)
        sequence_clock.length[i] = lengths[i];
    clock_publish();
    clock_mode = mode;
    fm1_web_resume();
}
FM1_DSP_CODE void groove_sequence_position(unsigned position)
{
    if (clock_mode != GROOVE_CLOCK_MIDI)
        return;
    struct clock_state *c = &sequence_clock;
    release(c);
    groove_arp_reset(0);
    c->tick = (uint8_t)(position & 63U);
    c->phase = c->pulse = 0;
    c->countdown = 0;
    c->first = 1;
    for (unsigned i = 0; i < GROOVE_TRACKS; ++i) {
        c->length[i] = lengths[i];
        c->next[i] = c->current[i] = (uint8_t)(position % c->length[i]);
    }
    clock_publish();
}
FM1_DSP_CODE void groove_sequence_transport(uint8_t status)
{
    struct clock_state *c = &sequence_clock;
    if (status == 0xfa || status == 0xff) {
        release(c);
        groove_arp_reset(status == 0xff);
        c->phase = c->tick = c->pulse = 0;
        c->first = 1;
        c->playing = status == 0xfa;
        c->countdown = status == 0xfa && options.count_in && groove_edit_recording() ? 17 : 0;
        for (unsigned i = 0; i < GROOVE_TRACKS; ++i) {
            c->next[i] = c->current[i] = 0;
            c->length[i] = lengths[i];
        }
    } else if (status == 0xfb)
        c->playing = 1;
    else if (status == 0xfc) {
        c->playing = 0;
        c->countdown = c->click = 0;
        release(c);
        groove_arp_reset(1);
    }
    if (!c->playing)
        for (unsigned i = 0; i < GROOVE_TRACKS; ++i) {
            c->length[i] = lengths[i];
            c->next[i] %= c->length[i];
            c->current[i] %= c->length[i];
        }
    if (status == 0xfa || status == 0xfb || status == 0xff) {
        c->lost = 0;
        c->age = 0;
    }
    clock_publish();
}
FM1_DSP_CODE void groove_sequence_clock(void)
{
    if (clock_mode != GROOVE_CLOCK_MIDI)
        return;
    struct clock_state *c = &sequence_clock;
    if (c->seen && c->age < FM1_SAMPLE_RATE * 2U) {
        c->intervals += c->age;
        if (++c->measured == 24) {
            unsigned bpm = c->intervals ? (PERIOD + c->intervals / 2U) / c->intervals : 0;
            c->measured_tempo = (uint16_t)(bpm > 999 ? 999 : bpm);
            c->intervals = c->measured = 0;
        }
    } else {
        c->intervals = c->measured = c->measured_tempo = 0;
    }
    c->seen = 1;
    c->age = 0;
    if (c->playing) {
        groove_arp_clock();
        if (c->first) {
            c->first = 0;
            sequence_event();
        } else {
            for (unsigned i = 0; i < GROOVE_TRACKS; ++i)
                if (c->gate[i] && !c->tied[i] && !--c->gate[i])
                    release_track(c, i);
            if (++c->pulse >= step_pulses(c)) {
                c->pulse = 0;
                c->tick = (uint8_t)((c->tick + 1U) & 63U);
                sequence_event();
            }
        }
    }
    clock_publish();
}
FM1_DSP_CODE void groove_sequence_render(void)
{
    struct clock_state *c = &sequence_clock;
    if (clock_mode == GROOVE_CLOCK_MIDI) {
        if (c->age < FM1_SAMPLE_RATE * 2U)
            c->age += FM1_BLOCK_FRAMES;
        if (c->age >= FM1_SAMPLE_RATE * 2U && (c->playing || c->seen)) {
            c->seen = 0;
            c->lost = 1;
            c->measured_tempo = 0;
            groove_sequence_transport(0xfc);
        }
        return;
    }
    groove_arp_render(tempo);
    if (!c->playing) return;
    if (c->first || c->phase >= step_period(c)) {
        if (c->first)
            c->first = 0;
        else {
            c->phase -= step_period(c);
            c->tick = (uint8_t)((c->tick + 1U) & 63U);
        }
        sequence_event();
    }
    for (unsigned i = 0; i < GROOVE_TRACKS; ++i)
        if (c->gate[i] && !c->tied[i] && c->phase >= c->gate[i])
            release_track(c, i);
    c->phase += tempo * 4U * FM1_BLOCK_FRAMES;
}
FM1_DSP_CODE void groove_sequence_click(int32_t *out, unsigned frames)
{
    struct clock_state *c = &sequence_clock;
    for (unsigned i = 0; i < frames && c->click; ++i, --c->click) {
        int32_t v = (c->click & 8U ? 1 : -1) * (int32_t)c->click * 384;
        for (unsigned ch = 0; ch < 2; ++ch) {
            int64_t s = (int64_t)out[i * 2 + ch] + v;
            out[i * 2 + ch] = s > INT32_MAX ? INT32_MAX : s < INT32_MIN ? INT32_MIN : (int32_t)s;
        }
    }
}
unsigned groove_sequence_count_in(void)
{
    return (published.word >> 10) & 31U;
}
unsigned groove_sequence_rewound(void)
{
    return (published.word & 32768U) != 0;
}
FM1_DSP_CODE unsigned groove_sequence_record_position(unsigned track)
{
    const struct clock_state *c = &sequence_clock;
    unsigned i = track;
    if (!c->playing)
        return 255;
    if (c->countdown)
        return 254;
    if (c->first)
        return c->next[i];
    unsigned late = clock_mode == GROOVE_CLOCK_MIDI ? c->pulse * 2U >= step_pulses(c)
                                                    : c->phase >= step_period(c) / 2U;
    return late ? c->next[i] : c->current[i];
}
__attribute__((cold, minsize)) void groove_sequence_keep_transport(unsigned restore)
{
    fm1_web_pause();
    if (!restore)
        saved_clock = sequence_clock;
    else {
        sequence_clock = saved_clock;
        memset(sequence_clock.held, 255, sizeof sequence_clock.held);
        memset(sequence_clock.gate, 0, sizeof sequence_clock.gate);
        memset(sequence_clock.tied, 0, sizeof sequence_clock.tied);
        for (unsigned i = 0; i < GROOVE_TRACKS; ++i) {
            sequence_clock.length[i] = lengths[i];
            sequence_clock.next[i] %= sequence_clock.length[i];
            sequence_clock.current[i] %= sequence_clock.length[i];
        }
        clock_publish();
    }
    fm1_web_resume();
}
__attribute__((cold, minsize)) void groove_sequence_edit(unsigned track, unsigned step, unsigned note, unsigned velocity)
{
    if (track >= 8 || step >= 64 || note > 127 || velocity > 127)
        return;
    struct groove_step *v = &pattern[track][step];
    int d = groove_pad_count(track) ? groove_pad_index(track, note) : -1;
    if (groove_pad_count(track) && d < 0)
        return;
    unsigned chord = v->drums & 31U;
    if (d < 0 && chord && note + options.chord[chord - 1][options.chord[chord - 1][0] - 1] > 127)
        return;
    uint16_t drums = d < 0      ? v->drums
                     : velocity ? v->drums | (uint16_t)(1U << d)
                                : v->drums & (uint16_t)~(1U << d);
    if (d < 0 ? (v->note == note && v->velocity == velocity)
              : (v->drums == drums && (!velocity || v->velocity == velocity)))
        return;
    groove_edit_checkpoint(0x10000U + track * 64U + step);
    fm1_web_pause();
    if (d >= 0) {
        v->drums = drums;
        if (velocity)
            v->velocity = (uint8_t)velocity;
    } else {
        v->note = (uint8_t)note;
        v->velocity = (uint8_t)velocity;
    }
    fm1_web_resume();
    masks(track);
    groove_project_touch();
}
unsigned groove_sequence_length(unsigned track)
{
    return track < 8 ? lengths[track] : 0;
}
__attribute__((cold, minsize)) void groove_sequence_length_set(unsigned track, unsigned steps)
{
    if (track >= 8 || !steps || steps > 64 || (steps & 15U) || lengths[track] == steps)
        return;
    groove_edit_checkpoint(0x20000U + track);
    fm1_web_pause();
    lengths[track] = (uint8_t)steps;
    unsigned i = track;
    struct clock_state *c = &sequence_clock;
    if (!c->playing) {
        c->length[i] = (uint8_t)steps;
        c->next[i] %= steps;
        c->current[i] %= steps;
        clock_publish();
    }
    fm1_web_resume();
    groove_project_touch();
}
__attribute__((cold, minsize)) void groove_tempo_edit(int delta)
{
    if (clock_mode == GROOVE_CLOCK_MIDI)
        return;
    int next = delta < 30 - (int)tempo ? 30 : delta > 300 - (int)tempo ? 300 : (int)tempo + delta;
    if ((unsigned)next == tempo)
        return;
    groove_edit_checkpoint(0x30000U);
    fm1_web_pause();
    tempo = (unsigned)next;
    fm1_web_resume();
    groove_project_touch();
}
void groove_sequence_view(unsigned track, unsigned step, struct groove_view *v)
{
    uint32_t state = published.word,
             position = published.positions[track];
    v->clock_mode = (uint8_t)clock_mode;
    v->clock_state = (state & 512U)   ? GROOVE_CLOCK_LOST
                     : (state & 256U) ? GROOVE_CLOCK_LOCKED
                                      : GROOVE_CLOCK_WAIT;
    v->tempo = clock_mode == GROOVE_CLOCK_MIDI ? (uint16_t)(state >> 16) : (uint16_t)tempo;
    v->playing = (state >> 7) & 1U;
    v->sequence_length = (uint8_t)((((position >> 6) & 3U) + 1U) * 16U);
    v->next_length = lengths[track];
    v->play_step = position & 63U;
    v->step = pattern[track][step];
    v->active_steps = active[track];
}
uint64_t groove_drum_steps(unsigned track, unsigned drum)
{
    return track < 8 && drum < 16 ? drum_active[track][drum] : 0;
}
__attribute__((cold, minsize)) void groove_sequence_source_changed(unsigned track)
{
    unsigned i = track;
    memset(sequence_clock.held[i], 255, 8);
    sequence_clock.gate[i] = sequence_clock.tied[i] = 0;
    masks(track);
}
__attribute__((cold, minsize)) void groove_sequence_copy(struct groove_project *p)
{
    memcpy(p->step, pattern, sizeof pattern);
    p->sequence = options;
    p->tempo = (uint16_t)tempo;
    for (unsigned t = 0; t < 8; ++t)
        p->preset[t].sequence_pages = (uint8_t)((lengths[t] >> 4) & 3U);
}
__attribute__((cold, minsize)) void groove_sequence_restore(const struct groove_project *p)
{
    groove_sequence_init();
    memcpy(pattern, p->step, sizeof pattern);
    options = p->sequence;
    tempo = p->tempo;
    for (unsigned t = 0; t < 8; ++t) {
        lengths[t] = p->preset[t].sequence_pages ? p->preset[t].sequence_pages * 16U : 64;
        sequence_clock.length[t] = lengths[t];
        groove_sequence_source_changed(t);
    }
    clock_publish();
}
void groove_sequence_read(unsigned track, unsigned first, struct groove_step *cells, unsigned count)
{
    if (track < 8 && first <= 64 && count <= 64 - first)
        memcpy(cells, pattern[track] + first, count * sizeof *cells);
}
__attribute__((cold, minsize)) void groove_sequence_replace(unsigned track, unsigned first, const struct groove_step *cells,
                             unsigned count)
{
    if (track >= 8 || first > 64 || count > 64 - first)
        return;
    fm1_web_pause();
    memcpy(pattern[track] + first, cells, count * sizeof *cells);
    fm1_web_resume();
    masks(track);
    groove_project_touch();
}
void groove_sequence_options_copy(struct groove_sequence_options *o)
{
    *o = options;
}
void groove_sequence_options_set(const struct groove_sequence_options *o)
{
    fm1_web_pause();
    options = *o;
    fm1_web_resume();
    groove_project_touch();
}
static unsigned adjusted(unsigned value, int delta, unsigned max)
{
    return delta<-(int)value ? 0 : delta>(int) max - (int)value ? max
                                                                : (unsigned)((int)value + delta);
}
unsigned groove_step_setting(unsigned track, unsigned step, unsigned setting)
{
    if (track >= 8 || step >= 64)
        return 0;
    const struct groove_step *v = &pattern[track][step];
    unsigned pads = groove_pad_count(track);
    if (setting == 0)
        return v->velocity;
    if (setting == 1)
        return pads ? 15 : (v->drums >> 5) & 15U;
    if (setting == 2)
        return pads ? v->note >> 4 : (v->drums >> 9) & 7U;
    if (setting == 3)
        return pads ? 0 : v->drums & 31U;
    if (setting == 4)
        return pads ? 0 : (v->drums >> 12) & 1U;
    for (unsigned i = 0; i < GROOVE_LOCKS; ++i)
        if (options.lock[i].track == track && options.lock[i].step == step)
            return setting == 5 ? options.lock[i].parameter + 1U : options.lock[i].value;
    return 0;
}
__attribute__((cold, minsize)) int groove_step_setting_edit(unsigned track, unsigned step, unsigned setting, int delta)
{
    if (track >= 8 || step >= 64 || setting > 6 || !delta)
        return -1;
    unsigned pads = groove_pad_count(track), old = groove_step_setting(track, step, setting);
    if (pads && (setting == 1 || setting == 3 || setting == 4))
        return -1;
    unsigned max = setting == 0 || setting == 6 ? 127
                   : setting == 1               ? 15
                   : setting == 2               ? 7
                   : setting == 3               ? 16
                   : setting == 4               ? 1
                                                : groove_lock_parameter_count(track),
             next = adjusted(old, delta, max);
    if (next == old)
        return 0;
    if (setting == 3 && next &&
        (!options.chord[next - 1][0] ||
         pattern[track][step].note + options.chord[next - 1][options.chord[next - 1][0] - 1] > 127))
        return -1;
    unsigned lock = GROOVE_LOCKS;
    if (setting >= 5) {
        for (unsigned i = 0; i < GROOVE_LOCKS; ++i)
            if (options.lock[i].track == track && options.lock[i].step == step) {
                lock = i;
                break;
            }
        if (lock == GROOVE_LOCKS && setting == 5 && next)
            for (unsigned i = 0; i < GROOVE_LOCKS; ++i)
                if (options.lock[i].track == 255) {
                    lock = i;
                    break;
                }
        if (lock == GROOVE_LOCKS)
            return -1;
    }
    groove_edit_checkpoint(0x40000U + track * 512U + step * 8U + setting);
    fm1_web_pause();
    struct groove_step *v = &pattern[track][step];
    if (setting == 0)
        v->velocity = (uint8_t)next;
    else if (setting == 1)
        v->drums = (uint16_t)((v->drums & ~0x1e0U) | (next << 5));
    else if (setting == 2) {
        if (pads)
            v->note = (uint8_t)(next << 4);
        else
            v->drums = (uint16_t)((v->drums & ~0xe00U) | (next << 9));
    } else if (setting == 3)
        v->drums = (uint16_t)((v->drums & ~31U) | next);
    else if (setting == 4)
        v->drums = (uint16_t)((v->drums & ~0x1000U) | (next << 12));
    else if (setting == 5) {
        options.lock[lock].track = next ? (uint8_t)track : 255;
        options.lock[lock].step = (uint8_t)step;
        options.lock[lock].parameter = (uint8_t)(next ? next - 1 : 0);
        options.lock[lock].value = 64;
    } else
        options.lock[lock].value = (uint8_t)next;
    fm1_web_resume();
    masks(track);
    groove_project_touch();
    return 0;
}
unsigned groove_sequence_setting(unsigned setting)
{
    return setting == 0   ? options.swing
           : setting == 1 ? options.metronome
           : setting == 2 ? options.count_in
                          : 0;
}
__attribute__((cold, minsize)) void groove_sequence_setting_edit(unsigned setting, int delta)
{
    if (setting > 2 || !delta)
        return;
    unsigned old = groove_sequence_setting(setting), next = adjusted(old, delta,
                                                                     setting == 0   ? 50
                                                                     : setting == 1 ? 2
                                                                                    : 1);
    if (next == old)
        return;
    groove_edit_checkpoint(0x50000U + setting);
    fm1_web_pause();
    if (!setting)
        options.swing = (uint8_t)next;
    else if (setting == 1)
        options.metronome = (uint8_t)next;
    else
        options.count_in = (uint8_t)next;
    fm1_web_resume();
    groove_project_touch();
}
/* Live recording: the step's alt flag follows alt hold at note-on. */
__attribute__((cold, minsize)) void groove_sequence_alt(unsigned track, unsigned step, unsigned alt)
{
    if (track >= 8 || step >= 64 || groove_pad_count(track)) return;
    fm1_web_pause();
    pattern[track][step].drums = (uint16_t)((pattern[track][step].drums & ~GROOVE_STEP_ALT) | (alt ? GROOVE_STEP_ALT : 0U));
    fm1_web_resume();
}
__attribute__((cold, minsize)) int groove_sequence_chord(unsigned track, unsigned step, const uint8_t *notes, unsigned count,
                          unsigned velocity)
{
    if (track >= 8 || step >= 64 || !count || count > 8 || velocity > 127)
        return -1;
    uint8_t chord[8] = {0};
    chord[0] = (uint8_t)count;
    for (unsigned i = 0; i < count; ++i)
        if (notes[i] > 127 || (i && notes[i] <= notes[i - 1]))
            return -1;
    for (unsigned i = 1; i < count; ++i)
        chord[i] = (uint8_t)(notes[i] - notes[0]);
    unsigned id = 0;
    if (count > 1) {
        for (unsigned i = 0; i < 16; ++i)
            if (!memcmp(options.chord[i], chord, 8)) {
                id = i + 1;
                break;
            }
        if (!id) {
            unsigned used = 0;
            for (unsigned t = 0; t < 8; ++t)
                if (!groove_pad_count(t))
                    for (unsigned s = 0; s < 64; ++s)
                        if (t != track || s != step) {
                            unsigned c = pattern[t][s].drums & 31U;
                            if (c)
                                used |= 1U << (c - 1);
                        }
            for (unsigned i = 0; i < 16; ++i)
                if (!(used & (1U << i))) {
                    id = i + 1;
                    break;
                }
            if (!id)
                return -2;
        }
    }
    groove_edit_checkpoint(0x60000U + track * 64 + step);
    fm1_web_pause();
    if (id)
        memcpy(options.chord[id - 1], chord, 8);
    pattern[track][step].note = notes[0];
    pattern[track][step].velocity = (uint8_t)velocity;
    pattern[track][step].drums = (uint16_t)((pattern[track][step].drums & ~31U) | id);
    fm1_web_resume();
    masks(track);
    groove_project_touch();
    return 0;
}
