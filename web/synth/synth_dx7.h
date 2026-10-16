/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#ifndef APP_SYNTH_DX7_H
#define APP_SYNTH_DX7_H

#include "engine.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    SYNTH_DX7_EXPANDED_PATCH_BYTES = 156,
    SYNTH_DX7_BLOCK_SAMPLES = 64 /* Internal FM control quantum. */
};

struct synth_dx7;
typedef struct synth_dx7 synth_dx7_t;

/* The instance owns all audio-block scratch storage. No allocation occurs
 * in synth_dx7_render() or in the MIDI event path. Create/destroy instances
 * before audio starts, or while rendering is paused. All live
 * instances share the same sample rate. Use FM1_SAMPLE_RATE and FM1_BLOCK_FRAMES
 * on this board (128 frames); 64-frame internal-quantum calls are also valid.
 * Unsupported rates or block sizes return NULL; rates that
 * overflow the fixed-point frequency table are rejected during initialization. */
synth_dx7_t *synth_dx7_create(uint8_t max_voices,
                              uint32_t sample_rate,
                              uint16_t block_samples);
void synth_dx7_destroy(synth_dx7_t *synth);
/* Restore creation defaults without allocation. Keeps the instance's voice
 * count, sample rate and lookup-table ownership. Stops notes and clears all
 * controls/workspaces; callers then apply their patch and settings. As with
 * create/destroy, suspend audio before calling on a live instance. */
void synth_dx7_reset(synth_dx7_t *synth);

/* One complete channel message: 2 bytes for Cn/Dn, otherwise 3; data is 7-bit.
 * Invalid messages or note-on patches return -1 before modifying voice state. */
int synth_dx7_midi(synth_dx7_t *synth,
                   uint8_t expanded_patch[SYNTH_DX7_EXPANDED_PATCH_BYTES],
                   const uint8_t *message,
                   size_t message_bytes);
/* As synth_dx7_midi; a note-on sounds `cents` (whole cents, signed) from 12-TET. */
int synth_dx7_midi_tuned(synth_dx7_t *synth,
                         uint8_t expanded_patch[SYNTH_DX7_EXPANDED_PATCH_BYTES],
                         const uint8_t *message,
                         size_t message_bytes,
                         int cents);
int synth_dx7_lfo_reset(
    synth_dx7_t *synth,
    const uint8_t expanded_patch[SYNTH_DX7_EXPANDED_PATCH_BYTES]);

/* Adds one mono Q24 block to output; clear the destination before rendering
 * unless intentionally accumulating voices. samples must equal the block
 * size passed to synth_dx7_create(). */
int synth_dx7_render(synth_dx7_t *synth, int32_t *output, size_t samples);

void synth_dx7_set_poly_mode(synth_dx7_t *synth, uint8_t mono);
uint8_t synth_dx7_is_mono(const synth_dx7_t *synth);
/* Releases every live voice without allocating memory. */
void synth_dx7_all_notes_off(synth_dx7_t *synth);
void synth_dx7_set_pitch_bend_range(synth_dx7_t *synth,
                                    uint8_t semitones_up,
                                    uint8_t semitones_down);
void synth_dx7_set_glide(synth_dx7_t *synth,
                         uint8_t amount,
                         uint8_t fingered);
void synth_dx7_set_operator_mask(synth_dx7_t *synth, uint8_t mask);
void synth_dx7_set_adsr(synth_dx7_t *synth,
                        uint8_t enabled,
                        uint8_t attack,
                        uint8_t decay,
                        uint8_t sustain,
                        uint8_t release);

/* Audio owner only. 1..allocated capacity; released voices retire first.
 * Returns the number retired. Future notes obey the limit; reset restores capacity. */
FM1_DSP_CODE unsigned synth_dx7_limit(synth_dx7_t *synth, unsigned voices);
/* Audio owner only: currently sounding voices, including release tails. */
FM1_DSP_CODE unsigned synth_dx7_active(const synth_dx7_t *synth);

/* Immediate bounded reset, including pedal, controller and DC state. */
void fm1_dx7_panic(synth_dx7_t *synth);

#ifdef __cplusplus
}
#endif

#endif
