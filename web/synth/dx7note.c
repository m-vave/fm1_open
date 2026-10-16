/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2012, 2013 Google Inc.
 * Copyright 2016-2025 Pascal Gauthier.
 * Modified for FM1 by M-VAVE, 2026: C port, fixed-point and block processing.
 * Apache-2.0 terms and upstream revisions: THIRD_PARTY.md. */
#include "fixed.h"
#include <string.h>
#include "dx7note.h"
#include "stdlib.h"

static FM1_DSP_DATA const int coarsemul[] = {
    -16777216, 0, 16777216, 26591258, 33554432, 38955489, 43368474, 47099600,
    50331648, 53182516, 55732705, 58039632, 60145690, 62083076, 63876816,
    65546747, 67108864, 68576247, 69959732, 71268397, 72509921, 73690858,
    74816848, 75892776, 76922906, 77910978, 78860292, 79773775, 80654032,
    81503396, 82323963, 83117622
};

FM1_DSP_CODE static inline int osc_freq(int notelogfreq, u8 mode, u8 coarse, u8 fine, u8 detune)
{
  int logfreq;
  if (mode == 0) {
    logfreq = notelogfreq;
    /* Operator detune uses e^x, matching the original FM1 pitch curve. */
    if (detune != 7) {
      int32_t exponent = -(int32_t)(((int64_t)logfreq * 425201762) >> 30);
      uint32_t decay = fm_exp2_fixed((int32_t)(((int64_t)exponent * 1549082005) >> 30), 30);
      int32_t ratio = (int32_t)(((int64_t)decay * 3205886) >> 30);
      logfreq += (int32_t)((int64_t)ratio * logfreq * (detune - 7) / FM_Q30_ONE);
    }
    logfreq += coarsemul[coarse & 31];
    if (fine) logfreq +=  k_log2_q24((1<<24) + 167772 * fine);
  }
  else {
    logfreq = (4458616 * ((coarse & 3) * 100 + fine)) >> 3;
    logfreq += detune > 7 ? 13457 * (detune - 7) : 0;
  }
  return logfreq;
}

static FM1_DSP_DATA const u8 velocity_data[64] = {
    0, 70, 86, 97, 106, 114, 121, 126, 132, 138, 142, 148, 152, 156, 160, 163,
    166, 170, 173, 174, 178, 181, 184, 186, 189, 190, 194, 196, 198, 200, 202,
    205, 206, 209, 211, 214, 216, 218, 220, 222, 224, 225, 227, 229, 230, 232,
    233, 235, 237, 238, 240, 241, 242, 243, 244, 246, 246, 248, 249, 250, 251,
    252, 253, 254
};

// Velocity scaling, in envelope microsteps.
FM1_DSP_CODE static inline int sacle_vel(int velocity, int sensitivity) {
    int vel_value = velocity_data[velocity >> 1] - 239;
    int scaled_vel = ((sensitivity * vel_value + 7) >> 3) * 16;
    return scaled_vel;
}

FM1_DSP_CODE static inline int scalerate(int midinote, int sensitivity) {
    int x = DSP_MIN(31, DSP_MAX(0, midinote / 3 - 7));
    int qratedelta = (sensitivity * x) >> 3;
    int rem = x & 7;
    if (sensitivity == 3 && rem == 3) qratedelta -= 1;
    else if (sensitivity == 7 && rem > 0 && rem < 4) qratedelta += 1;
    return qratedelta;
}

static FM1_DSP_DATA const u8 exp_scale_data[] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 11, 14, 16, 19, 23, 27, 33, 39, 47,
    56, 66, 80, 94, 110, 126, 142, 158, 174, 190, 206, 222, 238, 250
};

FM1_DSP_CODE static inline int scalecurve(int group, int depth, int curve) {
    int scale;
    if (curve == 0 || curve == 3) scale = (group * depth * 329) >> 12;
    else {
        // exponential
        int n_scale_data = sizeof(exp_scale_data);
        int raw_exp = exp_scale_data[DSP_MIN(group, n_scale_data - 1)];
        scale = (raw_exp * depth * 329) >> 15;
    }
    if (curve < 2) scale = -scale;
    return scale;
}

FM1_DSP_CODE static inline int scale_lvl(int midinote, int break_pt, int left_depth, int right_depth,int left_curve, int right_curve)
{
    int offset = midinote - break_pt - 17;
    if (offset >= 0) return scalecurve((offset+1) / 3, right_depth, right_curve);
    else return scalecurve(-(offset-1) / 3, left_depth, left_curve);
}

static FM1_DSP_DATA const u8 pitchmodsenstab[] = {
    0, 10, 20, 33, 55, 92, 153, 255
};

static FM1_DSP_DATA const u32 ampmodsenstab[] = {
    0, 4342338, 7171437, 16777216
};

FM1_DSP_CODE void dx7note_init(t_dx7note *this,u8 *patch,u8 midinote,u8 velocity,int cents)
{
  int notelogfreq = 50857777 + (((int)midinote<<24) / 12) + (cents * (1<<24) / 1200);
  this->fixed_ops = this->freq_valid = 0;
  this->freq_pitch[0] = this->freq_pitch[1] = 0;
  for (int op = 0; op < FM_OP_CNT; op++) {
        u8 *parm = patch + op*21;
        int outlvl = dx7_outlvl(parm[16]);
        outlvl += scale_lvl(midinote, parm[8], parm[9],parm[10], parm[11], parm[12]);
        outlvl = DSP_MIN(127, outlvl)<<5;
        outlvl += sacle_vel(velocity, parm[15]);
        outlvl = DSP_MAX(0, outlvl);
        dx7env_set(&this->env[op],parm ,parm + 4,outlvl,scalerate(midinote, parm[13]));
		this->basepitch[op] = osc_freq(notelogfreq,parm[17],parm[18],parm[19],parm[20]);
        this->ampmodsens[op] = parm[14] & 3;
        if (parm[17]) this->fixed_ops |= BIT(op);
        this->params[op].phase = 0;
        this->params[op].gaindst = 0;
  }
  dx7pitchenv_set(&this->pitchenv,patch + 126,patch + 130);
  this->algorithm = patch[134];
  this->fb_shift = patch[135]? 8 - patch[135]:16;
  this->pitchmoddepth = (patch[139] * 165) >> 6;
  this->pitchmodsens = pitchmodsenstab[patch[143] & 7];
  this->ampmoddepth = (patch[140] * 165) >> 6;
  this->glide_offset = 0;   // 新音默认无滑音；滑音由 mono keydown 处显式设置
  this->glide_step = 0;
}

FM1_DSP_CODE void dx7note_setglide(t_dx7note *this,int start_offset,int step)
{
    this->glide_offset = start_offset;
    this->glide_step = step;
}

FM1_DSP_CODE void dx7note_compute_64(t_dx7note *this, int *buf, int *scratch, int lfo_val,
                        int lfo_delay, t_dx7ctrl *ctrl, t_dx7_mod_cache *mod)
{
    // ==== PITCH ====
    u32 pmd = this->pitchmoddepth * lfo_delay;  // Q32
    int senslfo = this->pitchmodsens * (lfo_val - (1 << 23));  // Q32
    int pmd1 = abs((int)(((s64)pmd * (s64)senslfo) >> 39));
    /* A zero patch PMS disables programmed vibrato, but a live wheel still
     * needs a useful response. Keep the patch unchanged; use minimum PMS for
     * the performance controller alone. */
    int wheel_lfo = (this->pitchmodsens ? this->pitchmodsens : 10) * (lfo_val - (1 << 23));
    int pmd2 = abs((int)(((s64)ctrl->pitch_mod * (s64)wheel_lfo) >> 14));
    int pitchmod = DSP_MAX(pmd1,pmd2);
    pitchmod = dx7pitchenv_getsample(&this->pitchenv) + (pitchmod * (wheel_lfo < 0 ? -1 : 1));

    // ---- PITCH BEND ----
    int pb = (ctrl->pitchbend - 0x2000); //Q24  << 11
    if (pb != 0)
    {
        if (ctrl->pitchstep == 0)
        {
            if(pb >= 0)
                pb = (int)((int64_t)pb * 2048 * ctrl->pitchrangeup / 12);
            else
                pb = (int)((int64_t)pb * 2048 * ctrl->pitchrangedn / 12);
        }
    }
    int pitchbase = pb + ctrl->mastertune;
    pitchmod += pitchbase;

    // ---- GLIDE(滑音) ----
    // 只加进 pitchmod(ratio 算子)，不进 pitchbase，故 fixed 频率算子不受滑音影响。
    // glide_offset 每块朝 0 收敛，到 0 即滑音结束。
    if(this->glide_offset)
    {
        pitchmod += this->glide_offset;
        if(this->glide_offset > 0){
            this->glide_offset -= this->glide_step;
            if(this->glide_offset < 0) this->glide_offset = 0;
        }else{
            this->glide_offset += this->glide_step;
            if(this->glide_offset > 0) this->glide_offset = 0;
        }
    }

    // ==== AMP MOD ====
    lfo_val = (1<<24) - lfo_val;
    u32 amod_1 = (u32)(((s64)this->ampmoddepth * (s64)lfo_delay)>>8);
    amod_1 = (u32)(((s64)amod_1*(s64)lfo_val)>>24);
    u32 amod_2 = (u32)(((s64)ctrl->amp_mod*(s64)lfo_val)>>7);
    u32 amd_mod = DSP_MAX(amod_1,amod_2);

    // ==== EG AMP MOD ====
    u32 amod_3 = (ctrl->eg_mod + 1) << 17;
    amd_mod = DSP_MAX((1<<24) - amod_3,amd_mod);

    /* Invalidate disabled operators too: re-enabling one after a bend must
     * not reuse its old frequency. Base pitches only change in note_init. */
    if (pitchmod != this->freq_pitch[0])
        this->freq_valid &= this->fixed_ops;
    if (pitchbase != this->freq_pitch[1])
        this->freq_valid &= (u8)~this->fixed_ops;
    this->freq_pitch[0] = pitchmod;
    this->freq_pitch[1] = pitchbase;
    unsigned changed = ctrl->opswitch & ~this->freq_valid;

    /* Retain the last-sensitivity fast path within a voice, then reuse exact
     * attenuation inputs across voices. Patch edits and mixed-age voices are
     * safe because a hit requires the full modulation input, not a patch ID. */
    int cached_sensitivity = 0;
    u32 cached_attenuation = 0;
    for (int op = 0; op < FM_OP_CNT; op++) {
        this->params[op].gaincur = this->params[op].gaindst;
        if(ctrl->opswitch & BIT(op)){
            if (changed & BIT(op))
                this->params[op].freq = freqlut_qx(this->basepitch[op] +
                    ((this->fixed_ops & BIT(op)) ? pitchbase : pitchmod));
            int level = dx7env_getsample_64(&this->env[op]);
            if (this->ampmodsens[op]) {
                if (this->ampmodsens[op] != cached_sensitivity) {
                    unsigned sensitivity = this->ampmodsens[op];
                    /* Zero marks an empty entry. A zero result would simply
                     * be recomputed next time, never mistaken for a hit. */
                    if (!mod->attenuation[sensitivity] || mod->input[sensitivity] != amd_mod) {
                        u32 sensamp = (u32)(((u64)amd_mod*ampmodsenstab[sensitivity]) >> 24);
                        /* Validated patch depths and MIDI controls bound
                         * sensamp by 2^24; multiplying by 112 fits U32. */
                        int32_t exponent=204682035+(int32_t)(sensamp*112U/25U);
                        mod->attenuation[sensitivity]=fm_exp2_fixed((int32_t)(((int64_t)exponent*1549082005)>>30),0);
                        mod->input[sensitivity] = amd_mod;
                    }
                    cached_attenuation = mod->attenuation[sensitivity];
                    cached_sensitivity = sensitivity;
                }
                u32 ldiff = (u32)(((u64)level*((u64)cached_attenuation<<4)) >> 28);
                level -= ldiff;
            }
            this->params[op].gaindst = level;
        }
        else{
            dx7env_getsample_64(&this->env[op]);
            this->params[op].gaindst = 0;
        }
    }
  this->freq_valid |= ctrl->opswitch;
  dx7core_compute_64(buf, scratch, this->params, this->algorithm,
                     this->fb_buf, this->fb_shift);
}

FM1_DSP_CODE void dx7note_keyup(t_dx7note *this)
{
	for (int op = 0; op < 6; op++) dx7env_keyup(&this->env[op]);
	dx7pitchenv_keyup(&this->pitchenv);
}

FM1_DSP_CODE void dx7note_oscsync(t_dx7note *this)
{
    for(int op=0;op<6;op++){
        this->params[op].gaindst = 0;
        this->params[op].phase = 0;
    }
}

FM1_DSP_CODE void dx7note_transfer_signal(t_dx7note *this,t_dx7note *src)
{
    for (int op=0;op<6;op++) {
        this->params[op].gaindst = src->params[op].gaindst;
        this->params[op].phase = src->params[op].phase;
    }
}

FM1_DSP_CODE void dx7note_transfer_state(t_dx7note *this,t_dx7note *src)
{
    for (int op=0;op<6;op++) {
        dx7env_transfer(&this->env[op],&src->env[op]);
        this->params[op].gaindst = src->params[op].gaindst;
        this->params[op].phase = src->params[op].phase;
    }
}
