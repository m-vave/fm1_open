/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* Modified 2026-10-17: retain undo history when voice allocation fails. */
#include "engine.h"
#include "synth_dx7.h"
#include "bass303.h"
#include "drum808.h"
#include "psg.h"
#include "pluck.h"
#include "sampledrum.h"
#include "mixer.h"
#include "internal.h"
#include "fm_patch.h"
#include "fixed.h"
#include <stdlib.h>
#include <string.h>

_Static_assert(FM1_SYNTH_API == 5, "FM1 synthesis API mismatch");

/* Each track owns one opaque engine; no DSP layout is part of the app. */
static struct source {
    union {
        void *ptr;
        synth_dx7_t *fm;
        bass303_bank_t *bass;
        drum808_kit_t *drums;
        psg_t *psg;
        pluck_t *pluck;
        sampledrum_t *sample;
    };
    void *allocation;
    uint8_t patch[156];
    /* Align the persistent array, not this type: a temporary source lives on
     * the control stack, whose ABI guarantees only natural pointer alignment. */
    uint8_t padding[32U - (2U * sizeof(void *) + 156U) % 32U];
} sources[GROOVE_TRACKS] __attribute__((aligned(32)));
_Static_assert(__alignof__(struct source) == __alignof__(void *), "natural source stack alignment");
_Static_assert(sizeof(struct source) % 32U == 0, "independent source cache lines");
static struct {
    uint8_t volume, program, modified, audible;
    struct groove_fm_controls controls;
    uint8_t gain_volume, type;
    int32_t gain;
    uint8_t sends[2], expression, pan, padding[8];
} tracks[GROOVE_TRACKS] __attribute__((aligned(32)));
_Static_assert(sizeof tracks[0] == 32, "one cache line per track");
/* Tracks advance independent LFO phases. Control edits hold the gate.
 * Non-FM controls.enabled marks rate/depth/target/wave in controls.envelope;
 * old projects leave it zero, so their sound and serialized values stay intact. */
static struct {
    uint32_t phase, increment;
    int32_t left, right;
    uint8_t wheel, padding[15];
} modulation[GROOVE_TRACKS] __attribute__((aligned(32)));
unsigned groove_lfo_rate(unsigned value) { return 5U + 1995U * value * value / 10000U; }
void groove_lfo_copy(unsigned track, uint8_t values[4])
{
    static const uint8_t defaults[4] = {40, 0, 0, 0};
    const struct groove_fm_controls *c = &tracks[track].controls;
    memcpy(values, c->enabled ? c->envelope : defaults, 4);
}
__attribute__((cold, minsize)) static void lfo_reset(unsigned track)
{
    uint8_t values[4]; groove_lfo_copy(track, values);
    modulation[track].phase = 0;
    modulation[track].wheel = 0;
    tracks[track].expression = 127;
    tracks[track].pan = 64;
    modulation[track].increment = (uint32_t)((UINT64_C(4294967296) * FM1_BLOCK_FRAMES *
        groove_lfo_rate(values[0])) / (FM1_SAMPLE_RATE * 100U));
    modulation[track].left = modulation[track].right = FM_Q30_ONE;
}
/* Smooth the control-rate wave over each block, including depth-zero bypass.
 * Attenuation only: neither tremolo nor stereo balance increases the peak.
 * The dry stereo image and 808 send levels are retained. No voice parameters
 * are overwritten, so parameter locks and project snapshots remain stable. */
static FM1_DSP_CODE void lfo_render(unsigned track, int32_t *left, int32_t *right,
                                    int32_t *rev, int32_t *dly)
{
    const struct groove_fm_controls *c = &tracks[track].controls;
    if (tracks[track].type == GROOVE_DX7) return;
    unsigned depth = c->enabled ? c->envelope[1] : 0;
    unsigned target = c->enabled ? c->envelope[2] : 0;
    unsigned wave = c->enabled ? c->envelope[3] : 0;
    unsigned wheel = (modulation[track].wheel * 100U + 63U) / 127U;
    if (wheel > depth) depth = wheel;
    uint32_t phase = modulation[track].phase;
    modulation[track].phase += modulation[track].increment;
    /* Keep phase running while bypassed, without waveform or sample work. */
    if (!depth && modulation[track].left == FM_Q30_ONE &&
        modulation[track].right == FM_Q30_ONE) return;
    unsigned u = phase >> 16;
    switch (wave) {
    case 0: u = u < 32768U ? u * 2U : (65535U - u) * 2U; break;
    case 1: u = u < 32768U ? 0 : 65535U; break;
    case 3: u = 65535U - u; break;
    }
    int32_t target_l = FM_Q30_ONE, target_r = FM_Q30_ONE;
    if (!target) {
        target_l -= (int32_t)((uint64_t)u * depth * 16384U / 100U);
        target_r = target_l;
    } else {
        int32_t balance = ((int32_t)u - 32768) * (int32_t)depth / 100;
        if (balance > 0) target_l -= balance * 32768;
        else target_r += balance * 32768;
    }
    int32_t l = modulation[track].left, r = modulation[track].right;
    if (l == FM_Q30_ONE && r == FM_Q30_ONE && target_l == l && target_r == r) return;
    int32_t dl = (target_l - l) / (int32_t)FM1_BLOCK_FRAMES, dr = (target_r - r) / (int32_t)FM1_BLOCK_FRAMES;
    for (unsigned i = 0; i < FM1_BLOCK_FRAMES; ++i) {
        l += dl; r += dr;
        left[i] = fm_mul30(left[i], l);
        if (right) right[i] = fm_mul30(right[i], r);
        if (rev && !target) {
            rev[i] = fm_mul30(rev[i], l);
            dly[i] = fm_mul30(dly[i], l);
        }
    }
    modulation[track].left = target_l; modulation[track].right = target_r;
}
static FM1_DSP_CODE unsigned lfo_stereo(unsigned track)
{
    const struct groove_fm_controls *c = &tracks[track].controls;
    return tracks[track].type != GROOVE_DX7 && c->enabled &&
        ((c->envelope[2] && (c->envelope[1] || modulation[track].wheel)) || modulation[track].left != modulation[track].right);
}
/* Control-owned masks; per-track audible is published through the render gate. */
static unsigned mute_mask, solo_mask;
static volatile unsigned output_level;
static unsigned master_adc = 0xffffU;
int groove_master_update(unsigned adc)
{
    if (adc > 1023U) return 0;
    unsigned first = master_adc > 1023U;
    unsigned distance = adc > master_adc ? adc - master_adc : master_adc - adc;
    if (!first && adc > 4U && adc < 1019U && distance < 4U) return 0;
    unsigned level = adc <= 4U ? 0U : adc >= 1019U ? 127U : (adc * 127U + 511U) / 1023U;
    unsigned changed = output_level != level;
    master_adc = adc;
    output_level = level;
    return !first && changed;
}
uint32_t groove_master_state(void) { return master_adc | (output_level << 16); }
unsigned fm1_engine_output_level(void) { return output_level; }
static uint8_t live[GROOVE_TRACKS][128] __attribute__((aligned(32)));
static struct {
    uint8_t bits[16], padding[16];
} seq_note[GROOVE_TRACKS] __attribute__((aligned(32)));
/* Parameter locks are transient. Project/UI snapshots read the base value. */
static struct {
    uint8_t active, parameter, base, padding[29];
} parameter_lock[GROOVE_TRACKS] __attribute__((aligned(32)));
/* Reused by every track during one render pass. */
static struct {
    int32_t mono[FM1_BLOCK_FRAMES], mix[FM1_BLOCK_FRAMES], sends[FM1_BLOCK_FRAMES];
    int32_t right[FM1_BLOCK_FRAMES], mix_right[FM1_BLOCK_FRAMES], delay[FM1_BLOCK_FRAMES], reverb_bus[FM1_BLOCK_FRAMES], delay_bus[FM1_BLOCK_FRAMES];
} scratch;
/* Effect settings are stored in the project. */
static uint8_t effects_values[4] = {64, 64, 48, 100};
static unsigned stereo_enabled;
__attribute__((cold, minsize)) static void stereo_refresh(void)
{
    stereo_enabled = 0;
    for (unsigned t = 0; t < GROOVE_TRACKS; ++t) {
        if (lfo_stereo(t) || tracks[t].sends[0] || tracks[t].sends[1])
            stereo_enabled = 1;
        if (tracks[t].type != GROOVE_808 || !sources[t].ptr)
            continue;
        stereo_enabled |= drum808_kit_stereo(sources[t].drums);
    }
}
void groove_effects_get(uint8_t values[4]) { memcpy(values, effects_values, 4); }
void groove_sends_get(unsigned track, uint8_t values[2])
{ memcpy(values, tracks[track].sends, 2); }
__attribute__((cold, minsize)) void groove_sends_edit(unsigned track, unsigned parameter, int delta)
{
    if (track >= GROOVE_TRACKS || parameter >= 2 || !delta) return;
    int old = tracks[track].sends[parameter];
    unsigned value = delta < -old ? 0U : delta > 15 - old ? 15U : (unsigned)(old + delta);
    if (value == (unsigned)old) return;
    groove_edit_checkpoint(0x9c000U + track * 2U + parameter);
    groove_project_touch();
    fm1_web_pause();
    tracks[track].sends[parameter] = (uint8_t)value;
    stereo_refresh();
    fm1_web_resume();
}
void groove_effects_restore(const uint8_t values[4])
{
    memcpy(effects_values, values, 4);
    fm1_fx_configure(effects_values);
    fm1_fx_clear();
}
__attribute__((cold, minsize)) void groove_effects_edit(unsigned parameter, int delta)
{
    if (parameter >= 4 || !delta)
        return;
    unsigned old = effects_values[parameter];
    unsigned value = delta < -(int)old ? 0 : delta > 127 - (int)old ? 127 : (unsigned)((int)old + delta);
    if (value != old) {
        groove_edit_checkpoint(0x9b000U + parameter);
        groove_project_touch();
        fm1_web_pause();
        effects_values[parameter] = (uint8_t)value;
        fm1_fx_configure(effects_values);
        fm1_web_resume();
    }
}
#include <stdatomic.h>
#define STATUS_WORD _Atomic(uint32_t)
static struct {
    STATUS_WORD word, edit_dirty;
    uint32_t padding[6];
} status[GROOVE_TRACKS] __attribute__((aligned(32)));
/* One control-owned snapshot serves the visible editor. Tone values use
 * the first 12 patch bytes. Refreshing/clearing dirty holds the render gate. */
static struct {
    uint8_t patch[156];
    struct groove_fm_controls controls;
    unsigned track, part;
} edit_snapshot;
/* Browser voice limits and completed render statistics. */
static const uint8_t voice_capacity[GROOVE_SOURCES] = {8, 4, 11, 8, 8, 24};
static uint32_t render_capacity;
static FM1_DSP_CODE unsigned voice_limit(unsigned track, unsigned limit)
{
    if (!sources[track].ptr) return 0;
    switch (tracks[track].type) {
    case GROOVE_DX7: return synth_dx7_limit(sources[track].fm, limit);
    case GROOVE_303: return bass303_bank_limit(sources[track].bass, limit);
    case GROOVE_808: return drum808_kit_limit(sources[track].drums, limit);
    case GROOVE_PSG: return psg_limit(sources[track].psg, limit);
    case GROOVE_PLUCK: return pluck_limit(sources[track].pluck, limit);
    case GROOVE_SAMPLE: return sampledrum_limit(sources[track].sample, limit);
    default: return 0;
    }
}
static void audio_reset(void)
{
    render_capacity = 0;
    for (unsigned track = 0; track < GROOVE_TRACKS; ++track)
        (void)voice_limit(track, voice_capacity[tracks[track].type]);
    fm1_master_reset();
}
void groove_audio_stats(struct fm1_render_stats *out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);
    out->voice_capacity = render_capacity;
    out->master_gain = fm1_master_gain();
    out->deadline_ticks = 24000000U * FM1_BLOCK_FRAMES / FM1_SAMPLE_RATE;
}

/* DX7 programs: the app's patch library size (1..128). Without one (0, the
 * default), 16 programs and Program Change wraps modulo 16 as before. */
__attribute__((weak)) FM1_DSP_CODE unsigned groove_fm_program_count(void) { return 0; }
static FM1_DSP_CODE unsigned program_count(void)
{
    unsigned n = groove_fm_program_count();
    return !n ? FM1_FM_PROGRAMS : n > 128 ? 128 : n;
}
static FM1_DSP_CODE void publish(unsigned track, unsigned level)
{
    unsigned ready = sources[track].ptr != NULL;
    status[track].word = (tracks[track].program & 15U) | ((unsigned)tracks[track].volume << 4) |
                         ((level > 127 ? 127 : level) << 11) | (ready ? 262144U : 0U) |
                         ((unsigned)tracks[track].modified << 19) |
                         ((unsigned)tracks[track].type << 20) |
                         ((unsigned)(tracks[track].program >> 4) << 23);
}
static FM1_DSP_CODE inline unsigned meter_level(uint32_t peak)
{
    unsigned level = peak >> 17; /* abs(Q24) * 128 */
    return level > 127 ? 127 : level;
}
FM1_DSP_CODE unsigned groove_source_type(unsigned track)
{
    return track < GROOVE_TRACKS ? tracks[track].type : GROOVE_SOURCES;
}
static struct {
    uint8_t enabled, note, next, held, velocity[128];
    uint8_t rate, mode, octaves, gate, started, descending;
    uint8_t swing, hold, odd, latched;
    uint16_t tick;
    uint32_t phase, random;
    uint8_t padding[8];
} arpeggio[GROOVE_TRACKS] __attribute__((aligned(32)));
_Static_assert(sizeof arpeggio[0] == 160, "independent arpeggiator cache lines");
unsigned groove_arp_enabled(unsigned track)
{
    return track < GROOVE_TRACKS ? arpeggio[track].enabled : 0;
}
static FM1_DSP_CODE void arp_restart(unsigned t);
static struct groove_tuning tuning[GROOVE_TRACKS];
static uint8_t alt_hold[GROOVE_TRACKS]; /* live state, never saved */
int groove_tuning_set(unsigned track, const struct groove_tuning *t)
{
    if (track >= GROOVE_TRACKS || !t) return -1;
    for (unsigned i = 0; i < GROOVE_TUNING_KEYS; ++i)
        if (t->main[i] == -128 || t->alt[i] == -128) return -2;
    tuning[track] = *t;
    return 0;
}
void groove_tuning_get(unsigned track, struct groove_tuning *t)
{
    if (!t) return;
    if (track < GROOVE_TRACKS) *t = tuning[track];
    else memset(t, 0, sizeof *t);
}
void groove_tuning_alt_hold(unsigned track, unsigned on)
{
    if (track < GROOVE_TRACKS) alt_hold[track] = on != 0;
}
static FM1_DSP_CODE void engine_note(unsigned track, unsigned note, unsigned velocity, int cents)
{
    if (groove_source_type(track) == GROOVE_DX7) {
        uint8_t m[3] = {(uint8_t)(velocity ? 0x90 : 0x80), (uint8_t)note, (uint8_t)velocity};
        if (sources[track].fm)
            (void)synth_dx7_midi_tuned(sources[track].fm, sources[track].patch, m, 3, cents);
    } else if (groove_source_type(track) == GROOVE_PSG) {
        if (sources[track].psg)
            psg_note(sources[track].psg, note, velocity, cents);
    } else if (groove_source_type(track) == GROOVE_PLUCK) {
        if (sources[track].pluck)
            pluck_note(sources[track].pluck, note, velocity, cents);
    } else {
        bass303_bank_t *b = sources[track].bass;
        if (!b)
            return;
        bass303_bank_note(b, tracks[track].controls.mono, note, velocity, cents);
    }
}
/* Live notes (sequenced == 0) take alt from the track's alt hold; sequenced and
 * arpeggiated notes pass the step's GROOVE_STEP_ALT through groove_note_ex. */
FM1_DSP_CODE void groove_note(unsigned track, unsigned note, unsigned velocity, int sequenced)
{
    groove_note_ex(track, note, velocity, sequenced, !sequenced && track < GROOVE_TRACKS && alt_hold[track]);
}
FM1_DSP_CODE unsigned groove_alt_held(unsigned track)
{
    return track < GROOVE_TRACKS && alt_hold[track];
}
FM1_DSP_CODE void groove_note_ex(unsigned track, unsigned note, unsigned velocity, int sequenced, unsigned alt)
{
    if (track >= GROOVE_TRACKS || note > 127 || velocity > 127)
        return;
    /* Live keys, pattern gates and arpeggiator gates share pitch ownership.
     * Releasing one source never cuts an identically pitched other source. */
    unsigned held = (!arpeggio[track].enabled && live[track][note]) ||
                    (seq_note[track].bits[note >> 3] & (1U << (note & 7U))) || arpeggio[track].note == note;
    if (sequenced == 2)
        arpeggio[track].note = velocity ? (uint8_t)note : 0xff;
    else if (sequenced) {
        if (velocity) seq_note[track].bits[note >> 3] |= (uint8_t)(1U << (note & 7U));
        else seq_note[track].bits[note >> 3] &= (uint8_t)~(1U << (note & 7U));
    }
    else if (velocity) {
        /* A fresh chord replaces the latched chord once every physical key
         * has been released. Duplicate presses still have separate owners. */
        if (arpeggio[track].enabled && arpeggio[track].hold && !arpeggio[track].held) {
            arp_restart(track);
            memset(arpeggio[track].velocity, 0, sizeof arpeggio[track].velocity);
            held = (seq_note[track].bits[note >> 3] & (1U << (note & 7U))) != 0;
        }
        if (!live[track][note]) ++arpeggio[track].held;
        if (live[track][note] < 255) ++live[track][note];
        arpeggio[track].velocity[note] = (uint8_t)(velocity | (alt ? 128U : 0U)); /* bit 7: pressed with alt */
        arpeggio[track].latched = 1;
    } else if (live[track][note]) {
        --live[track][note];
        if (!live[track][note]) --arpeggio[track].held;
        if (!live[track][note] && !arpeggio[track].hold) {
            arpeggio[track].velocity[note] = 0;
            if (arpeggio[track].note == note) arpeggio[track].note = 0xff;
        }
    }
    if (groove_pad_count(track)) {
        int drum = groove_pad_index(track, note);
        if (velocity && (sequenced || !arpeggio[track].enabled) && drum >= 0 && sources[track].ptr) {
            if (groove_source_type(track) == GROOVE_SAMPLE)
                sampledrum_trigger(sources[track].sample, (unsigned)drum, velocity);
            else
                drum808_kit_trigger(sources[track].drums, drum, velocity);
        }
        return;
    }
    unsigned next = (!arpeggio[track].enabled && live[track][note]) ||
                    (seq_note[track].bits[note >> 3] & (1U << (note & 7U))) || arpeggio[track].note == note;
    int cents = (alt ? tuning[track].alt : tuning[track].main)[note % 12U];
    if (!held && next) engine_note(track, note, velocity, cents);
    else if (held && !next) engine_note(track, note, 0, 0);
}
static FM1_DSP_CODE void arp_restart(unsigned t)
{
    if (arpeggio[t].note != 255) groove_note(t, arpeggio[t].note, 0, 2);
    arpeggio[t].next = arpeggio[t].started = arpeggio[t].descending = 0;
    arpeggio[t].tick = 0; arpeggio[t].phase = 0;
    arpeggio[t].odd = 0;
}
FM1_DSP_CODE void groove_arp_reset(unsigned clear_latch)
{
    for (unsigned t = 0; t < GROOVE_TRACKS; ++t) {
        arp_restart(t);
        if (!clear_latch) continue;
        for (unsigned n = 0; n < 128; ++n)
            if (!live[t][n]) arpeggio[t].velocity[n] = 0;
        arpeggio[t].latched = arpeggio[t].held != 0;
    }
}
static __attribute__((cold, minsize)) void arp_defaults(void)
{
    memset(arpeggio, 0, sizeof arpeggio);
    for (unsigned t = 0; t < GROOVE_TRACKS; ++t) {
        arpeggio[t].note = 255;
        arpeggio[t].rate = 2; /* 1/16, ascending, one octave, 50% gate. */
        arpeggio[t].gate = 1;
        arpeggio[t].random = 0x9e3779b9U ^ (t + 1U);
    }
}
__attribute__((cold, minsize)) void groove_project_arp_defaults(struct groove_project *p)
{
    for (unsigned t = 0; t < GROOVE_TRACKS; ++t) {
        p->arp[t][0] = 2U | (1U << 6); /* 1/16, ascending, one octave, 50%. */
        p->arp[t][1] = 0;
    }
}
unsigned groove_arp_setting(unsigned t, unsigned parameter)
{
    if (t >= GROOVE_TRACKS) return 0;
    switch (parameter) {
    case 0: return arpeggio[t].rate;
    case 1: return arpeggio[t].mode;
    case 2: return arpeggio[t].octaves;
    case 3: return arpeggio[t].gate;
    case 4: return arpeggio[t].swing;
    case 5: return arpeggio[t].hold;
    default: return 0;
    }
}
__attribute__((cold, minsize)) void groove_arp_edit(unsigned t, unsigned parameter, int delta)
{
    if (t >= GROOVE_TRACKS || parameter >= 6 || !delta) return;
    int value = (int)groove_arp_setting(t, parameter) + delta;
    int limit = parameter == 4 ? 50 : parameter == 5 ? 1 : 3;
    if (value < 0) value = 0;
    if (value > limit) value = limit;
    if (value == (int)groove_arp_setting(t, parameter)) return;
    groove_edit_checkpoint(0x9d000U + t * 8U + parameter);
    fm1_web_pause();
    arp_restart(t); /* Keep held keys; the next event uses the edited values. */
    if (parameter == 0) arpeggio[t].rate = (uint8_t)value;
    if (parameter == 1) arpeggio[t].mode = (uint8_t)value;
    if (parameter == 2) arpeggio[t].octaves = (uint8_t)value;
    if (parameter == 3) arpeggio[t].gate = (uint8_t)value;
    if (parameter == 4) arpeggio[t].swing = (uint8_t)value;
    if (parameter == 5) {
        arpeggio[t].hold = (uint8_t)value;
        if (!value) {
            for (unsigned n = 0; n < 128; ++n)
                if (!live[t][n]) arpeggio[t].velocity[n] = 0;
            arpeggio[t].latched = arpeggio[t].held != 0;
        }
    }
    fm1_web_resume();
    groove_project_touch();
}
static FM1_DSP_CODE void arp_next(unsigned t)
{
    if (arpeggio[t].note != 255) groove_note(t, arpeggio[t].note, 0, 2);
    uint8_t notes[128]; unsigned count = 0;
    unsigned octaves = groove_pad_count(t) ? 0 : arpeggio[t].octaves;
    /* Merge overlapping octave ranges, clip at MIDI 127, and never transpose
     * drum lanes. The list is bounded even with all input keys held. */
    for (unsigned pitch = 0; pitch < 128; ++pitch)
        for (unsigned oct = 0; oct <= octaves && oct * 12 <= pitch; ++oct)
            if (arpeggio[t].velocity[pitch - oct * 12]) { notes[count++] = (uint8_t)pitch; break; }
    if (!count) { arp_restart(t); return; }
    unsigned index = arpeggio[t].next;
    unsigned mode = arpeggio[t].mode;
    if (!arpeggio[t].started) index = mode == 1 ? count - 1 : 0;
    if (index >= count) index = mode == 1 ? count - 1 : 0;
    if (mode == 3) {
        uint32_t r = arpeggio[t].random;
        r ^= r << 13; r ^= r >> 17; r ^= r << 5;
        arpeggio[t].random = r; index = r % count;
    }
    unsigned note = notes[index], velocity = 100, alt = 0;
    for (unsigned oct = 0; oct <= octaves && oct * 12 <= note; ++oct)
        if (arpeggio[t].velocity[note - oct * 12]) {
            velocity = arpeggio[t].velocity[note - oct * 12] & 127U;
            alt = arpeggio[t].velocity[note - oct * 12] >> 7;
            break;
        }
    groove_note_ex(t, note, velocity, 2, alt);
    arpeggio[t].started = 1;
    if (mode == 1) index = index ? index - 1 : count - 1;
    else if (mode == 2 && count > 1) {
        if (!index) arpeggio[t].descending = 0;
        if (index == count - 1) arpeggio[t].descending = 1;
        index = arpeggio[t].descending ? index - 1 : index + 1;
    } else index = (index + 1) % count;
    arpeggio[t].next = (uint8_t)index;
}
static FM1_DSP_CODE unsigned arp_period(unsigned t, unsigned external)
{
    unsigned base = 24U >> arpeggio[t].rate;
    unsigned shift = (base * arpeggio[t].swing + 50U) / 100U;
    if (shift > base / 2U) shift = base / 2U;
    if (!external) {
        base *= FM1_SAMPLE_RATE * 60U;
        shift = base / 100U * arpeggio[t].swing;
    }
    return arpeggio[t].odd ? base - shift : base + shift;
}
FM1_DSP_CODE void groove_arp_render(unsigned tempo)
{
    for (unsigned t = 0; t < GROOVE_TRACKS; ++t) {
        if (!arpeggio[t].enabled || !(arpeggio[t].held || (arpeggio[t].hold && arpeggio[t].latched))) { arp_restart(t); continue; }
        unsigned period = arp_period(t, 0);
        if (!arpeggio[t].started) { arpeggio[t].phase = 0; arp_next(t); }
        else if (arpeggio[t].phase >= period) {
            arpeggio[t].phase -= period; arpeggio[t].odd ^= 1U;
            arp_next(t); period = arp_period(t, 0);
        }
        if (arpeggio[t].phase >= period / 4U * (arpeggio[t].gate + 1U) && arpeggio[t].note != 255)
            groove_note(t, arpeggio[t].note, 0, 2);
        arpeggio[t].phase += tempo * 24U * FM1_BLOCK_FRAMES;
    }
}
FM1_DSP_CODE void groove_arp_clock(void)
{
    for (unsigned t = 0; t < GROOVE_TRACKS; ++t) {
        if (!arpeggio[t].enabled || !(arpeggio[t].held || (arpeggio[t].hold && arpeggio[t].latched))) { arp_restart(t); continue; }
        unsigned period = arp_period(t, 1);
        if (!arpeggio[t].started) { arpeggio[t].tick = 0; arp_next(t); }
        else if (++arpeggio[t].tick >= period) {
            arpeggio[t].tick = 0; arpeggio[t].odd ^= 1U;
            arp_next(t); period = arp_period(t, 1);
        }
        if (arpeggio[t].tick >= (period * (arpeggio[t].gate + 1U) + 3U) / 4U && arpeggio[t].note != 255)
            groove_note(t, arpeggio[t].note, 0, 2);
    }
}
unsigned groove_arp_test_note(unsigned t) { return t < GROOVE_TRACKS ? arpeggio[t].note : 255; }

static FM1_DSP_CODE void panic(unsigned track)
{
    modulation[track].wheel = 0;
    arpeggio[track].note = 0xff;
    arpeggio[track].next = arpeggio[track].held = 0;
    arpeggio[track].started = arpeggio[track].descending = 0;
    arpeggio[track].phase = arpeggio[track].tick = 0;
    arpeggio[track].odd = arpeggio[track].latched = 0;
    memset(arpeggio[track].velocity, 0, sizeof arpeggio[track].velocity);
    memset(live[track], 0, sizeof live[track]);
    memset(seq_note[track].bits, 0, sizeof seq_note[track].bits);
    groove_lock_restore(track);
    if (!sources[track].ptr)
        return;
    if (groove_source_type(track) == GROOVE_DX7)
        fm1_dx7_panic(sources[track].fm);
    else if (groove_source_type(track) == GROOVE_303) {
        bass303_bank_t *b = sources[track].bass;
        bass303_bank_panic(b);
    } else if (groove_source_type(track) == GROOVE_PSG)
        psg_panic(sources[track].psg);
    else if (groove_source_type(track) == GROOVE_PLUCK)
        pluck_panic(sources[track].pluck);
    else if (groove_source_type(track) == GROOVE_SAMPLE)
        sampledrum_panic(sources[track].sample);
    else
        drum808_kit_panic(sources[track].drums);
}
__attribute__((cold, minsize)) void groove_arp_set(unsigned track, unsigned enabled)
{
    if (track >= GROOVE_TRACKS || enabled > 1 || enabled == arpeggio[track].enabled) return;
    groove_edit_checkpoint(0x9d000U + track * 8U + 6U);
    fm1_web_pause();
    panic(track);
    arpeggio[track].enabled = (uint8_t)enabled;
    fm1_web_resume();
    groove_project_touch();
}
/* MIDI overrides only the addressed step-lock parameter. The caller owns the
 * audio lane or holds the render gate. */
static FM1_DSP_CODE void unlock_parameter(unsigned track, unsigned parameter)
{
    if (parameter_lock[track].active && parameter_lock[track].parameter == parameter)
        groove_lock_restore(track);
}
/* Keep panic's reset copy in sync, so stopping notes never discards edits. */
static FM1_DSP_CODE unsigned bass_parameter(unsigned track, unsigned parameter, unsigned value)
{
    bass303_bank_t *b = sources[track].bass;
    if (!b)
        return 0;
    unlock_parameter(track, parameter);
    if (bass303_bank_get(sources[track].bass, parameter) == value)
        return 0;
    bass303_bank_set(b, parameter, value);
    if (parameter == BASS303_WAVE)
        tracks[track].program = (uint8_t)value;
    tracks[track].modified = 1;
    status[track].edit_dirty = 1;
    return 1;
}
static FM1_DSP_CODE unsigned simple_parameter(unsigned track, unsigned parameter, unsigned value)
{
    if (!sources[track].ptr)
        return 0;
    unlock_parameter(track, parameter);
    unsigned psg = groove_source_type(track) == GROOVE_PSG;
    unsigned previous =
        psg ? psg_get(sources[track].psg, parameter) : pluck_get(sources[track].pluck, parameter);
    if (previous == value)
        return 0;
    if (psg) {
        psg_set(sources[track].psg, parameter, value);
        if (!parameter)
            tracks[track].program = (uint8_t)value;
    } else
        pluck_set(sources[track].pluck, parameter, value);
    tracks[track].modified = 1;
    status[track].edit_dirty = 1;
    return 1;
}
static FM1_DSP_CODE unsigned message(unsigned track, const uint8_t *m, size_t n)
{
    unsigned type = m[0] & 0xf0U, changed = 0;
    if (type == 0x90 || type == 0x80) {
        groove_note(track, m[1], type == 0x90 ? m[2] : 0, 0);
        return 0;
    }
    if (type == 0xb0 && m[1] == 121) {
        modulation[track].wheel = 0;
        tracks[track].expression = 127;
        tracks[track].pan = 64;
    }
    if (type == 0xc0 && groove_source_type(track) == GROOVE_DX7 && groove_fm_program_count() &&
        m[1] >= program_count())
        return 0; /* beyond the app's program count: ignored, not wrapped */
    if (type == 0xc0) {
        groove_lock_restore(track);
        if (groove_source_type(track) == GROOVE_DX7) {
            panic(track);
            tracks[track].program = (uint8_t)(m[1] % program_count());
            tracks[track].modified = 0;
            groove_fm_program(sources[track].patch, tracks[track].program);
            memset(&tracks[track].controls, 0, sizeof tracks[track].controls);
            tracks[track].controls.envelope[2] = 100;
            if (sources[track].fm) {
                synth_dx7_set_operator_mask(sources[track].fm, 63);
                synth_dx7_set_adsr(sources[track].fm, 0, 0, 0, 100, 0);
                synth_dx7_set_poly_mode(sources[track].fm, 0);
                synth_dx7_lfo_reset(sources[track].fm, sources[track].patch);
            }
            status[track].edit_dirty = 1;
            changed = 1;
        } else if (groove_source_type(track) == GROOVE_303)
            changed = bass_parameter(track, BASS303_WAVE, m[1] & 1U);
        else if (groove_source_type(track) == GROOVE_PSG)
            changed = simple_parameter(track, 0, m[1] & 3U);
    } else if (type == 0xb0 && m[1] == 7) {
        if (groove_source_type(track) == GROOVE_808) unlock_parameter(track, 0);
        changed = tracks[track].volume != m[2];
        tracks[track].volume = m[2];
    } else if (type == 0xb0 && (m[1] == 10 || m[1] == 11)) {
        if (m[1] == 10) tracks[track].pan = m[2];
        else tracks[track].expression = m[2];
    } else if (type == 0xb0 && m[1] == 91) {
        unsigned send = (m[2] * 15U + 63U) / 127U;
        changed = tracks[track].sends[0] != send;
        tracks[track].sends[0] = (uint8_t)send;
    } else if (type == 0xb0 && m[1] == 120)
        panic(track);
    else if (type == 0xb0 && m[1] == 123 && arpeggio[track].enabled) {
        arp_restart(track);
        memset(live[track], 0, sizeof live[track]);
        memset(arpeggio[track].velocity, 0, sizeof arpeggio[track].velocity);
        arpeggio[track].latched = 0;
        arpeggio[track].held = arpeggio[track].next = 0;
    } else if (type == 0xb0 && m[1] == 123 && !groove_pad_count(track)) {
        arpeggio[track].held = 0;
        for (unsigned note = 0; note < 128; ++note)
            if (live[track][note]) {
                live[track][note] = 0;
                arpeggio[track].velocity[note] = 0;
                if (!(seq_note[track].bits[note >> 3] & (1U << (note & 7U))))
                    engine_note(track, note, 0, 0);
            }
    } else if (type == 0xb0 && m[1] == 1 && groove_source_type(track) != GROOVE_DX7 &&
               groove_source_type(track) != GROOVE_303) {
        /* Live wheel depth is channel-local and never changes the saved LFO. */
        modulation[track].wheel = m[2];
    } else if (groove_source_type(track) == GROOVE_PSG) {
        if (type == 0xb0 && (m[1] == 74 || m[1] == 72))
            changed = simple_parameter(track, m[1] == 74 ? 1 : 3, m[1] == 74 ? m[2] >> 2 : m[2]);
        else if (sources[track].psg)
            psg_midi(sources[track].psg, m);
    } else if (groove_source_type(track) == GROOVE_PLUCK) {
        if (type == 0xb0 && (m[1] == 75 || m[1] == 74 || m[1] == 72))
            changed = simple_parameter(track, m[1] == 75 ? 0 : m[1] == 74 ? 1 : 3, m[2]);
        else if (sources[track].pluck)
            pluck_midi(sources[track].pluck, m);
    } else if (groove_source_type(track) == GROOVE_DX7) {
        if (sources[track].fm)
            (void)synth_dx7_midi(sources[track].fm, sources[track].patch, m, n);
    } else if (groove_source_type(track) == GROOVE_303 && type == 0xe0 && sources[track].bass) {
        bass303_bank_bend(sources[track].bass, ((int)m[1] + ((int)m[2] << 7) - 8192) * 16);
    } else if (groove_source_type(track) == GROOVE_303 && type == 0xb0 && sources[track].bass) {
        bass303_bank_t *b = sources[track].bass;
        int param = -1;
        switch (m[1]) {
        case 1:
            param = BASS303_ENVMOD;
            break;
        case 5:
            param = BASS303_SLIDE;
            break;
        case 71:
            param = BASS303_RESO;
            break;
        case 74:
            param = BASS303_CUTOFF;
            break;
        case 75:
            param = BASS303_DECAY;
            break;
        case 64:
            bass303_bank_pedal(b, tracks[track].controls.mono, m[2] >= 64);
            break;
        case 121:
            bass303_bank_bend(b, 0);
            bass303_bank_pedal(b, tracks[track].controls.mono, 0);
            break;
        }
        if (param >= 0)
            changed = bass_parameter(track, (unsigned)param, m[2]);
    }
    publish(track, 0);
    return changed;
}
/* A complete short message on one channel; malformed data never reaches DSP. */
static FM1_DSP_CODE inline int demo_message(const uint8_t *m, size_t n, unsigned channel)
{
    if (!m || !n) return 0;
    if (m[0] == 0xff) return n == 1;
    if (m[0] < 0x80 || m[0] >= 0xf0 || (m[0] & 15u) != channel) return 0;
    unsigned want = (m[0] & 0xe0u) == 0xc0u ? 2u : 3u;
    if (n != want) return 0;
    for (size_t i = 1; i < n; ++i) if (m[i] & 0x80u) return 0;
    return 1;
}
static FM1_DSP_CODE void midi(const uint8_t *m, size_t n)
{
    if (!m || !n)
        return;
    if (n == 1 && m[0] == 0xf8) {
        groove_sequence_clock();
        return;
    }
    if (n == 3 && m[0] == 0xf2 && !(m[1] & 0x80) && !(m[2] & 0x80)) {
        groove_sequence_position((unsigned)m[1] | ((unsigned)m[2] << 7));
        return;
    }
    if (n == 1 && (m[0] == 0xfa || m[0] == 0xfb || m[0] == 0xfc || m[0] == 0xff)) {
        groove_sequence_transport(m[0]);
        if (m[0] == 0xff) {
            groove_record_midi(m, n);
            fm1_fx_clear();
            for (unsigned t = 0; t < GROOVE_TRACKS; ++t) panic(t);
        }
        return;
    }
    unsigned channel = m[0] & 15U;
    unsigned track = channel == 9 ? 7 : channel;
    if (track < GROOVE_TRACKS &&
        demo_message(m, n, groove_channel(track))) {
        groove_record_midi(m, n);
        if (message(track, m, n))
            groove_edit_midi_changed();
    }
}
static int32_t source_gain(unsigned type, unsigned volume)
{
    /* Q29 permits the calibrated 303 maximum gain of exactly 2.0. */
    static const unsigned twice_divisor[GROOVE_SOURCES] = {508, 127, 1016, 508, 508, 254};
    return (int32_t)(((uint64_t)volume << 30) / twice_divisor[type]);
}
/* Constructors/destructors run before audio starts or under the render gate. */
__attribute__((cold, minsize)) static void source_reset(struct source *s, unsigned type)
{
    if (type == GROOVE_DX7) {
        synth_dx7_reset(s->fm);
        synth_dx7_set_pitch_bend_range(s->fm, 2, 2);
    } else if (type == GROOVE_303) {
        bass303_bank_init(s->bass);
    } else if (type == GROOVE_PSG)
        psg_init(s->psg);
    else if (type == GROOVE_PLUCK)
        pluck_init(s->pluck);
    else if (type == GROOVE_SAMPLE)
        sampledrum_init(s->sample);
    else
        drum808_kit_init(s->drums);
}
__attribute__((cold, minsize)) static int source_create(struct source *s, unsigned type, unsigned program)
{
    memset(s, 0, sizeof *s);
    if (type == GROOVE_DX7) {
        groove_fm_program(s->patch, program);
        s->fm = synth_dx7_create(GROOVE_VOICES, FM1_SAMPLE_RATE, FM1_BLOCK_FRAMES);
        if (s->fm) {
            synth_dx7_lfo_reset(s->fm, s->patch);
            synth_dx7_set_pitch_bend_range(s->fm, 2, 2);
        }
    } else {
        size_t size = type == GROOVE_303      ? bass303_bank_size()
                      : type == GROOVE_808    ? drum808_kit_size()
                      : type == GROOVE_PSG    ? psg_size()
                      : type == GROOVE_SAMPLE ? sampledrum_size()
                                              : pluck_size();
        const size_t mask = FM1_SYNTH_ALIGNMENT - 1U;
        s->allocation = calloc(1, size + mask);
        if (!s->allocation)
            return -1;
        s->ptr = (void *)(((uintptr_t)s->allocation + mask) & ~(uintptr_t)mask);
        source_reset(s, type);
    }
    return s->ptr ? 0 : -1;
}
__attribute__((cold, minsize)) static void source_dispose(void *ptr, void *allocation, unsigned type)
{
    if (type == GROOVE_DX7 && ptr)
        synth_dx7_destroy(ptr);
    free(allocation);
}
__attribute__((cold, minsize)) static void source_destroy(unsigned track)
{
    source_dispose(sources[track].ptr, sources[track].allocation, tracks[track].type);
}
__attribute__((cold, minsize)) int groove_source_set(unsigned track, unsigned type)
{
    if (track >= GROOVE_TRACKS || type >= GROOVE_SOURCES)
        return -1;
    if (tracks[track].type == type && sources[track].ptr)
        return 0;
    struct source next;
    unsigned old_pads = groove_pad_count(track);
    fm1_web_pause();
    /* A failed allocation leaves the previous source and its notes intact. */
    if (source_create(&next, type, 0)) {
        fm1_web_resume();
        return -1;
    }
    groove_edit_checkpoint(0x90000U + track);
    panic(track);
    source_destroy(track);
    sources[track] = next;
    arpeggio[track].enabled = 0;
    tracks[track].type = (uint8_t)type;
    tracks[track].program = tracks[track].modified = 0;
    memset(&tracks[track].controls, 0, sizeof tracks[track].controls);
    tracks[track].controls.envelope[2] = 100;
    lfo_reset(track);
    tracks[track].gain_volume = tracks[track].volume;
    tracks[track].gain = source_gain(type, tracks[track].volume);
    status[track].edit_dirty = 1;
    if (!!old_pads != !!groove_pad_count(track)) {
        struct groove_step cells[64] = {{0}};
        for (unsigned i = 0; i < 64; ++i) cells[i].note = groove_pad_count(track) ? 0 : 60;
        groove_sequence_replace(track, 0, cells, 64);
    }
    struct groove_sequence_options options;
    groove_sequence_options_copy(&options);
    for (unsigned i = 0; i < GROOVE_LOCKS; ++i)
        if (options.lock[i].track == track) options.lock[i].track = 255;
    groove_sequence_options_set(&options);
    groove_sequence_source_changed(track);
    stereo_refresh();
    publish(track, 0);
    fm1_web_resume();
    return 0;
}
__attribute__((cold, minsize)) void fm1_web_init(void)
{
    output_level = 0;
    master_adc = 0xffffU;
    memset(tuning, 0, sizeof tuning);
    memset(alt_hold, 0, sizeof alt_hold);
    arp_defaults();
    memset(live, 0, sizeof live);
    memset(seq_note, 0, sizeof seq_note);
    memset(parameter_lock, 0, sizeof parameter_lock);
    mute_mask = (1U << 5) | (1U << 6);
    solo_mask = 0;
    edit_snapshot.track = ~0U;
    for (unsigned t = 0; t < GROOVE_TRACKS; ++t) {
        /* Six ready-to-play sources, plus two muted expansion tracks.
         * Keep the 808 on MIDI channel 10. Every track can change its type. */
        static const uint8_t types[GROOVE_TRACKS] = {
            GROOVE_DX7, GROOVE_303, GROOVE_PSG, GROOVE_PLUCK,
            GROOVE_SAMPLE, GROOVE_DX7, GROOVE_303, GROOVE_808
        };
        unsigned type = types[t];
        unsigned program = 0;
        source_destroy(t);
        tracks[t].type = (uint8_t)type;
        (void)source_create(&sources[t], type, program);
        tracks[t].program = (uint8_t)program;
        tracks[t].volume = tracks[t].gain_volume = 100;
        tracks[t].gain = source_gain(type, 100);
        tracks[t].modified = 0;
        tracks[t].sends[0] = tracks[t].sends[1] = 0;
        tracks[t].audible = !(mute_mask & (1U << t));
        memset(&tracks[t].controls, 0, sizeof tracks[t].controls);
        tracks[t].controls.envelope[2] = 100;
        lfo_reset(t);
        status[t].edit_dirty = 1;
        publish(t, 0);
    }
    audio_reset();
    groove_sequence_init();
    groove_clock_set(GROOVE_CLOCK_INTERNAL);
    const uint8_t default_effects[4] = {64, 64, 48, 100};
    groove_effects_restore(default_effects);
    stereo_refresh();
    groove_edit_init();
    groove_samples_init();
    groove_project_init();
    groove_transfer_init();
    groove_screen_init();
}
void fm1_web_tick(void) { fm1_control_request(); }
__attribute__((cold, minsize)) void groove_samples_stop(void)
{
    for (unsigned t = 0; t < GROOVE_TRACKS; ++t)
        if (tracks[t].type == GROOVE_SAMPLE && sources[t].ptr) panic(t);
}
void fm1_web_control(void)
{
    groove_samples_poll();
    groove_project_poll();
    groove_transfer_poll();
    groove_edit_poll();
    groove_screen_control();
}
void fm1_web_midi(const uint8_t *message, size_t bytes)
{
    midi(message, bytes);
}
static FM1_DSP_CODE inline int32_t track_gain(unsigned track)
{
    /* Audio owns this cache. A burst of CC7 messages needs at most one
     * calculation per audible track and block; muted tracks defer it. */
    if (!tracks[track].audible)
        return 0;
    if (tracks[track].gain_volume != tracks[track].volume) {
        tracks[track].gain_volume = tracks[track].volume;
        tracks[track].gain = source_gain(tracks[track].type, tracks[track].gain_volume);
    }
    return tracks[track].expression == 127 ? tracks[track].gain :
        (int32_t)((int64_t)tracks[track].gain * tracks[track].expression / 127);
}
static FM1_DSP_CODE void render_stereo(int32_t *out)
{
    int32_t *left = scratch.mono, *right = scratch.right;
    int32_t *rev = scratch.sends, *dly = scratch.delay;
    int32_t *mix = scratch.mix, *mix_right = scratch.mix_right;
    memset(mix, 0, sizeof scratch.mix);
    memset(mix_right, 0, sizeof scratch.mix_right);
    memset(scratch.reverb_bus, 0, sizeof scratch.reverb_bus);
    memset(scratch.delay_bus, 0, sizeof scratch.delay_bus);
    for (unsigned t = 0; t < GROOVE_TRACKS; ++t) {
        unsigned type = tracks[t].type, peak = 0;
        if (!sources[t].ptr) {
            publish(t, 0);
            continue;
        }
        if (type == GROOVE_DX7 || type == GROOVE_808)
            memset(left, 0, sizeof scratch.mono);
        if (type == GROOVE_DX7)
            synth_dx7_render(sources[t].fm, left, FM1_BLOCK_FRAMES);
        else if (type == GROOVE_303)
            bass303_bank_render(sources[t].bass, tracks[t].controls.mono, left, rev);
        else if (type == GROOVE_PSG)
            psg_render(sources[t].psg, left, FM1_BLOCK_FRAMES);
        else if (type == GROOVE_PLUCK)
            pluck_render(sources[t].pluck, left, FM1_BLOCK_FRAMES);
        else if (type == GROOVE_SAMPLE)
            sampledrum_render(sources[t].sample, left, FM1_BLOCK_FRAMES);
        else {
            memset(right, 0, sizeof scratch.right);
            memset(rev, 0, sizeof scratch.sends);
            memset(dly, 0, sizeof scratch.delay);
            drum808_kit_render_st(sources[t].drums, left, right, rev, dly);
        }
        unsigned modulated = lfo_stereo(t);
        if (modulated && type != GROOVE_808) memcpy(right, left, sizeof scratch.right);
        lfo_render(t, left, type == GROOVE_808 || modulated ? right : NULL,
                   type == GROOVE_808 ? rev : NULL, dly);
        int32_t gain = track_gain(t);
        if (gain != 0) {
            if (type != GROOVE_808 && !modulated && tracks[t].pan == 64) {
                /* Mono tracks share their gain and meter calculation. The
                 * two destination sums still saturate independently. */
                peak = fm1_mix_mono_stereo128(mix, mix_right, left, gain);
            } else {
                unsigned pan = tracks[t].pan;
                int32_t lg = pan <= 64 ? gain : (int32_t)((int64_t)gain * (127U - pan) / 63);
                int32_t rg = pan >= 64 ? gain : (int32_t)((int64_t)gain * pan / 64);
                peak = fm1_mix128(mix, left, lg, 1);
                unsigned right_peak = fm1_mix128(mix_right,
                    type == GROOVE_808 || modulated ? right : left, rg, 1);
                if (right_peak > peak)
                    peak = right_peak;
                /* Send buses do not publish meters. Keep only scale/add. */
                if (type == GROOVE_808) {
                    fm1_send128(scratch.reverb_bus, rev, gain);
                    fm1_send128(scratch.delay_bus, dly, gain);
                }
            }
        }
        if (gain && (tracks[t].sends[0] || tracks[t].sends[1])) {
            /* Post-fader sends follow mute/solo and track modulation. For stereo
             * tracks, feed their average without changing the dry stereo mix. */
            if (type == GROOVE_808 || modulated)
                for (unsigned i = 0; i < FM1_BLOCK_FRAMES; ++i)
                    left[i] = left[i] / 2 + right[i] / 2;
            if (tracks[t].sends[0])
                fm1_send128(scratch.reverb_bus, left,
                    (int32_t)((int64_t)gain * tracks[t].sends[0] / 15));
            if (tracks[t].sends[1])
                fm1_send128(scratch.delay_bus, left,
                    (int32_t)((int64_t)gain * tracks[t].sends[1] / 15));
        }
        publish(t, meter_level(peak));
    }
    fm1_fx_render(mix, mix_right, scratch.reverb_bus, scratch.delay_bus);
    fm1_bus_output128(out, mix, mix_right);
}
static FM1_DSP_CODE void render(int32_t *out, unsigned frames)
{
    if (frames != FM1_BLOCK_FRAMES) {
        memset(out, 0, frames * 2U * sizeof *out);
        return;
    }
    groove_sequence_render();
    unsigned stereo = stereo_enabled || fm1_fx_active();
    for (unsigned t = 0; t < GROOVE_TRACKS; ++t)
        stereo |= lfo_stereo(t) || tracks[t].pan != 64 || tracks[t].sends[0] || tracks[t].sends[1];
    if (stereo) {
        render_stereo(out);
        return;
    }
    int32_t *mono = scratch.mono, *mix = scratch.mix, *sends = scratch.sends;
    for (unsigned t = 0; t < GROOVE_TRACKS; ++t) {
        unsigned type = tracks[t].type, peak = 0;
        /* Melodic C engines overwrite; DX7 and 808 add to the buffer. */
        if (type == GROOVE_DX7 || type == GROOVE_808 || !sources[t].ptr)
            memset(mono, 0, sizeof scratch.mono);
        if (sources[t].ptr) {
            if (type == GROOVE_DX7)
                synth_dx7_render(sources[t].fm, mono, FM1_BLOCK_FRAMES);
            else if (type == GROOVE_303)
                bass303_bank_render(sources[t].bass, tracks[t].controls.mono, mono, sends);
            else if (type == GROOVE_PSG)
                psg_render(sources[t].psg, mono, FM1_BLOCK_FRAMES);
            else if (type == GROOVE_PLUCK)
                pluck_render(sources[t].pluck, mono, FM1_BLOCK_FRAMES);
            else if (type == GROOVE_SAMPLE)
                sampledrum_render(sources[t].sample, mono, FM1_BLOCK_FRAMES);
            else {
                /* Dry mix: the two unused send buses may share scratch. */
                memset(sends, 0, sizeof scratch.sends);
                drum808_kit_render(sources[t].drums, mono, sends, sends);
            }
        }
        lfo_render(t, mono, NULL, NULL, NULL);
        int32_t gain = track_gain(t);
        /* Muted/zero-volume tracks still advance their DSP above. Their
         * samples need no scaling, summing or peak scan, and cannot inject
         * unwanted samples into another track's mix. */
        if (gain == 0 || !sources[t].ptr) {
            if (t == 0)
                memset(mix, 0, sizeof scratch.mix);
            publish(t, 0);
            continue;
        }
        /* Mix tracks in order and quantize the completed bus once. */
        peak = fm1_mix128(mix, mono, gain, t != 0);
        publish(t, meter_level(peak));
    }
    /* Convert the completed mix to interleaved stereo. */
    fm1_bus_output128(out, mix, mix);
}
int fm1_engine_render(int32_t *output, unsigned frames)
{
    if (!output || frames != FM1_BLOCK_FRAMES) return -1;
    render(output, frames);
    groove_sequence_click(output, frames);
    render_capacity = 0;
    for (unsigned track = 0; track < GROOVE_TRACKS; ++track)
        render_capacity += voice_capacity[tracks[track].type];
    fm1_master_process128(output);
    return 0;
}
void fm1_engine_pause(void) { fm1_web_pause(); }
void fm1_engine_resume(void) { fm1_web_resume(); }
void fm1_engine_stop(void)
{
    groove_sequence_transport(0xfc);
}
static __attribute__((noinline)) unsigned adjust_value(unsigned value, int delta, unsigned limit)
{
    /* Compare before adding so even INT_MIN/INT_MAX increments are bounded. */
    return delta<-(int)value ? 0 : delta>(int) limit - (int)value ? limit
                                                                  : (unsigned)((int)value + delta);
}
/* Edit the saved value even while a sequencer lock supplies a temporary one.
 * Call under the audio gate so the value and lock metadata agree. */
static unsigned edit_base(unsigned track, unsigned parameter, unsigned value)
{
    return parameter_lock[track].active && parameter_lock[track].parameter == parameter
        ? parameter_lock[track].base : value;
}
__attribute__((cold, minsize)) void groove_control_edit(unsigned track, int program_steps, int volume_steps, int cutoff_steps)
{
    if (track >= GROOVE_TRACKS)
        return;
    unsigned type = groove_source_type(track);
    int count = type == GROOVE_DX7 ? (int)program_count() : type == GROOVE_PSG ? 4 : type == GROOVE_303 ? 2 : 1;
    program_steps %= count;
    if (!program_steps && !volume_steps && (!cutoff_steps || type != GROOVE_303))
        return;
    fm1_web_pause();
    unsigned volume = tracks[track].volume;
    if (type == GROOVE_808) volume = edit_base(track, 0, volume);
    unsigned next_volume = adjust_value(volume, volume_steps, 127), cutoff = 0, next_cutoff = 0;
    if (cutoff_steps && type == GROOVE_303 && sources[track].bass) {
        cutoff = edit_base(track, BASS303_CUTOFF, bass303_bank_get(sources[track].bass, BASS303_CUTOFF));
        next_cutoff = adjust_value(cutoff, cutoff_steps, 127);
    }
    if (!program_steps && next_volume == volume && next_cutoff == cutoff) {
        fm1_web_resume();
        return;
    }
    groove_edit_checkpoint(0x91000U + track);
    groove_project_touch();
    groove_lock_restore(track);
    if (program_steps) {
        uint8_t m[2] = {
            (uint8_t)(0xc0 + groove_channel(track)),
            (uint8_t)(((int)tracks[track].program + program_steps % count + count) % count)};
        message(track, m, 2);
    }
    if (next_volume != volume) {
        uint8_t m[3] = {(uint8_t)(0xb0 + groove_channel(track)), 7, (uint8_t)next_volume};
        message(track, m, 3);
    }
    if (next_cutoff != cutoff) {
        uint8_t m[3] = {(uint8_t)(0xb0 + groove_channel(track)), 74, (uint8_t)next_cutoff};
        message(track, m, 3);
    }
    fm1_web_resume();
}
void groove_get_view(unsigned track, unsigned step, struct groove_view *view)
{
    if (!view || track >= 8 || step >= 64)
        return;
    uint32_t s = status[track].word;
    view->program = (uint8_t)((s & 15U) | ((s >> 23) & 7U) << 4);
    view->volume = (s >> 4) & 127U;
    view->level = (s >> 11) & 127U;
    view->ready = (s >> 18) & 1U;
    view->modified = (s >> 19) & 1U;
    view->type = (s >> 20) & 7U;
    view->muted = (mute_mask >> track) & 1U;
    view->solo = (solo_mask >> track) & 1U;
    view->audible = tracks[track].audible;
    view->mono = tracks[track].controls.mono;
    groove_sequence_view(track, step, view);
}
__attribute__((cold, minsize)) void groove_mix_toggle(unsigned track, int solo)
{
    if (track >= GROOVE_TRACKS)
        return;
    groove_edit_checkpoint(0x92000U + track);
    groove_project_touch();
    fm1_web_pause();
    groove_lock_restore(track);
    if (solo) {
        solo_mask ^= 1U << track;
        if (solo_mask & (1U << track)) mute_mask &= ~(1U << track);
    } else {
        mute_mask ^= 1U << track;
        if (mute_mask & (1U << track)) solo_mask &= ~(1U << track);
    }
    for (unsigned t = 0; t < GROOVE_TRACKS; ++t)
        tracks[t].audible = !(mute_mask & (1U << t)) && (!solo_mask || (solo_mask & (1U << t)));
    fm1_web_resume();
}
static unsigned drum_count(unsigned part)
{
    return (unsigned)drum808_nparams((int)part);
}
unsigned groove_tone_count(unsigned track, unsigned part)
{
    if (groove_source_type(track) == GROOVE_303)
        return part == 0 ? BASS303_NPARAMS : 0;
    if (groove_source_type(track) == GROOVE_PSG)
        return part == 0 ? PSG_PARAMS : 0;
    if (groove_source_type(track) == GROOVE_PLUCK)
        return part == 0 ? PLUCK_PARAMS : 0;
    if (groove_source_type(track) == GROOVE_SAMPLE)
        return part < SAMPLE_DRUM_PADS ? SAMPLE_DRUM_PARAMS : 0;
    if (groove_source_type(track) != GROOVE_808 || part > GROOVE_DRUMS)
        return 0;
    return drum_count(part);
}
const fm1_param_t *groove_tone_parameter(unsigned track, unsigned part, unsigned parameter)
{
    if (parameter >= groove_tone_count(track, part))
        return NULL;
    switch (groove_source_type(track)) {
    case GROOVE_303:
        return bass303_param((int)parameter);
    case GROOVE_PSG:
        return psg_param(parameter);
    case GROOVE_PLUCK:
        return pluck_param(parameter);
    case GROOVE_SAMPLE:
        return sampledrum_param(parameter);
    default:
        return drum808_param((int)part, (int)parameter);
    }
}
static FM1_DSP_CODE unsigned tone_value(unsigned track, unsigned part, unsigned parameter)
{
    switch (groove_source_type(track)) {
    case GROOVE_303:
        return bass303_bank_get(sources[track].bass, parameter);
    case GROOVE_PSG:
        return psg_get(sources[track].psg, parameter);
    case GROOVE_PLUCK:
        return pluck_get(sources[track].pluck, parameter);
    case GROOVE_SAMPLE:
        return sampledrum_get(sources[track].sample, part, parameter);
    default:
        return (unsigned)drum808_kit_get(sources[track].drums, (int)part, (int)parameter);
    }
}
__attribute__((cold, minsize)) int groove_tone_copy(unsigned track, unsigned part, uint8_t values[GROOVE_TONE_PARAMS])
{
    unsigned count = groove_tone_count(track, part);
    if (!values || !count || count > GROOVE_TONE_PARAMS || !sources[track].ptr)
        return -1;
    if (edit_snapshot.track != track || edit_snapshot.part != part || status[track].edit_dirty) {
        fm1_web_pause();
        for (unsigned i = 0; i < count; ++i) {
            edit_snapshot.patch[i] = (uint8_t)tone_value(track, part, i);
            unsigned index = groove_source_type(track) == GROOVE_SAMPLE ? part * 6U + i : i;
            if (parameter_lock[track].active && parameter_lock[track].parameter == index &&
                groove_source_type(track) != GROOVE_808)
                edit_snapshot.patch[i] = parameter_lock[track].base;
        }
        memset(edit_snapshot.patch + count, 0, GROOVE_TONE_PARAMS - count);
        edit_snapshot.track = track;
        edit_snapshot.part = part;
        status[track].edit_dirty = 0;
        fm1_web_resume();
    }
    memcpy(values, edit_snapshot.patch, GROOVE_TONE_PARAMS);
    return (int)count;
}
__attribute__((cold, minsize)) void groove_tone_edit(unsigned track, unsigned part, unsigned parameter, int delta)
{
    const fm1_param_t *p = groove_tone_parameter(track, part, parameter);
    if (!p || !delta || !sources[track].ptr)
        return;
    fm1_web_pause();
    unsigned old = tone_value(track, part, parameter);
    unsigned type = groove_source_type(track);
    if (type != GROOVE_808) old = edit_base(track, type == GROOVE_SAMPLE ? part * 6U + parameter : parameter, old);
    unsigned value = adjust_value(old, delta, p->max);
    if (value != old) {
        groove_edit_checkpoint(0x93000U + track * 256U + part * 12U + parameter);
        groove_project_touch();
        groove_lock_restore(track);
        if (groove_source_type(track) == GROOVE_303)
            bass_parameter(track, parameter, value);
        else if (groove_source_type(track) == GROOVE_PSG ||
                 groove_source_type(track) == GROOVE_PLUCK)
            simple_parameter(track, parameter, value);
        else {
            if (groove_source_type(track) == GROOVE_SAMPLE)
                sampledrum_set(sources[track].sample, part, parameter, value);
            else {
                drum808_kit_set(sources[track].drums, (int)part, (int)parameter, (int)value);
                stereo_refresh();
            }
            tracks[track].modified = 1;
            status[track].edit_dirty = 1;
        }
        publish(track, 0);
    }
    fm1_web_resume();
}
__attribute__((cold, minsize)) int groove_fm_copy(unsigned track, uint8_t patch[156], struct groove_fm_controls *controls)
{
    if (groove_source_type(track) != GROOVE_DX7 || !patch)
        return -1;
    if (edit_snapshot.track != track || status[track].edit_dirty) {
        fm1_web_pause();
        memcpy(edit_snapshot.patch, sources[track].patch, sizeof edit_snapshot.patch);
        if (parameter_lock[track].active)
            edit_snapshot.patch[parameter_lock[track].parameter] = parameter_lock[track].base;
        edit_snapshot.controls = tracks[track].controls;
        edit_snapshot.track = track;
        status[track].edit_dirty = 0;
        fm1_web_resume();
    }
    /* A MIDI program change after the dirty check may leave this frame on
     * the prior complete snapshot. Its dirty flag refreshes the next frame. */
    memcpy(patch, edit_snapshot.patch, sizeof edit_snapshot.patch);
    if (controls)
        *controls = edit_snapshot.controls;
    return 0;
}
static FM1_DSP_CODE unsigned fm_limit(unsigned parameter)
{
    static FM1_DSP_DATA const uint8_t op_max[21] = {99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
                                       3,  3,  7,  3,  7,  99, 1,  31, 99, 14};
    static FM1_DSP_DATA const uint8_t global_max[19] = {99, 99, 99, 99, 99, 99, 99, 99, 31, 7,
                                           1,  99, 99, 99, 99, 1,  5,  7,  48};
    return parameter < 126    ? op_max[parameter % 21]
           : parameter < 145  ? global_max[parameter - 126]
           : parameter == 155 ? 63
                              : 127;
}
unsigned groove_lock_parameter_count(unsigned track)
{
    unsigned type = groove_source_type(track);
    return type == GROOVE_DX7 ? 145 : type == GROOVE_303 ? 12 : type == GROOVE_SAMPLE ? SAMPLE_DRUM_PADS * SAMPLE_DRUM_PARAMS
           : type == GROOVE_808 ? 1 : type < GROOVE_SOURCES ? 4 : 0;
}
__attribute__((cold, minsize)) const char *groove_lock_parameter_name(unsigned track, unsigned parameter)
{
    /* Control-thread scratch: consumed immediately by the panel renderer. */
    static char name[32];
    unsigned type = groove_source_type(track);
    if (parameter >= groove_lock_parameter_count(track)) return "NONE";
    if (type == GROOVE_DX7) {
        static const char *const op[] = {
            "RATE 1", "RATE 2", "RATE 3", "RATE 4", "LEVEL 1", "LEVEL 2", "LEVEL 3", "LEVEL 4",
            "BREAKPOINT", "LEFT DEPTH", "RIGHT DEPTH", "LEFT CURVE", "RIGHT CURVE", "RATE SCALE",
            "AM SENS", "VELOCITY", "OUTPUT", "OSC MODE", "COARSE", "FINE", "DETUNE"
        };
        static const char *const global[] = {
            "PITCH RATE 1", "PITCH RATE 2", "PITCH RATE 3", "PITCH RATE 4",
            "PITCH LEVEL 1", "PITCH LEVEL 2", "PITCH LEVEL 3", "PITCH LEVEL 4",
            "ALGORITHM", "FEEDBACK", "OSC SYNC", "LFO SPEED", "LFO DELAY", "LFO PITCH DEPTH",
            "LFO AM DEPTH", "LFO SYNC", "LFO WAVE", "PITCH SENS", "TRANSPOSE"
        };
        if (parameter >= 126) return global[parameter - 126];
        memcpy(name, "OP6 ", 4);
        name[2] = (char)('6' - parameter / 21);
        strcpy(name + 4, op[parameter % 21]);
        return name;
    }
    if (type == GROOVE_808) return "VOLUME";
    const fm1_param_t *p = groove_tone_parameter(track, type == GROOVE_SAMPLE ? parameter / 6U : 0,
                                                type == GROOVE_SAMPLE ? parameter % 6U : parameter);
    if (p && type == GROOVE_SAMPLE) {
        unsigned pad = parameter / 6U + 1;
        memcpy(name, "PAD00 ", 7);
        name[3] = (char)('0' + pad / 10);
        name[4] = (char)('0' + pad % 10);
        strncat(name, p->name, sizeof name - 7);
        return name;
    }
    return p ? p->name : "NONE";
}
static FM1_DSP_CODE void lock_value(unsigned track, unsigned parameter, unsigned value)
{
    switch (groove_source_type(track)) {
    case GROOVE_DX7: sources[track].patch[parameter] = (uint8_t)value; break;
    case GROOVE_303: bass303_bank_set(sources[track].bass, parameter, value); break;
    case GROOVE_PSG: psg_set(sources[track].psg, parameter, value); break;
    case GROOVE_PLUCK: pluck_set(sources[track].pluck, parameter, value); break;
    case GROOVE_SAMPLE: sampledrum_set(sources[track].sample, parameter / 6U, parameter % 6U, value); break;
    case GROOVE_808: tracks[track].volume = (uint8_t)value; break;
    }
}
FM1_DSP_CODE void groove_lock_restore(unsigned track)
{
    if (!parameter_lock[track].active) return;
    lock_value(track, parameter_lock[track].parameter, parameter_lock[track].base);
    parameter_lock[track].active = 0;
}
FM1_DSP_CODE void groove_lock_apply(unsigned track, unsigned parameter, unsigned value)
{
    if (track >= GROOVE_TRACKS || !sources[track].ptr ||
        parameter >= groove_lock_parameter_count(track)) return;
    groove_lock_restore(track);
    unsigned type = groove_source_type(track), base, limit;
    if (type == GROOVE_DX7) { base = sources[track].patch[parameter]; limit = fm_limit(parameter); }
    else if (type == GROOVE_808) { base = tracks[track].volume; limit = 127; }
    else {
        unsigned part = type == GROOVE_SAMPLE ? parameter / 6U : 0;
        unsigned index = type == GROOVE_SAMPLE ? parameter % 6U : parameter;
        base = tone_value(track, part, index);
        limit = groove_tone_parameter(track, part, index)->max;
    }
    parameter_lock[track].parameter = (uint8_t)parameter;
    parameter_lock[track].base = (uint8_t)base;
    parameter_lock[track].active = 1;
    lock_value(track, parameter, (value * limit + 63U) / 127U);
}
__attribute__((cold, minsize)) int groove_fm_import(unsigned track, const uint8_t patch[156])
{
    if (!patch || groove_source_type(track) != GROOVE_DX7 || !sources[track].ptr)
        return GROOVE_PROJECT_INVALID;
    for (unsigned i = 0; i < 156; ++i)
        if (patch[i] > fm_limit(i))
            return GROOVE_PROJECT_INVALID;
    groove_edit_checkpoint(0x94000U + track);
    groove_project_touch();
    fm1_web_pause();
    groove_lock_restore(track);
    panic(track);
    memcpy(sources[track].patch, patch, 156);
    memset(&tracks[track].controls, 0, sizeof tracks[track].controls);
    tracks[track].controls.envelope[2] = 100;
    for (unsigned i = 0; i < 6; ++i)
        if (!(patch[155] & (1U << (5U - i))))
            tracks[track].controls.operators |= (uint8_t)(1U << i);
    synth_dx7_set_operator_mask(sources[track].fm, patch[155]);
    synth_dx7_set_poly_mode(sources[track].fm, 0);
    synth_dx7_set_adsr(sources[track].fm, 0, 0, 0, 100, 0);
    synth_dx7_lfo_reset(sources[track].fm, sources[track].patch);
    tracks[track].modified = 1;
    status[track].edit_dirty = 1;
    publish(track, 0);
    fm1_web_resume();
    return 0;
}
__attribute__((cold, minsize)) void groove_fm_name_edit(unsigned track, unsigned character, char value)
{
    if (groove_source_type(track) != GROOVE_DX7 || !sources[track].ptr ||
        character >= 10 || value < 32 || value > 126)
        return;
    fm1_web_pause();
    if (sources[track].patch[145 + character] != (uint8_t)value) {
        groove_edit_checkpoint(0x95000U + track);
        groove_project_touch();
        groove_lock_restore(track);
        sources[track].patch[145 + character] = (uint8_t)value;
        tracks[track].modified = 1;
        status[track].edit_dirty = 1;
        publish(track, 0);
    }
    fm1_web_resume();
}
__attribute__((cold, minsize)) void groove_fm_edit(unsigned track, unsigned parameter, int delta)
{
    if (groove_source_type(track) != GROOVE_DX7 || parameter >= 145 || !delta)
        return;
    unsigned limit = fm_limit(parameter);
    fm1_web_pause();
    unsigned old = edit_base(track, parameter, sources[track].patch[parameter]);
    unsigned value = adjust_value(old, delta, limit);
    if (value != old) {
        groove_edit_checkpoint(0x96000U + track * 256U + parameter);
        groove_project_touch();
        groove_lock_restore(track);
        sources[track].patch[parameter] = (uint8_t)value;
        status[track].edit_dirty = 1;
        tracks[track].modified = 1;
        /* Match the original LFO page: depths do not restart the LFO. */
        if ((parameter == 137 || parameter == 138 || parameter == 141 || parameter == 142) &&
            sources[track].fm)
            synth_dx7_lfo_reset(sources[track].fm, sources[track].patch);
        publish(track, 0);
    }
    fm1_web_resume();
}
__attribute__((cold, minsize)) void groove_lfo_edit(unsigned track, unsigned parameter, int delta)
{
    if (track >= GROOVE_TRACKS || groove_source_type(track) == GROOVE_DX7 || parameter >= 4 || !delta) return;
    uint8_t values[4]; groove_lfo_copy(track, values);
    unsigned limit = parameter < 2 ? 100 : parameter == 2 ? 1 : 3;
    unsigned value = adjust_value(values[parameter], delta, limit);
    if (value == values[parameter]) return;
    groove_edit_checkpoint(0x9c000U + track * 4U + parameter);
    groove_project_touch();
    fm1_web_pause();
    values[parameter] = (uint8_t)value;
    memcpy(tracks[track].controls.envelope, values, 4);
    tracks[track].controls.enabled = 1;
    /* Rate edits retain phase, and changing depth never retriggers a note. */
    modulation[track].increment = (uint32_t)((UINT64_C(4294967296) * FM1_BLOCK_FRAMES *
        groove_lfo_rate(values[0])) / (FM1_SAMPLE_RATE * 100U));
    tracks[track].modified = 1;
    stereo_refresh();
    publish(track, 0);
    fm1_web_resume();
}
static void adsr_apply(unsigned track)
{
    const struct groove_fm_controls *c = &tracks[track].controls;
    if (sources[track].fm)
        synth_dx7_set_adsr(sources[track].fm, c->enabled, c->envelope[0], c->envelope[1],
                           c->envelope[2], c->envelope[3]);
    tracks[track].modified = 1;
    status[track].edit_dirty = 1;
    publish(track, 0);
}
__attribute__((cold, minsize)) void groove_adsr_edit(unsigned track, unsigned parameter, int delta)
{
    if (groove_source_type(track) != GROOVE_DX7 || parameter >= 4 || !delta)
        return;
    fm1_web_pause();
    unsigned value = adjust_value(tracks[track].controls.envelope[parameter], delta, 100);
    if (value != tracks[track].controls.envelope[parameter] || !tracks[track].controls.enabled) {
        groove_edit_checkpoint(0x97000U + track * 4U + parameter);
        groove_project_touch();
        groove_lock_restore(track);
        tracks[track].controls.envelope[parameter] = (uint8_t)value;
        /* Turning an envelope control is enough to hear it. Browsing ENV
         * leaves the original DX7 envelope intact; PRESETS can bypass it. */
        tracks[track].controls.enabled = 1;
        adsr_apply(track);
    }
    fm1_web_resume();
}
__attribute__((cold, minsize)) void groove_adsr_enable(unsigned track, int enabled)
{
    if (groove_source_type(track) != GROOVE_DX7)
        return;
    fm1_web_pause();
    if (tracks[track].controls.enabled != (enabled != 0)) {
        groove_edit_checkpoint(0x98000U + track);
        groove_project_touch();
        groove_lock_restore(track);
        tracks[track].controls.enabled = enabled != 0;
        adsr_apply(track);
    }
    fm1_web_resume();
}
__attribute__((cold, minsize)) void groove_operator_toggle(unsigned track, unsigned op)
{
    if (groove_source_type(track) != GROOVE_DX7 || op >= 6)
        return;
    groove_edit_checkpoint(0x99000U + track);
    groove_project_touch();
    fm1_web_pause();
    groove_lock_restore(track);
    unsigned muted = tracks[track].controls.operators ^= (uint8_t)(1U << op), mask = 0;
    /* UI OP1..OP6 is the reverse of the engine's VCED storage order. */
    for (unsigned i = 0; i < 6; ++i)
        if (!(muted & (1U << i)))
            mask |= 1U << (5 - i);
    sources[track].patch[155] = (uint8_t)mask;
    if (sources[track].fm)
        synth_dx7_set_operator_mask(sources[track].fm, (uint8_t)mask);
    tracks[track].modified = 1;
    status[track].edit_dirty = 1;
    publish(track, 0);
    fm1_web_resume();
}
__attribute__((cold, minsize)) void groove_mono_set(unsigned track, unsigned mono)
{
    unsigned type = groove_source_type(track);
    if (type != GROOVE_DX7 && type != GROOVE_303)
        return;
    fm1_web_pause();
    if (tracks[track].controls.mono != (mono != 0)) {
        groove_edit_checkpoint(0x9a000U + track);
        groove_project_touch();
        groove_lock_restore(track);
        tracks[track].controls.mono = mono != 0;
        if (type == GROOVE_303)
            panic(track);
        else if (sources[track].fm)
            synth_dx7_set_poly_mode(sources[track].fm, (uint8_t)(mono != 0));
        tracks[track].modified = 1;
        status[track].edit_dirty = 1;
        publish(track, 0);
    }
    fm1_web_resume();
}
static FM1_DSP_DATA const uint8_t drum_notes[GROOVE_DRUMS] = {36, 38, 41, 45, 48, 37,
                                                              39, 56, 49, 46, 42};
FM1_DSP_CODE unsigned groove_drum_note(unsigned drum) { return drum_notes[drum % GROOVE_DRUMS]; }
FM1_DSP_CODE int groove_drum_index(unsigned note)
{
    for (unsigned i = 0; i < GROOVE_DRUMS; ++i)
        if (drum_notes[i] == note)
            return (int)i;
    switch (note) {
    case 35:
        return 0;
    case 40:
        return 1;
    case 43:
        return 2;
    case 47:
        return 3;
    case 50:
        return 4;
    case 51:
        return 8;
    case 44:
        return 10;
    default:
        return -1;
    }
}
unsigned groove_key_note(unsigned track, unsigned key, int octave)
{
    if (groove_source_type(track) == GROOVE_SAMPLE) {
        static const uint8_t keys[SAMPLE_DRUM_PADS] = {0, 2, 4, 6, 7, 9, 11, 12};
        for (unsigned i = 0; i < SAMPLE_DRUM_PADS; ++i)
            if (key == keys[i])
                return sampledrum_note(i);
        return 128;
    }
    if (groove_source_type(track) == GROOVE_808)
        return groove_drum_note(key);
    int note = 53 + (int)key + octave * 12;
    return (unsigned)(note < 0 ? 0 : note > 127 ? 127 : note);
}
FM1_DSP_CODE unsigned groove_pad_count(unsigned track)
{
    unsigned type = groove_source_type(track);
    return type == GROOVE_808 ? GROOVE_DRUMS : type == GROOVE_SAMPLE ? SAMPLE_DRUM_PADS : 0;
}
FM1_DSP_CODE unsigned groove_pad_note(unsigned track, unsigned pad)
{
    if (pad >= groove_pad_count(track))
        return 128;
    return groove_source_type(track) == GROOVE_SAMPLE ? sampledrum_note(pad)
                                                      : groove_drum_note(pad);
}
FM1_DSP_CODE int groove_pad_index(unsigned track, unsigned note)
{
    unsigned type = groove_source_type(track);
    return type == GROOVE_SAMPLE ? sampledrum_index(note)
           : type == GROOVE_808  ? groove_drum_index(note)
                                 : -1;
}
const char *groove_pad_name(unsigned track, unsigned pad)
{
    static const char *const names[GROOVE_DRUMS] = {"KICK",     "SNARE",    "LOW TOM",   "MID TOM",
                                                    "HIGH TOM", "RIM",      "CLAP",      "COWBELL",
                                                    "CYMBAL",   "OPEN HAT", "CLOSED HAT"};
    return groove_source_type(track) == GROOVE_SAMPLE ? sampledrum_name(pad)
           : pad < GROOVE_DRUMS                       ? names[pad]
                                                      : "KIT SETTINGS";
}
const char *groove_pad_label(unsigned track, unsigned pad)
{
    static const char *const labels[GROOVE_DRUMS + 1] = {"BD",   "SD",  "LT",  "MT", "HT", "RIM",
                                                         "CLAP", "COW", "CYM", "OH", "CH", "KIT"};
    return groove_source_type(track) == GROOVE_SAMPLE ? sampledrum_label(pad)
           : pad <= GROOVE_DRUMS                      ? labels[pad]
                                                      : "";
}
static const fm1_param_t *preset_parameter(unsigned type, unsigned index)
{
    return type == GROOVE_303   ? bass303_param((int)index)
           : type == GROOVE_PSG ? psg_param(index)
                                : pluck_param(index);
}
__attribute__((cold, minsize)) int groove_project_valid(const struct groove_project *p)
{
    if (!p || p->tempo < 30 || p->tempo > 300 || !groove_sequence_valid(p))
        return 0;
    for (unsigned i = 0; i < sizeof p->name; ++i)
        if (p->name[i] < 32 || p->name[i] > 126)
            return 0;
    for (unsigned i = 0; i < sizeof p->effects; ++i)
        if (p->effects[i] > 127)
            return 0;
    for (unsigned t = 0; t < GROOVE_TRACKS; ++t) {
        const struct groove_preset *v = &p->preset[t];
        const struct groove_fm_controls *c = &v->controls;
        unsigned type = v->type;
        if ((p->arp[t][1] & 63U) > 50 ||
            type >= GROOVE_SOURCES || v->volume > 127 || v->modified > 1 || c->enabled > 1 ||
            v->sequence_pages > 3 ||
            v->sampler_pads != (type == GROOVE_SAMPLE ? SAMPLE_DRUM_PADS : 0) ||
            c->operators > 63 || c->mono > 1 ||
            v->program >= (type == GROOVE_DX7   ? program_count()
                           : type == GROOVE_303 ? 2
                           : type == GROOVE_PSG ? 4
                                                : 1))
            return 0;
        for (unsigned i = 0; i < 4; ++i)
            if (c->envelope[i] > 100)
                return 0;
        if (type != GROOVE_DX7 && c->enabled &&
            (c->envelope[2] > 1 || c->envelope[3] > 3)) return 0;
        if (type == GROOVE_DX7) {
            for (unsigned i = 0; i < 156; ++i)
                if (v->data[i] > fm_limit(i))
                    return 0;
        } else if (type == GROOVE_808) {
            if (!drum808_preset_valid(v->data)) return 0;
        } else if (type == GROOVE_SAMPLE) {
            for (unsigned pad = 0; pad < SAMPLE_DRUM_PADS; ++pad)
                for (unsigned i = 0; i < SAMPLE_DRUM_PARAMS; ++i)
                    if (v->data[pad * SAMPLE_DRUM_PARAMS + i] > sampledrum_param(i)->max)
                        return 0;
        } else {
            unsigned count = type == GROOVE_303 ? BASS303_NPARAMS : 4;
            for (unsigned i = 0; i < count; ++i)
                if (v->data[i] > preset_parameter(type, i)->max)
                    return 0;
            if ((type == GROOVE_303 && v->program != v->data[BASS303_WAVE]) ||
                (type == GROOVE_PSG && v->program != v->data[0]))
                return 0;
        }
    }
    return 1;
}
__attribute__((cold, minsize)) int groove_project_capture(struct groove_project *p)
{
    if (!p)
        return GROOVE_PROJECT_INVALID;
    fm1_web_pause();
    for (unsigned t = 0; t < GROOVE_TRACKS; ++t)
        if (!sources[t].ptr) {
            fm1_web_resume();
            return GROOVE_PROJECT_MEMORY;
        }
    memset(p, 0, sizeof *p);
    groove_sequence_copy(p);
    p->mute = (uint8_t)mute_mask;
    p->solo = (uint8_t)solo_mask;
    char name[13];
    groove_project_name(name);
    memcpy(p->name, name, sizeof p->name);
    groove_effects_get(p->effects);
    for (unsigned t = 0; t < GROOVE_TRACKS; ++t) {
        struct groove_preset *v = &p->preset[t];
        v->type = tracks[t].type;
        v->program = tracks[t].program;
        v->volume = tracks[t].volume;
        v->modified = tracks[t].modified;
        v->controls = tracks[t].controls;
        if (v->type == GROOVE_DX7)
            memcpy(v->data, sources[t].patch, sizeof sources[t].patch);
        else if (v->type == GROOVE_808) {
            drum808_kit_store(sources[t].drums, v->data);
        } else if (v->type == GROOVE_SAMPLE) {
            v->sampler_pads = SAMPLE_DRUM_PADS;
            for (unsigned pad = 0; pad < SAMPLE_DRUM_PADS; ++pad)
                for (unsigned i = 0; i < SAMPLE_DRUM_PARAMS; ++i)
                    v->data[pad * SAMPLE_DRUM_PARAMS + i] =
                        (uint8_t)sampledrum_get(sources[t].sample, pad, i);
        } else {
            unsigned count = v->type == GROOVE_303 ? BASS303_NPARAMS : 4;
            for (unsigned i = 0; i < count; ++i)
                v->data[i] = (uint8_t)tone_value(t, 0, i);
        }
        if (parameter_lock[t].active) {
            unsigned index = parameter_lock[t].parameter;
            if (v->type == GROOVE_808) v->volume = parameter_lock[t].base;
            else v->data[index] = parameter_lock[t].base;
        }
        v->data[209] = tracks[t].sends[0] | (tracks[t].sends[1] << 4);
        p->arp[t][0] = arpeggio[t].rate | (arpeggio[t].mode << 2) |
            (arpeggio[t].octaves << 4) | (arpeggio[t].gate << 6);
        p->arp[t][1] = arpeggio[t].swing | (arpeggio[t].hold << 6) |
            (arpeggio[t].enabled << 7);
    }
    fm1_web_resume();
    return 0;
}
static __attribute__((cold, minsize)) void preset_apply(struct source *s, const struct groove_preset *p)
{
    if (p->type == GROOVE_DX7) {
        const struct groove_fm_controls *c = &p->controls;
        memcpy(s->patch, p->data, sizeof s->patch);
        unsigned mask = 0;
        for (unsigned i = 0; i < 6; ++i)
            if (!(c->operators & (1U << i)))
                mask |= 1U << (5 - i);
        synth_dx7_set_operator_mask(s->fm, (uint8_t)mask);
        s->patch[155] = (uint8_t)mask;
        synth_dx7_set_poly_mode(s->fm, c->mono);
        synth_dx7_set_adsr(s->fm, c->enabled, c->envelope[0], c->envelope[1], c->envelope[2],
                           c->envelope[3]);
        synth_dx7_lfo_reset(s->fm, s->patch);
    } else if (p->type == GROOVE_808) {
        (void)drum808_kit_load(s->drums, p->data);
    } else if (p->type == GROOVE_SAMPLE) {
        for (unsigned pad = 0; pad < SAMPLE_DRUM_PADS; ++pad)
            for (unsigned i = 0; i < SAMPLE_DRUM_PARAMS; ++i)
                sampledrum_set(s->sample, pad, i, p->data[pad * SAMPLE_DRUM_PARAMS + i]);
    } else {
        unsigned count = p->type == GROOVE_303 ? BASS303_NPARAMS : 4;
        for (unsigned i = 0; i < count; ++i) {
            if (p->type == GROOVE_303)
                bass303_bank_set(s->bass, i, p->data[i]);
            else if (p->type == GROOVE_PSG)
                psg_set(s->psg, i, p->data[i]);
            else
                pluck_set(s->pluck, i, p->data[i]);
        }
        if (p->type == GROOVE_303)
            bass303_bank_panic(s->bass);
    }
}
static __attribute__((cold, minsize)) void track_preset_apply(unsigned t, const struct groove_preset *v)
{
    preset_apply(&sources[t], v);
    tracks[t].type = v->type;
    tracks[t].program = v->program;
    tracks[t].volume = tracks[t].gain_volume = v->volume;
    tracks[t].modified = v->modified;
    tracks[t].controls = v->controls;
    tracks[t].sends[0] = v->data[209] & 15U;
    tracks[t].sends[1] = v->data[209] >> 4;
    lfo_reset(t);
    tracks[t].gain = source_gain(v->type, v->volume);
    tracks[t].audible = !(mute_mask & (1U << t)) && (!solo_mask || (solo_mask & (1U << t)));
    status[t].edit_dirty = 1;
    publish(t, 0);
}
/* A track import must leave other voices, modulation, effects and clocks running. */
__attribute__((cold, minsize)) int groove_timbre_apply(unsigned track, const struct groove_project *p)
{
    if (track >= GROOVE_TRACKS || !groove_project_valid(p))
        return GROOVE_PROJECT_INVALID;
    const struct groove_preset *v = &p->preset[track];
    struct source next;
    unsigned reuse = sources[track].ptr && tracks[track].type == v->type;
    fm1_web_pause();
    if (!reuse && source_create(&next, v->type, v->program)) {
        fm1_web_resume();
        return GROOVE_PROJECT_MEMORY;
    }
    groove_edit_checkpoint(0xb0000U + track);
    panic(track);
    if (reuse)
        source_reset(&sources[track], v->type);
    else {
        source_destroy(track);
        sources[track] = next;
    }
    track_preset_apply(track, v);
    groove_sequence_replace(track, 0, p->step[track], 64);
    groove_sequence_options_set(&p->sequence);
    groove_sequence_source_changed(track);
    stereo_refresh();
    edit_snapshot.track = ~0U;
    fm1_web_resume();
    return 0;
}
/* Restore runs only on the control path; keep its allocation plan compact. */
__attribute__((cold, minsize)) static int project_apply(const struct groove_project *p, unsigned operation)
{
    if (!groove_project_valid(p))
        return GROOVE_PROJECT_INVALID;
    /* Patches come from the project, so stage only allocation handles. */
    struct {
        void *ptr, *allocation;
    } next[GROOVE_TRACKS];
    struct source created;
    unsigned used = 0, reused = 0;
    fm1_web_pause();
    /* Reserve matching tracks first, then reuse other instances of the same
     * type. Until every missing instance is allocated, all old state stays
     * untouched. Reloading/reordering the same types needs no heap allocation. */
    for (unsigned t = 0; t < GROOVE_TRACKS; ++t) {
        if (sources[t].ptr && tracks[t].type == p->preset[t].type) {
            next[t].ptr = sources[t].ptr;
            next[t].allocation = sources[t].allocation;
            used |= 1U << t;
            reused |= 1U << t;
        }
    }
    for (unsigned t = 0; t < GROOVE_TRACKS; ++t) {
        if (reused & (1U << t))
            continue;
        for (unsigned old = 0; old < GROOVE_TRACKS; ++old) {
            if (!(used & (1U << old)) && sources[old].ptr &&
                tracks[old].type == p->preset[t].type) {
                next[t].ptr = sources[old].ptr;
                next[t].allocation = sources[old].allocation;
                used |= 1U << old;
                reused |= 1U << t;
                break;
            }
        }
        if (!(reused & (1U << t))) {
            if (source_create(&created, p->preset[t].type, p->preset[t].program)) {
                for (unsigned i = 0; i < t; ++i)
                    if (!(reused & (1U << i)))
                        source_dispose(next[i].ptr, next[i].allocation, p->preset[i].type);
                fm1_web_resume();
                return GROOVE_PROJECT_MEMORY;
            }
            next[t].ptr = created.ptr;
            next[t].allocation = created.allocation;
        }
    }
    /* No fallible operations remain. Retire unused instances, reset retained
     * ones to fresh-engine defaults, then publish all tracks before resuming. */
    if (operation)
        groove_edit_checkpoint(operation);
    for (unsigned t = 0; t < GROOVE_TRACKS; ++t)
        if (!(used & (1U << t)))
            source_destroy(t);
    arp_defaults();
    memset(live, 0, sizeof live);
    memset(seq_note, 0, sizeof seq_note);
    memset(parameter_lock, 0, sizeof parameter_lock);
    /* Older projects could save both flags. Solo wins on import. */
    mute_mask = p->mute & ~p->solo;
    solo_mask = p->solo;
    for (unsigned t = 0; t < GROOVE_TRACKS; ++t) {
        const struct groove_preset *v = &p->preset[t];
        sources[t].ptr = next[t].ptr;
        sources[t].allocation = next[t].allocation;
        if (reused & (1U << t))
            source_reset(&sources[t], v->type);
        track_preset_apply(t, v);
        unsigned notes = p->arp[t][0], behavior = p->arp[t][1];
        arpeggio[t].rate = notes & 3U;
        arpeggio[t].mode = (notes >> 2) & 3U;
        arpeggio[t].octaves = (notes >> 4) & 3U;
        arpeggio[t].gate = notes >> 6;
        arpeggio[t].swing = behavior & 63U;
        arpeggio[t].hold = (behavior >> 6) & 1U;
        arpeggio[t].enabled = behavior >> 7;
    }
    audio_reset();
    groove_sequence_restore(p);
    groove_effects_restore(p->effects);
    stereo_refresh();
    groove_project_metadata(p);
    edit_snapshot.track = ~0U;
    fm1_web_resume();
    return 0;
}
int groove_project_apply(const struct groove_project *p)
{
    return project_apply(p, 0);
}
int groove_project_apply_edit(const struct groove_project *p, unsigned operation)
{
    return project_apply(p, operation);
}
