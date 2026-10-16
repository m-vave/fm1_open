/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* Per-track tuning tables (tasks 2.2, 3.1-3.4, 3.6 of engine-tuning-hook):
 * API contract, drum immunity, per-engine pitch offsets, bend stacking, 303
 * slide and DX7 glide targets, and note-on capture. Exits non-zero on failure.
 * The measurement code duplicates the minimal parts of render.c. */
#include "internal.h"
#include "fm_patch.h"
#include "synth_dx7.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

void sim_boot(unsigned clear);
const float *sim_render(void);

enum { RATE = 44100 };
static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)
#define NEAR(what, got, want, tol) do { double g_ = (got), w_ = (want); int ok_ = fabs(g_ - w_) <= (tol); \
    printf("%-44s %+8.3f (want %+8.3f +-%.1f) %s\n", what, g_, w_, (double)(tol), ok_ ? "ok" : "FAIL"); \
    if (!ok_) ++failures; } while (0)

static const char *const name[] = {"DX7", "303", "808", "PSG", "PLUCK", "SAMPLE"};

/* ---- measurement (autocorrelation, see render.c) ----------------------- */
static double acf(const float *x, unsigned n, double lag)
{
    unsigned l = (unsigned)lag;
    double a = 0, e0 = 0, e1 = 0;
    for (unsigned i = 0; i + l < n; ++i) {
        a += (double)x[i] * x[i + l];
        e0 += (double)x[i] * x[i];
        e1 += (double)x[i + l] * x[i + l];
    }
    return e0 > 0 && e1 > 0 ? a / sqrt(e0 * e1) : 0;
}
static double peak_near(const float *x, unsigned n, unsigned lag0, unsigned radius)
{
    unsigned best = lag0;
    double bv = -1e30;
    for (unsigned l = lag0 - radius; l <= lag0 + radius; ++l) {
        double v = acf(x, n, l);
        if (v > bv) { bv = v; best = l; }
    }
    double a = acf(x, n, best - 1), b = acf(x, n, best), c = acf(x, n, best + 1), d = a - 2 * b + c;
    return best + (d != 0 ? 0.5 * (a - c) / d : 0);
}
static double estimate_hz(const float *x, unsigned n, double expect_hz)
{
    unsigned p0 = (unsigned)(RATE / expect_hz + 0.5);
    double period = peak_near(x, n, p0, p0 / 4 + 1);
    unsigned m = 1;
    while (2 * m * period < n / 2.0) {
        period = peak_near(x, n, (unsigned)(2 * m * period + 0.5), 2) / (2 * m);
        m *= 2;
    }
    return RATE / period;
}
static double note_hz(double note) { return 440.0 * pow(2.0, (note - 69.0) / 12.0); }

enum { SETTLE = 120, WINDOW = 256, MAXBLOCKS = 1024 };
static float pcm[MAXBLOCKS * FM1_BLOCK_FRAMES];
static unsigned filled;
static void run(unsigned blocks) /* append left channel */
{
    for (unsigned b = 0; b < blocks && filled < MAXBLOCKS; ++b, ++filled) {
        const float *o = sim_render();
        for (unsigned i = 0; i < FM1_BLOCK_FRAMES; ++i) pcm[filled * FM1_BLOCK_FRAMES + i] = o[2 * i];
    }
}
/* Cents from `ref_note` of the last WINDOW blocks rendered. */
static double tail_cents(double ref_note)
{
    unsigned n = WINDOW * FM1_BLOCK_FRAMES;
    float *w = pcm + (filled - WINDOW) * FM1_BLOCK_FRAMES;
    double mean = 0; /* raw synth output carries DC, which would bias the autocorrelation */
    for (unsigned i = 0; i < n; ++i) mean += w[i];
    for (unsigned i = 0; i < n; ++i) w[i] -= (float)(mean / n);
    return 1200.0 * log2(estimate_hz(w, n, note_hz(ref_note)) / note_hz(ref_note));
}
static void tone_set(unsigned track, unsigned parameter, unsigned value)
{
    uint8_t v[GROOVE_TONE_PARAMS];
    if (groove_tone_copy(track, 0, v) >= 0) groove_tone_edit(track, 0, parameter, (int)value - (int)v[parameter]);
}
static void setup(unsigned source, const struct groove_tuning *t, int bend14)
{
    sim_boot(3);
    filled = 0;
    if (groove_source_type(0) != source) CHECK(groove_source_set(0, source) == 0);
    if (source == GROOVE_303) { tone_set(0, 0, 127); tone_set(0, 1, 0); tone_set(0, 2, 0); }
    if (t) CHECK(groove_tuning_set(0, t) == 0);
    if (bend14) {
        uint8_t m[3] = {0xe0, (uint8_t)(bend14 & 127), (uint8_t)(bend14 >> 7)};
        fm1_web_midi(m, 3);
    }
}
/* One held note, cents relative to 12-TET `note`. */
static double measure(unsigned source, unsigned note, const struct groove_tuning *t, int bend14)
{
    setup(source, t, bend14);
    groove_note(0, note, 100, 0);
    run(SETTLE + WINDOW);
    return tail_cents(note);
}

/* PSG chip period is an integer: predict from the tuned frequency. */
static double psg_predict(unsigned note, int cents)
{
    double hz = note_hz(note + cents / 100.0);
    double period = floor(1789772.0 / (16.0 * hz) + 0.5);
    return 1200.0 * log2(1789772.0 / (16.0 * period) / note_hz(note));
}

static const unsigned pitched[] = {GROOVE_DX7, GROOVE_303, GROOVE_PSG, GROOVE_PLUCK};

static void api(void)
{
    sim_boot(3);
    struct groove_tuning a, z, got;
    memset(&z, 0, sizeof z);
    for (unsigned i = 0; i < GROOVE_TUNING_KEYS; ++i) { a.main[i] = (int8_t)(i * 10 - 55); a.alt[i] = (int8_t)(127 - 20 * (int)i); }
    a.main[0] = 127; a.alt[11] = -127;
    memset(&got, 0x55, sizeof got);
    groove_tuning_get(3, &got);
    CHECK(!memcmp(&got, &z, sizeof z));                    /* boot: all zero */
    CHECK(groove_tuning_set(3, &a) == 0);
    memset(&got, 0, sizeof got);
    groove_tuning_get(3, &got);
    CHECK(!memcmp(&got, &a, sizeof a));                    /* round trip */
    groove_tuning_get(2, &got);
    CHECK(!memcmp(&got, &z, sizeof z));                    /* other tracks untouched */
    CHECK(groove_tuning_set(GROOVE_TRACKS, &a) < 0);       /* track 8 */
    CHECK(groove_tuning_set(3, NULL) < 0);
    memset(&got, 0x55, sizeof got);
    groove_tuning_get(GROOVE_TRACKS, &got);
    CHECK(!memcmp(&got, &z, sizeof z));                    /* out of range reads zero */
    struct groove_tuning bad = a;
    bad.main[5] = -128;
    CHECK(groove_tuning_set(3, &bad) < 0);
    bad = a; bad.alt[7] = -128;
    CHECK(groove_tuning_set(3, &bad) < 0);
    groove_tuning_get(3, &got);
    CHECK(!memcmp(&got, &a, sizeof a));                    /* old tables preserved */
    CHECK(groove_tuning_set(7, &a) == 0);                  /* last track ok */
    sim_boot(3);
    groove_tuning_get(3, &got);
    CHECK(!memcmp(&got, &z, sizeof z));                    /* boot resets */
    puts("api done");
}

/* 3.1: drum tracks ignore the tables. Same notes, zero vs non-zero tables. */
static void drums(void)
{
    static float out[2][64 * FM1_BLOCK_FRAMES * 2];
    for (unsigned s = GROOVE_808; s <= GROOVE_SAMPLE; s += GROOVE_SAMPLE - GROOVE_808) {
        for (unsigned pass = 0; pass < 2; ++pass) {
            sim_boot(3);
            CHECK(groove_source_set(0, s) == 0);
            struct groove_tuning t;
            memset(&t, 0, sizeof t);
            if (pass) for (unsigned i = 0; i < GROOVE_TUNING_KEYS; ++i) { t.main[i] = (int8_t)(40 + i); t.alt[i] = -90; }
            CHECK(groove_tuning_set(0, &t) == 0);
            groove_tuning_alt_hold(0, pass);
            for (unsigned d = 0; d < 4; ++d) groove_note(0, groove_pad_note(0, d), 100, 0);
            for (unsigned b = 0; b < 64; ++b) memcpy(out[pass] + b * FM1_BLOCK_FRAMES * 2, sim_render(), FM1_BLOCK_FRAMES * 2 * sizeof(float));
        }
        int same = !memcmp(out[0], out[1], sizeof out[0]);
        int sound = 0;
        for (unsigned i = 0; i < sizeof out[0] / sizeof(float); ++i) sound |= out[0][i] != 0;
        printf("%-6s drum render identical with tables %s (audible %d)\n", name[s], same ? "ok" : "FAIL", sound);
        CHECK(same); CHECK(sound);
    }
}

/* 3.2-3.4: tuned pitch relative to untuned, per engine. */
static void pitches(void)
{
    struct groove_tuning t;
    char label[96];
    for (unsigned i = 0; i < 4; ++i) {
        unsigned s = pitched[i];
        double base = measure(s, 64, NULL, 0);
        memset(&t, 0, sizeof t);
        t.main[4] = -50;
        double c = measure(s, 64, &t, 0);
        snprintf(label, sizeof label, "%s E4 main[4]=-50 delta", name[s]);
        if (s == GROOVE_PSG) { NEAR(label, c - base, psg_predict(64, -50) - psg_predict(64, 0), 0.5); NEAR("PSG E4 tuned vs predicted", c, psg_predict(64, -50), 0.5); }
        else NEAR(label, c - base, -50, 1.0);
        /* other pitch class, positive, other octave: A5 (81) +33 */
        double b2 = measure(s, 81, NULL, 0);
        memset(&t, 0, sizeof t);
        t.main[9] = 33;
        double c2 = measure(s, 81, &t, 0);
        snprintf(label, sizeof label, "%s A5 main[9]=+33 delta", name[s]);
        if (s == GROOVE_PSG) NEAR(label, c2 - b2, psg_predict(81, 33) - psg_predict(81, 0), 0.5);
        else NEAR(label, c2 - b2, 33, 1.0);
        /* an untouched pitch class is unaffected (E4 table, play F4) */
        memset(&t, 0, sizeof t);
        t.main[4] = -50;
        double f0 = measure(s, 65, NULL, 0), f1 = measure(s, 65, &t, 0);
        snprintf(label, sizeof label, "%s F4 unaffected by main[4]", name[s]);
        NEAR(label, f1 - f0, 0, 0.05);
        /* alt table via alt hold; main stays untouched */
        memset(&t, 0, sizeof t);
        t.alt[4] = 40;
        setup(s, &t, 0);
        groove_tuning_alt_hold(0, 1);
        groove_note(0, 64, 100, 0);
        run(SETTLE + WINDOW);
        double ca = tail_cents(64);
        snprintf(label, sizeof label, "%s E4 alt hold on, alt[4]=+40", name[s]);
        if (s == GROOVE_PSG) NEAR(label, ca - base, psg_predict(64, 40) - psg_predict(64, 0), 0.5);
        else NEAR(label, ca - base, 40, 1.0);
        setup(s, &t, 0);
        groove_tuning_alt_hold(0, 1);
        groove_tuning_alt_hold(0, 0);
        groove_note(0, 64, 100, 0);
        run(SETTLE + WINDOW);
        snprintf(label, sizeof label, "%s E4 alt hold off uses main", name[s]);
        NEAR(label, tail_cents(64) - base, 0, 0.05);
        /* explicit alt flag (what sequenced steps pass) */
        setup(s, &t, 0);
        groove_note_ex(0, 64, 100, 1, 1);
        run(SETTLE + WINDOW);
        snprintf(label, sizeof label, "%s E4 groove_note_ex alt=1", name[s]);
        if (s == GROOVE_PSG) NEAR(label, tail_cents(64) - base, psg_predict(64, 40) - psg_predict(64, 0), 0.5);
        else NEAR(label, tail_cents(64) - base, 40, 1.0);
    }
}

/* 3.2/3.3/3.4: tuning stacks with pitch bend (+~half of the +-2 st range). */
static void bends(void)
{
    struct groove_tuning t;
    memset(&t, 0, sizeof t);
    t.main[4] = -50;
    char label[96];
    for (unsigned i = 0; i < 4; ++i) {
        unsigned s = pitched[i];
        int bend = 8192 + 4096; /* +1 semitone on a +-2 range */
        double b0 = measure(s, 64, NULL, bend), b1 = measure(s, 64, &t, bend);
        snprintf(label, sizeof label, "%s E4 bent %+.1f c, tuned -50: delta", name[s], b0);
        if (s == GROOVE_PSG) { /* integer period: compare with the quantised prediction */
            NEAR(label, b1 - b0, psg_predict(65, -50) - psg_predict(65, 0), 0.5);
        } else NEAR(label, b1 - b0, -50, 1.0);
        printf("   bent untuned %+.2f c  bent tuned %+.2f c\n", b0, b1);
    }
}

/* 3.3: a mono slide from C4 to tuned E4 settles on the tuned target. */
static void slide303(void)
{
    struct groove_tuning t;
    memset(&t, 0, sizeof t);
    t.main[4] = -50;
    setup(GROOVE_303, &t, 0);
    groove_mono_set(0, 1);
    groove_note(0, 60, 100, 0);
    run(60);
    groove_note(0, 64, 100, 0); /* legato: slides */
    run(SETTLE + WINDOW);
    NEAR("303 slide C4->E4 (-50) settles", tail_cents(64), -50, 1.0);
    /* releasing E4 re-triggers C4 untuned */
    groove_note(0, 64, 0, 0);
    run(SETTLE + WINDOW);
    NEAR("303 slide back to C4 (untuned)", tail_cents(60), 0, 1.0);
}

/* 3.2: DX7 glide lands on the tuned pitch. Driven directly because the engine
 * never sets a glide amount; the baseline is the same note un-glided. */
static int32_t blk[FM1_BLOCK_FRAMES];
static void dx7_run(synth_dx7_t *s, unsigned blocks)
{
    for (unsigned b = 0; b < blocks; ++b) {
        memset(blk, 0, sizeof blk); /* render accumulates into the bus */
        synth_dx7_render(s, blk, FM1_BLOCK_FRAMES);
        for (unsigned i = 0; i < FM1_BLOCK_FRAMES; ++i) pcm[filled * FM1_BLOCK_FRAMES + i] = (float)blk[i];
        ++filled;
    }
}
static double dx7_note(synth_dx7_t *s, uint8_t *patch, unsigned first, unsigned note, int cents)
{
    uint8_t on1[3] = {0x90, (uint8_t)first, 100}, on2[3] = {0x90, (uint8_t)note, 100};
    synth_dx7_reset(s); /* no release tail from the previous measurement */
    synth_dx7_set_operator_mask(s, 63);
    synth_dx7_set_adsr(s, 0, 0, 0, 100, 0);
    synth_dx7_set_pitch_bend_range(s, 2, 2);
    synth_dx7_lfo_reset(s, patch);
    synth_dx7_set_poly_mode(s, 1);
    synth_dx7_set_glide(s, 10, 0); /* 100 ms */
    filled = 0;
    if (first) {
        synth_dx7_midi_tuned(s, patch, on1, 3, 0);
        dx7_run(s, 60);
        filled = 0;
    }
    synth_dx7_midi_tuned(s, patch, on2, 3, cents);
    dx7_run(s, SETTLE + WINDOW);
    return tail_cents(note);
}
static void glide_dx7(void)
{
    uint8_t patch[SYNTH_DX7_EXPANDED_PATCH_BYTES];
    struct groove_fm_controls controls;
    sim_boot(3);
    CHECK(groove_source_type(0) == GROOVE_DX7 && groove_fm_copy(0, patch, &controls) == 0); /* the engine's own patch */
    synth_dx7_t *s = synth_dx7_create(GROOVE_VOICES, FM1_SAMPLE_RATE, FM1_BLOCK_FRAMES);
    CHECK(s != NULL);
    if (!s) return;
    double e4 = dx7_note(s, patch, 0, 64, 0), c4s = dx7_note(s, patch, 0, 61, 0);
    NEAR("DX7 direct E4 untuned (baseline)", e4, 0, 1.0);
    NEAR("DX7 direct E4 tuned -50 (no glide)", dx7_note(s, patch, 0, 64, -50), -50, 1.0);
    NEAR("DX7 glide C4->E4 settles (untuned)", dx7_note(s, patch, 60, 64, 0), e4, 1.0);
    NEAR("DX7 glide C4->E4(-50) settles", dx7_note(s, patch, 60, 64, -50), -50, 1.0);
    NEAR("DX7 glide C4->C#4(-30) settles", dx7_note(s, patch, 60, 61, -30), c4s - 30, 1.0);
    synth_dx7_destroy(s);
}

/* 3.6: a table change leaves held notes alone; the next note-on uses it. */
static void capture(void)
{
    struct groove_tuning t0, t1;
    memset(&t0, 0, sizeof t0);
    t0.main[4] = -50;
    t1 = t0;
    t1.main[4] = 30;
    char label[96];
    for (unsigned i = 0; i < 4; ++i) {
        unsigned s = pitched[i];
        setup(s, &t0, 0);
        groove_note(0, 64, 100, 0);
        run(SETTLE + WINDOW);
        double before = tail_cents(64);
        CHECK(groove_tuning_set(0, &t1) == 0);
        run(WINDOW);
        double during = tail_cents(64);
        snprintf(label, sizeof label, "%s held note after table edit", name[s]);
        NEAR(label, during, before, 0.3);
        groove_note(0, 64, 0, 0);
        run(2);
        filled = 0;
        groove_note(0, 64, 100, 0);
        run(SETTLE + WINDOW);
        snprintf(label, sizeof label, "%s next note-on uses new value", name[s]);
        if (s == GROOVE_PSG) NEAR(label, tail_cents(64), psg_predict(64, 30), 0.5);
        else NEAR(label, tail_cents(64) - (before + 50), 30, 1.0);
    }
}

#ifndef TUNING_NO_MAIN
int main(void)
{
    api();
    drums();
    pitches();
    bends();
    slide303();
    glide_dx7();
    capture();
    printf("%s (%d failure%s)\n", failures ? "FAILED" : "all tuning tests passed", failures, failures == 1 ? "" : "s");
    return failures != 0;
}
#endif
