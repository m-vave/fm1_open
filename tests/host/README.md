# Host test harness

Native build of the open engine mirror (`web/engine`, `web/synth`,
`third_party/synth`, `app/*.c`) with the browser platform adapter
`web/engine/Simulator.c`, plus an offline driver (`render.c`). Sources, include
order and defines match `tools/Build-Web.ps1`; no engine file is modified.

    tests/host/build.sh                 # -> build/host/fm1_host (CC=/usr/bin/clang)
    tests/host/check_golden.sh          # re-render 16 demos, compare to golden/demos.sha256
    tests/host/check_golden.sh --record # rewrite the golden hashes (only on purpose)
    build/host/fm1_host pitch-test      # A4 per pitched engine (exit 1 on failure)
    build/host/tuning_test              # per-track tuning tables (exit 1 on failure)
    build/host/fm1_host pitch <src> <note>        # src: 0 DX7, 1 303, 3 PSG, 4 PLUCK
    build/host/fm1_host render <0..15> [blocks] [out.f32|-]  # float32 stereo PCM

Demos render 2756 blocks of 128 frames (about 8 s) after a MIDI Start. The
engine takes no host time or randomness, so output is deterministic for a given
compiler and CPU. Golden hashes cover this platform's float behaviour
(clang, `-O2 -ffp-contract=off`); re-record them elsewhere only on the
unmodified tree.

## Pitch helper

`measure_hz(source, note)` and `measure_cents(source, note)` (in `render.c`)
boot a clean engine, set track 0 to `source` with default patch and zero sends,
hold `note`, and estimate the fundamental from the left channel by normalised
autocorrelation: coarse lag, then repeated lag doubling with parabolic
interpolation, so the interpolation error shrinks by the final multiple.
The 303 is set to a stationary timbre (cutoff max, reso 0, env mod 0) so the
filter sweep does not bias the estimate. TUNE (303), TUNE (PSG) and Pluck
default pots are at their neutral values after boot.

Measured A4 (cents from 440 Hz): DX7 +0.21, 303 -0.09, PLUCK +0.05, PSG +1.56.
PSG is structural: the chip pitch is an integer period (1789772/(16*440) =
254.23 -> 254), giving +1.56 c at A4, so its limit is 2 c and the result must
also match that prediction within 0.5 c. DX7 (default patch, program 0) measures +0.14..+0.33 c over notes 36..84,
a small constant engine offset.

## Tuning test

`tuning_test` (built by `build.sh` like every `tests/host/*_test.c`) covers the
`groove_tuning_*` API (round trip, track 8 and -128 rejected with old tables
kept, boot reset), drum tracks rendering bit-identically with non-zero tables,
tuned pitch per engine as a delta from the untuned measurement (DX7, 303, PLUCK
within 1 c; PSG within 0.5 c of the prediction from its integer chip period),
alt hold / `groove_note_ex` alt selection, stacking with pitch bend, a 303 mono
slide and a DX7 glide settling on the tuned target, and offsets captured at
note-on (a table edit leaves a held note alone; the next note-on uses it). The
DX7 glide is driven on a bare `synth_dx7` because the engine never sets a glide
amount. Measurement code is duplicated from `render.c` (static, so test
binaries stay independent).

## Step alt test

`step_alt_test` includes `tuning_test.c` (with `TUNING_NO_MAIN`) to reuse its
measurement helpers. Sequenced tied steps on every pitched engine measure each
of C4/E4/G4 at the main or alt entry (alt hold has no effect on a sequenced
step). A C/E/G chord step is measured with a Hann-window spectral peak scan per
chord note (DX7), against an untuned chord baseline. Live recording is driven
through `groove_edit_record` and MIDI (hold on/off, chord, overdub replace,
drum track untouched), then played back with hold off. The arpeggiator test
measures the first arp step (gate 100%, rate 0) for a key pressed with and
without alt hold; a two-key case (E4 alt, G4 not, then the reverse) measures the first
and second arp steps in one run (MIDI Start is sent once, since it restarts the arp) and
checks each note against its own key's table.


## Storage test

`storage_test` (tasks 5.1-5.4) covers `groove_tuning_capture/apply` (round trip,
any -128 or NULL rejected with nothing changed), the 462,848 B sample cap
(`groove_samples_begin` accepts it, rejects 462,849; SysEx cmd 11 reports it; a
stored bank of 463,352 B still validates and reads), the tuning sector at
`GROOVE_TUNING_OFFSET` (save/reload per slot, blank/wrong-magic/wrong-version
sector and a corrupted entry load as zero tables with an identical render, a
stale entry after a slot replacement is ignored, factory loads zero, an
over-size bank blocks tuning saves and loads but not project saves), and SysEx
project transfer with the optional tuning block (kind 3, 4264 B; kind 0 stays
4064 B and a block-less import zeroes the tables; bad block CRC, magic or a
-128 entry is rejected whole). The test `#include`s `app/transfer.c` with its
external symbols renamed, to capture replies (the simulator's `fm1_midi_send`
discards them) and drive the real framing. Linking without
`web/engine/tuning.c` leaves `groove_tuning_*` as null weak references; on
macOS that needs `-Wl,-undefined,dynamic_lookup` (ELF targets resolve them to 0).
