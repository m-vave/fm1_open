/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#ifndef FM1_TUNING_HOOK_H
#define FM1_TUNING_HOOK_H
#include "instrument.h"
/* The shipped closed engine has no tuning hook. Redeclare the whole-instrument
 * tuning calls weak so firmware linked against it resolves them to null; callers
 * must test the address before use. The open mirror defines them (tuning.c). */
extern int groove_tuning_capture(struct groove_tuning t[GROOVE_TRACKS]) __attribute__((weak));
extern int groove_tuning_apply(const struct groove_tuning t[GROOVE_TRACKS]) __attribute__((weak));
#endif
