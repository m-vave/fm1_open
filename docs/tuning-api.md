# Per-track tuning API

## Purpose

The engine plays every note in 12-tone equal temperament (12-TET). This note
describes a small hook that lets firmware retune each track: 12 signed cent
offsets per track, applied when a note sounds. Quarter tones, just or historical
intonation and other scales then work for panel keys, the sequencer, chords, the
arpeggiator and external MIDI, with no change to how notes are stored. A second
"alt" table, selected per step or by a live hold, lets a note step outside the
track's tuning.

All-zero tables reproduce today's output bit for bit.

## Behaviour

- **Tables.** Each of the 8 tracks has `main[12]` and `alt[12]`, indexed by
  absolute pitch class (`MIDI note % 12`, C = 0). Entries are `int8_t` whole
  cents from 12-TET, range -127..+127. -128 is reserved and rejected.
- **Main vs alt.** Both tables are relative to 12-TET, never to each other. A
  note takes `main[note % 12]` normally and `alt[note % 12]` when flagged alt.
  The offset applies in every octave.
- **Step flag.** Melodic step cells use bit 13 (`GROOVE_STEP_ALT`, `1U << 13`)
  as the alt flag. Every note of the step, including all chord notes, takes its
  offset from the alt table. Validation accepts bit 13 on melodic cells and still
  rejects bits 14 and 15; drum cell validation is unchanged. Copy, paste, repeat,
  undo and redo carry the bit; clearing a step clears it.
- **Live alt hold.** `groove_tuning_alt_hold(track, on)` makes live note-ons on
  that track use the alt table while on. Sequenced steps ignore alt hold; they
  use their own bit 13. Alt hold is live state and is not saved.
- **Recording.** A live note recorded while alt hold is on gets bit 13 set;
  recorded with hold off, the bit is clear. Overdubbing replaces the flag with
  the note. Drum tracks never get the flag.
- **Arpeggiator.** Each held key remembers whether it was pressed with alt hold
  on. Every arpeggiated note from that key uses the alt table; releasing alt hold
  later does not change it. Two held keys can use different tables.
- **Tuned sources.** DX7, 303, PSG and Pluck. The 808 and sampler ignore the
  tables (their note values address pads); their output is bit-identical with
  non-zero tables.
- **Note-on capture.** The offset is resolved once, at note-on, and stored with
  the voice. Editing a table, or toggling alt hold, affects later note-ons only;
  a sounding note keeps its pitch. A 303 mono re-trigger after a release reuses
  the offset captured by that note.
- **Stacking.** The offset is added to the existing pitch terms and replaces none
  of them: pitch bend, engine TUNE, patch transpose, operator ratio and detune,
  LFO and pitch envelope, glide. Glide and 303 slide targets include the offset,
  so they settle on the tuned pitch. A note tuned -50 and bent +100 sounds at +50
  relative to 12-TET.
- **Resolution.** Q16-semitone engines (303, Pluck, PSG) add
  `cents * 16384 / 25` (exact integer for `cents * 65536 / 100`); DX7 adds
  `cents * (1 << 24) / 1200` to its log2(Hz) Q24 note frequency. PSG pitch is an
  integer chip period, so its result is quantised by the chip, not by the table.

## API reference

Declarations from `app/instrument.h`:

```c
enum { GROOVE_TUNING_KEYS = 12 };
#define GROOVE_STEP_ALT (1U << 13)
struct groove_tuning { int8_t main[GROOVE_TUNING_KEYS], alt[GROOVE_TUNING_KEYS]; };
int groove_tuning_set(unsigned track, const struct groove_tuning *t); /* 0 ok, <0 invalid */
void groove_tuning_get(unsigned track, struct groove_tuning *t);
void groove_tuning_alt_hold(unsigned track, unsigned on);
int groove_tuning_capture(struct groove_tuning t[GROOVE_TRACKS]);
int groove_tuning_apply(const struct groove_tuning t[GROOVE_TRACKS]);
```

- `groove_tuning_set` returns 0, or <0 for `track >= GROOVE_TRACKS`, a NULL
  pointer, or any of the 24 values equal to -128. An invalid call leaves the
  current tables unchanged.
- `groove_tuning_get` copies the track's tables out; an out-of-range track yields
  zeroed tables.
- `groove_tuning_alt_hold` sets or clears live alt hold for a track (any nonzero
  `on` means on); out-of-range tracks are ignored.
- `groove_tuning_capture` copies all `GROOVE_TRACKS` tables and returns 0 (<0 for
  NULL). `groove_tuning_apply` validates every track first; a NULL pointer or any
  -128 entry returns <0 and changes nothing, otherwise all tables are replaced.
  Neither touches alt hold. They exist so storage and transfer code can save and
  move tuning as one unit.

**Thread contract.** All of these are control-task functions, under the same
rules as the existing patch edits such as `groove_source_set`. The audio task
reads the tables and alt hold; do not call these from it. Changes take effect at
the next note-on.

## Detecting the hook (weak symbols)

Firmware that must build against engines with and without the hook redeclares
the whole-instrument calls weak (`app/tuning_hook.h`):

```c
extern int groove_tuning_capture(struct groove_tuning t[GROOVE_TRACKS]) __attribute__((weak));
extern int groove_tuning_apply(const struct groove_tuning t[GROOVE_TRACKS]) __attribute__((weak));
```

On an engine without the hook these resolve to null; test the address before
calling (`if (groove_tuning_apply) ...`). The same applies to
`groove_tuning_set`, `groove_tuning_get` and `groove_tuning_alt_hold` if
firmware calls them directly. Without the hook, firmware behaves as 12-TET.
Linking a host binary without `web/engine/tuning.c` leaves these as null weak
references; on macOS that needs `-Wl,-undefined,dynamic_lookup`, while ELF
targets resolve them to 0.

## Versioning

- The open mirror's `FM1_SYNTH_API` (`third_party/synth/engine.h`) goes from 3 to
  4, and `web/engine/sound.c` asserts it.
- The proposed native target is `FM1_SYNTH_API` 7, since the shipped firmware's
  `app/synth/engine.h` is at 6. That header describes the shipped binary and is
  unchanged until an engine with the hook ships.
- Firmware can compare the version, or use the weak-symbol check above.
- `struct groove_project` is unchanged (still 4064 bytes), so firmware built
  against engines without the hook keeps building and loading projects.

## Storage

Tuning is stored outside the project record, because the project structure and
its 4088-byte flash record are full.

**Tuning sector.** The last 4 KiB sector of user flash,
`GROOVE_TUNING_OFFSET = GROOVE_FLASH_BYTES - 4096`. Layout (`app/project.c`):

```
struct tuning_entry  { uint32_t generation; struct groove_tuning t[8]; uint32_t crc; }; /* 200 B */
struct tuning_sector { uint32_t magic, version; struct tuning_entry entry[16]; };
```

- `magic` is 0x54314d46 (`FM1T`, little-endian), `version` is 1.
- One entry per project slot (`GROOVE_PROJECT_SLOTS` = 16): the slot's project
  record generation, the 8 x 24 byte tables, and a CRC.
- The entry CRC is the CRC-32 (`groove_crc32`, final inversion) of the
  generation, the tables, and the CRC of the project record the entry belongs
  to. An entry is therefore valid only for the exact record it was saved with.
- **Save.** After a project record commits, its slot entry is written
  (best effort; a failure does not fail the project save).
- **Load.** The entry is applied only if its generation and CRC match the loaded
  record; otherwise the tables are zeroed. Projects saved before this change,
  factory projects, and slots replaced since the entry was written all load
  with zero tables.
- **Atomicity.** The sector is rewritten with erase then write, which is not
  atomic. A power loss can lose saved tables for every slot, but each entry
  has its own CRC and the projects themselves are unaffected.

**Sample bank cap.** The sector is taken from the end of the sample bank.
`GROOVE_SAMPLE_LIMIT = GROOVE_SAMPLE_BYTES - 4096 = 462,848` bytes is the largest
bank accepted for new transfers (`groove_samples_begin` in `app/samples.c`).
SysEx command 11 (`app/transfer.c`) still reports the full region, 466,944 bytes,
because the shipped SDK clients refuse any other value; a larger bank is rejected
when its transfer begins. `GROOVE_SAMPLE_BYTES` (466,944) and the other shared
constants are unchanged.

**Over-size banks.** A bank stored before the cap and larger than 462,848 bytes
still validates and reads. Because it overlaps the tuning sector, tuning is
neither saved nor loaded while it is present (loads give zero tables), and the
bank is left alone until a smaller one is sent. Project saves are unaffected.

**SysEx wire change** (`app/transfer.c`, `app/project.c`). Project transfer kind 3
is the project plus an optional tuning block, 4264 bytes
(`GROOVE_PROJECT_TUNED_BYTES` = `GROOVE_PROJECT_BYTES` 4064 + `GROOVE_TUNING_BLOCK_BYTES` 200).

- Block (`struct tuning_block`): `uint32_t magic` (0x54314d46, `FM1T`, little-endian),
  8 x `struct groove_tuning` (24 B each, `main[12]` then `alt[12]`), `uint32_t crc`.
  `crc` is `~groove_crc32(~0, block, 196)`, i.e. over the first 196 bytes.
- Export: kind 3 read returns 4264 bytes; `groove_project_export` appends the
  block whenever capacity allows it, with zero tables if the engine lacks the
  hook. Kind 0 stays 4064 B.
- Import: kind 3 write requires 4264 bytes; kind 0 write also accepts 4264 or the
  existing 4064 / 4048 byte sizes. A block-less import sets all tables to zero. A
  bad block magic, bad CRC or any -128 entry rejects the whole import.

## Notes for the native port

The mirror's function names match the native engine, so these map one-to-one:

- **Lookup.** `web/engine/sound.c`: `groove_note_ex` resolves
  `(alt ? tuning[track].alt : tuning[track].main)[note % 12]` once and passes it
  to `engine_note`; `groove_note` wraps it and takes alt from alt hold for live
  notes (`sequenced == 0`). The tables and `groove_tuning_set/get/alt_hold` live
  in the same file; `groove_tuning_capture/apply` are in `web/engine/tuning.c`.
  Note-offs pass 0 cents. Drum sources are not given an offset.
- **DX7.** `web/synth/dx7note.c` `dx7note_init`: `notelogfreq` gains
  `cents * (1 << 24) / 1200`. `web/synth/dx7synth.c`: `synth_dx7_midi_tuned`
  carries the offset to `dx7synth_keydown`; `glide_apply` includes it in the
  glide source and target (`last_cents`).
- **303.** `third_party/synth/bass303.c`: `bass303_note_on` stores
  `b->tune = cents * 16384 / 25` and `pitch_target` adds `b->tune` to the bend
  term. `web/synth/voices.c`: `bass303_bank_note` passes the offset and keeps it
  per note for mono re-triggers (`struct bass_voice.cents`).
- **Pluck.** `third_party/synth/pluck.c`: `pluck_note` stores a per-voice
  `tune` added to the Q16 pitch term.
- **PSG.** `web/synth/psg.c`: `psg_note` stores a per-voice `tune` added in the
  pitch computation (`(note + pot - 24) * 65536 + bend + tune`).
- **Sequencer.** `web/engine/sequencer.c`: melodic validation mask `0xe000` to
  `0xc000`; `sequence_event` calls `groove_note_ex` with `(drums >> 13) & 1` for
  every chord note. Recording: `web/engine/edit.c` queues `groove_alt_held(track)`
  with each recorded event and writes it with `groove_sequence_alt`.
- **Arpeggiator.** Each held key stores its alt bit next to its velocity (bit 7 of
  the per-key velocity byte); `arp_next` passes it to `groove_note_ex`. The native
  engine needs one extra bit per held key.
- **Golden equivalence.** The requirement is that with all-zero tables every
  output sample is bit-identical to an engine without the hook. In the host tests,
  all 16 factory demos are rendered and compared with hashes recorded before any
  tuning change. Any port should be checked against its own pre-change renders
  the same way.
- **Hardware sample rate.** `app/board/fm1_audio.h` declares
  `FM1_AUDIO_HARDWARE_RATE 44118U`, while the DSP is written for 44100 Hz. If the
  DAC really runs at 44118 Hz, every pitch is about +0.7 cent sharp. This is
  inferred from the declaration and has not been measured. The host pitch tests
  run at the DSP rate (44100) and cannot see it.
- **SDK client limits.** The sample-bank size is hard-coded in the web client
  files shipped under `sdk/`, which this change does not modify. Until M-VAVE
  updates them, the device keeps reporting 466,944 and a client may attempt a
  bank the device then refuses (between 462,849 and 466,944 bytes):
  - `sdk/Samples.js:4` `CAPACITY=466944`. Line 46 requires the device's reported
    capacity (SysEx command 11, bytes 5..8) to equal `CAPACITY`; this is why the
    device does not report the reduced cap.
  - `sdk/WebUpdate.js` around lines 605 and 611: size checks against 466944.
  - `sdk/Simulator.js` lines 65-66: length check and flash window of 466944.
- **Storage freedom.** Only the API and behaviour are specified. The native engine
  may keep tables anywhere, including a larger project format in a future SDK,
  without changing callers.

## Testing

Host tests build the open mirror natively (`tests/host/README.md` has details):

```
tests/host/build.sh                  # builds build/host/* with /usr/bin/clang
tests/host/check_golden.sh           # expect: golden: all 16 demos match
build/host/fm1_host pitch-test       # untuned A4 per pitched engine
build/host/tuning_test               # tables, API, per-engine pitch, stacking, glide
build/host/alt_test                  # step alt flag validation and edit operations
build/host/step_alt_test             # alt steps, chords, recording, arpeggiator
build/host/storage_test              # capture/apply, sample cap, tuning sector, SysEx
```

`check_golden.sh --record` rewrites the golden hashes and should only be used on
an unmodified tree.
