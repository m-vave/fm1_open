/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2012, 2013 Google Inc.
 * Copyright 2016-2025 Pascal Gauthier.
 * Modified for FM1 by M-VAVE, 2026: C port, fixed-point and block processing.
 * Apache-2.0 terms and upstream revisions: THIRD_PARTY.md. */
#ifndef _DX7NOTE_H_
#define _DX7NOTE_H_

#include "dx7synth.h"
#include "dx7core.h"
#include "dx7env.h"
#include "dx7pitchenv.h"
#include "dx7ctrl.h"

typedef struct
{
    t_dx7env env[6];
    t_dx7op_params params[6];
    int basepitch[6];
    int freq_pitch[2]; /* Last ratio/fixed pitch inputs, excluding basepitch. */
    u8 ampmodsens[6];  /* Patch sensitivity indices, 0..3. */
    u8 fixed_ops;
    u8 freq_valid;
    t_dx7pitchenv pitchenv;
    int fb_buf[2];
    int fb_shift;
    int algorithm;
    int pitchmoddepth;
    int pitchmodsens;
    int ampmoddepth;
    int glide_offset;   // 滑音：加进 pitchmod 的对数域偏移(起始=旧音-新音)，每块朝0收敛
    int glide_step;     // 滑音：每块收敛步长(正值)；0=无滑音
}t_dx7note;

/* Per-synth cache. Each nonzero sensitivity has
 * its own exact input/result, reusable across voices and control blocks. */
typedef struct {
    u32 input[4], attenuation[4];
} t_dx7_mod_cache;

void dx7note_init(t_dx7note *this,u8 *patch,u8 midinote,u8 velocity,int cents);
void dx7note_compute_64(t_dx7note *this, int *buf, int *scratch, int lfo_val,
                        int lfo_delay, t_dx7ctrl *ctrl, t_dx7_mod_cache *mod);
void dx7note_keyup(t_dx7note *this);
void dx7note_oscsync(t_dx7note *this);
void dx7note_transfer_signal(t_dx7note *this,t_dx7note *src);
void dx7note_transfer_state(t_dx7note *this,t_dx7note *src);
void dx7note_setglide(t_dx7note *this,int start_offset,int step);
#endif
