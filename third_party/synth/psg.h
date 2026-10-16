/* SPDX-License-Identifier: MIT */
#ifndef FM1_PSG_H
#define FM1_PSG_H
#include "engine.h"
#include "fm1_param.h"

enum { PSG_VOICES = 8, PSG_PARAMS = 4 };
typedef struct psg psg_t;
/* Caller-owned, 32-byte-aligned storage; query its size with psg_size().
 * Initialize before use. Live calls belong to one audio owner or a held gate. */
size_t psg_size(void);
void psg_init(psg_t *p);
FM1_DSP_CODE void psg_panic(psg_t *p);
FM1_DSP_CODE void psg_note(psg_t *p, unsigned note, unsigned velocity, int cents);
FM1_DSP_CODE void psg_midi(psg_t *p, const uint8_t message[3]);
/* Process one 128-frame block; release/AY control advances every 64 samples. */
FM1_DSP_CODE void psg_render(psg_t *p, int32_t *out, unsigned frames);
FM1_DSP_CODE void psg_set(psg_t *p, unsigned parameter, unsigned value);
FM1_DSP_CODE unsigned psg_get(const psg_t *p, unsigned parameter);
const fm1_param_t *psg_param(unsigned parameter);
/* Audio owner only. 1..PSG_VOICES; retires released/old voices first.
 * Returns the number retired. No allocation; future notes obey the limit. */
FM1_DSP_CODE unsigned psg_limit(psg_t *synth, unsigned voices);
FM1_DSP_CODE unsigned psg_active(const psg_t *synth);
/* Audio owner only. Occupied emulated chips (0..3), including released tails.
 * Useful for CPU admission: up to three voices share one chip's clock loop. */
FM1_DSP_CODE unsigned psg_active_chips(const psg_t *synth);
#endif
