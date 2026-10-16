/* SPDX-License-Identifier: MIT */
/* Portable DSP state. Application code uses handles and size queries only. */
#ifndef FM1_VOICES_H
#define FM1_VOICES_H
#include "bass303_state.h"
#include "drum808_state.h"
#include "psg.h"
#include "pluck.h"
#include "sampledrum.h"
#include "emu2149.h"
struct bass_voice {
    bass303_t engine[BASS303_VOICES], reset;
    struct {
        uint32_t age;
        uint8_t note;
    } voice[BASS303_VOICES];
    uint32_t age;
    int8_t cents[128]; /* tuning captured at note-on, for mono re-triggers */
    uint8_t order[128], velocity[128], down[128], pedal;
    unsigned count, limit;
};
struct drum_voice {
    drum808_t engine, reset;
    uint32_t age, hit[D8_NUM];
    unsigned limit, mix_mode;
};

enum { PSG_CHIPS = (PSG_VOICES + 2) / 3 };

struct psg {
    PSG chip[PSG_CHIPS];
    struct {
        int32_t gain, tune; /* tune: Q16 semitones, fixed at note-on. */
        uint32_t age;
        uint8_t note, down;
    } voice[PSG_VOICES];
    int32_t release, bend, dc_input, dc_output;
    uint32_t dc_error;
    uint32_t age;
    uint8_t pot[PSG_PARAMS], pedal, limit;
};

enum { PLUCK_DELAY = 1024 };

typedef struct {
    float delay[PLUCK_DELAY], delay_fraction, rate, clock, filter, feedback, damping;
    float gain, release, previous, current;
    uint32_t position, age, quiet, delay_length;
    int32_t tune; /* Q16 semitones, fixed at note-on. */
    uint8_t note, down;
} pluck_voice_t;
struct pluck {
    pluck_voice_t voice[PLUCK_VOICES];
    uint32_t random, age;
    int32_t bend;
    uint8_t pot[PLUCK_PARAMS], pedal, limit;
};

struct sampledrum {
    struct {
        uint32_t position, fraction, step, end, next_group, age;
        int32_t state[8];
        int16_t pcm[40];
        int32_t gain, last, tail, lowpass, filter;
        uint32_t fade_reciprocal; /* Q48 reciprocal of the EOF fade span. */
        uint8_t pad, active, fade, stopping, choke;
    } voice[SAMPLE_DRUM_VOICES];
    uint32_t age, limit;
    uint8_t pot[SAMPLE_DRUM_PADS][SAMPLE_DRUM_PARAMS];
    uint8_t round[SAMPLE_DRUM_PADS];
};

#endif
