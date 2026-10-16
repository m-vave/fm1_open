/* SPDX-License-Identifier: MIT */
#ifndef FM1_BASS303_H
#define FM1_BASS303_H
#include "engine.h"
#include "fm1_param.h"

/* Open303-based instrument. Parameters retain existing FM1 project values. */
/* parameter indices (pages of four: the UI shows KNOB 1-4) */
enum {
    BASS303_CUTOFF, BASS303_RESO, BASS303_ENVMOD, BASS303_DECAY,     /* page 1 */
    BASS303_ACCENT, BASS303_WAVE, BASS303_TUNE, BASS303_VOLUME,      /* page 2 */
    BASS303_DRIVE, BASS303_DRVTYPE, BASS303_SLIDE, BASS303_ACCDEC,   /* page 3 */
    BASS303_NPARAMS
};
enum { BASS303_DRV_OFF, BASS303_DRV_SOFT, BASS303_DRV_RAT };


enum { BASS303_VOICES = 4 };
typedef struct bass_voice bass303_bank_t;
/* Caller owns 32-byte-aligned storage. Only size/init run on control; live
 * calls belong to one audio owner or a held gate. Panic retains parameters. */
FM1_DSP_CODE size_t bass303_bank_size(void);
FM1_DSP_CODE void bass303_bank_init(bass303_bank_t *b);
FM1_DSP_CODE void bass303_bank_panic(bass303_bank_t *b);
FM1_DSP_CODE void bass303_bank_note(bass303_bank_t *b, unsigned mono, unsigned note, unsigned velocity, int cents);
FM1_DSP_CODE void bass303_bank_pedal(bass303_bank_t *b, unsigned mono, unsigned down);
FM1_DSP_CODE void bass303_bank_bend(bass303_bank_t *b, int32_t semitones_q16);
/* mono selects last-note-priority slide; poly uses four voices and fixed 1/8
 * gain. Both disjoint buffers hold 128 Q24 samples; output is overwritten. */
FM1_DSP_CODE void bass303_bank_render(bass303_bank_t *b, unsigned mono, int32_t *out, int32_t *scratch);
FM1_DSP_CODE void bass303_bank_set(bass303_bank_t *b, unsigned parameter, unsigned value);
FM1_DSP_CODE unsigned bass303_bank_get(const bass303_bank_t *b, unsigned parameter);
const fm1_param_t *bass303_param(int parameter);
/* Audio owner only. 1..4; released/old poly voices retire first. Mono stays mono. */
FM1_DSP_CODE unsigned bass303_bank_limit(bass303_bank_t *synth, unsigned voices);
FM1_DSP_CODE unsigned bass303_bank_active(const bass303_bank_t *synth);
#endif
