// SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
// Cross-check app/library.c's VMEM unpacking against the workbench's own
// readVoices (sdk/WebUpdate.js). Run after build/host/library_test, which
// writes build/host/library_vectors.bin: a 4104-byte 32-voice .syx, then the
// 32 expected 156-byte patches.
//   node tests/host/library_readvoices.mjs
import {readFileSync} from 'node:fs';
import vm from 'node:vm';
const root = new URL('../../', import.meta.url);
const context = vm.createContext({Uint8Array, Uint16Array, DataView, TextDecoder, TextEncoder, crypto, console});
context.globalThis = context;
vm.runInContext(readFileSync(new URL('sdk/WebUpdate.js', root), 'utf8'), context);
const data = new Uint8Array(readFileSync(new URL('build/host/library_vectors.bin', root)));
const voices = context.FM1Web.readVoices(data.subarray(0, 4104));
let bad = voices.length === 32 ? 0 : 1;
for (let v = 0; v < voices.length; v++) {
  const want = data.subarray(4104 + v * 156, 4104 + (v + 1) * 156);
  for (let i = 0; i < 155; i++)  // readVoices leaves the operator mask (155) to the caller
    if (voices[v][i] !== want[i]) { console.log(`voice ${v} byte ${i}: workbench ${voices[v][i]}, library ${want[i]}`); bad++; break; }
}
console.log(bad ? `library_readvoices: ${bad} mismatch(es)` : 'library_readvoices: 32 voices match the workbench');
process.exit(bad ? 1 : 0);
