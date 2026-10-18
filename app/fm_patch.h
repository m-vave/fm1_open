/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#ifndef FM1_FM_PATCH_H
#define FM1_FM_PATCH_H
#include "engine.h"
/* Engines without groove_fm_program_count() address 16 programs. */
enum { FM1_FM_PROGRAMS = 16 };
/* Expanded DX7 voices from the patch library (app/library.c); program n is
 * library slot n, panel position n + 1. Empty slots give INIT VOICE. */
FM1_DSP_CODE void groove_fm_program(uint8_t patch[156], unsigned program);
const char *groove_fm_program_name(unsigned program);
FM1_DSP_CODE unsigned groove_fm_program_count(void); /* 1..128; engines default to 0 = 16, wrapped */
/* Engines that take the count from the app: the open mirror from API 5, and
 * the native engine from API 8 (proposed). Older engines address 16. */
#if defined(FM1_OPEN_WEB) ? FM1_SYNTH_API >= 5 : FM1_SYNTH_API >= 8
#define GROOVE_ENGINE_PROGRAMS groove_fm_program_count()
#else
#define GROOVE_ENGINE_PROGRAMS FM1_FM_PROGRAMS
#endif
#if defined(FM1_OPEN_WEB)
/* The former built-in DX7 programs, kept for the web workbench. */
enum { GROOVE_FACTORY_VOICES = 16 };
void groove_factory_voice(unsigned voice, uint8_t patch[156]);
#endif
#endif
