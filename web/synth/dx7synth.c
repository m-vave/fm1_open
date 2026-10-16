/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#include "engine.h"
#include "fixed.h"
#include <string.h>
#include "dx7synth.h"
#include "dx7note.h"
#include "dx7lfo.h"
#include "dx7ctrl.h"
#include "adsr.h"
#include <limits.h>

typedef struct
{
    u8 key;
    u8 midi;
    u8 channel;
    u8 keydown;
    u8 sustained;
    u8 live;
    AdsrEnvelope env;
    t_dx7note dx7note;
}t_note;

struct synth_dx7
{
    void *allocation_base;
    int fs;
    u8 maxnote, limit;
    t_note *note;
    u8 curnote;
    u8 sustain;
    u8 mono;
    u8 glide;         // 滑音量/时间 0-100(0=关)，全局；仅 mono 生效；恒定时间曲线
    u8 glide_finger;  // 0=持续(总从上一个音滑) 1=指法(仅legato滑)
    int last_midi;    // mono 上一次按下的音(滑音起点)；-1=无
    int last_cents;   // 该音的调音偏移(音分)，滑音起点的一部分
    int *out;
    int *sum;
    int *scratch;
    t_dx7lfo lfo;
    t_dx7ctrl ctrl;
    t_dx7_mod_cache mod_cache;
    int32_t dc_r;
    int32_t dc_hist[3];

    t_dx7_adsr_config adsr;
    int fade_samples;
    int mute_fade_step;
    int32_t fade_gain, fade_whole;
    unsigned fade_fraction, fade_error;
};
typedef struct synth_dx7 t_dx7synth;

FM1_DSP_CODE unsigned synth_dx7_limit(synth_dx7_t *s, unsigned limit)
{
    if (!s || !limit || limit > s->maxnote) return 0;
    s->limit = (uint8_t)limit;
    unsigned active = 0, retired = 0;
    for (unsigned i = 0; i < s->maxnote; ++i) active += s->note[i].live != 0;
    while (active > limit) {
        unsigned slot = s->maxnote, best = 0;
        for (unsigned n = 0, i = s->curnote; n < s->maxnote; ++n, i = (i+1U) % s->maxnote) {
            t_note *v = &s->note[i];
            if (!v->live) continue;
            unsigned rank = !v->keydown && !v->sustained ? 2U : !v->keydown ? 1U : 0U;
            if (slot == s->maxnote || rank > best) { slot = i; best = rank; }
        }
        s->note[slot].live = s->note[slot].keydown = s->note[slot].sustained = 0;
        s->curnote = slot + 1U == s->maxnote ? 0 : (uint8_t)(slot + 1U);
        --active; ++retired;
    }
    return retired;
}

FM1_DSP_ENTRY void fm1_dx7_panic(synth_dx7_t *this)
{
    if (!this) return;
    for (unsigned i = 0; i < this->maxnote; ++i) {
        t_note *v = &this->note[i];
        v->live = v->keydown = v->sustained = 0;
        v->dx7note.fb_buf[0] = v->dx7note.fb_buf[1] = 0;
        for (unsigned op = 0; op < 6; ++op) v->dx7note.params[op].gaincur = 0;
    }
    this->sustain = this->curnote = 0;
    this->last_midi = -1;
    this->mute_fade_step = 0;
    this->fade_gain = 0;
    this->fade_error = 0;
    this->dc_hist[0] = this->dc_hist[1] = this->dc_hist[2] = 0;
    this->ctrl.pitchbend = 8192;
    this->ctrl.modwheel_cc = this->ctrl.aftertouch_cc = 0;
    this->ctrl.foot_cc = this->ctrl.breath_cc = 127;
    this->ctrl.pitch_mod = this->ctrl.amp_mod = this->ctrl.eg_mod = 0;
}

/* Validate at patch/event boundaries, never in the per-sample loop. */
static FM1_DSP_CODE int patch_valid(const uint8_t *p)
{
    static const uint8_t op_max[21] FM1_DSP_DATA = {
        99,99,99,99,99,99,99,99,99,99,99,3,3,7,3,7,99,1,31,99,14
    };
    static const uint8_t global_max[19] FM1_DSP_DATA = {
        99,99,99,99,99,99,99,99,31,7,1,99,99,99,99,1,5,7,48
    };
    if (!p) return 0;
    for (unsigned op = 0; op < 6; ++op)
        for (unsigned i = 0; i < 21; ++i)
            if (p[op * 21 + i] > op_max[i]) return 0;
    for (unsigned i = 0; i < 19; ++i)
        if (p[126 + i] > global_max[i]) return 0;
    for (unsigned i = 145; i < 155; ++i)
        if (p[i] > 127U) return 0;
    return p[155] <= 63U;
}

FM1_DSP_CODE int synth_dx7_lfo_reset(synth_dx7_t *handle, const uint8_t *patch)
{
    t_dx7synth *this = handle;
    if (this == NULL || !patch_valid(patch)) return -1;
    dx7lfo_reset(&this->lfo, patch + 137, this->fs,
                 SYNTH_DX7_BLOCK_SAMPLES);
    return 0;
}

FM1_DSP_CODE void synth_dx7_destroy(synth_dx7_t *handle)
{
    t_dx7synth *this = handle;
    void *allocation;

    if (this == NULL) return;
    allocation = this->allocation_base;
    /* Notes and all block workspaces share the synth object's allocation. */
    freq_lut_release();
    free(allocation);
}

/* Initialize zeroed storage. Creation and in-place reset share the same
 * defaults; the sample-rate lookup table is acquired only by creation. */
static void init_instance(t_dx7synth *this, void *allocation,
                          uint8_t maxnote, uint32_t fs)
{
    size_t note_bytes = (size_t)maxnote * sizeof(*this->note);
    this->allocation_base = allocation;
    this->maxnote = this->limit = maxnote;
    this->fs = fs;
    this->note = (t_note *)(this + 1);
    this->out = (int *)((uint8_t *)this->note + note_bytes);
    this->sum = this->out + SYNTH_DX7_BLOCK_SAMPLES;
    this->scratch = this->sum + SYNTH_DX7_BLOCK_SAMPLES;
    this->sustain = 0;
    this->ctrl.pitchbend = 0x2000;
    this->ctrl.opswitch = 0x3f;
    this->ctrl.pitchrange = 12;
    this->ctrl.pitchrangeup = 12;
    this->ctrl.pitchrangedn = 12;
    this->ctrl.modwheel_cc = 0;
    /* CC2/CC4 default to no attenuation until a controller supplies a value. */
    this->ctrl.foot_cc = 127;
    this->ctrl.breath_cc = 127;
    this->ctrl.aftertouch_cc = 0;
    this->ctrl.wheel.pitchswitch = 1;
    this->ctrl.breath.ampswitch = 1;
    this->ctrl.foot.ampswitch = 1;
    this->dc_r = FM_Q30_ONE - (int32_t)((INT64_C(126) * FM_Q30_ONE) / fs);
    memset(this->dc_hist, 0, sizeof this->dc_hist);

    this->ctrl.eg_mod = 127;
    this->ctrl.aftertouch.pitchswitch = 1;


    adsr_config_set(&this->adsr, 0, 0, 0, 0, 0, fs);
    this->mute_fade_step = 0;
    this->fade_samples = DSP_MAX((int)fs / 100, 1);
    this->fade_whole = FM_Q30_ONE / this->fade_samples;
    this->fade_fraction = FM_Q30_ONE % this->fade_samples;
    this->fade_gain = 0;
    this->fade_error = 0;
    this->last_midi = -1;   // 滑音起点：开机无上一个音
}

synth_dx7_t *synth_dx7_create(uint8_t maxnote, uint32_t fs,
                              uint16_t samples)
{
    t_dx7synth *this;
    void *allocation;
    size_t note_bytes;
    size_t workspace_bytes;
    size_t allocation_bytes;

    if (maxnote == 0U || fs == 0U || fs > INT_MAX ||
        (samples != SYNTH_DX7_BLOCK_SAMPLES && samples != FM1_BLOCK_FRAMES))
        return NULL;
    note_bytes = (size_t)maxnote * sizeof(*this->note);
    workspace_bytes = (size_t)SYNTH_DX7_BLOCK_SAMPLES *
                      4U * sizeof(*this->out);
    if (note_bytes > SIZE_MAX - sizeof(*this) ||
        workspace_bytes > SIZE_MAX - sizeof(*this) - note_bytes)
        return NULL;
    allocation_bytes = sizeof(*this) + note_bytes + workspace_bytes;
    if (allocation_bytes > SIZE_MAX - 31U)
        return NULL;
    allocation_bytes = (allocation_bytes + 31U) & ~(size_t)31U;
    if (freq_lut_acquire((int)fs) != 0)
        return NULL;
    dx7pitchenv_init(SYNTH_DX7_BLOCK_SAMPLES, fs);
    /* All five objects have identical lifetime. One zeroed block removes four
     * allocator headers and cannot leave a partially initialized synth. */
    allocation = calloc(1, allocation_bytes + 31U);
    if (allocation == NULL) {
        freq_lut_release();
        return NULL;
    }
    this = (t_dx7synth *)(((uintptr_t)allocation + 31U) &
                          ~(uintptr_t)31U);
    init_instance(this, allocation, maxnote, fs);
    return this;
}

void synth_dx7_reset(synth_dx7_t *this)
{
    if (!this) return;
    void *allocation = this->allocation_base;
    uint32_t fs = (uint32_t)this->fs;
    uint8_t maxnote = this->maxnote;
    size_t bytes = sizeof(*this) + (size_t)maxnote * sizeof(*this->note) +
                   (size_t)SYNTH_DX7_BLOCK_SAMPLES * 4U * sizeof(*this->out);
    memset(this, 0, bytes);
    init_instance(this, allocation, maxnote, fs);
}

// 滑音：从旧音 src_midi 滑到新音 dst_pitch(对数域)。仅 mono、glide>0 时调用。
// 恒定时间曲线：总时长 = glide*10 ms，与音程无关(音程越大滑得越快)。
FM1_DSP_CODE static void glide_apply(t_dx7synth *this,int note,int src_midi,int dst_pitch,int dst_cents)
{
    if(!this->glide || src_midi < 0 || (src_midi == dst_pitch && this->last_cents == dst_cents)) return;
    /* Avoid undefined behavior from left-shifting a negative interval. */
    int delta = (int)(((long long)src_midi - dst_pitch) *
                      (1LL << 24) / 12LL) +
                (int)((long long)(this->last_cents - dst_cents) * (1LL << 24) / 1200);
    int mag = delta < 0 ? -delta : delta;
    int nblk = this->glide * 10 * this->fs /
               (1000 * SYNTH_DX7_BLOCK_SAMPLES);
    if(nblk < 1) nblk = 1;
    int step = mag / nblk;
    if(step < 1) step = 1;
    dx7note_setglide(&this->note[note].dx7note,delta,step);
}

/* Prefer silence, then release tails, then pedal-held notes. Rotate the
 * steal cursor even when every key is held, so repeated steals stay fair. */
FM1_DSP_CODE static inline int allocatenote(t_dx7synth *this) {
    unsigned active = 0;
    for (unsigned i = 0; i < this->maxnote; ++i) active += this->note[i].live != 0;
    int chosen = this->curnote, best = -1;
    for (int i = 0, note = this->curnote; i < this->maxnote; ++i) {
        t_note *v = &this->note[note];
        if (!this->mono && active >= this->limit && !v->live) {
            if (++note >= this->maxnote) note = 0;
            continue;
        }
        int rank = !v->live && !v->keydown ? 3 :
                   !v->keydown && !v->sustained ? 2 : !v->keydown ? 1 : 0;
        if (rank > best) { chosen = note; best = rank; }
        if (++note >= this->maxnote) note = 0;
    }
    this->curnote = chosen + 1;
    if (this->curnote >= this->maxnote) this->curnote = 0;
    return chosen;
}

FM1_DSP_CODE static void dx7synth_keyup(t_dx7synth *this,u8 *patch,u8 midi,u8 channel)
{
    (void)patch;
    (void)channel;
    int note;
    for (note= 0; note<this->maxnote; note++) {
        if (this->note[note].key == midi && this->note[note].keydown){
            this->note[note].keydown = false;
            break;
        }
    }
    if (note>= this->maxnote) {
        return;
    }
    if (this->mono) {
        int highnote = -1;
        int target = 0;
        for (int i=0; i<this->maxnote;i++){
            if (this->note[i].keydown){
                target = i;
                highnote = this->note[i].midi;
            }
        }
        if (highnote != -1 && this->note[note].live) {
            this->note[note].live = false;
            this->note[target].live = true;
            dx7note_transfer_state(&this->note[target].dx7note,&this->note[note].dx7note);
        }
    }
    if(this->sustain) this->note[note].sustained = true;
    else  {
        dx7note_keyup(&this->note[note].dx7note);
        adsr_note_off(&this->note[note].env);
    }
}

FM1_DSP_CODE static void dx7synth_keydown(t_dx7synth *this,u8 *patch,u8 midi,u8 channel,u8 vel,int cents)
{
    int pitch = midi + patch[144] - 24;
    if(pitch >= 0 && pitch <= 127){
        /* A poly/mono change fades the old voice set before retiring it.
         * If a new Note On arrives during that short transition, finish the
         * retirement immediately so the fade completion cannot later kill
         * the newly allocated voice. */
        if (this->mute_fade_step != 0) {
            this->mute_fade_step = 0;
            for (int i = 0; i < this->maxnote; ++i) {
                this->note[i].live = false;
                this->note[i].keydown = false;
                this->note[i].sustained = false;
            }
        }
        int note = allocatenote(this);
        dx7lfo_keydown(&this->lfo);
        this->note[note].key = midi;
        this->note[note].midi = pitch;
        this->note[note].channel = channel;
        this->note[note].keydown = true;
        this->note[note].sustained = this->sustain;

        dx7note_init(&this->note[note].dx7note,patch,pitch,(vel*100)>>7,cents);
        if(patch[136]) dx7note_oscsync(&this->note[note].dx7note);

        adsr_init(&this->note[note].env, &this->adsr);

        if (this->mono) {
            u8 prev_held = 0;   // 上一个音是否仍按着(legato)：指法滑音判据
            for(int i=0; i<this->maxnote;i++) {
                if (this->note[i].live) {
                    prev_held = this->note[i].keydown;
                    this->note[i].live = false;
                    if (!this->note[i].keydown)
                        dx7note_transfer_signal(&this->note[note].dx7note,&this->note[i].dx7note);
                    else
                        dx7note_transfer_state(&this->note[note].dx7note,&this->note[i].dx7note);
                    break;
                }
            }
            // 持续模式(glide_finger=0)：总从上一个音滑；指法模式(=1)：仅 legato(上一个音仍按着)才滑
            if(!this->glide_finger || prev_held)
                glide_apply(this,note,this->last_midi,pitch,cents);
            this->last_midi = pitch;
            this->last_cents = cents;
        }
        this->note[note].live = true;
    }
}

FM1_DSP_CODE static void apply_mod(int cc, t_dx7ctrl *ctrl, ctrl_mod *mod)
{
    int total = (cc * 100 + 64) >> 7;
    if(mod->ampswitch){
        ctrl->amp_mod = DSP_MAX(ctrl->amp_mod,total);
    }
    if(mod->pitchswitch){
        ctrl->pitch_mod = DSP_MAX(ctrl->pitch_mod,total);

    }
    if(mod->egswitch){
        ctrl->eg_mod = DSP_MAX(ctrl->eg_mod,total);
    }
}

FM1_DSP_CODE static void refresh_modulation(t_dx7synth *this)
{
    this->ctrl.amp_mod = 0;
    this->ctrl.pitch_mod = 0;
    this->ctrl.eg_mod = 0;
    apply_mod(this->ctrl.modwheel_cc,&this->ctrl,&this->ctrl.wheel);
    /* The amplitude path is attenuation-based: invert CC2/CC4 so larger
     * controller values reduce attenuation. Wheel/aftertouch keep their mapping. */
    apply_mod(127 - this->ctrl.breath_cc,&this->ctrl,&this->ctrl.breath);
    apply_mod(127 - this->ctrl.foot_cc,&this->ctrl,&this->ctrl.foot);
    apply_mod(this->ctrl.aftertouch_cc,&this->ctrl,&this->ctrl.aftertouch);
    if (!((this->ctrl.wheel.egswitch || this->ctrl.foot.egswitch)
    || (this->ctrl.breath.egswitch || this->ctrl.aftertouch.egswitch)))
    {
        this->ctrl.eg_mod = 127;
    }

}

FM1_DSP_ENTRY int synth_dx7_midi(synth_dx7_t *handle, uint8_t *patch,
                   const uint8_t *buf, size_t buf_size)
{
    return synth_dx7_midi_tuned(handle, patch, buf, buf_size, 0);
}
FM1_DSP_ENTRY int synth_dx7_midi_tuned(synth_dx7_t *handle, uint8_t *patch,
                   const uint8_t *buf, size_t buf_size, int cents)
{
    t_dx7synth *this = handle;
    if (this == NULL || patch == NULL || buf == NULL || buf_size == 0U)
        return -1;
    if (buf[0] < 0x80U || buf[0] >= 0xf0U) return -1;
    size_t expected = (buf[0] & 0xe0U) == 0xc0U ? 2U : 3U;
    if (buf_size != expected || buf[1] > 127U ||
        (expected == 3U && buf[2] > 127U)) return -1;
    u8 cmd     =  buf[0]&0xf0;
    u8 channel =  buf[0]&0x0f;
    if (cmd == 0x80) {
        if (buf_size >= 3) dx7synth_keyup(this,patch,buf[1],channel);
    }
    else if (cmd == 0x90) {
        if (buf_size >= 3) {
            if (buf[2]) {
                if (!patch_valid(patch)) return -1;
                dx7synth_keydown(this,patch,buf[1],channel,buf[2],cents);
            }
            else dx7synth_keyup(this,patch,buf[1],channel);
        }
    }
    else if (cmd == 0xb0) {
        if (buf_size >= 3) {
            int controller = buf[1];
            int value = buf[2];
            if (controller == 121) {
                this->ctrl.pitchbend = 8192;
                this->ctrl.modwheel_cc = this->ctrl.aftertouch_cc = 0;
                this->ctrl.breath_cc = this->ctrl.foot_cc = 127;
                refresh_modulation(this);
                controller = 64; value = 0;
            }
            if (controller == 1) {
                this->ctrl.modwheel_cc =  value;
                refresh_modulation(this);
            }
            else if (controller == 2) {
                this->ctrl.breath_cc = value;
                refresh_modulation(this);
            }
            else if (controller == 4) {
                this->ctrl.foot_cc = value;
                refresh_modulation(this);
            }
            else if (controller == 64) {
                this->sustain = (value >= 64);
                if (!this->sustain) {
                    for (int note = 0; note < this->maxnote; note++) {
                        if (this->note[note].sustained && !this->note[note].keydown) {
                            dx7note_keyup(&this->note[note].dx7note);
                            adsr_note_off(&this->note[note].env);
                            this->note[note].sustained = false;
                        }
                    }
                }
            }
            else if (controller == 123) {
                synth_dx7_all_notes_off(this);
            }
        }
    }
    else if (cmd == 0xd0 && buf_size >= 2U)// after touch
    {
        this->ctrl.aftertouch_cc = buf[1];
        refresh_modulation(this);
    }
    else if (cmd == 0xe0) {
        if(buf_size >= 3) this->ctrl.pitchbend = buf[1] | (buf[2] << 7);
    }
    return 0;
}


/* Retire only released voices that cannot become audible again. The operator
 * renderer drops levels below 100 << 14; all six envelopes must have reached
 * their final stage below that limit. A nonzero DX7 L4 may sustain after key-up
 * and must remain live. Held/pedal notes retain their state for later edits. */
static FM1_DSP_CODE int note_finished(const t_note *note)
{
    if (note->keydown || note->sustained) return 0;
    for (unsigned op = 0; op < FM_OP_CNT; ++op) {
        const t_dx7env *env = &note->dx7note.env[op];
        if (env->down || env->ix < 4 || env->lvl >= (100 << 14))
            return 0;
    }
    return 1;
}

static FM1_DSP_CODE int render_64(t_dx7synth *this, int32_t *buf);
FM1_DSP_CODE unsigned synth_dx7_active(const synth_dx7_t *s)
{
    unsigned count = 0;
    for (unsigned i = 0; i < s->maxnote; ++i) count += s->note[i].live != 0;
    return count;
}

/* Retain the original FM envelope/LFO quantum inside a 128-frame callback.
 * Each half consumes the same scratch; voice state continues across halves. */
FM1_DSP_ENTRY int synth_dx7_render(synth_dx7_t *handle, int32_t *buf, size_t samples)
{
    if (!handle || !buf || (samples != SYNTH_DX7_BLOCK_SAMPLES && samples != FM1_BLOCK_FRAMES))
        return -1;
    for (size_t offset = 0; offset < samples; offset += SYNTH_DX7_BLOCK_SAMPLES)
        render_64(handle, buf + offset);
    return 0;
}

static FM1_DSP_CODE int render_64(t_dx7synth *this, int32_t *buf)
{
    int lfovalue = dx7lfo_getsample(&this->lfo);  //q24
    int lfodelay = dx7lfo_getdelay(&this->lfo);
    const u8 adsr_enabled = this->adsr.enabled;
    u8 sum_has_contents = 0U;

    for (int note = 0; note < this->maxnote; note++) {
        if (this->note[note].live) {
            if (note_finished(&this->note[note])) {
                this->note[note].live = 0U;
                continue;
            }
            dx7note_compute_64(&this->note[note].dx7note, this->out,
                               this->scratch, lfovalue, lfodelay,
                               &this->ctrl, &this->mod_cache);
            if (adsr_enabled != 0U) {
                AdsrEnvelope *env=&this->note[note].env;
                for(unsigned i=0;i<SYNTH_DX7_BLOCK_SAMPLES;++i) {
                    int32_t level=adsr_update(env,1);
                    int32_t sample=fm_mul30(this->out[i]>>4,level);
                    this->sum[i]=sum_has_contents?fm_add_sat(this->sum[i],sample):sample;
                }
            } else {
                /* The operator output is reduced by four bits into Q24.
                 * Saturate additions so overload never wraps the bus. */
                if (sum_has_contents != 0U) {
                    for (size_t i = 0U;
                         i < (size_t)SYNTH_DX7_BLOCK_SAMPLES; ++i) {
                        this->sum[i] = fm_add_sat(this->sum[i], this->out[i] >> 4);
                    }
                } else {
                    for (size_t i = 0U;
                         i < (size_t)SYNTH_DX7_BLOCK_SAMPLES; ++i) {
                        this->sum[i] = this->out[i] >> 4;
                    }
                }
            }
            sum_has_contents = 1U;
        }
    }

    if (sum_has_contents == 0U) {
        memset(this->sum, 0,
               SYNTH_DX7_BLOCK_SAMPLES * sizeof(*this->sum));
    }

    int32_t previous=this->dc_hist[0],filtered=this->dc_hist[1];
    uint32_t remainder=(uint32_t)this->dc_hist[2];
    unsigned fading=this->mute_fade_step!=0;
    for(unsigned i=0;i<SYNTH_DX7_BLOCK_SAMPLES;++i) {
        int32_t input=buf[i];
        if(!fading || this->mute_fade_step) {
            input=fm_add_sat(input,this->sum[i]);
            if (fading) {
                input = fm_mul30(input, this->fade_gain);
                --this->mute_fade_step;
                /* Exact floor(step * 2^30 / fade_samples), advanced by a
                 * quotient/remainder recurrence. No audio-rate division. */
                this->fade_gain -= this->fade_whole;
                if (this->fade_error < this->fade_fraction) {
                    --this->fade_gain;
                    this->fade_error += (unsigned)this->fade_samples;
                }
                this->fade_error -= this->fade_fraction;
            }
        }
        int64_t total=(int64_t)input*FM_Q30_ONE-(int64_t)previous*FM_Q30_ONE+
                      (int64_t)filtered*this->dc_r+remainder;
        filtered=fm_sat32(total>>30);remainder=(uint32_t)total&0x3fffffffU;
        buf[i]=filtered;previous=input;
    }
    if(fading && !this->mute_fade_step)
        for(unsigned i=0;i<this->maxnote;++i)this->note[i].live=this->note[i].keydown=0;
    this->dc_hist[0]=previous;this->dc_hist[1]=filtered;this->dc_hist[2]=(int32_t)remainder;
    return 0;
}/*********************  效果参数设置  *****************************/
//mono:0、1
FM1_DSP_CODE void synth_dx7_set_poly_mode(synth_dx7_t *handle, uint8_t val)
{
    t_dx7synth *this = handle;
    if (this == NULL) return;
    this->mono = val;
    this->curnote = 0;
    this->last_midi = -1;   // 切换 poly/mono：清滑音起点，避免从陈旧音滑入
    this->mute_fade_step = this->fade_samples;
    this->fade_gain = FM_Q30_ONE;
    this->fade_error = 0;
}

FM1_DSP_CODE uint8_t synth_dx7_is_mono(const synth_dx7_t *handle)
{
    const t_dx7synth *this = handle;
    return this != NULL ? this->mono : 0U;
}

FM1_DSP_CODE void synth_dx7_all_notes_off(synth_dx7_t *handle)
{
    t_dx7synth *this = handle;
    int note;

    if (this == NULL)
        return;
    this->sustain = 0U;
    this->last_midi = -1;
    /* Start the next chord from the first voice slot after releasing all keys. */
    this->curnote = 0;
    for (note = 0; note < this->maxnote; ++note) {
        if (this->note[note].live) {
            dx7note_keyup(&this->note[note].dx7note);
            adsr_note_off(&this->note[note].env);
        }
        this->note[note].keydown = 0U;
        this->note[note].sustained = 0U;
    }
}

FM1_DSP_CODE void synth_dx7_set_pitch_bend_range(synth_dx7_t *handle,
                                    uint8_t semitones_up,
                                    uint8_t semitones_down)
{
    t_dx7synth *this = handle;
    if (this == NULL) return;
    this->ctrl.pitchrangeup = semitones_up;
    this->ctrl.pitchrangedn = semitones_down;
}

FM1_DSP_CODE void synth_dx7_set_glide(synth_dx7_t *handle, uint8_t amt,
                         uint8_t finger)
{
    t_dx7synth *this = handle;
    if (this == NULL) return;
    this->glide = amt;
    this->glide_finger = finger ? 1 : 0;
}

// 算子开关：bit0-5 对应 OP1-6，1=开。静音的算子在 dx7note_compute 里
// 包络照常推进，输出增益清零。
FM1_DSP_CODE void synth_dx7_set_operator_mask(synth_dx7_t *handle, uint8_t mask)
{
    t_dx7synth *this = handle;
    if (this == NULL) return;
    this->ctrl.opswitch = mask & 0x3f;
}

FM1_DSP_CODE void synth_dx7_set_adsr(synth_dx7_t *handle, uint8_t enabled,
                        uint8_t attack, uint8_t decay,
                        uint8_t sustain, uint8_t release)
{
    t_dx7synth *this = handle;
    if (this == NULL) return;
    adsr_config_set(&this->adsr, enabled, attack, decay, sustain, release, (uint32_t)this->fs);
}
