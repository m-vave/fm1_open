/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#ifndef FM1_INSTRUMENT_H
#define FM1_INSTRUMENT_H
#include "engine.h"
#include "bass303.h"
#include "sampledrum.h"
#include "fm1.h"
#include "fm1_param.h"

enum {
    GROOVE_TRACKS = 8,
    GROOVE_STEPS = 64,
    GROOVE_VOICES = 8,
    GROOVE_303_VOICES = BASS303_VOICES,
    GROOVE_DRUMS = 11,
    GROOVE_PADS = 16,
    GROOVE_TONE_PARAMS = 12
};
enum groove_source {
    GROOVE_DX7,
    GROOVE_303,
    GROOVE_808,
    GROOVE_PSG,
    GROOVE_PLUCK,
    GROOVE_SAMPLE,
    GROOVE_SOURCES
};
/* Type changes belong to control. MIDI channels and mixer settings survive.
 * Crossing between drums and pitched sources clears incompatible pattern cells. */
FM1_DSP_CODE unsigned groove_source_type(unsigned track);
int groove_source_set(unsigned track, unsigned type);
/* Approximate instrument-wide counters; read without pausing playback. */
struct fm1_render_stats {
    uint32_t render_ticks, peak_ticks, midi_ticks, adapted;
    uint32_t overloads, voice_capacity, master_gain, deadline_ticks;
};
void groove_audio_stats(struct fm1_render_stats *stats);
struct groove_step {
    uint8_t note, velocity;
    uint16_t drums;
};
/* Melodic cells: bits 0..4 chord, 5..8 gate, 9..11 probability,
 * bit 12 tie, bit 13 alternate tuning. Drum cells retain their pad mask;
 * note holds probability.
 * Chord 0 is a single note. Gate 0 is the default 50%, 15 is a full step.
 * Probability 0 is always, 1..7 are 1/8..7/8. */
enum { GROOVE_CHORDS = 16, GROOVE_LOCKS = 16 };
struct groove_parameter_lock { uint8_t track, step, parameter, value; };
struct groove_sequence_options {
    uint8_t version, swing, metronome, count_in;
    uint8_t chord[GROOVE_CHORDS][8]; /* count, then positive offsets from root */
    struct groove_parameter_lock lock[GROOVE_LOCKS]; /* track 255: unused */
};
/* Per-track tuning: whole-cent offsets from 12-TET, -127..+127, indexed by
 * absolute pitch class (MIDI note % 12, C = 0). `main` applies normally; `alt`
 * applies to notes flagged alt: a melodic step with GROOVE_STEP_ALT (bit 13)
 * or a live note while groove_tuning_alt_hold is on. All zero is 12-TET.
 * Control task only (audio gate rules as for groove_source_set). The offset is
 * read at note-on and kept by that voice: edits affect subsequent note-ons,
 * never sounding notes. Drum sources (808, sampler) ignore it. Alt hold is
 * live state and is not saved. set returns 0, or <0 for track >= GROOVE_TRACKS,
 * a NULL pointer, or any value outside -127..+127 (-128 is reserved); an
 * invalid call leaves the current tables unchanged. */
enum { GROOVE_TUNING_KEYS = 12 };
#define GROOVE_STEP_ALT (1U << 13)
struct groove_tuning { int8_t main[GROOVE_TUNING_KEYS], alt[GROOVE_TUNING_KEYS]; };
int groove_tuning_set(unsigned track, const struct groove_tuning *t); /* 0 ok, <0 invalid */
void groove_tuning_get(unsigned track, struct groove_tuning *t);
void groove_tuning_alt_hold(unsigned track, unsigned on);
/* Whole-instrument snapshot for project storage (control task only). capture
 * copies all GROOVE_TRACKS tables and returns 0. apply validates every track
 * first; any NULL pointer or -128 entry returns <0 and changes nothing,
 * otherwise all tables are replaced and it returns 0. Neither touches alt hold. */
int groove_tuning_capture(struct groove_tuning t[GROOVE_TRACKS]);
int groove_tuning_apply(const struct groove_tuning t[GROOVE_TRACKS]);
struct groove_view {
    uint8_t type, program, volume, level, ready, playing, play_step, modified, muted, solo, audible,
        mono, clock_mode, clock_state, sequence_length, next_length;
    uint16_t tempo;
    struct groove_step step;
    uint64_t active_steps;
};
static inline unsigned groove_channel(unsigned track) { return track == 7 ? 9U : track; }

/* Roles and input channels are device/session preferences. The default is
 * eight normal tracks with the original channel map. Play ignores arrangement
 * playback and retains its instrument/held notes across load and undo. Off
 * releases its engine; its project row remains available for later restoration.
 * Mute and Solo use the same mixer controls within each role group. */
enum { GROOVE_TRACK_NORMAL, GROOVE_TRACK_PLAY, GROOVE_TRACK_OFF };
FM1_DSP_CODE unsigned groove_track_role(unsigned track);
FM1_DSP_CODE unsigned groove_midi_input_channel(unsigned track);
int groove_track_role_set(unsigned track, unsigned role);
int groove_midi_input_set(unsigned track, unsigned channel);
void groove_edit_forget_history(void);
void groove_edit_track_reset(unsigned track); /* control-only, render gate held */

/* Live arpeggiator / drum repeat: rate, direction, range, gate, swing, hold.
 * internal tempo also runs while stopped, external clock follows transport.
 * Settings and enable are stored per project. Held notes are never stored. */
unsigned groove_arp_enabled(unsigned track);
void groove_arp_set(unsigned track, unsigned enabled);
unsigned groove_arp_setting(unsigned track, unsigned parameter);
void groove_arp_edit(unsigned track, unsigned parameter, int delta);
/* Clock mode is a device setting. Switching modes stops playback. MIDI
 * mode advances only on received F8; Start/Continue wait for the next tick. */
enum { GROOVE_CLOCK_INTERNAL, GROOVE_CLOCK_MIDI };
enum { GROOVE_CLOCK_WAIT, GROOVE_CLOCK_LOCKED, GROOVE_CLOCK_LOST };
FM1_DSP_CODE unsigned groove_clock_mode(void);
void groove_clock_set(unsigned mode);
/* Initialize before audio starts. Sequence edits and views belong to the
 * single control callback; unchanged edits do not pause rendering. */
void groove_sequence_init(void);
void groove_sequence_edit(unsigned track, unsigned step, unsigned note, unsigned velocity);
/* Each track loops over 16, 32, 48 or 64 steps. During playback a request is
 * applied at that track's next loop start; stopped tracks change immediately.
 * Shortening keeps hidden cells. The getter returns the requested length.
 * Control callback only; a project stores the requested length. */
unsigned groove_sequence_length(unsigned track);
void groove_sequence_length_set(unsigned track, unsigned steps);
void groove_tempo_edit(int delta);
/* Control task only. Tap returns BPM, 0 while arming, or -1 for MIDI clock.
 * Millisecond timestamps wrap naturally. Current is zero without MIDI lock. */
int groove_tempo_tap(uint32_t now);
unsigned groove_tempo_current(void);
unsigned groove_tempo_saved(void);
void groove_screen_tap(uint32_t now);
void groove_sequence_view(unsigned track, unsigned step, struct groove_view *view);
uint64_t groove_drum_steps(unsigned track, unsigned drum);
/* Called with playback suspended, after changing the source. */
void groove_sequence_source_changed(unsigned track);
void groove_get_view(unsigned track, unsigned step, struct groove_view *view);
void groove_control_edit(unsigned track, int program_steps, int volume_steps, int cutoff_steps);
/* Physical MASTER is global and is not stored in a project. Invalid ADC
 * readings are ignored; return nonzero for a change after initialization. */
int groove_master_update(unsigned adc);
uint32_t groove_master_state(void); /* ADC in bits 0..15, level 0..127 in 16..23. */
void groove_mix_toggle(unsigned track, int solo);
/* 303, PSG and pluck use part0; 808 uses parts0..10 and part11 for kit
 * controls. The sampler uses pads0..7, each with six independent
 * parameters. All 808 pan/FX sends follow the engine descriptors.
 * Values are 0..max pot positions, not Hz or milliseconds. Like
 * FM editing, copy/edit belong to the control callback; unchanged copies take
 * no gate. */
unsigned groove_tone_count(unsigned track, unsigned part);
const fm1_param_t *groove_tone_parameter(unsigned track, unsigned part, unsigned parameter);
int groove_tone_copy(unsigned track, unsigned part, uint8_t values[GROOVE_TONE_PARAMS]);
void groove_tone_edit(unsigned track, unsigned part, unsigned parameter, int delta);
/* Expanded DX7 parameter offsets. Operator 1 starts at 105, operator 6 at 0.
 * Patch edits affect subsequent note-ons; LFO rate/wave edits also reset its
 * phase. Call only from the control callback. Writes and snapshot refreshes
 * hold the audio gate; reading an unchanged snapshot does not pause rendering.
 */
struct groove_fm_controls {
    /* FM: ADSR plus bypass. Other engines: enabled marks stored LFO
     * rate/depth (0..100), target (volume/pan), wave (triangle/square/up/down).
     * enabled=0 preserves legacy projects and means an unmodulated track.
     * The existing seven-byte layout and 303 mono flag are unchanged. */
    uint8_t envelope[4], enabled, operators, mono;
};
void groove_lfo_copy(unsigned track, uint8_t values[4]);
void groove_lfo_edit(unsigned track, unsigned parameter, int delta);
unsigned groove_lfo_rate(unsigned value); /* Hundredths of Hz, 5..2000. */
/* Version-4 little-endian project: values only, never DSP state or pointers.
 * A full eight-track snapshot fits one 4 KiB Flash sector. */
struct groove_preset {
    uint8_t type, volume, program, modified;
    struct groove_fm_controls controls;
    uint8_t data[210]; /* Byte 209: track reverb/delay sends, low/high nibble (0..15). */
    uint8_t sequence_pages; /* 0 = 64 steps; 1/2/3 = 16/32/48. */
    uint8_t sampler_pads;   /* 8 for sampler presets, zero for other sources. */
};
_Static_assert(sizeof(struct groove_preset) == 223, "preset layout");
struct groove_project {
    struct groove_step step[GROOVE_TRACKS][GROOVE_STEPS];
    struct groove_preset preset[GROOVE_TRACKS];
    uint16_t tempo;
    uint8_t mute, solo;
    char name[12]; /* Space-padded printable ASCII, no terminator. */
    uint8_t effects[4]; /* Reverb decay, delay time/feedback, return level. */
    struct groove_sequence_options sequence;
    /* Per track: rate/direction/range/gate (two bits each); swing (0..50),
     * Hold (bit 6), enable (bit 7). No held notes or playback state. */
    uint8_t arp[GROOVE_TRACKS][2];
};
_Static_assert(sizeof(struct groove_project) == 4064, "project v4 layout");
/* Arrangement snapshots include dormant rows; raw capture still exports the
 * actual Play timbre. Tuning follows the same selective transaction boundary. */
int groove_arrangement_capture(struct groove_project *p);
int groove_arrangement_tuning_capture(struct groove_tuning t[GROOVE_TRACKS]);
int groove_arrangement_tuning_apply(const struct groove_tuning t[GROOVE_TRACKS]);

enum { GROOVE_PROJECT_SLOTS = 16, GROOVE_PROJECT_BYTES = 4064,
       GROOVE_PROJECT_LEGACY_BYTES = 4048 };
/* Optional tuning block after an exported/imported project: magic 'FM1T'
 * (0x54314d46, little-endian), 8 x 24 B tables, then ~CRC-32 of the first 196
 * bytes. Exports append it when the capacity allows; imports accept either size. */
enum { GROOVE_TUNING_BLOCK_BYTES = 200,
       GROOVE_PROJECT_TUNED_BYTES = GROOVE_PROJECT_BYTES + GROOVE_TUNING_BLOCK_BYTES };
void groove_project_arp_defaults(struct groove_project *p);
enum { GROOVE_PROJECT_EMPTY = 1, GROOVE_PROJECT_INVALID = -7,
       GROOVE_PROJECT_MEMORY = -8, GROOVE_PROJECT_TUNING_SPACE = -9 };
/* Control only. Apply reuses matching engines and stages only missing
 * instances. A validation/allocation failure leaves the current project
 * untouched. */
int groove_project_capture(struct groove_project *p);
int groove_project_valid(const struct groove_project *p);
int groove_project_apply(const struct groove_project *p);
/* Open engine: checkpoint only after allocation succeeds, before applying.
 * Optional on binary SDKs; callers linking those must use a weak reference. */
int groove_project_apply_edit(const struct groove_project *p, unsigned operation);
int groove_timbre_apply(unsigned track, const struct groove_project *p);
void groove_sequence_copy(struct groove_project *p);          /* gate already held */
void groove_sequence_restore(const struct groove_project *p); /* gate already held */
void groove_project_init(void);
/* Incremental CRC-32: start with ~0U and complement the final result. */
uint32_t groove_crc32(uint32_t crc, const void *data, unsigned bytes);
void groove_project_poll(void);
unsigned groove_project_slot(void);
uint32_t groove_project_revision(void);
int groove_project_save_slot(unsigned slot);
int groove_project_load_slot(unsigned slot);
int groove_project_info(unsigned slot, char name[13]);
void groove_project_name(char name[13]);
void groove_project_rename(unsigned character, char value);
void groove_project_metadata(const struct groove_project *p); /* audio gate held */
/* File transfers use the current working project, without writing Flash. */
int groove_project_import(const uint8_t *bytes, unsigned length);
int groove_project_export(uint8_t *bytes, unsigned capacity);
/* Read-only factory projects. Building only writes the caller's
 * snapshot; loading changes RAM and supports undo, never writes user slots. */
enum { GROOVE_FACTORY_PROJECTS = 16 };
const char *groove_factory_name(unsigned index);
int groove_factory_build(unsigned index, struct groove_project *project);
int groove_factory_load(unsigned index);
/* Bluetooth master preserves audio preference and MIDI role while disabled. */
enum { GROOVE_RADIO_ON = 1, GROOVE_RADIO_AUDIO = 2, GROOVE_RADIO_MIDI = 4,
       GROOVE_RADIO_ALL = 7 };
unsigned groove_settings_radio_flags(void);
unsigned groove_settings_role_value(void);
int groove_settings_radio(unsigned flags, unsigned role);
void groove_settings_clock(unsigned mode);
int groove_settings_status(void);
void groove_transfer_poll(void);
void groove_transfer_init(void);
int groove_fm_import(unsigned track, const uint8_t patch[156]);
void groove_fm_name_edit(unsigned track, unsigned character, char value);
/* Delay byte: 0..127 keeps all legacy times; 128..136 selects
 * 1/32T, 1/32, 1/16T, 1/16, 1/8T, 1/16D, 1/8, 1/8D, 1/4.
 * FREE 137..201 = 360..1000 ms in 10 ms steps; 202..253 = 1020..2040 ms
 * in 20 ms steps. 254/255 remain invalid. The project layout is unchanged.
 * Edit parameter 4 toggles SYNC; conversion selects the nearest time.
 * Parameters 0..3 retain their meaning. New projects default to 1/8. */
enum { GROOVE_DELAY_SYNC = 128, GROOVE_DELAY_LAST = 136,
       GROOVE_DELAY_FREE_LAST = 253, GROOVE_DELAY_DEFAULT = 134 };
static inline unsigned groove_delay_synced(unsigned value)
{ return value >= GROOVE_DELAY_SYNC && value <= GROOVE_DELAY_LAST; }
void groove_effects_get(uint8_t values[4]);
unsigned groove_delay_ticks(void); /* 44.1 kHz / 8, applied delay time */
void groove_sends_get(unsigned track, uint8_t values[2]);
void groove_sends_edit(unsigned track, unsigned parameter, int delta);
void groove_effects_edit(unsigned parameter, int delta);
void groove_effects_restore(const uint8_t values[4]); /* audio gate held */
int groove_fm_copy(unsigned track, uint8_t patch[156], struct groove_fm_controls *controls);
void groove_fm_edit(unsigned track, unsigned parameter, int delta);
void groove_adsr_edit(unsigned track, unsigned parameter, int delta);
void groove_adsr_enable(unsigned track, int enabled);
void groove_operator_toggle(unsigned track, unsigned op);
void groove_mono_set(unsigned track, unsigned mono);
unsigned groove_key_note(unsigned track, unsigned key, int octave);
/* Zero pads means a melodic source. Unmapped drum notes return128/-1. */
FM1_DSP_CODE unsigned groove_pad_count(unsigned track);
FM1_DSP_CODE unsigned groove_pad_note(unsigned track, unsigned pad);
FM1_DSP_CODE int groove_pad_index(unsigned track, unsigned note);
const char *groove_pad_name(unsigned track, unsigned pad);
const char *groove_pad_label(unsigned track, unsigned pad);
unsigned groove_drum_note(unsigned drum);
int groove_drum_index(unsigned note);
void groove_screen_init(void);
void groove_screen_control(void);
void groove_screen_mirror_hello(unsigned all); /* SysEx hook: keep mirroring the screen, on the port that asked. */

/* Control-side editing. Undo swaps one complete snapshot; no audio allocation. */
void groove_edit_init(void);
void groove_edit_poll(void);
void groove_edit_checkpoint(unsigned operation);
int groove_edit_undo(void);
unsigned groove_project_dirty(void);
uint32_t groove_edit_revision(void);
void groove_project_clean(void);
void groove_project_touch(void);
void groove_edit_record(unsigned enabled, unsigned track, unsigned step);
void groove_edit_seek(unsigned track, unsigned step);
void groove_screen_mmc(unsigned command); /* Control thread, validated MMC. */
FM1_DSP_CODE unsigned groove_edit_recording(void);
unsigned groove_edit_cursor(void);
unsigned groove_edit_error(void); /* 1 input overflow, 2 chord table/voice limit */
/* The instrument reports persistent MIDI edits; transient controllers stay clean. */
unsigned groove_midi_activity(unsigned track);
/* scope: 0 one step, 1 sixteen-step page, 2 whole track. */
int groove_edit_copy(unsigned track, unsigned step, unsigned scope);
unsigned groove_edit_clipboard_count(void);
int groove_edit_paste(unsigned track, unsigned step, unsigned scope);
int groove_edit_clear(unsigned track, unsigned step, unsigned scope);
int groove_edit_repeat(unsigned track, unsigned steps);
int groove_timbre_export(unsigned track, uint8_t *data, unsigned capacity);
int groove_timbre_import(unsigned track, const uint8_t *data, unsigned bytes);
void groove_sequence_defaults(struct groove_sequence_options *options);
int groove_sequence_valid(const struct groove_project *project);
unsigned groove_step_setting(unsigned track, unsigned step, unsigned setting);
int groove_step_setting_edit(unsigned track, unsigned step, unsigned setting, int delta);
unsigned groove_sequence_setting(unsigned setting);
const char *groove_sequence_chord_name(unsigned id);
void groove_sequence_setting_edit(unsigned setting, int delta);
int groove_sequence_chord(unsigned track, unsigned step, const uint8_t *notes, unsigned count, unsigned velocity);
void groove_sequence_replace(unsigned track, unsigned first, const struct groove_step *cells, unsigned count);
void groove_sequence_read(unsigned track, unsigned first, struct groove_step *cells, unsigned count);
void groove_sequence_options_copy(struct groove_sequence_options *options);
void groove_sequence_options_set(const struct groove_sequence_options *options);
unsigned groove_sequence_count_in(void);
unsigned groove_sequence_rewound(void);
FM1_DSP_CODE unsigned groove_sequence_record_position(unsigned track);
void groove_sequence_keep_transport(unsigned restore);
unsigned groove_lock_parameter_count(unsigned track);
const char *groove_lock_parameter_name(unsigned track, unsigned parameter);
struct groove_project *groove_project_scratch(void); /* control-only, transient */
/* Project sectors stay at 0..68 KiB; a single shared 456 KiB sample bank follows. */
enum { GROOVE_SAMPLE_OFFSET = 69632, GROOVE_SAMPLE_BYTES = 466944,
       GROOVE_FLASH_BYTES = GROOVE_SAMPLE_OFFSET + GROOVE_SAMPLE_BYTES };
/* Five final sectors hold the four DX7 banks and tuning/session preferences.
 * New sample transfers stop before them. Larger legacy banks stay readable;
 * writes to overlapping extension storage are rejected before erasing Flash. */
enum { GROOVE_TUNING_OFFSET = GROOVE_FLASH_BYTES - 4096,
       GROOVE_LIBRARY_OFFSET = GROOVE_TUNING_OFFSET - 4 * 4096,
       GROOVE_SAMPLE_LIMIT = GROOVE_LIBRARY_OFFSET - GROOVE_SAMPLE_OFFSET };
/* Finish any committed shared journal before starting a new transaction. */
int groove_storage_prepare(void);
void groove_samples_init(void);
void groove_samples_poll(void);
void groove_samples_stop(void); /* gate already held */
unsigned groove_samples_status(void); /* 0 empty, 1 validating, 2 ready, 3 receiving, 4 error */
unsigned groove_samples_size(void);
int groove_samples_begin(unsigned bytes);
int groove_samples_write(unsigned offset, const uint8_t *data, unsigned bytes);
int groove_samples_finish(void);
void groove_samples_cancel(void);
int groove_samples_read(unsigned offset, uint8_t *data, unsigned bytes);

/* MIDI performance routing. Device preferences occupy eight bytes and are
 * independent of project/undo data. Modes: 0 local, 1 local+MIDI, 2 MIDI only.
 * Settings: mode, output channel, output ports, Local Control, live output,
 * clock output, clock source (AUTO/USB/BLE/DIN), Thru source, Thru destinations. */
struct groove_midi_config { uint8_t channel[4], mode[2], flags, thru; };
void groove_midi_defaults(struct groove_midi_config *config);
int groove_midi_config_valid(const struct groove_midi_config *config);
void groove_midi_config_read(struct groove_midi_config *config);
void groove_midi_config_apply(const struct groove_midi_config *config);
void groove_midi_init(void);
void groove_midi_clock_reset(void);
void groove_midi_poll(void);
void groove_midi_panic(void);
int groove_midi_local(const uint8_t *message, size_t bytes);
void groove_midi_fader(unsigned track, unsigned value);
unsigned groove_midi_setting(unsigned track, unsigned parameter);
void groove_settings_midi(unsigned track, unsigned parameter, int delta);
/* parameter 0 role, 1 input channel. Changes retain all eight project rows. */
int groove_settings_track(unsigned track, unsigned parameter, int delta);
int groove_midi_clock_accept(unsigned source, uint8_t status);
uint32_t groove_midi_dropped(void);
FM1_DSP_CODE unsigned groove_midi_mode(unsigned track);
FM1_DSP_CODE void groove_midi_note(unsigned track, unsigned note, unsigned velocity);
FM1_DSP_CODE void groove_midi_track_reset(unsigned track);
FM1_DSP_CODE void groove_midi_transport(unsigned owner, uint8_t status);
FM1_DSP_CODE void groove_midi_clock(unsigned owner, unsigned tempo);
#endif
