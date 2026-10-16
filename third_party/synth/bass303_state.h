/* SPDX-License-Identifier: MIT
 * Open303 C adaptation. Copyright (c) 2009 Robin Schmidt; 2026 M-VAVE.
 * See THIRD_PARTY.md for the MIT terms and pinned upstream source. */
#pragma once
#include "bass303.h"
#include "dsp_math.h"
typedef struct bass303 {
    uint8_t pot[BASS303_NPARAMS];
    int gate,idle,started,quiet,accent_note,note;
    unsigned control_phase;
    int32_t bend,tune; /* tune: Q16 semitones, fixed at note-on */
    float frequency,pitch,phase,cutoff,reso,env_scale,env_offset,gain;
    float decay,accent_decay,slide,release,env,amp,rc1,rc2,accent;
    float y[4],b0,k,comp;
    dsp_hp_t pre,feedback,post,drive_dc;
    dsp_svf_t click,notch,drive_pre,drive_post;
    dsp_lp_t down[2];
    float ap_x,ap_y,drive_gain,drive_norm,drive_high,drive_trim;
} bass303_t;
void bass303_init(bass303_t *b);
void bass303_note_on(bass303_t *b,int note,int accent,int slide,int cents);
void bass303_note_off(bass303_t *b);
void bass303_bend(bass303_t *b,int32_t bend);
void bass303_all_off(bass303_t *b);
void bass303_advance_idle(bass303_t *b,int n);
void bass303_render(bass303_t *b,int32_t *out,int n);
int bass303_nparams(void);
void bass303_set(bass303_t *b,int parameter,int value);
int bass303_get(const bass303_t *b,int parameter);
