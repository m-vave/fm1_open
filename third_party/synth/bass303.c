/* SPDX-License-Identifier: MIT
 * Copyright (c) 2009 Robin Schmidt (Open303), 2026 M-VAVE (C adaptation).
 * MIT terms and source revision: THIRD_PARTY.md.
 * Open303 envelope mapping, TeeBee ladder and transistor-shaped pulse.
 * Fixed 44.1 kHz, bounded instance storage, no plugin/framework dependency.
 * FM1 additions: two drive modes. */
#ifndef FM1_HOST_TEST
#pragma code_seg(".volatile_ram_code")
#pragma const_seg(".volatile_ram.fm1")
#endif
#include "bass303_state.h"
#include <string.h>
static FM1_DSP_DATA const char *const waves[]={"Saw","Sqr"};
static FM1_DSP_DATA const char *const drives[]={"Off","Soft","RAT"};
static FM1_DSP_DATA const fm1_param_t parameters[]={
    {"Cutoff",127,64,0},{"Reso",127,64,0},{"EnvMod",127,64,0},{"Decay",127,64,0},
    {"Accent",127,64,0},{"Wave",1,0,waves},{"Tune",127,64,0},{"Volume",127,96,0},
    {"Drive",127,0,0},{"DrvTyp",2,1,drives},{"Slide",127,21,0},{"AccDec",127,7,0}};
const fm1_param_t *bass303_param(int p) {return (unsigned)p<BASS303_NPARAMS?parameters+p:0;}
int bass303_nparams(void) {return BASS303_NPARAMS;}
int bass303_get(const bass303_t *b,int p) {return (unsigned)p<BASS303_NPARAMS?b->pot[p]:0;}
static FM1_DSP_CODE void pitch_target(bass303_t *b) {
    float a=440.0f+((int)b->pot[BASS303_TUNE]-64)*.625f;
    b->frequency=dsp_min(18000.0f,a*dsp_exp2(((float)b->note-69+(float)(b->bend+b->tune)/65536.0f)/12));
}
FM1_DSP_CODE void bass303_set(bass303_t *b,int p,int v) {
    const fm1_param_t *desc=bass303_param(p);if(!desc)return;
    if(v<0)v=0;if(v>desc->max)v=desc->max;
    if(b->pot[p]==v)return;
    b->pot[p]=(uint8_t)v;
    float u=v*(1.0f/127.0f);
    switch(p) {
    case BASS303_CUTOFF:b->cutoff=314*dsp_exp2(2.9305886f*u);break;
    case BASS303_RESO:b->reso=(1-dsp_exp(-3*u))/(1-dsp_exp(-3));break;
    case BASS303_DECAY:b->decay=dsp_decay(.2f*dsp_exp2(3.321928095f*u));break;
    case BASS303_ACCDEC:b->accent_decay=dsp_decay(.030f+2.970f*u);break;
    case BASS303_ACCENT:b->accent=u;break;
    case BASS303_VOLUME:b->gain=dsp_exp2((u-1)*9.965784285f);break;
    case BASS303_SLIDE:b->slide=dsp_decay(.0004f+.0716f*u);break;
    case BASS303_TUNE:if(b->started)pitch_target(b);break;
    case BASS303_DRIVE:break;
    default:break;
    }
    if(p==BASS303_CUTOFF||p==BASS303_ENVMOD) {
        float c=dsp_log2(dsp_max(b->cutoff,1)/313.815278606f)/2.93169f;
        float e=b->pot[BASS303_ENVMOD]*(1.0f/127.0f);
        b->env_scale=(1-c)*(3.773996325f*e+.736965594f)+c*(4.194548788f*e+.864344901f);
        b->env_offset=.048292931f*c+.294391201f;
    }
    if(p==BASS303_DRIVE||p==BASS303_DRVTYPE) {
        float drive=b->pot[BASS303_DRIVE]/127.0f;
        if(b->pot[BASS303_DRVTYPE]==2) {
            b->drive_gain=6+drive*drive*(1500-800*drive);
            b->drive_high=1.6f-.8f*drive;
            b->drive_trim=.254f;
        } else {
            b->drive_gain=dsp_exp2(3.986313714f*drive);
            b->drive_trim=1-.3f*drive*drive;
        }
        b->drive_norm=1/b->drive_gain;
        dsp_svf_set(&b->drive_post,b->pot[BASS303_DRVTYPE]==2?3300-1400*drive:5200,.7071f);
        dsp_hp_set(&b->drive_dc,b->pot[BASS303_DRVTYPE]==2?3:8,44100);
    }
    b->control_phase=0;
}
FM1_DSP_CODE void bass303_init(bass303_t *b) {
    memset(b,0,sizeof *b);memset(b->pot,255,sizeof b->pot);b->idle=1;b->note=69;
    for(int p=0;p<BASS303_NPARAMS;p++)bass303_set(b,p,parameters[p].def);
    dsp_hp_set(&b->pre,44.486f,88200);dsp_hp_set(&b->feedback,150,88200);
    dsp_hp_set(&b->post,24.167f,44100);
    dsp_svf_set(&b->click,200,.70710678f);dsp_svf_set(&b->notch,7.5164f,.2f);
    dsp_lp_set(&b->down[0],9500,.5411961f);dsp_lp_set(&b->down[1],9500,1.306563f);
    dsp_svf_set(&b->drive_pre,720,.7071f);
}
FM1_DSP_CODE void bass303_bend(bass303_t *b,int32_t bend) {b->bend=bend;if(b->started)pitch_target(b);}
FM1_DSP_CODE void bass303_note_on(bass303_t *b,int note,int accent,int slide,int cents) {
    b->note=note;b->tune=cents*16384/25;b->accent_note=accent!=0;
    b->release=dsp_decay(b->pot[BASS303_DRVTYPE]==2&&b->pot[BASS303_DRIVE]?(accent?.060f:.006f):(accent?.050f:.001f));
    pitch_target(b);
    if(!slide||!b->started){b->pitch=b->frequency;b->env=1;b->amp=1;}
    b->gate=1;b->idle=0;b->started=1;b->quiet=0;b->control_phase=0;
}
FM1_DSP_CODE void bass303_note_off(bass303_t *b) {b->gate=0;}
FM1_DSP_CODE void bass303_all_off(bass303_t *b) {b->gate=0;}
FM1_DSP_CODE void bass303_advance_idle(bass303_t *b,int n) {
    if(!b->started||n<=0)return;
    b->pitch=b->frequency+(b->pitch-b->frequency)*dsp_exp2(dsp_log2(b->slide)*n);
    b->phase+=(float)n*b->pitch/44100;b->phase-=(float)(int)b->phase;
}
static FM1_DSP_CODE float blep(float p,float d) {
    if(p<d){float x=p/d;return 2*x-x*x-1;}
    if(p>1-d){float x=(p-1)/d;return x*x+2*x+1;}
    return 0;
}
static FM1_DSP_CODE void ladder_coeff(bass303_t *b,float hz) {
    float f=dsp_clamp(hz,200,20000)*(.7071067811865f/88200);
    b->b0=(.00045522346f+6.1922189f*f)/(1+12.358354f*f+4.4156345f*f*f);
    float k=f*(f*(f*(f*(f*(f+7198.6997f)-5837.7917f)-476.47308f)+614.95611f)+213.87126f)+16.998792f;
    b->comp=2*((k/17-1)*b->reso+1)*(1+b->reso);b->k=k*b->reso;
}
FM1_DSP_CODE void bass303_render(bass303_t *restrict b,int32_t *restrict out,int count) {
    if(count<=0)return;
    if(b->idle){memset(out,0,(size_t)count*sizeof *out);bass303_advance_idle(b,count);return;}
    float decay=b->accent_note?b->accent_decay:b->decay;
    float accent=b->accent_note?b->accent:0;
    for(int i=0;i<count;i++) {
        b->pitch=b->frequency+b->slide*(b->pitch-b->frequency);
        float env=b->env;b->env*=decay;
        b->rc1=env; b->rc2+=(accent>0?env-b->rc2:-b->rc2)*.001510574f;
        if(!(b->control_phase++&3)) {
            float cutoff=b->cutoff*dsp_exp2(b->env_scale*(b->rc1-b->env_offset)+accent*b->rc2);
            ladder_coeff(b,cutoff);
        }
        float amp=b->amp;b->amp*=b->gate?.999981565f:b->release;
        if(b->gate)amp+=(.45f+4*accent)*env;
        amp=dsp_svf(&b->click,amp,0);
        float sample=0,increment=b->pitch/88200;
        for(unsigned os=0;os<2;os++) {
            float p=b->phase;
            float saw=2*p-1-blep(p,increment);
            if(b->pot[BASS303_WAVE]) {
                float raw=dsp_tanh(70.0f*(2*p-1)+4.37f);
                sample=-(raw-.0625f-blep(p,increment))*.5f;
            } else sample=-saw;
            b->phase+=increment;if(b->phase>=1)b->phase-=1;
            sample=dsp_hp(&b->pre,sample)-dsp_hp(&b->feedback,b->k*b->y[3]);
            b->y[0]+=2*b->b0*(sample-b->y[0]+b->y[1]);
            b->y[1]+=b->b0*(b->y[0]-2*b->y[1]+b->y[2]);
            b->y[2]+=b->b0*(b->y[1]-2*b->y[2]+b->y[3]);
            b->y[3]+=b->b0*(b->y[2]-2*b->y[3]);
            sample=dsp_lp(&b->down[1],dsp_lp(&b->down[0],b->comp*b->y[3]));
        }
        float ap=-.9980063f*(sample-b->ap_y)+b->ap_x;b->ap_x=sample;b->ap_y=ap;
        sample=dsp_svf(&b->notch,dsp_hp(&b->post,ap),3)*amp*b->gain;
        if(b->pot[BASS303_DRIVE]&&b->pot[BASS303_DRVTYPE]) {
            float x=sample;
            if(b->pot[BASS303_DRVTYPE]==1) x=(dsp_tanh(x*b->drive_gain+.15f)-.148885034f)*b->drive_norm*b->drive_trim;
            else {float high=dsp_svf(&b->drive_pre,x,2);x=dsp_tanh((x+b->drive_high*high)*b->drive_gain)*b->drive_trim;}
            sample=dsp_hp(&b->drive_dc,dsp_svf(&b->drive_post,x,0));
        }
        sample=dsp_clamp(sample,-32,32);out[i]=(int32_t)(sample*16777216);
        if(!b->gate&&dsp_abs(sample)<.000015f&&b->amp<.000001f)b->quiet++;else b->quiet=0;
        if(b->quiet>22050){b->idle=1;memset(out+i+1,0,(size_t)(count-i-1)*sizeof *out);break;}
    }
}
