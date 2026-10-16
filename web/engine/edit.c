/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#include "internal.h"
#include <string.h>

static struct groove_project undo;
static unsigned undo_ready, undo_busy, undo_tag, undo_time, record_track, edit_error;
static unsigned recording;
static unsigned dirty, clean_hash, check_time, hash_pending;
static uint32_t edit_revision;
static uint8_t cursor[8], take_step[8];
static uint32_t held[8][4], take[8][4];
static unsigned observed;
enum { RECORD_QUEUE = 128 };
static struct {
    unsigned read, write, overflow, changed, activity[GROOVE_TRACKS];
    struct { uint8_t track, note, velocity, step, alt; } event[RECORD_QUEUE];
} queue;
static struct {
    struct groove_step step[64];
    struct groove_preset preset;
    struct groove_sequence_options options;
    unsigned count, type, ready;
} clipboard;
static unsigned project_hash(const struct groove_project *p)
{
    const uint8_t *b = (const uint8_t *)p;
    unsigned h = 2166136261U;
    for (unsigned i = 0; i < sizeof *p; ++i)
        h = (h ^ b[i]) * 16777619U;
    return h;
}
void groove_project_touch(void)
{
    ++edit_revision;
    dirty = 1;
    hash_pending = 1;
    check_time = fm1_millis();
}
unsigned groove_project_dirty(void)
{
    return dirty;
}
uint32_t groove_edit_revision(void) { return edit_revision; }
void groove_project_clean(void)
{
    struct groove_project *p = groove_project_scratch();
    if (!groove_project_capture(p)) {
        clean_hash = project_hash(p);
        dirty = 0;
        hash_pending = 0;
    }
}
void groove_edit_init(void)
{
    undo_ready = undo_busy = undo_tag = undo_time = recording = record_track = dirty = clean_hash =
        check_time = hash_pending = 0;
    memset(&clipboard, 0, sizeof clipboard);
    memset(cursor, 0, sizeof cursor);
    memset(held, 0, sizeof held);
    memset(take, 0, sizeof take);
    memset(take_step, 255, sizeof take_step);
    edit_error = 0;
    queue.read = queue.write = queue.overflow = queue.changed = observed = 0;
    for (unsigned i = 0; i < GROOVE_TRACKS; ++i)
        queue.activity[i] = 0;
}
void groove_edit_checkpoint(unsigned operation)
{
    if (undo_busy || recording)
        return;
    unsigned now = fm1_millis();
    if (!undo_ready || operation != undo_tag || now - undo_time > 400U) {
        if (groove_project_capture(&undo))
            return;
        undo_ready = 1;
        undo_tag = operation;
    }
    undo_time = now;
}
int groove_edit_undo(void)
{
    if (!undo_ready || recording)
        return -1;
    struct groove_project *p = groove_project_scratch();
    int r = groove_project_capture(p);
    if (r)
        return r;
    undo_busy = 1;
    groove_sequence_keep_transport(0);
    r = groove_project_apply(&undo);
    if (!r) {
        undo = *p;
        groove_sequence_keep_transport(1);
        dirty = 1;
        groove_project_touch();
        undo_tag = ~0U;
    }
    undo_busy = 0;
    return r;
}
void groove_edit_record(unsigned enabled, unsigned track, unsigned step)
{
    if (track >= 8 || step >= 64)
        return;
    /* The panel calls this every poll; only transitions need the audio gate. */
    if (recording == (enabled != 0)) {
        record_track = track;
        return;
    }
    if (enabled && !recording) {
        groove_edit_checkpoint(0x70000U);
        edit_error = 0;
        memset(held, 0, sizeof held);
        memset(take, 0, sizeof take);
        memset(take_step, 255, sizeof take_step);
        for (unsigned t = 0; t < 8; ++t)
            cursor[t] = (uint8_t)(step % groove_sequence_length(t));
    }
    fm1_web_pause();
    queue.read = queue.write;
    queue.overflow = 0;
    recording = enabled != 0;
    fm1_web_resume();
    record_track = track;
    if (!recording)
        memset(held, 0, sizeof held);
}
FM1_DSP_CODE unsigned groove_edit_recording(void)
{
    return recording;
}
unsigned groove_edit_error(void)
{
    return edit_error;
}
unsigned groove_edit_cursor(void)
{
    return cursor[record_track];
}
void groove_edit_seek(unsigned track, unsigned step)
{
    if (track >= GROOVE_TRACKS || step >= groove_sequence_length(track)) return;
    groove_edit_poll();
    cursor[track] = (uint8_t)step;
    take_step[track] = 255;
    memset(held[track], 0, sizeof held[track]);
    memset(take[track], 0, sizeof take[track]);
}
FM1_DSP_CODE void groove_edit_midi_changed(void)
{
    ++queue.changed;
}
FM1_DSP_CODE void groove_record_midi(const uint8_t *m, size_t n)
{
    if (!m || n < 1)
        return;
    if (n == 1 && m[0] == 0xff) {
        queue.overflow = 2; /* Reset transport/keys without claiming overflow. */
        return;
    }
    unsigned channel = m[0] & 15U, track = channel == 9 ? 7 : channel;
    if (track >= 8 || groove_channel(track) != channel)
        return;
    unsigned type = m[0] & 0xf0U;
    if (n != 3 || (type != 0x90 && type != 0x80) || m[1] > 127 || m[2] > 127)
        return;
    ++queue.activity[track];
    if (!recording)
        return;
    unsigned position = groove_sequence_record_position(track);
    if (position == 254)
        return;
    unsigned w = queue.write, next = (w + 1U) & (RECORD_QUEUE - 1U);
    if (next == queue.read) {
        queue.overflow = 1;
        return;
    }
    queue.event[w].track = (uint8_t)track;
    queue.event[w].note = m[1];
    queue.event[w].velocity = type == 0x90 ? m[2] : 0;
    queue.event[w].step = (uint8_t)position;
    queue.event[w].alt = (uint8_t)groove_alt_held(track);
    queue.write = next;
}
unsigned groove_midi_activity(unsigned track)
{
    return track < 8 ? queue.activity[track] : 0;
}
void groove_edit_poll(void)
{
    if (queue.changed != observed) {
        observed = queue.changed;
        groove_project_touch();
    }
    if (queue.overflow) {
        if (recording && queue.overflow == 1)
            edit_error = 1;
        fm1_web_pause();
        queue.read = queue.write;
        queue.overflow = 0;
        fm1_web_resume();
        memset(held, 0, sizeof held);
        recording = 0;
    }
    for (unsigned budget = 0; budget < RECORD_QUEUE && queue.read != queue.write;
         ++budget) {
        unsigned r = queue.read;
        unsigned t = queue.event[r].track, note = queue.event[r].note,
                 velocity = queue.event[r].velocity, step = queue.event[r].step, alt = queue.event[r].alt;
        queue.read = (r + 1U) & (RECORD_QUEUE - 1U);
        if (!recording)
            continue;
        unsigned word = note >> 5;
        uint32_t bit = 1U << (note & 31U), was_held = held[t][word] & bit;
        if (velocity)
            held[t][word] |= bit;
        else
            held[t][word] &= ~bit;
        unsigned stopped = step == 255;
        if (stopped)
            step = cursor[t];
        if (groove_pad_count(t)) {
            if (velocity)
                groove_sequence_edit(t, step, note, velocity);
            continue;
        }
        if (take_step[t] != step) {
            memset(take[t], 0, sizeof take[t]);
            take_step[t] = (uint8_t)step;
        }
        if (velocity)
            take[t][word] |= bit;
        uint8_t notes[8];
        unsigned count = 0;
        uint32_t active = held[t][0] | held[t][1] | held[t][2] | held[t][3];
        /* Keep ascending MIDI order; a ninth note is enough to reject the chord. */
        for (unsigned k = 0; k < 4 && count <= 8; ++k) {
            for (uint32_t bits = take[t][k]; bits; bits &= bits - 1U) {
                if (count == 8) {
                    ++count;
                    break;
                }
                notes[count++] = (uint8_t)(k * 32U + (unsigned)__builtin_ctz(bits));
            }
        }
        if (velocity && (count > 8 || groove_sequence_chord(t, step, notes, count, velocity)))
            edit_error = 2;
        else if (velocity)
            groove_sequence_alt(t, step, alt);
        if (stopped && was_held && !active && !velocity) {
            cursor[t] = (uint8_t)((cursor[t] + 1U) % groove_sequence_length(t));
            take_step[t] = 255;
        }
    }
    /* Compare only after an edit settles; never hash in the audio callback. */
    if (hash_pending && fm1_millis() - check_time >= 500U && !recording) {
        struct groove_project *p = groove_project_scratch();
        check_time = fm1_millis();
        if (!groove_project_capture(p)) {
            dirty = project_hash(p) != clean_hash;
            hash_pending = 0;
        }
    }
}
unsigned groove_edit_clipboard_count(void) { return clipboard.ready ? clipboard.count : 0; }
int groove_edit_copy(unsigned track, unsigned step, unsigned scope)
{
    if (track >= 8 || step >= 64 || scope > 2)
        return -1;
    struct groove_project *p = groove_project_scratch();
    int r = groove_project_capture(p);
    if (r)
        return r;
    unsigned first = scope == 2 ? 0 : scope == 1 ? step & ~15U : step;
    clipboard.count = scope == 2 ? 64 : scope == 1 ? 16 : 1;
    memcpy(clipboard.step, p->step[track] + first, clipboard.count * sizeof clipboard.step[0]);
    clipboard.preset = p->preset[track];
    clipboard.options = p->sequence;
    clipboard.type = p->preset[track].type;
    clipboard.ready = 1;
    for (unsigned i = 0; i < GROOVE_LOCKS; ++i) {
        struct groove_parameter_lock *l = &clipboard.options.lock[i];
        if (l->track != track || l->step < first || l->step >= first + clipboard.count)
            l->track = 255;
        else {
            l->track = 0;
            l->step = (uint8_t)(l->step - first);
        }
    }
    return 0;
}
int groove_edit_paste(unsigned track, unsigned step, unsigned scope)
{
    if (track >= 8 || step >= 64 || scope > 2 || !clipboard.ready)
        return -1;
    unsigned first = scope == 2   ? 0
                     : scope == 1 ? step & ~15U
                                  : step,
             count = scope == 2   ? 64
                     : scope == 1 ? 16
                                  : 1;
    if (count != clipboard.count)
        return -1;
    if (scope != 2 && ((groove_pad_count(track) != 0) !=
                       (clipboard.type == GROOVE_808 || clipboard.type == GROOVE_SAMPLE)))
        return -1;
    struct groove_project *p = groove_project_scratch();
    int r = groove_project_capture(p);
    if (r)
        return r;
    if (scope == 2)
        p->preset[track] = clipboard.preset;
    memcpy(p->step[track] + first, clipboard.step, count * sizeof clipboard.step[0]);
    unsigned melodic = clipboard.type != GROOVE_808 && clipboard.type != GROOVE_SAMPLE;
    if (melodic) {
        unsigned used = 0;
        for (unsigned t = 0; t < 8; ++t)
            if (p->preset[t].type != GROOVE_808 && p->preset[t].type != GROOVE_SAMPLE)
                for (unsigned s = 0; s < 64; ++s)
                    if (t != track || s < first || s >= first + count) {
                        unsigned c = p->step[t][s].drums & 31U;
                        if (c)
                            used |= 1U << (c - 1);
                    }
        unsigned map[17] = {0};
        for (unsigned s = first; s < first + count; ++s) {
            unsigned id = p->step[track][s].drums & 31U;
            if (!id)
                continue;
            if (!map[id]) {
                for (unsigned i = 0; i < 16; ++i)
                    if (!memcmp(p->sequence.chord[i], clipboard.options.chord[id - 1], 8)) {
                        map[id] = i + 1;
                        break;
                    }
                if (!map[id])
                    for (unsigned i = 0; i < 16; ++i)
                        if (!(used & (1U << i))) {
                            memcpy(p->sequence.chord[i], clipboard.options.chord[id - 1], 8);
                            map[id] = i + 1;
                            break;
                        }
                if (!map[id])
                    return -2;
                used |= 1U << (map[id] - 1);
            }
            p->step[track][s].drums = (uint16_t)((p->step[track][s].drums & ~31U) | map[id]);
        }
    }
    for (unsigned i = 0; i < 16; ++i)
        if (p->sequence.lock[i].track == track && p->sequence.lock[i].step >= first &&
            p->sequence.lock[i].step < first + count)
            p->sequence.lock[i].track = 255;
    for (unsigned i = 0; i < 16; ++i)
        if (clipboard.options.lock[i].track != 255 &&
            (scope == 2 || clipboard.type == groove_source_type(track))) {
            unsigned slot = 16;
            for (unsigned j = 0; j < 16; ++j)
                if (p->sequence.lock[j].track == 255) {
                    slot = j;
                    break;
                }
            if (slot == 16)
                return -2;
            p->sequence.lock[slot] = clipboard.options.lock[i];
            p->sequence.lock[slot].track = (uint8_t)track;
            p->sequence.lock[slot].step += (uint8_t)first;
        }
    if (!groove_project_valid(p))
        return -1;
    groove_edit_checkpoint(0x80000U + track);
    if (scope < 2) {
        /* A step/page paste edits the pattern only. Preserve live voices,
         * arpeggiators and the transport instead of reloading every engine. */
        fm1_web_pause();
        groove_sequence_replace(track, first, p->step[track] + first, count);
        groove_sequence_options_set(&p->sequence);
        fm1_web_resume();
        return 0;
    }
    groove_sequence_keep_transport(0);
    r = groove_project_apply(p);
    if (!r) {
        groove_sequence_keep_transport(1);
        groove_project_touch();
    }
    return r;
}
int groove_edit_clear(unsigned track, unsigned step, unsigned scope)
{
    if (track >= 8 || step >= 64 || scope > 2)
        return -1;
    unsigned first = scope == 2   ? 0
                     : scope == 1 ? step & ~15U
                                  : step,
             count = scope == 2   ? 64
                     : scope == 1 ? 16
                                  : 1;
    struct groove_step cells[64];
    memset(cells, 0, sizeof cells);
    for (unsigned s = 0; s < count; ++s)
        cells[s].note = groove_pad_count(track) ? 0 : 60;
    groove_edit_checkpoint(0x90000U + track * 64 + step);
    groove_sequence_replace(track, first, cells, count);
    struct groove_sequence_options o;
    groove_sequence_options_copy(&o);
    for (unsigned i = 0; i < 16; ++i)
        if (o.lock[i].track == track && o.lock[i].step >= first && o.lock[i].step < first + count)
            o.lock[i].track = 255;
    groove_sequence_options_set(&o);
    return 0;
}
int groove_edit_repeat(unsigned track, unsigned steps)
{
    if (track >= 8 || steps < 16 || steps > 64 || (steps & 15U))
        return -1;
    unsigned old = groove_sequence_length(track);
    if (steps <= old)
        return -1;
    struct groove_step cells[64];
    groove_sequence_read(track, 0, cells, old);
    for (unsigned s = old; s < steps; ++s)
        cells[s] = cells[s % old];
    struct groove_sequence_options options;
    groove_sequence_options_copy(&options);
    for (unsigned i = 0; i < GROOVE_LOCKS; ++i)
        if (options.lock[i].track == track && options.lock[i].step >= old &&
            options.lock[i].step < steps)
            options.lock[i].track = 255;
    for (unsigned s = old; s < steps; ++s)
        for (unsigned i = 0; i < GROOVE_LOCKS; ++i)
            if (options.lock[i].track == track && options.lock[i].step == s % old) {
                unsigned free = 0;
                while (free < GROOVE_LOCKS && options.lock[free].track != 255)
                    ++free;
                if (free == GROOVE_LOCKS)
                    return -2;
                options.lock[free] = options.lock[i];
                options.lock[free].step = (uint8_t)s;
                break;
            }
    groove_edit_checkpoint(0xa0000U + track);
    unsigned busy = undo_busy;
    undo_busy = 1;
    groove_sequence_replace(track, 0, cells, steps);
    groove_sequence_options_set(&options);
    groove_sequence_length_set(track, steps);
    undo_busy = busy;
    return 0;
}
int groove_timbre_export(unsigned track, uint8_t *data, unsigned capacity)
{
    if (track >= 8 || !data || capacity < sizeof(struct groove_preset))
        return -1;
    struct groove_project *p = groove_project_scratch();
    int r = groove_project_capture(p);
    if (!r)
        memcpy(data, &p->preset[track], sizeof p->preset[track]);
    return r;
}
int groove_timbre_import(unsigned track, const uint8_t *data, unsigned bytes)
{
    if (track >= 8 || !data || bytes != sizeof(struct groove_preset))
        return -1;
    struct groove_project *p = groove_project_scratch();
    int r = groove_project_capture(p);
    if (r)
        return r;
    struct groove_preset preset;
    memcpy(&preset, data, sizeof preset);
    unsigned old = p->preset[track].type;
    preset.sequence_pages = p->preset[track].sequence_pages;
    p->preset[track] = preset;
    if ((old == GROOVE_808 || old == GROOVE_SAMPLE) !=
        (preset.type == GROOVE_808 || preset.type == GROOVE_SAMPLE))
        for (unsigned s = 0; s < 64; ++s) {
            p->step[track][s].drums = 0;
            p->step[track][s].velocity = 0;
            p->step[track][s].note =
                preset.type == GROOVE_808 || preset.type == GROOVE_SAMPLE ? 0 : 60;
        }
    for (unsigned i = 0; i < 16; ++i)
        if (p->sequence.lock[i].track == track)
            p->sequence.lock[i].track = 255;
    if (!groove_project_valid(p))
        return -1;
    groove_edit_checkpoint(0xb0000U + track);
    return groove_timbre_apply(track, p);
}
