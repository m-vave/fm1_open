/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* Native offline driver for the open engine mirror (web/engine + synths).
 * Links against the unmodified Simulator.c platform adapter.
 *   fm1_host render <demo 0..15> [blocks] [out.f32|-]   raw float32 stereo PCM
 *   fm1_host pitch <source> <note>                      measured Hz and cents
 *   fm1_host pitch-test                                 A4 check per engine
 * The engine is fully deterministic: time is the block counter and nothing
 * is seeded from the host, so identical sources give identical PCM. */
#include "internal.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Simulator.c exports (EXPORT() expands to nothing natively). */
void sim_boot(unsigned clear);
const float *sim_render(void);

enum { RATE = 44100, DEFAULT_BLOCKS = 2756 }; /* 2756 * 128 frames ~ 8.0 s */

static void boot(void) { sim_boot(3); } /* clear storage, skip the screen */

/* Render K blocks, optionally writing the interleaved stereo float samples. */
static void run(unsigned blocks, FILE *out, float *left, unsigned *peak_zero)
{
    for (unsigned b = 0; b < blocks; ++b) {
        const float *pcm = sim_render();
        if (out && fwrite(pcm, sizeof *pcm, FM1_BLOCK_WORDS, out) != FM1_BLOCK_WORDS) exit(2);
        for (unsigned i = 0; left && i < FM1_BLOCK_FRAMES; ++i) left[b * FM1_BLOCK_FRAMES + i] = pcm[2 * i];
        for (unsigned i = 0; peak_zero && i < FM1_BLOCK_WORDS; ++i) if (pcm[i] != 0.0f) *peak_zero = 1;
    }
}

static int render_demo(unsigned demo, unsigned blocks, const char *path)
{
    FILE *out = strcmp(path, "-") ? fopen(path, "wb") : stdout;
    if (!out) { perror(path); return 2; }
    boot();
    if (groove_factory_load(demo)) { fprintf(stderr, "demo %u failed to load\n", demo); return 2; }
    const uint8_t start = 0xfa;
    fm1_web_midi(&start, 1);
    unsigned audible = 0;
    run(blocks, out, NULL, &audible);
    if (out != stdout) fclose(out); else fflush(out);
    if (!audible) { fprintf(stderr, "demo %u rendered silence\n", demo); return 3; }
    return 0;
}

/* ---- pitch measurement ------------------------------------------------- */

static const char *const source_name[] = {"DX7", "303", "808", "PSG", "PLUCK", "SAMPLE"};

/* Fundamental by autocorrelation. Find the first strong peak for a coarse
 * period, then re-measure at a long multiple of it so that the quantization
 * of the lag shrinks by that multiple (parabolic interpolation on top). */
static double acf(const float *x, unsigned n, double lag)
{
    unsigned l = (unsigned)lag;
    double a = 0, e0 = 0, e1 = 0;
    for (unsigned i = 0; i + l < n; ++i) {
        a += (double)x[i] * x[i + l];
        e0 += (double)x[i] * x[i];
        e1 += (double)x[i + l] * x[i + l];
    }
    /* Normalised so decaying tones do not pull the peak towards short lags. */
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
    double a = acf(x, n, best - 1), b = acf(x, n, best), c = acf(x, n, best + 1);
    double d = a - 2 * b + c;
    return best + (d != 0 ? 0.5 * (a - c) / d : 0);
}
double fm1_estimate_hz(const float *x, unsigned n, double expect_hz)
{
    unsigned p0 = (unsigned)(RATE / expect_hz + 0.5);
    unsigned r = p0 / 4 + 1;
    double period = peak_near(x, n, p0, r);
    /* Double the lag repeatedly (each search is +-2 lags around the
     * prediction), so interpolation error is divided by the final multiple. */
    unsigned m = 1;
    while (2 * m * period < n / 2.0) {
        period = peak_near(x, n, (unsigned)(2 * m * period + 0.5), 2) / (2 * m);
        m *= 2;
    }
    return RATE / period;
}

static double note_hz(double note) { return 440.0 * pow(2.0, (note - 69.0) / 12.0); }

/* Set a part-0 tone pot to an absolute value via the public edit API. */
static void tone_set(unsigned track, unsigned parameter, unsigned value)
{
    uint8_t v[GROOVE_TONE_PARAMS];
    if (groove_tone_copy(track, 0, v) >= 0) groove_tone_edit(track, 0, parameter, (int)value - (int)v[parameter]);
}

/* Render one sustained voice of `source` on track 0 and return the measured
 * fundamental in Hz. All sends are zero and the mix is mono/centre after boot;
 * nothing but the engine's own tone shaping remains in the path. */
double measure_hz(unsigned source, unsigned note)
{
    enum { SETTLE = 120, BLOCKS = 120 + 256 }; /* ~70 ms settle, ~0.75 s analysed */
    static float pcm[BLOCKS * FM1_BLOCK_FRAMES];
    boot();
    if (groove_source_type(0) != source && groove_source_set(0, source)) return 0;
    if (source == GROOVE_303) { /* stationary timbre: bright, no resonance or envelope sweep */
        tone_set(0, 0, 127); tone_set(0, 1, 0); tone_set(0, 2, 0);
    }
    groove_note(0, note, 100, 0);
    run(BLOCKS, NULL, pcm, NULL);
    double hz = fm1_estimate_hz(pcm + SETTLE * FM1_BLOCK_FRAMES, (BLOCKS - SETTLE) * FM1_BLOCK_FRAMES, note_hz(note));
    return hz;
}
/* Cents of the measurement relative to 12-TET for `note`. */
double measure_cents(unsigned source, unsigned note)
{
    double hz = measure_hz(source, note);
    return hz > 0 ? 1200.0 * log2(hz / note_hz(note)) : NAN;
}

/* Per-engine A4 limits, in cents from 12-TET. PSG pitch is an integer chip
 * period (tune() in web/synth/psg.c): 1789772 / (16 * 440) = 254.23 rounds
 * to 254, which sounds 1.56 c sharp. That is structural, so its limit is 2 c
 * (half a period step is ~3.4 c at A4) and the measurement must also match
 * the quantised prediction, `expect`, to 0.5 c. Others must be within 1 c. */
static const struct { unsigned source; double tol, expect; } a4_limit[] = {
    {GROOVE_DX7, 1.0, 0}, {GROOVE_303, 1.0, 0}, {GROOVE_PSG, 2.0, 1.557}, {GROOVE_PLUCK, 1.0, 0},
};

static int pitch_test(void)
{
    int fail = 0;
    for (unsigned i = 0; i < sizeof a4_limit / sizeof *a4_limit; ++i) {
        unsigned s = a4_limit[i].source;
        double hz = measure_hz(s, 69), c = 1200.0 * log2(hz / 440.0);
        int ok = hz > 0 && fabs(c) <= a4_limit[i].tol && fabs(c - a4_limit[i].expect) <= 0.5;
        printf("%-6s A4 %9.4f Hz %+7.3f c (limit %.1f, expect %+.3f) %s\n", source_name[s], hz, c,
               a4_limit[i].tol, a4_limit[i].expect, ok ? "ok" : "FAIL");
        fail |= !ok;
    }
    return fail;
}

int main(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "render")) {
        unsigned demo = (unsigned)atoi(argv[2]);
        unsigned blocks = argc > 3 ? (unsigned)atoi(argv[3]) : DEFAULT_BLOCKS;
        if (demo >= GROOVE_FACTORY_PROJECTS) return 2;
        return render_demo(demo, blocks, argc > 4 ? argv[4] : "-");
    }
    if (argc >= 4 && !strcmp(argv[1], "pitch")) {
        unsigned s = (unsigned)atoi(argv[2]), n = (unsigned)atoi(argv[3]);
        if (s >= GROOVE_SOURCES || n > 127) return 2;
        printf("%s note %u %.4f Hz %+.3f c\n", source_name[s], n, measure_hz(s, n), measure_cents(s, n));
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "pitch-test")) return pitch_test();
    fprintf(stderr, "usage: fm1_host render <0..15> [blocks] [out|-] | pitch <src> <note> | pitch-test\n");
    return 2;
}
