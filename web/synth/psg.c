/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* Eight-note MIDI/panel adapter for Mitsutaka Okazaki's emu2149.
 * Three emulated PSGs provide eight voices; the ninth channel stays silent.
 * Each group of three voices shares its chip's hardware-style noise generator.
 */
#include "voices.h"
#include "fixed.h"
#include <string.h>

enum { PSG_CONTROL_FRAMES = 64 };


static const char *const modes[] = {"YM", "AY", "NOISE", "MIX"};
static FM1_DSP_DATA const fm1_param_t params[PSG_PARAMS] = {
    {"MODE", 3, 0, modes}, {"NOISE", 31, 16, 0}, {"TUNE", 48, 24, 0}, {"RELEASE", 127, 32, 0}};
const fm1_param_t *psg_param(unsigned parameter)
{
    return parameter < PSG_PARAMS ? &params[parameter] : NULL;
}
FM1_DSP_CODE unsigned psg_get(const psg_t *p, unsigned parameter)
{
    return parameter < PSG_PARAMS ? p->pot[parameter] : 0;
}
static FM1_DSP_CODE void tune(psg_t *p, unsigned voice)
{
    int32_t note = ((int32_t)p->voice[voice].note + (int)p->pot[2] - 24) * 65536 + p->bend + p->voice[voice].tune;
    uint32_t hz = fm_note_hz_q16(note);
    uint64_t denominator = (uint64_t)16 * hz;
    unsigned period = (unsigned)((UINT64_C(1789772) * 65536 + denominator / 2) / denominator);
    if (period < 1) period = 1;
    if (period > 4095) period = 4095;
    PSG *chip = &p->chip[voice / 3];
    unsigned channel = voice % 3;
    PSG_writeReg(chip, channel * 2, period & 255);
    PSG_writeReg(chip, channel * 2 + 1, period >> 8);
}
FM1_DSP_CODE void psg_set(psg_t *p, unsigned parameter, unsigned value)
{
    if (parameter >= PSG_PARAMS || value > params[parameter].max)
        return;
    p->pot[parameter] = (uint8_t)value;
    if (parameter == 0) {
        for (unsigned i = 0; i < PSG_CHIPS; ++i) {
            PSG_setVolumeMode(&p->chip[i], value == 1 ? 2 : 1);
            PSG_writeReg(&p->chip[i], 7, value == 2 ? 7 : value == 3 ? 0 : 56);
        }
    } else if (parameter == 1) {
        for (unsigned i = 0; i < PSG_CHIPS; ++i)
            PSG_writeReg(&p->chip[i], 6, value);
    } else if (parameter == 2) {
        for (unsigned i = 0; i < PSG_VOICES; ++i)
            if (p->voice[i].note != 0xff)
                tune(p, i);
    } else {
        uint64_t time = 635U + 995U * value;
        p->release = (int32_t)fm_exp2_fixed(-(int32_t)(INT64_C(21234160675974) * PSG_CONTROL_FRAMES / (FM1_SAMPLE_RATE * time)), 30);
    }
}
/* Runtime protection keeps sounding keys ahead of released tails. */
FM1_DSP_CODE unsigned psg_limit(psg_t *p, unsigned limit)
{
    if (!limit || limit > PSG_VOICES) return 0;
    p->limit = (uint8_t)limit;
    unsigned active = 0, retired = 0;
    for (unsigned i = 0; i < PSG_VOICES; ++i) active += p->voice[i].note != 0xff;
    while (active > limit) {
        unsigned slot = PSG_VOICES, best = 0;
        for (unsigned i = 0; i < PSG_VOICES; ++i) {
            if (p->voice[i].note == 0xff) continue;
            unsigned rank = !p->voice[i].down && !p->pedal ? 2U : !p->voice[i].down ? 1U : 0U;
            if (slot == PSG_VOICES || rank > best || (rank == best &&
                (uint32_t)(p->age - p->voice[i].age) > (uint32_t)(p->age - p->voice[slot].age))) {
                slot = i; best = rank;
            }
        }
        p->voice[slot].note = 0xff;
        p->voice[slot].gain = 0;
        --active; ++retired;
    }
    return retired;
}
FM1_DSP_CODE void psg_panic(psg_t *p)
{
    for (unsigned i = 0; i < PSG_CHIPS; ++i)
        PSG_reset(&p->chip[i]);
    for (unsigned i = 0; i < PSG_VOICES; ++i)
        p->voice[i].note = 0xff;
    p->pedal = 0;
    p->bend = p->dc_input = p->dc_output = 0;
    p->dc_error = 0;
    psg_set(p, 0, p->pot[0]);
    psg_set(p, 1, p->pot[1]);
}
void psg_init(psg_t *p)
{
    memset(p, 0, sizeof *p);
    p->limit = PSG_VOICES;
    /* Quality 1 uses the chip clock; no allocator or external resampler. */
    for (unsigned i = 0; i < PSG_CHIPS; ++i) {
        p->chip[i].rate = FM1_SAMPLE_RATE;
        p->chip[i].quality = 1;
        PSG_setClock(&p->chip[i], 1789772);
    }
    psg_panic(p);
    for (unsigned i = 0; i < PSG_PARAMS; ++i)
        psg_set(p, i, params[i].def);
}
FM1_DSP_CODE void psg_note(psg_t *p, unsigned note, unsigned velocity, int cents)
{
    if (note > 127 || velocity > 127)
        return;
    if (!velocity) {
        for (unsigned i = 0; i < PSG_VOICES; ++i)
            if (p->voice[i].note == note)
                p->voice[i].down = 0;
        return;
    }
    unsigned active = 0;
    for (unsigned i = 0; i < PSG_VOICES; ++i) active += p->voice[i].note != 0xff;
    unsigned slot = PSG_VOICES, best = 0;
    for (unsigned i = 0; i < PSG_VOICES; ++i) {
        if (p->voice[i].note == note) {
            slot = i;
            break;
        }
        if (active >= p->limit && p->voice[i].note == 0xff) continue;
        unsigned rank = p->voice[i].note == 0xff         ? 3U
                        : !p->voice[i].down && !p->pedal ? 2U
                        : !p->voice[i].down              ? 1U
                                                         : 0U;
        if (slot == PSG_VOICES || rank > best || (rank == best && (uint32_t)(p->age - p->voice[i].age) >
                                                (uint32_t)(p->age - p->voice[slot].age))) {
            best = rank;
            slot = i;
        }
    }
    p->voice[slot].note = (uint8_t)note;
    p->voice[slot].down = 1;
    p->voice[slot].tune = cents * 16384 / 25;
    p->voice[slot].age = ++p->age;
    p->voice[slot].gain = (int32_t)((int64_t)velocity * FM_Q30_ONE / 127);
    tune(p, slot);
}
FM1_DSP_CODE void psg_midi(psg_t *p, const uint8_t m[3])
{
    unsigned reset = (m[0] & 0xf0) == 0xb0 && m[1] == 121;
    if (reset) p->pedal = 0;
    if ((m[0] & 0xf0) == 0xb0 && m[1] == 64)
        p->pedal = m[2] >= 64;
    else if ((m[0] & 0xf0) == 0xe0 || reset) {
        int32_t bend = reset ? 0 : ((int)m[1] + ((int)m[2] << 7) - 8192) * 16;
        if (bend == p->bend) return;
        p->bend = bend;
        for (unsigned i = 0; i < PSG_VOICES; ++i)
            if (p->voice[i].note != 0xff)
                tune(p, i);
    }
}
static FM1_DSP_CODE void render_quantum(psg_t *p, int32_t *out, unsigned frames)
{
    unsigned active = 0;
    for (unsigned i = 0; i < PSG_VOICES; ++i) {
        unsigned level = 0;
        if (p->voice[i].note != 0xff) {
            if (!p->voice[i].down && !p->pedal)
                p->voice[i].gain = fm_mul30(p->voice[i].gain, p->release);
            level = (unsigned)(((int64_t)15 * p->voice[i].gain + FM_Q30_ONE / 2) >> 30);
            if (!level && (p->voice[i].down || p->pedal))
                level = 1;
            if (!level)
                p->voice[i].note = 0xff;
        }
        if (level)
            active |= 1U << (i / 3);
        PSG_writeReg(&p->chip[i / 3], 8 + i % 3, level);
    }
    for (unsigned i = 0; i < PSG_CHIPS; ++i)
        if (!(active & (1U << i)))
            p->chip[i].out = 0;
    if (!active) {
        if (p->dc_output > -17 && p->dc_output < 17 && p->dc_input == 0) {
            p->dc_output = 0;
            p->dc_error = 0;
            memset(out, 0, frames * sizeof *out);
            return;
        }
    }
    /* Chip state is independent. Render a whole block per chip to avoid
     * testing the active mask and calling the rate converter per sample.
     * Three int16 chip outputs sum exactly in int32 before normalization. */
    memset(out, 0, frames * sizeof *out);
    for (unsigned chip = 0; chip < PSG_CHIPS; ++chip) {
        if (!(active & (1U << chip))) continue;
        PSG_mix(&p->chip[chip], out, frames);
    }
    int32_t previous = p->dc_input, filtered = p->dc_output;
    for (unsigned i = 0; i < frames; ++i) {
        /* Three S16 chip outputs bound this product below 2^28. */
        int32_t sample = (out[i] * 2048) / 3;
        /* Feed back the reduction remainder so a quiet negative tail cannot
         * stick at a fixed-point limit cycle or accumulate a DC bias. */
        int64_t feedback = (int64_t)1072668082 * filtered + p->dc_error;
        p->dc_error = (uint32_t)feedback & 0x3fffffffU;
        filtered = sample - previous + (int32_t)(feedback >> 30);
        previous = sample;
        out[i] = filtered;
    }
    p->dc_input = previous;
    p->dc_output = filtered;
}

FM1_DSP_CODE void psg_render(psg_t *p, int32_t *out, unsigned frames)
{
    /* Release and AY volume quantization use the original 64-sample control
     * rate. A longer device callback must not lengthen the release envelope. */
    while (frames) {
        unsigned count = frames > PSG_CONTROL_FRAMES ? PSG_CONTROL_FRAMES : frames;
        render_quantum(p, out, count);
        out += count;
        frames -= count;
    }
}

size_t psg_size(void) { return sizeof(psg_t); }
FM1_DSP_CODE unsigned psg_active(const psg_t *p)
{
    unsigned count = 0;
    for (unsigned i = 0; i < PSG_VOICES; ++i) count += p->voice[i].note != 0xff;
    return count;
}

FM1_DSP_CODE unsigned psg_active_chips(const psg_t *p)
{
    unsigned active = 0;
    for (unsigned chip = 0; chip < PSG_CHIPS; ++chip) {
        unsigned start = chip * 3, end = start + 3;
        if (end > PSG_VOICES) end = PSG_VOICES;
        for (unsigned i = start; i < end; ++i)
            if (p->voice[i].note != 0xff) { ++active; break; }
    }
    return active;
}
