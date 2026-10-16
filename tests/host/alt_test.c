/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* Step alt flag (bit 13 of a melodic cell): validation and edit operations.
 * Tasks 4.1 and 4.5 of engine-tuning-hook. Exits non-zero on failure. */
#include "internal.h"
#include <stdio.h>
#include <string.h>

#ifndef GROOVE_STEP_ALT
#define GROOVE_STEP_ALT (1U << 13)
#endif

void sim_boot(unsigned clear);

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static struct groove_project p;

static void cap(void) { CHECK(groove_project_capture(&p) == 0); }
static unsigned cell(unsigned t, unsigned s)
{
    struct groove_step c;
    groove_sequence_read(t, s, &c, 1);
    return c.drums;
}
static void put(unsigned t, unsigned s, unsigned drums, unsigned note, unsigned vel)
{
    struct groove_step c = {(uint8_t)note, (uint8_t)vel, (uint16_t)drums};
    groove_sequence_replace(t, s, &c, 1);
}

int main(void)
{
    sim_boot(3);
    const unsigned T = 1;
    CHECK(groove_source_type(T) != GROOVE_808 && groove_source_type(T) != GROOVE_SAMPLE);

    /* --- 4.1 validation --- */
    cap();
    CHECK(groove_project_valid(&p));
    p.step[T][3].drums |= GROOVE_STEP_ALT;
    p.step[T][3].note = 60; p.step[T][3].velocity = 100;
    CHECK(groove_project_valid(&p));                        /* bit 13 accepted */
    p.step[T][4].drums = GROOVE_STEP_ALT | 1U << 12 | 0x1e0 | 0xe00; /* all other fields */
    p.step[T][4].note = 60; p.step[T][4].velocity = 90;
    CHECK(groove_project_valid(&p));
    CHECK(groove_project_apply(&p) == 0);
    CHECK(cell(T, 3) & GROOVE_STEP_ALT);                    /* survives apply */
    cap();
    CHECK(p.step[T][3].drums & GROOVE_STEP_ALT);            /* and capture */
    for (unsigned bit = 14; bit < 16; ++bit) {
        struct groove_project q = p;
        q.step[T][5].drums |= (uint16_t)(1U << bit);
        CHECK(!groove_project_valid(&q));
        q.step[T][5].drums = (uint16_t)(1U << bit) | GROOVE_STEP_ALT;
        CHECK(!groove_project_valid(&q));
    }
    /* Every melodic track and every step accepts bit 13. */
    {
        struct groove_project q = p;
        for (unsigned t = 0; t < 8; ++t)
            if (q.preset[t].type != GROOVE_808 && q.preset[t].type != GROOVE_SAMPLE)
                for (unsigned s = 0; s < 64; ++s) { q.step[t][s].drums |= GROOVE_STEP_ALT; }
        CHECK(groove_project_valid(&q));
    }
    /* Drum cells: the original rules, unchanged. 808 ignores the high bits;
     * the sampler rejects anything above its eight pads. */
    {
        unsigned drum = 8;
        for (unsigned t = 0; t < 8; ++t)
            if (p.preset[t].type == GROOVE_808) drum = t;
        if (drum == 8) { groove_source_set(7, GROOVE_808); cap(); drum = 7; }
        CHECK(p.preset[drum].type == GROOVE_808);
        static const uint16_t pads[] = {0x0001, 0x2000, 0x4000, 0x8000, 0xe000, 0xffff, 0x1fff};
        for (unsigned i = 0; i < sizeof pads / sizeof *pads; ++i) {
            struct groove_project q = p;
            q.step[drum][2].drums = pads[i];
            CHECK(groove_project_valid(&q));            /* 808: any pad mask */
        }
        struct groove_project q = p;
        q.step[drum][2].note = 1;
        CHECK(!groove_project_valid(&q));               /* low nibble reserved */
        groove_source_set(drum, GROOVE_SAMPLE);
        cap();
        CHECK(p.preset[drum].type == GROOVE_SAMPLE);
        for (unsigned i = 0; i < sizeof pads / sizeof *pads; ++i) {
            struct groove_project r = p;
            r.step[drum][2].drums = pads[i];
            int expect = (pads[i] >> SAMPLE_DRUM_PADS) == 0;
            CHECK(groove_project_valid(&r) == expect);
        }
        groove_source_set(drum, GROOVE_808);
        cap();
    }

    /* --- 4.5 edit operations carry bit 13 --- */
    const unsigned A = GROOVE_STEP_ALT;
    /* Clean start: track T steps 0..15 empty, alt on step 2 (single note) and 3 (tie+gate). */
    for (unsigned s = 0; s < 16; ++s) put(T, s, 0, 60, 0);
    put(T, 2, A, 62, 100);
    put(T, 3, A | 1U << 12 | 8U << 5, 64, 90);
    put(T, 4, 0, 65, 80);
    CHECK(cell(T, 2) == A && cell(T, 3) == (A | 1U << 12 | 8U << 5));

    /* step settings and note edits preserve the alt bit */
    groove_step_setting_edit(T, 2, 1, 1);  /* gate */
    CHECK(cell(T, 2) & A);
    groove_step_setting_edit(T, 2, 2, 1);  /* probability */
    CHECK(cell(T, 2) & A);
    groove_step_setting_edit(T, 2, 4, 1);  /* tie */
    CHECK(cell(T, 2) & A);
    groove_step_setting_edit(T, 2, 3, 1);  /* chord */
    CHECK(cell(T, 2) & A);
    groove_step_setting_edit(T, 2, 0, -5); /* velocity */
    CHECK(cell(T, 2) & A);
    groove_sequence_edit(T, 2, 67, 100);   /* note change */
    CHECK(cell(T, 2) & A);
    put(T, 2, A, 62, 100);

    /* copy / paste: single step and page */
    CHECK(groove_edit_copy(T, 3, 0) == 0);
    CHECK(groove_edit_paste(T, 9, 0) == 0);
    CHECK(cell(T, 9) == cell(T, 3) && (cell(T, 9) & A));
    CHECK(groove_edit_copy(T, 0, 1) == 0);
    CHECK(groove_edit_paste(T, 16, 1) == 0);
    CHECK(cell(T, 18) == cell(T, 2) && (cell(T, 18) & A));
    CHECK(cell(T, 19) == cell(T, 3) && (cell(T, 19) & A));
    CHECK(!(cell(T, 20) & A));

    /* undo / redo of a paste */
    groove_edit_checkpoint(0xdead0000U); /* distinct operation tag */
    CHECK(groove_edit_copy(T, 3, 0) == 0);
    CHECK(groove_edit_paste(T, 11, 0) == 0);
    CHECK(cell(T, 11) & A);
    CHECK(groove_edit_undo() == 0);
    CHECK(!(cell(T, 11) & A));                 /* undone */
    CHECK((cell(T, 2) & A) && (cell(T, 3) & A) && (cell(T, 9) & A)); /* others kept */
    CHECK(groove_edit_undo() == 0);            /* redo */
    CHECK(cell(T, 11) & A);

    /* undo of a clear restores the alt bit; redo clears it again */
    CHECK(cell(T, 3) & A);
    CHECK(groove_edit_clear(T, 3, 0) == 0);
    CHECK(cell(T, 3) == 0 && !(cell(T, 3) & A));
    CHECK(groove_edit_undo() == 0);
    CHECK(cell(T, 3) & A);
    CHECK(groove_edit_undo() == 0);            /* redo */
    CHECK(cell(T, 3) == 0);
    CHECK(groove_edit_undo() == 0);            /* back again for the next checks */
    CHECK(cell(T, 3) & A);

    /* clear page and track */
    CHECK(groove_edit_clear(T, 16, 1) == 0);
    CHECK(!(cell(T, 18) & A) && !(cell(T, 19) & A));
    CHECK(cell(T, 2) & A);                     /* other page untouched */
    CHECK(groove_edit_undo() == 0);
    CHECK((cell(T, 18) & A) && (cell(T, 19) & A));

    /* repeat */
    CHECK(groove_sequence_length(T) == 16 || groove_sequence_length(T) == 64);
    if (groove_sequence_length(T) != 16) { groove_sequence_length_set(T, 16); }
    CHECK(groove_sequence_length(T) == 16);
    for (unsigned s = 16; s < 64; ++s) put(T, s, 0, 60, 0);
    CHECK(groove_edit_repeat(T, 48) == 0);
    for (unsigned k = 1; k < 3; ++k) {
        CHECK(cell(T, 16 * k + 2) & A);
        CHECK(cell(T, 16 * k + 3) & A);
        CHECK(cell(T, 16 * k + 9) & A);
        CHECK(!(cell(T, 16 * k + 4) & A));
    }
    /* Whole project still validates with alt bits in play. */
    cap();
    CHECK(groove_project_valid(&p));

    if (failures) { printf("alt_test: %d failure(s)\n", failures); return 1; }
    printf("alt_test: ok\n");
    return 0;
}
