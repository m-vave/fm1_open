/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* Alt flag through sequencer playback, live recording and the arpeggiator
 * (tasks 4.2-4.4 of engine-tuning-hook). Reuses tuning_test.c's measurement
 * helpers. Exits non-zero on failure. */
#define TUNING_NO_MAIN
#include "tuning_test.c"
#ifndef M_PI /* not defined by strict C11 libcs */
#define M_PI 3.14159265358979323846
#endif

#ifndef GROOVE_STEP_ALT
#define GROOVE_STEP_ALT (1U << 13)
#endif
enum { TIE = 1U << 12 };

static struct groove_tuning tab(void)
{
    struct groove_tuning t;
    memset(&t, 0, sizeof t);
    t.main[4] = -50;
    t.alt[0] = 10; t.alt[4] = 40; t.alt[7] = -20;
    return t;
}
static void fill(unsigned track, unsigned note, unsigned drums)
{ /* tied cells on the first 16 steps: the note is held for the whole window */
    struct groove_step c = {(uint8_t)note, 100, (uint16_t)drums};
    groove_sequence_length_set(track, 16);
    for (unsigned s = 0; s < 16; ++s) groove_sequence_replace(track, s, &c, 1);
}
static void play(unsigned blocks)
{
    const uint8_t start = 0xfa;
    fm1_web_midi(&start, 1);
    run(blocks);
}
static double seq_cents(unsigned source, unsigned note, unsigned flags, int hold)
{
    struct groove_tuning t = tab();
    setup(source, &t, 0);
    groove_tuning_alt_hold(0, hold);
    fill(0, note, TIE | flags);
    play(SETTLE + WINDOW);
    return tail_cents(note);
}

/* Spectral peak (Hann DFT, 0.5 c scan, +-80 c) around a note's fundamental. */
static double peak_cents(const float *x, unsigned n, double note)
{
    double best = -1, bc = 0, v[321];
    for (int k = 0; k <= 320; ++k) {
        double w = 2 * M_PI * note_hz(note + (k - 160) * 0.005) / RATE, re = 0, im = 0;
        for (unsigned i = 0; i < n; ++i) {
            double h = 0.5 - 0.5 * cos(2 * M_PI * i / n);
            re += h * x[i] * cos(w * i); im += h * x[i] * sin(w * i);
        }
        v[k] = re * re + im * im;
        if (v[k] > best) { best = v[k]; bc = k; }
    }
    int k = (int)bc;
    double d = k > 0 && k < 320 ? v[k - 1] - 2 * v[k] + v[k + 1] : 0;
    return (k - 160 + (d != 0 ? 0.5 * (v[k - 1] - v[k + 1]) / d : 0)) * 0.5;
}
static void chord_cents(unsigned flags, int hold, double out[3])
{
    struct groove_tuning t = tab();
    setup(GROOVE_DX7, &t, 0);
    groove_tuning_alt_hold(0, hold);
    static const uint8_t notes[3] = {60, 64, 67};
    CHECK(groove_sequence_chord(0, 0, notes, 3, 100) == 0);
    struct groove_step c;
    groove_sequence_read(0, 0, &c, 1);
    c.drums |= (uint16_t)(TIE | flags);
    fill(0, 60, c.drums);
    play(SETTLE + 700);
    unsigned n = 600 * FM1_BLOCK_FRAMES;
    float *w = pcm + (filled - 600) * FM1_BLOCK_FRAMES;
    double mean = 0;
    for (unsigned i = 0; i < n; ++i) mean += w[i];
    for (unsigned i = 0; i < n; ++i) w[i] -= (float)(mean / n);
    for (unsigned i = 0; i < 3; ++i) out[i] = peak_cents(w, n, notes[i]);
}

static void sequenced(void)
{
    static const unsigned pc[3] = {60, 64, 67};
    static const int alt[3] = {10, 40, -20}, main_[3] = {0, -50, 0};
    for (unsigned s = 0; s < 4; ++s) {
        unsigned src = pitched[s];
        for (unsigned i = 0; i < 3; ++i) {
            char l[96];
            double base = seq_cents(src, pc[i], 0, 0), a = seq_cents(src, pc[i], GROOVE_STEP_ALT, 0);
            double base0 = measure(src, pc[i], NULL, 0);
            snprintf(l, sizeof l, "%s note %u step alt delta", name[src], pc[i]);
            if (src == GROOVE_PSG) NEAR(l, a, psg_predict(pc[i], alt[i]), 0.5);
            else NEAR(l, a - base0, alt[i], 1.0);
            snprintf(l, sizeof l, "%s note %u step main delta", name[src], pc[i]);
            if (src == GROOVE_PSG) NEAR(l, base, psg_predict(pc[i], main_[i]), 0.5);
            else NEAR(l, base - base0, main_[i], 1.0);
        }
    }
    /* alt hold must not affect a sequenced step */
    NEAR("DX7 E4 step main, alt hold on", seq_cents(GROOVE_DX7, 64, 0, 1) - measure(GROOVE_DX7, 64, NULL, 0), -50, 1.0);
    /* chord: C/E/G as one step */
    double m[3], a[3];
    chord_cents(0, 0, m);
    chord_cents(GROOVE_STEP_ALT, 0, a);
    static const char *const nn[3] = {"C4", "E4", "G4"};
    /* the spectral estimate carries the same small DX7 offset as measure(); take it from an untuned chord */
    struct groove_tuning z; memset(&z, 0, sizeof z);
    setup(GROOVE_DX7, &z, 0);
    double u[3];
    {
        static const uint8_t notes[3] = {60, 64, 67};
        CHECK(groove_sequence_chord(0, 0, notes, 3, 100) == 0);
        struct groove_step c; groove_sequence_read(0, 0, &c, 1);
        fill(0, 60, c.drums | TIE);
        play(SETTLE + 700);
        unsigned n = 600 * FM1_BLOCK_FRAMES; float *w = pcm + (filled - 600) * FM1_BLOCK_FRAMES; double mean = 0;
        for (unsigned i = 0; i < n; ++i) mean += w[i];
        for (unsigned i = 0; i < n; ++i) w[i] -= (float)(mean / n);
        for (unsigned i = 0; i < 3; ++i) u[i] = peak_cents(w, n, notes[i]);
    }
    for (unsigned i = 0; i < 3; ++i) {
        char l[96];
        snprintf(l, sizeof l, "DX7 chord %s main delta", nn[i]);
        NEAR(l, m[i] - u[i], main_[i], 2.0);
        snprintf(l, sizeof l, "DX7 chord %s alt delta", nn[i]);
        NEAR(l, a[i] - u[i], alt[i], 2.0);
    }
}

/* The recorded step's flag, read back from the sequence. */
static unsigned flag_at(unsigned track, unsigned step)
{
    struct groove_step c;
    groove_sequence_read(track, step, &c, 1);
    return (c.drums & GROOVE_STEP_ALT) != 0;
}
static void rec_note(unsigned note)
{
    uint8_t on[3] = {0x90, (uint8_t)note, 100}, off[3] = {0x80, (uint8_t)note, 0};
    fm1_web_midi(on, 3);
    groove_edit_poll();
    fm1_web_midi(off, 3);
    groove_edit_poll();
}
static void recording(void)
{
    struct groove_tuning t = tab();
    setup(GROOVE_DX7, &t, 0);
    groove_edit_record(1, 0, 2);
    groove_tuning_alt_hold(0, 1);
    rec_note(64);                                   /* cursor step 2 */
    groove_edit_seek(0, 5);
    groove_tuning_alt_hold(0, 0);
    rec_note(64);                                   /* step 5, hold off */
    groove_edit_seek(0, 7);
    groove_tuning_alt_hold(0, 1);
    { /* a chord recorded with hold on */
        uint8_t on[3][3] = {{0x90, 60, 100}, {0x90, 64, 100}, {0x90, 67, 100}};
        for (unsigned i = 0; i < 3; ++i) fm1_web_midi(on[i], 3);
        groove_edit_poll();
        for (unsigned i = 0; i < 3; ++i) { on[i][0] = 0x80; fm1_web_midi(on[i], 3); }
        groove_edit_poll();
    }
    CHECK(flag_at(0, 2) == 1);
    CHECK(flag_at(0, 5) == 0);
    CHECK(flag_at(0, 7) == 1);
    printf("recorded alt flags: step2=%u (hold on) step5=%u (hold off) step7 chord=%u\n", flag_at(0, 2), flag_at(0, 5), flag_at(0, 7));
    groove_edit_seek(0, 2);                         /* overdub replaces the flag */
    groove_tuning_alt_hold(0, 0);
    rec_note(64);
    CHECK(flag_at(0, 2) == 0);
    groove_edit_seek(0, 5);
    groove_tuning_alt_hold(0, 1);
    rec_note(64);
    CHECK(flag_at(0, 5) == 1);
    printf("overdub: step2 now %u (hold off), step5 now %u (hold on)\n", flag_at(0, 2), flag_at(0, 5));
    groove_edit_record(0, 0, 0);

    /* Record E4 with hold on, hold off, play back. */
    setup(GROOVE_DX7, &t, 0);
    groove_edit_record(1, 0, 0);
    groove_tuning_alt_hold(0, 1);
    rec_note(64);
    groove_edit_record(0, 0, 0);
    groove_tuning_alt_hold(0, 0);
    struct groove_step c;
    groove_sequence_read(0, 0, &c, 1);
    CHECK(c.note == 64 && (c.drums & GROOVE_STEP_ALT));
    fill(0, 64, c.drums | TIE);
    play(SETTLE + WINDOW);
    double got = tail_cents(64), base = measure(GROOVE_DX7, 64, NULL, 0);
    NEAR("DX7 recorded E4 (alt hold on), played hold off", got - base, 40, 1.0);
    setup(GROOVE_DX7, &t, 0);
    groove_edit_record(1, 0, 0);
    rec_note(64);
    groove_edit_record(0, 0, 0);
    groove_sequence_read(0, 0, &c, 1);
    CHECK(!(c.drums & GROOVE_STEP_ALT));
    fill(0, 64, c.drums | TIE);
    play(SETTLE + WINDOW);
    NEAR("DX7 recorded E4 (hold off), played", tail_cents(64) - base, -50, 1.0);
    /* drum track recording keeps working and never touches bit 13 */
    setup(GROOVE_DX7, &t, 0);
    CHECK(groove_source_set(7, GROOVE_808) == 0);
    groove_edit_record(1, 7, 0);
    groove_tuning_alt_hold(7, 1);
    { uint8_t on[3] = {0x99, 36, 100}; fm1_web_midi(on, 3); groove_edit_poll(); }
    groove_edit_record(0, 7, 0);
    groove_sequence_read(7, 0, &c, 1);
    CHECK(c.drums != 0 && !(c.drums & GROOVE_STEP_ALT));
}

static double arp_cents(unsigned note, int hold_on, int tuned)
{
    struct groove_tuning t = tab();
    if (!tuned) memset(&t, 0, sizeof t);
    setup(GROOVE_DX7, &t, 0);
    groove_arp_set(0, 1);
    groove_arp_edit(0, 3, 2);                       /* gate 100%: no gaps */
    groove_arp_edit(0, 0, -2);                      /* slowest rate: few retriggers */
    groove_tuning_alt_hold(0, hold_on);
    groove_note(0, note, 100, 0);
    groove_tuning_alt_hold(0, 0);                   /* release of hold must not matter */
    /* One arp step lasts ~170 blocks at rate 0; measure inside the first, before any retrigger. */
    play(150);
    unsigned n = 90 * FM1_BLOCK_FRAMES;
    float *w = pcm + (filled - 90) * FM1_BLOCK_FRAMES;
    double mean = 0;
    for (unsigned i = 0; i < n; ++i) mean += w[i];
    for (unsigned i = 0; i < n; ++i) w[i] -= (float)(mean / n);
    return 1200.0 * log2(estimate_hz(w, n, note_hz(note)) / note_hz(note));
}
static void arp(void)
{
    double base = arp_cents(64, 0, 0);
    NEAR("DX7 arp E4 key pressed with alt hold", arp_cents(64, 1, 1) - base, 40, 1.0);
    NEAR("DX7 arp E4 key pressed without alt", arp_cents(64, 0, 1) - base, -50, 1.0);
}

/* Two held keys on one track, one pressed with alt hold on and one with it off.
 * The arp (up mode, gate 100%, rate 0) plays one note per ~170-block step, so
 * the first note is measured in blocks 60..150 and the second in blocks 230..320
 * (Start is sent once; the second window continues the same run); each window is inside a single arp step and holds one
 * note only. Every window is compared with an untuned run of the same keys.
 * Each note must use the table of its own key's flag. */
static void arp_two(unsigned note_a, int alt_a, unsigned note_b, int alt_b, int tuned, double out[2])
{
    struct groove_tuning t = tab();
    if (!tuned) memset(&t, 0, sizeof t);
    setup(GROOVE_DX7, &t, 0);
    groove_arp_set(0, 1);
    groove_arp_edit(0, 3, 2);
    groove_arp_edit(0, 0, -2);
    groove_tuning_alt_hold(0, alt_a);
    groove_note(0, note_a, 100, 0);
    groove_tuning_alt_hold(0, alt_b);
    groove_note(0, note_b, 100, 0);
    groove_tuning_alt_hold(0, 0);
    unsigned lo = note_a < note_b ? note_a : note_b, hi = note_a < note_b ? note_b : note_a;
    unsigned want[2] = {lo, hi}, upto[2] = {150, 170};
    for (unsigned k = 0; k < 2; ++k) {
        if (k) run(upto[k]); else play(upto[k]); /* play() sends MIDI Start, which restarts the arp: once only */
        unsigned n = 90 * FM1_BLOCK_FRAMES;
        float *w = pcm + (filled - 90) * FM1_BLOCK_FRAMES;
        double mean = 0;
        for (unsigned i = 0; i < n; ++i) mean += w[i];
        for (unsigned i = 0; i < n; ++i) w[i] -= (float)(mean / n);
        out[k] = 1200.0 * log2(estimate_hz(w, n, note_hz(want[k])) / note_hz(want[k]));
    }
}
static void arp_mixed(void)
{
    double b[2], m[2];
    /* E4 (pc 4: main -50, alt +40) with alt, G4 (pc 7: main 0, alt -20) without */
    arp_two(64, 1, 67, 0, 0, b); arp_two(64, 1, 67, 0, 1, m);
    NEAR("DX7 arp 2 keys: E4 (alt key) uses alt", m[0] - b[0], 40, 1.0);
    NEAR("DX7 arp 2 keys: G4 (main key) uses main", m[1] - b[1], 0, 1.0);
    /* reversed assignment and press order: G4 with alt, E4 without */
    arp_two(67, 1, 64, 0, 0, b); arp_two(67, 1, 64, 0, 1, m);
    NEAR("DX7 arp 2 keys: E4 (main key) uses main", m[0] - b[0], -50, 1.0);
    NEAR("DX7 arp 2 keys: G4 (alt key) uses alt", m[1] - b[1], -20, 1.0);
}

int main(void)
{
    sequenced();
    recording();
    arp();
    arp_mixed();
    printf("%s (%d failure%s)\n", failures ? "FAILED" : "all step-alt tests passed", failures, failures == 1 ? "" : "s");
    return failures != 0;
}
