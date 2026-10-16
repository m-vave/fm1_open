/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#ifndef FM1_WEB_INTERNAL_H
#define FM1_WEB_INTERNAL_H
#include "instrument.h"
#include "fm1_engine_control.h"
void fm1_web_init(void);
void fm1_web_tick(void);
void fm1_web_control(void);
void fm1_web_midi(const uint8_t *message, size_t bytes);
void fm1_web_pause(void);
void fm1_web_resume(void);
FM1_DSP_CODE void groove_note(unsigned track, unsigned note, unsigned velocity, int sequenced);
/* groove_note with an explicit alt flag (GROOVE_STEP_ALT); selects the alt tuning table. */
FM1_DSP_CODE void groove_note_ex(unsigned track, unsigned note, unsigned velocity, int sequenced, unsigned alt);
FM1_DSP_CODE unsigned groove_alt_held(unsigned track);
void groove_sequence_alt(unsigned track, unsigned step, unsigned alt);
FM1_DSP_CODE void groove_arp_reset(unsigned clear_latch);
FM1_DSP_CODE void groove_arp_render(unsigned tempo);
FM1_DSP_CODE void groove_arp_clock(void);
FM1_DSP_CODE void groove_sequence_render(void);
FM1_DSP_CODE void groove_sequence_transport(uint8_t status);
FM1_DSP_CODE void groove_sequence_clock(void);
FM1_DSP_CODE void groove_sequence_position(unsigned position);
FM1_DSP_CODE void groove_record_midi(const uint8_t *message, size_t bytes);
FM1_DSP_CODE void groove_edit_midi_changed(void);
FM1_DSP_CODE void groove_sequence_click(int32_t *output, unsigned frames);
FM1_DSP_CODE void groove_lock_apply(unsigned track, unsigned parameter, unsigned value);
FM1_DSP_CODE void groove_lock_restore(unsigned track);
#endif
