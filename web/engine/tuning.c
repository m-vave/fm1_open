/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#include "instrument.h"

/* Whole-instrument snapshot of the per-track tuning tables, built on the
 * per-track set/get. Control task only. */
int groove_tuning_capture(struct groove_tuning t[GROOVE_TRACKS])
{
    if (!t) return -1;
    for (unsigned i = 0; i < GROOVE_TRACKS; ++i) groove_tuning_get(i, &t[i]);
    return 0;
}
int groove_tuning_apply(const struct groove_tuning t[GROOVE_TRACKS])
{
    if (!t) return -1;
    for (unsigned i = 0; i < GROOVE_TRACKS; ++i)
        for (unsigned k = 0; k < GROOVE_TUNING_KEYS; ++k)
            if (t[i].main[k] == -128 || t[i].alt[k] == -128) return -2;
    for (unsigned i = 0; i < GROOVE_TRACKS; ++i)
        if (groove_tuning_set(i, &t[i])) return -3; /* unreachable after validation */
    return 0;
}
