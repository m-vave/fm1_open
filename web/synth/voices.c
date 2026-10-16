/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#include "voices.h"
#include "fixed.h"
#include <string.h>

/* The 808 circuit uses float internally; public audio buses use Q24.
 * Bounded local buffers keep float and integer storage disjoint. */
static FM1_DSP_CODE int32_t circuit_sample(float value)
{
    if (!(value == value)) return 0;
    if (value >= 128.0f) return INT32_MAX;
    if (value <= -128.0f) return INT32_MIN;
    return (int32_t)(value * 16777216.0f);
}

/* Saturating float-to-Q24 mix. Apply reverb before delay when sends alias. */
static FM1_DSP_CODE void circuit_add32(int32_t *out, const float *input)
{
    for (unsigned i = 0; i < 32; ++i)
        out[i] = fm_add_sat(out[i], circuit_sample(input[i]));
}

static FM1_DSP_CODE void bass_forget(struct bass_voice *b, unsigned note)
{
    for (unsigned i = 0; i < b->count; ++i)
        if (b->order[i] == note) {
            memmove(b->order + i, b->order + i + 1, b->count - i - 1);
            --b->count;
            break;
        }
}
/* Mono retains the original last-key priority and legato slide. Poly owns a
 * complete filter/envelope per voice, preferring idle or released slots. */
static FM1_DSP_CODE void bass_poly_note(struct bass_voice *b, unsigned note, unsigned velocity, int cents)
{
    b->down[note] = velocity != 0;
    if (!velocity) {
        if (!b->pedal)
            for (unsigned i = 0; i < BASS303_VOICES; ++i)
                if (b->voice[i].note == note)
                    bass303_note_off(&b->engine[i]);
        return;
    }
    unsigned active = 0;
    for (unsigned i = 0; i < BASS303_VOICES; ++i) active += !b->engine[i].idle;
    unsigned slot = BASS303_VOICES, best = 0;
    for (unsigned i = 0; i < BASS303_VOICES; ++i) {
        if (b->voice[i].note == note && (!b->engine[i].idle || active < b->limit)) {
            slot = i;
            break;
        }
        if (active >= b->limit && b->engine[i].idle) continue;
        unsigned rank = b->engine[i].idle ? 3U : !b->engine[i].gate ? 2U : 1U;
        if (slot == BASS303_VOICES || rank > best || (rank == best && (uint32_t)(b->age - b->voice[i].age) >
                                                (uint32_t)(b->age - b->voice[slot].age))) {
            best = rank;
            slot = i;
        }
    }
    b->voice[slot].note = (uint8_t)note;
    b->voice[slot].age = ++b->age;
    bass303_note_on(&b->engine[slot], (int)note, velocity >= 100, 0, cents);
}
/* Each contribution is S32 / 8. Even eight such contributions fit S32, so
 * the fixed four-voice sum needs no saturating add. */
_Static_assert(BASS303_VOICES <= 8, "303 fixed-gain mix headroom");
FM1_DSP_CODE unsigned bass303_bank_active(const bass303_bank_t *b)
{
    unsigned count = 0;
    for (unsigned i = 0; i < BASS303_VOICES; ++i)
        count += b->engine[i].started && !b->engine[i].idle;
    return count;
}
FM1_DSP_CODE void bass303_bank_render(struct bass_voice *b, unsigned mono_mode, int32_t *out,
                                    int32_t *temp)
{
    if (mono_mode) {
        bass303_render(&b->engine[0], out, FM1_BLOCK_FRAMES);
        return;
    }
    memset(out, 0, FM1_BLOCK_FRAMES * sizeof *out);
    for (unsigned voice = 0; voice < BASS303_VOICES; ++voice) {
        if (!b->engine[voice].started)
            continue;
        if (b->engine[voice].idle) {
            bass303_advance_idle(&b->engine[voice], FM1_BLOCK_FRAMES);
            continue; /* Output is already clear; no all-zero temp or mix. */
        }
        bass303_render(&b->engine[voice], temp, FM1_BLOCK_FRAMES);
        /* Fixed headroom: adding/releasing a key never changes other gains. */
        for (unsigned i = 0; i < FM1_BLOCK_FRAMES; ++i)
            out[i] += temp[i] / 8;
    }
}
FM1_DSP_CODE void bass303_bank_note(bass303_bank_t *b, unsigned mono, unsigned note, unsigned velocity, int cents)
{
    if (note > 127 || velocity > 127) return;
    if (!mono) {
        bass_poly_note(b, note, velocity, cents);
        return;
    }
    if (velocity) {
        int slide = b->count != 0;
        bass_forget(b, note);
        b->order[b->count++] = (uint8_t)note;
        b->velocity[note] = (uint8_t)velocity;
        b->down[note] = 1;
        b->cents[note] = (int8_t)cents;
        bass303_note_on(&b->engine[0], (int)note, velocity >= 100, slide, cents);
    } else {
        int top = b->count && b->order[b->count - 1] == note;
        b->down[note] = 0;
        if (!b->pedal)
            bass_forget(b, note);
        if (!b->count)
            bass303_note_off(&b->engine[0]);
        else if (top && !b->pedal) {
            unsigned last = b->order[b->count - 1];
            bass303_note_on(&b->engine[0], (int)last, b->velocity[last] >= 100, 1, b->cents[last]);
        }
    }
}
FM1_DSP_CODE void bass303_bank_pedal(bass303_bank_t *b, unsigned mono, unsigned down)
{
    b->pedal = down != 0;
    if (!mono) {
        if (!b->pedal)
            for (unsigned i = 0; i < BASS303_VOICES; ++i)
                if (b->voice[i].note != 0xff && !b->down[b->voice[i].note])
                    bass303_note_off(&b->engine[i]);
    } else if (!b->pedal) {
        /* Keep last-key order in one pass; repeated searches/memmoves are
         * unnecessary when releasing many sustained keys together. */
        unsigned kept = 0;
        for (unsigned i = 0; i < b->count; ++i) {
            unsigned note = b->order[i];
            if (b->down[note])
                b->order[kept++] = (uint8_t)note;
        }
        b->count = kept;
        if (!b->count)
            bass303_note_off(&b->engine[0]);
        else {
            unsigned note = b->order[b->count - 1];
            bass303_note_on(&b->engine[0], (int)note, b->velocity[note] >= 100, 1, b->cents[note]);
        }
    }
}
FM1_DSP_CODE unsigned bass303_bank_limit(bass303_bank_t *b, unsigned limit)
{
    if (!limit || limit > BASS303_VOICES) return 0;
    b->limit = limit;
    unsigned active = 0, retired = 0;
    for (unsigned i = 0; i < BASS303_VOICES; ++i) active += !b->engine[i].idle;
    while (active > limit) {
        unsigned slot = BASS303_VOICES;
        for (unsigned i = 0; i < BASS303_VOICES; ++i) {
            if (b->engine[i].idle) continue;
            if (slot == BASS303_VOICES || b->engine[i].gate < b->engine[slot].gate ||
                (b->engine[i].gate == b->engine[slot].gate &&
                 (uint32_t)(b->age - b->voice[i].age) > (uint32_t)(b->age - b->voice[slot].age))) slot = i;
        }
        b->engine[slot] = b->reset;
        b->voice[slot].note = 0xff;
        --active; ++retired;
    }
    return retired;
}
FM1_DSP_CODE void bass303_bank_panic(bass303_bank_t *b)
{
    bass303_bend(&b->reset, 0);
    for (unsigned i = 0; i < BASS303_VOICES; ++i) {
        b->engine[i] = b->reset;
        b->voice[i].note = 0xff;
    }
    b->age = 0;
    b->count = b->pedal = 0;
    memset(b->down, 0, sizeof b->down);
}
FM1_DSP_CODE size_t bass303_bank_size(void) { return sizeof(bass303_bank_t); }
FM1_DSP_CODE void bass303_bank_bend(bass303_bank_t *b, int32_t bend)
{
    for (unsigned i = 0; i < BASS303_VOICES; ++i) bass303_bend(&b->engine[i], bend);
    bass303_bend(&b->reset, bend);
}
FM1_DSP_CODE void bass303_bank_init(bass303_bank_t *b)
{
    memset(b, 0, sizeof *b);
    bass303_init(&b->reset);
    b->limit = BASS303_VOICES;
    for (unsigned i = 0; i < BASS303_VOICES; ++i) {
        b->engine[i] = b->reset;
        b->voice[i].note = 0xff;
    }
}
FM1_DSP_CODE void bass303_bank_set(bass303_bank_t *b, unsigned parameter, unsigned value)
{
    if (parameter >= BASS303_NPARAMS || value > bass303_param((int)parameter)->max) return;
    for (unsigned i = 0; i < BASS303_VOICES; ++i)
        bass303_set(&b->engine[i], (int)parameter, (int)value);
    bass303_set(&b->reset, (int)parameter, (int)value);
}
FM1_DSP_CODE unsigned bass303_bank_get(const bass303_bank_t *b, unsigned parameter)
{
    return (unsigned)bass303_get(&b->reset, (int)parameter);
}
FM1_DSP_CODE size_t drum808_kit_size(void) { return sizeof(drum808_kit_t); }
/* Routing changes only with an edited/imported preset. Inspect both variants
 * here so switching sounds and letting the previous variant ring remain safe. */
static FM1_DSP_CODE void kit_routing(drum808_kit_t *kit)
{
    const drum808_t *d = &kit->reset;
    unsigned mode = 0;
    for (unsigned p = 0; p < D8_NUM; ++p)
        if (d->tpan[p] != 64) mode |= 1U;
    for (unsigned p = 0; p < D8S_NUM; ++p) {
        if (d->pot[p][D8P_REV]) mode |= 2U;
        if (d->pot[p][D8P_DLY]) mode |= 4U;
    }
    kit->mix_mode = mode;
}
FM1_DSP_CODE void drum808_kit_init(drum808_kit_t *d)
{
    drum808_init(&d->engine);
    d->reset = d->engine;
    d->age = 0;
    memset(d->hit, 0, sizeof d->hit);
    d->limit = D8_NUM;
    kit_routing(d);
}
FM1_DSP_CODE void drum808_kit_panic(drum808_kit_t *d) { d->engine = d->reset; }
static FM1_DSP_CODE unsigned kit_part(drum808_t *d, unsigned part, unsigned stop)
{
    uint8_t *a, *b = NULL;
    switch (part) {
    case D8_BD: a = &d->bd.active; break;
    case D8_SD: a = &d->sd.active; break;
    case D8_LT: case D8_MT: case D8_HT: a = &d->tom[part-D8_LT].active; break;
    case D8_RS: a = &d->rs.active; b = &d->cl.active; break;
    case D8_CP: a = &d->cp.active; b = &d->ma.active; break;
    case D8_CB: a = &d->cb.active; break;
    case D8_CY: a = &d->cy.active; break;
    case D8_OH: a = &d->oh.active; break;
    default: a = &d->ch.active; break;
    }
    unsigned active = *a || (b && *b);
    if (stop) { *a = 0; if (b) *b = 0; }
    return active;
}
FM1_DSP_CODE unsigned drum808_kit_active(const drum808_kit_t *kit)
{
    const drum808_t *d = &kit->engine;
    return d->bd.active + d->sd.active + d->tom[0].active + d->tom[1].active + d->tom[2].active +
           (d->rs.active || d->cl.active) + (d->cp.active || d->ma.active) +
           d->cb.active + d->cy.active + d->oh.active + d->ch.active;
}
FM1_DSP_CODE unsigned drum808_kit_limit(drum808_kit_t *d, unsigned limit)
{
    if (!limit || limit > D8_NUM) return 0;
    d->limit = limit;
    unsigned active = 0, retired = 0;
    for (unsigned p = 0; p < D8_NUM; ++p) active += kit_part(&d->engine, p, 0);
    while (active > limit) {
        unsigned oldest = D8_NUM;
        for (unsigned p = 0; p < D8_NUM; ++p)
            if (kit_part(&d->engine, p, 0) && (oldest == D8_NUM ||
                (uint32_t)(d->age-d->hit[p]) > (uint32_t)(d->age-d->hit[oldest]))) oldest = p;
        kit_part(&d->engine, oldest, 1); --active; ++retired;
    }
    return retired;
}
FM1_DSP_CODE void drum808_kit_trigger(drum808_kit_t *d, int part, unsigned velocity)
{
    if ((unsigned)part >= D8_NUM || !velocity || velocity > 127U) return;
    if (d->limit < D8_NUM && !kit_part(&d->engine, (unsigned)part, 0)) {
        /* Reserve one slot for this hit without changing the sustained cap. */
        unsigned limit = d->limit;
        if (limit == 1) {
            for (unsigned p = 0; p < D8_NUM; ++p) kit_part(&d->engine, p, 1);
        } else (void)drum808_kit_limit(d, limit-1U);
        d->limit = limit;
    }
    d->hit[part] = ++d->age;
    drum808_trigger(&d->engine, part, (float)velocity * (1.0f / 127.0f));
}
static FM1_DSP_CODE void kit_render(drum808_kit_t *d, int32_t *left, int32_t *right,
                                    int32_t *rev, int32_t *dly)
{
    /* With no sounding circuit only the pan ramps can advance. Avoid
     * clearing/converting four silent float buses, while retaining the exact
     * chunk cadence so a later trigger starts at the same pan position. */
    if (!drum808_active(&d->engine)) {
        if (right) drum808_advance_idle(&d->engine, FM1_BLOCK_FRAMES);
        return;
    }
    /* The circuit renderer already advances pans, metal oscillators and tail
     * detection in 32-frame chunks. Preserve those exact boundaries without
     * adding persistent per-kit buffers or allocations in the audio task. */
    float bus[4][32];
    for (unsigned off = 0; off < FM1_BLOCK_FRAMES; off += 32) {
        memset(bus, 0, sizeof bus);
        if (right)
            drum808_render_st(&d->engine, bus[0], bus[1], bus[2], bus[3], 32);
        else
            drum808_render(&d->engine, bus[0], bus[2], bus[3], 32);
        circuit_add32(left + off, bus[0]);
        if (right) circuit_add32(right + off, bus[1]);
        if (d->mix_mode & 2U) circuit_add32(rev + off, bus[2]);
        if (d->mix_mode & 4U) circuit_add32(dly + off, bus[3]);
    }
}
FM1_DSP_CODE void drum808_kit_render(drum808_kit_t *d, int32_t *dry, int32_t *rev, int32_t *dly)
{
    kit_render(d, dry, 0, rev, dly);
}
FM1_DSP_CODE void drum808_kit_render_st(drum808_kit_t *d, int32_t *left, int32_t *right, int32_t *rev, int32_t *dly)
{
    kit_render(d, left, right, rev, dly);
}
FM1_DSP_CODE unsigned drum808_kit_stereo(const drum808_kit_t *kit)
{
    return kit->mix_mode != 0;
}
FM1_DSP_CODE void drum808_kit_set(drum808_kit_t *d, int part, int parameter, int value)
{
    drum808_set(&d->engine, part, parameter, value);
    drum808_set(&d->reset, part, parameter, value);
    drum808_pan_settle(&d->reset);
    kit_routing(d);
}
FM1_DSP_CODE int drum808_kit_get(const drum808_kit_t *d, int part, int parameter)
{
    return drum808_get(&d->reset, part, parameter);
}
FM1_DSP_CODE int drum808_preset_valid(const uint8_t values[D8_PRESET_BYTES])
{
    unsigned at = D8_NUM;
    for (unsigned part = 0; part < D8_NUM; ++part) if (values[part] > 1) return 0;
    for (unsigned variant = 0; variant < 2; ++variant)
        for (unsigned part = 0; part <= D8_NUM; ++part)
            for (int i = 0; i < drum808_nparams((int)part); ++i)
                if (values[at++] > drum808_param((int)part, i)->max) return 0;
    return at == D8_PRESET_BYTES;
}
FM1_DSP_CODE void drum808_kit_store(drum808_kit_t *kit, uint8_t values[D8_PRESET_BYTES])
{
    drum808_t *d = &kit->reset;
    memcpy(values, d->sw, D8_NUM);
    unsigned at = D8_NUM;
    for (unsigned variant = 0; variant < 2; ++variant) {
        memset(d->sw, (int)variant, sizeof d->sw);
        for (unsigned part = 0; part <= D8_NUM; ++part)
            for (int i = 0; i < drum808_nparams((int)part); ++i)
                values[at++] = (uint8_t)drum808_get(d, (int)part, i);
    }
    memcpy(d->sw, values, D8_NUM);
}
FM1_DSP_CODE int drum808_kit_load(drum808_kit_t *kit, const uint8_t values[D8_PRESET_BYTES])
{
    if (!drum808_preset_valid(values)) return -1;
    drum808_t *d = &kit->engine;
    /* Import starts silent even when called on a sounding kit. The reset
     * template must never capture active voices for a later panic. */
    drum808_init(d);
    unsigned at = D8_NUM;
    for (unsigned variant = 0; variant < 2; ++variant) {
        memset(d->sw, (int)variant, sizeof d->sw);
        for (unsigned part = 0; part <= D8_NUM; ++part)
            for (int i = 0; i < drum808_nparams((int)part); ++i)
                drum808_set(d, (int)part, i, values[at++]);
    }
    memcpy(d->sw, values, D8_NUM);
    drum808_pan_settle(d);
    kit->reset = *d;
    kit_routing(kit);
    return 0;
}
