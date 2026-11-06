# FM1 Open V14

English · [简体中文](README.zh-CN.md)

8 tracks, 8 sample slots, 16 FX demos; binary SDK included.

[Workbench and development kit](https://open.m-vave.com/fm1-workbench) · [Official forum](https://forum.m-vave.com)

Use the workbench to update firmware, preview the instrument and manage projects, samples and DX7 voices. Select "Source + SDK" to download the development kit matching the online release. Back up projects, voices and samples before updating, and keep the device powered during writes.

## V14 features

- **DX7 library:** 128 voices in four groups of 32, with the M-VAVE FM1 original V15 factory voices and order. Saved user banks take priority. DX7 single-voice and 32-voice bank SysEx import/export is supported; extension parameters are not handled. Changing the library does not change patches already saved in projects.
- **Track roles:** on SEQ **8/8 TRACK ROLE**, K1 selects NORMAL, PLAY or OFF; K2 selects input MIDI channel 1–16. PLAY retains the track's timbre and held notes across project changes. MUTE/SOLO use the existing controls, with independent NORMAL and PLAY solo groups. PLAY timbre edits are not saved with projects: export a timbre file, or save DX7 voices to the independent library.
- **Step recording:** with the track set to NORMAL, press REC while stopped and play notes. Releasing all played keys advances to the next step, including DRUM 808 and SAMPLER. Overlapping notes share a step; during playback, REC records in time. PLAY tracks are for live performance and do not record notes.
- **Tempo and delay:** internal tempo is 30–300 BPM. Tap Tempo is available on the workbench, with SEQ on the FX DELAY page, or with HOME held while tapping SEQ elsewhere. Delay supports tempo synchronization and fixed times of about 30–2040 ms.

## MIDI

FM1 accepts DIN, USB and BLE MIDI input, with USB/BLE output. Default input channels 1–7 address tracks 1–7; channel 10 addresses track 8. Change each input channel on TRACK ROLE.

- **5/8 SYNC / LOOP:** select internal or external clock, tempo, loop length and clock input.
- **6/8 MIDI TRACK:** select SEQ/ARP LOCAL, BOTH or MIDI output, output channel, output ports and physical KEY OUT.
- **7/8 MIDI ROUTING:** configure LOCAL, CLK OUT and MIDI Thru. For DAW monitoring with Local Off, enable KEY OUT and select the output ports.

MIDI preferences are saved independently of projects. The browser simulator previews these settings without sending performance MIDI to external ports; configure the instrument on its panel. MIDI Clock, Start, Continue, Stop and Song Position Pointer input are supported. Clock loss stops synchronized playback.

## Updates and compatibility

V13 can update to V14. The workbench supports returning to original V15 and migrating from compatible third-party firmware. The standard FM1 update protocol remains available. Compatibility has been checked with selected firmware versions; it is not a guarantee for every third-party release.

Firmware switching does not make project and sample formats interchangeable. Keep exported backups for rollback. Older firmware may not support V14 track preferences, the independent voice library or synchronized/extended delay settings.

To back up the independent voice library, read and export groups A–D separately. Project files do not contain sample audio. Restore custom samples from their sample bank backup; writing the built-in sample bank replaces the current samples.

The eight sample slots share about 19.55 seconds of new sample storage. Existing larger banks remain readable; they may need to be backed up and reduced before writing the independent DX7 library.

## Build

You can build on **Windows** or on **macOS and Linux**. The two routes make the same three builds, in the same folders:

| Build | Windows | macOS / Linux |
|---|---|---|
| Firmware and its package (`build/dx7/`) | `build.bat` | `./build.sh` |
| Browser preview (`build/preview/index.html`) | `build.bat --web` | `./build.sh --web` |
| Firmware and the complete workbench page (`build/official-web/`) | `build.bat --package` | `./build.sh --package` |

**Get the source by cloning the repository** into any folder, and run every command below from its root (the folder that
contains `build.bat`); nothing depends on where it is:

~~~
git -c core.autocrlf=false clone https://github.com/m-vave/fm1_open.git
cd fm1_open
~~~

(`-c core.autocrlf=false` keeps Git from changing line endings: the build checks the SDK files byte for byte.) The
[build guide](docs/BUILDING.md) has the details, the troubleshooting tables and how the two routes were compared.

### Option A: Windows

Requires Windows x64 and PowerShell, `curl.exe` and `tar.exe`, and **JieLi Windows toolchain 2.5.3** (PI32v2 clang 4.0.1; FM1 continues
to use the `r3` CPU target). The build checks all 22 toolchain files against `sdk/manifest.json`; older V14 kits were pinned to 2.5.2 and
reject 2.5.3, so update the complete source + SDK kit together and keep the manifest checks enabled. JieLi's installer needs
Code::Blocks, so install the toolchain in this order:

1. Install [Code::Blocks](https://www.codeblocks.org/downloads/binaries/).
2. Start Code::Blocks once, then **close it completely**: end every `codeblocks.exe` in Task Manager.
3. Download and run [JieLi's 2.5.3 installer](https://jl-update.oss-cn-shenzhen.aliyuncs.com/2.5.3.exe) and keep the default folder, `C:\JL`.

For this revision the expected SHA-256 of `pi32/bin/clang.exe` is `d4bc97cf421c1ccc373dd8924ce88720ff7457b3f1478d15df4aeda6ae923bce`.
Then, in PowerShell from the repository root:

~~~powershell
$env:FM1_PI32_TOOLCHAIN = 'C:\JL\pi32'
.\build.bat                  # firmware
.\build.bat --web            # browser preview
$env:FM1_SOURCE_URL = "https://github.com/<you>/fm1_open/archive/$(git rev-parse HEAD).zip"
.\build.bat --package        # firmware and the complete workbench page
~~~

The first `--package` downloads WASI SDK 27 (set `FM1_WASI_SDK` to reuse an installation). For a local firmware build without a
distribution source URL, run `./sdk/Build.ps1 -FirmwareOnly -Toolchain 'C:\JL\pi32'`.

### Option B: macOS and Linux (Docker)

Requires Docker and Node 18 or newer. Nothing else: no PowerShell, no Windows. From the repository root:

~~~bash
./build.sh --setup            # once: JieLi's Linux toolchain (26 MB) and WASI SDK 27, both hash-checked
./build.sh                    # firmware: build, the SDK's checks, package
./build.sh --web              # browser preview
./build.sh --package --source-url "https://github.com/<you>/fm1_open/archive/$(git rev-parse HEAD).zip"
~~~

It uses the same compiler release (clang 4.0.1) and the same flags and checks as the Windows route, in a container, and takes
about 20 seconds. The Windows build stays the reference; [docs/BUILDING.md](docs/BUILDING.md#how-the-two-routes-compare) says what
has been compared.

### Distribution builds

For a build you distribute, `FM1_SOURCE_URL` (or `--source-url`) must be the HTTPS URL of the matching source archive, including your
changes, licenses and build/install scripts. A moving GitHub `tree/main` page is not a matching source archive.

The [per-track tuning API](app/instrument.h) supports custom tuning; a dedicated tuning editor is not included.

## License

APP: [GPLv3 with the FM1 SDK linking exception](LICENSE.md). The device engine is supplied as a binary SDK with separate terms. Retain the applicable [SDK terms, third-party licenses and contributor notices](THIRD_PARTY.md) when distributing builds.
