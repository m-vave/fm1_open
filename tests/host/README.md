# Host tests

Native builds of the open engine mirror and the app, with the same sources,
include order and defines as `tools/Build-Web.ps1` (minus wasm). They need only
clang (or cc); the workbench checks also need node.

    tests/host/build.sh                       # build everything into build/host
    tests/host/check_golden.sh                # the 16 factory demos match their hashes
    tests/host/check_golden.sh --determinism  # two renders in separate processes agree
    build/host/fm1_host pitch-test            # A4 per engine within a cent
    build/host/library_test                   # patch library (below)
    node tests/host/library_readvoices.mjs    # VMEM unpacking matches the workbench
    node tests/host/workbench_test.mjs        # the workbench client against the device protocol

`check_golden.sh --record` rewrites `golden/demos.sha256` from the current tree.

## Patch library

`library_test` covers:
- the device image's single `DEFAULT` project (`app/presets.c` without `FM1_OPEN_WEB`);
- the factory voices bank from the web build;
- VMEM packing and INIT VOICE for empty or damaged slots;
- sample banks that overlap library banks, and the new upload limit;
- Program Change and PRESETS over 128 programs, a smaller app count, and an app without one (16 programs, wrapped as before);
- projects keeping their own patches;
- bank transfer (kind 4) and the layout query (command 22).

It writes `build/host/library_vectors.bin` for `library_readvoices.mjs`, which
unpacks the same bank with the workbench's own `readVoices`.

`workbench_test.mjs` runs `sdk/WebUpdate.js` (or a patched copy given as its
argument) against `build/host/workbench_bridge`, which speaks the device side
of the transfer protocol over stdin/stdout. The stock client must still connect
and move projects; a client that reads command 22 must also write and read back
library banks.
