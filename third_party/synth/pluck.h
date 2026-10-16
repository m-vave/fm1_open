/* SPDX-License-Identifier: MIT */
#ifndef FM1_PLUCK_H
#define FM1_PLUCK_H
#include "engine.h"
#include "fm1_param.h"

enum { PLUCK_VOICES = 8, PLUCK_PARAMS = 4 };
typedef struct pluck pluck_t;
/* Caller-owned, 32-byte-aligned storage; query its size with pluck_size().
 * Initialize before use. Live calls belong to one audio owner or a held gate. */
size_t pluck_size(void);
void pluck_init(pluck_t *p);
FM1_DSP_CODE void pluck_panic(pluck_t *p);
FM1_DSP_CODE void pluck_note(pluck_t *p, unsigned note, unsigned velocity, int cents);
FM1_DSP_CODE void pluck_midi(pluck_t *p, const uint8_t message[3]);
FM1_DSP_CODE void pluck_render(pluck_t *p, int32_t *out, unsigned frames);
FM1_DSP_CODE void pluck_set(pluck_t *p, unsigned parameter, unsigned value);
FM1_DSP_CODE unsigned pluck_get(const pluck_t *p, unsigned parameter);
const fm1_param_t *pluck_param(unsigned parameter);
/* Audio owner only. 1..PLUCK_VOICES; retires released/old voices first.
 * Returns the number retired. No allocation; future notes obey the limit. */
FM1_DSP_CODE unsigned pluck_limit(pluck_t *synth, unsigned voices);
FM1_DSP_CODE unsigned pluck_active(const pluck_t *synth);
#endif
