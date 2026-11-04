# Building FM1 Open

There are two ways to build the firmware, the browser preview and the complete workbench page. They produce the
same files in the same folders and are meant to be equally good choices:

| | Windows | macOS and Linux |
|---|---|---|
| Firmware and its package | `build.bat` | `./build.sh` |
| Browser preview | `build.bat --web` | `./build.sh --web` |
| Firmware and the complete workbench page | `build.bat --package` | `./build.sh --package` |
| Needs | Windows x64, PowerShell, JieLi toolchain 2.5.3 | Docker and Node 18+ |
| Toolchain | JieLi Windows 2.5.3, checked file by file against `sdk/manifest.json` | JieLi Linux toolchain in a container, checked against `tools/docker/toolchain.json` |

The Windows route is the reference: it is what the SDK was written for and what its hashes pin. The Docker route runs the
same compiler release (clang 4.0.1, GNU ld 2.26.51) with the same flags and the same checks, and
[how the two have been compared](#how-the-two-routes-compare) is described below.

**Every command is run from the root of the repository** (the folder that contains `build.bat` and `sdk/`). You can clone the
repository to any folder; paths in these instructions and in the scripts are relative to it.

```
git -c core.autocrlf=false clone https://github.com/m-vave/fm1_open.git
cd fm1_open
```

`-c core.autocrlf=false` keeps Git from converting line endings: the build checks the SDK files byte for byte.

## Outputs

| Path | What |
|---|---|
| `build/dx7/sdk.elf`, `sdk.map` | the linked firmware and its map |
| `build/dx7/firmware-report.json` | what the checks found: sections, heap, sizes |
| `build/dx7/generated/FM1_OFFICIAL_OPEN.fwsc` | the firmware package. It cannot be installed by itself: it is installed from the page |
| `build/official-web/FM1-Official-Open-Source.html`, `SHA256SUMS.txt` | the complete workbench page with the firmware (and the original V15 for going back) inside (`--package`) |
| `build/preview/index.html` | the browser preview without a device (`--web`) |

`build/` is ignored by Git. Every build replaces the previous output; copy the page somewhere else if you want to keep it.

For a build that you distribute, the page needs the HTTPS address of an archive of the exact source it was built from
(`FM1_SOURCE_URL`, or `--source-url` for `build.sh`). Use the archive of a commit, not of a branch, which moves:
`https://github.com/<you>/fm1_open/archive/<full commit id>.zip`.

---

## Option A: Windows

Requires Windows 10 or 11 x64, PowerShell 5.1 or 7, `curl.exe` and `tar.exe` (both ship with Windows 10 1803 and later), and
for `--package` about 2 GB of disk space, an internet connection for the first run (it downloads the 518 MiB WASI SDK 27
with a pinned hash), and the Microsoft Visual C++ Redistributable for x64, version 14.40 or newer.

**1. Install the JieLi toolchain 2.5.3.** JieLi's installer needs Code::Blocks. This is the order that works:

1. Install Code::Blocks (https://www.codeblocks.org/downloads/binaries/).
2. Start Code::Blocks once, then close it **completely**: closing the window is not enough, end every `codeblocks.exe` in
   Task Manager.
3. Download and run JieLi's installer, https://jl-update.oss-cn-shenzhen.aliyuncs.com/2.5.3.exe, and keep the default
   folder, `C:\JL`.
4. Check that `C:\JL\pi32\bin\clang.exe` exists and that
   `(Get-FileHash C:\JL\pi32\bin\clang.exe -Algorithm SHA256).Hash.ToLower()` prints
   `d4bc97cf421c1ccc373dd8924ce88720ff7457b3f1478d15df4aeda6ae923bce`.

**2. Build**, in PowerShell from the repository root:

```powershell
$env:FM1_PI32_TOOLCHAIN = 'C:\JL\pi32'
.\build.bat                                   # firmware
.\build.bat --web                             # browser preview
$env:FM1_SOURCE_URL = "https://github.com/<you>/fm1_open/archive/$(git rev-parse HEAD).zip"
.\build.bat --package                         # firmware and the complete page
```

For a local firmware build without a distribution address: `powershell -NoProfile -ExecutionPolicy Bypass -File sdk\Build.ps1 -FirmwareOnly -Toolchain C:\JL\pi32`.

Keep the full path of your clone short (under about 90 characters), ASCII, and outside OneDrive: Windows limits paths to 260
characters and the WASI SDK that `--package` unpacks into `build\tools` has file names about 165 characters long.

### Windows troubleshooting

| You see | What it means |
|---|---|
| `running scripts is disabled on this system` | Start PowerShell scripts with `powershell -NoProfile -ExecutionPolicy Bypass -File ...` |
| `External toolchain does not match the SDK: <file>` | Wrong toolchain version or a damaged install: reinstall exactly 2.5.3 |
| `SDK payload changed: <file>`, `Native DSP kernels and SDK audio contracts do not match`, `Fixed DSP coefficients differ from the native kernels` | A file under `sdk/` or `app/synth/` is not byte for byte what the manifest says, almost always because of line endings or an editor. Make a fresh clone with `core.autocrlf=false` and do not edit `sdk/` |
| `Unexpected SDK file: <path>` | An extra file under `sdk/` (`Thumbs.db`, `.DS_Store`, an editor backup): delete it |
| `The specified path ... too long` | The clone's path is too long: clone to a shorter folder |
| `Command failed: ...wasi-sdk-27.0-x86_64-windows\bin\clang.exe (-1073741819)` with no other message | The WASI compiler crashes in `MSVCP140.dll` because the Visual C++ runtime is older than 14.40. Install the latest from https://aka.ms/vs/17/release/vc_redist.x64.exe |
| `Set FM1_WASI_SDK to an extracted WASI SDK 27 directory.` | The unpacking was interrupted: delete `build\tools\wasi-sdk-27.0-x86_64-windows` and run again |
| `WASI SDK archive SHA-256 mismatch`, or `curl.exe` errors | The download was cut off or blocked: run again |
| `Set FM1_SOURCE_URL or -SourceUrl to the HTTPS URL ...` | `--package` needs the source address (see Outputs) |
| `clang.exe` or `pi32v2-ld.exe` disappears | Antivirus quarantined a toolchain file: restore it and exclude `C:\JL` and your clone |

---

## Option B: macOS and Linux (Docker)

Requires Docker (Docker Desktop on macOS; on Apple Silicon it runs the x86-64 toolchain through Rosetta, a build takes about
10 seconds) and Node 18 or newer. No PowerShell, no Windows, no Python.

**1. One-time downloads**, which `./build.sh --setup` does for you: JieLi's Linux toolchain (26 MB, verified against the
SHA-256 in `tools/docker/toolchain.json`) and the WASI SDK 27 that compiles the browser core (114 to 125 MB, verified against
the SHA-256 GitHub publishes for the release). Both go to `~/.cache/fm1-open` (`FM1_CACHE` changes it) and nothing else is
ever downloaded or run on your machine outside the container.

**2. Build**, from the repository root:

```bash
./build.sh --setup                            # once
./build.sh                                    # firmware: build, checks, package
./build.sh --web                              # browser preview
./build.sh --package --source-url "https://github.com/<you>/fm1_open/archive/$(git rev-parse HEAD).zip"
```

`./build.sh --from-git <commit>` builds a clean export of a commit (nothing from your working tree) into `build/git-<commit>/`.

What `build.sh` runs, so that each step can also be run alone:

| Step | Script | What |
|---|---|---|
| Firmware | `tools/docker/build.sh [-o OUT] [SRC]` | compiles and links in a linux/amd64 container with the same flags as `sdk/Build.ps1`, then `tools/docker/gates.mjs` runs the symbol, section, heap and registration checks of `Build.ps1`'s `Test-Firmware` and the app-image size limit from `sdk/Package.cs` |
| Package | `node tools/portable/fwsc.mjs SDK.elf OUT.fwsc` | the packager of `sdk/Package.cs` and the image step of `Build.ps1`, in Node |
| Browser core | `tools/portable/browser-core.sh [-o OUT] [SRC]` | `Build-Simulator` of `tools/Build-Web.ps1`: the same sources, include paths and flags, with a macOS or Linux WASI SDK |
| Page | `node tools/portable/page.mjs ...` | `tools/Build-Web.ps1` and the package step of `Build.ps1`, in Node (`--preview` is `-LocalPreview`) |
| Compare | `node tools/portable/elf-compare.mjs A.elf B.elf` | compares two builds, see below |

`FM1_LINUX_TOOLCHAIN` and `FM1_WASI_SDK` point at an existing extracted toolchain or WASI SDK instead of the cache, and
`FM1_BUILD_IMAGE` renames the Docker image.

Tested: macOS on Apple Silicon. Linux, and macOS on Intel, use the same scripts and the published hashes for their platforms but
have not been run by the authors; please report what you find.

### Docker route troubleshooting

| You see | What it means |
|---|---|
| `the Docker daemon is not running` | Start Docker Desktop (or the docker service) |
| `the Linux toolchain is not at ...` | Run `./build.sh --setup` (or `tools/docker/fetch-toolchain.sh --download`) |
| `WASI SDK 27 not found` | Run `./build.sh --setup` (or `tools/portable/fetch-wasi-sdk.sh --download`) |
| `error: App image is N bytes; the FM1 layout allows at most 453921` | The firmware does not fit; the same limit stops `sdk/Build.ps1` |
| `error: ... changed since tools/docker and tools/portable were ported from it` | `sdk/Build.ps1`, `sdk/Package.cs` or `tools/Build-Web.ps1` changed, so the ports may no longer match it. See "Keeping the ports in step" |

---

## How the two routes compare

The Docker route is a port: it must produce what the Windows route produces, and that is checked rather than assumed.

- **The same compiler release.** The Windows toolchain reports `clang version 4.0.1` and `GNU ld 2.26.51.20160621`; the Linux
  one reports the same two versions. The 16 compiler headers match `sdk/manifest.json` byte for byte after line-ending
  conversion. The Linux programs are not the Windows `.exe` files, so their hashes cannot be compared.
- **The same flags, objects and link line.** `tools/docker/container-build.sh` keeps the options, per-file options, object order
  and link arguments of `sdk/Build.ps1`; every check of `Test-Firmware` passes unchanged.
- **Byte-for-byte comparison on a derived firmware.** A firmware built from this SDK (DX747, an independent project that
  replaces the app code) was built twice on Windows (in two differently named folders) and in Docker: the build identity and every size were
  equal, **every loadable section (`.text`, `.ram0_data`, `.boot_info`, `.cache_ram_data`) was identical byte for byte, and the
  3,633 function and data symbols had the same addresses**. The ELF files themselves differed only in the symbol and string tables,
  which hold build paths. The package made by `tools/portable/fwsc.mjs` from the Linux-built ELF was byte for byte the package
  the Windows build made (SHA-256 `385f83e5a01a17e273f73563aeb655504a1909566c07cd5f35cb29b7861182e6`, 471,604 bytes), and two
  Windows builds of the same commit in differently named folders produced the same package.
- **This tree (commit `4c7496d`).** In Docker it builds to 453,576 bytes of the 453,921 allowed (headroom 345), heap 247,340 bytes,
  every check passing. Built on Windows (`build.bat --package`) and with `./build.sh --package` with the same
  `FM1_SOURCE_URL`, the two gave: the same build identity; **every loadable section identical** (`.text` 372,576 bytes,
  `.ram0_data` 80,968, `.boot_info`, `.cache_ram_data`) and the same 3,755 function and data symbols at the same addresses and
  sizes; **a byte-identical package** (SHA-256 `380ec5c38c0aa143aa4dc6c6f12e87c771a3916812c0eb1ff0875e3a1d8f7c4c`, 492,084
  bytes); and a **byte-identical workbench page** (SHA-256 `72a7a90ecd9063c95efb1383e4dd1944a2b3ee049d925cedd4d7e2dd07341015`,
  2,543,418 bytes), which also means that the browser core compiled by the macOS WASI SDK equals the one the Windows WASI SDK
  compiles. The page is accepted by the page's own validation code (`FM1Web.prepare`), and the preview renders in a browser.

Not shown: that the two toolchains generate the same code for every possible source: equivalence is checked per tree, and has
been checked for two so far (this one and the derived firmware above). The page is only equal when the source address is the
same, because the page embeds it. Linux and macOS on Intel use the same scripts but have not been run. If you change the
firmware, compare your tree once (below) before you rely on the Docker route for what you flash.

### Comparing your own builds

`elf-compare.mjs` compares what the chip loads from two builds: every loadable section byte by byte, and the function and
data symbols by name, address and size. The `sdk.elf` files themselves are never byte-identical between builds (their
tables contain file names), so comparing them whole would say nothing.

```
node tools/portable/elf-compare.mjs build/dx7/sdk.elf /path/to/windows/build/dx7/sdk.elf
```

It ends with `loadable content: IDENTICAL` or `DIFFERENT`, and for a difference says how many bytes differ and where the first one
is. Compare `generated/FM1_OFFICIAL_OPEN.fwsc` with `shasum -a 256` or `Get-FileHash`. Build the same commit in two differently named
folders on one machine to tell a toolchain difference from a path effect.

### Keeping the ports in step

`tools/docker` and `tools/portable` are ports of `sdk/Build.ps1`, `sdk/Package.cs` and `tools/Build-Web.ps1`.
`tools/portable/ported-from.json` holds the SHA-256 of the versions they were written against, and the scripts refuse to
run when one of those files has changed (`FM1_ALLOW_UNSYNCED=1` overrides, with a warning). After a change to one of them: read
the change, update the port, compare a Windows build with a Docker build as above, then update `ported-from.json`.
