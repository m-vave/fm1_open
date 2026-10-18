// SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
// Drive the workbench's own project library client (sdk/WebUpdate.js, or a
// patched copy given as the first argument) against the device protocol in
// build/host/workbench_bridge, through a fake Web MIDI port. Checks that the
// stock client still connects and moves projects, and, when the client reads
// the layout (command 22), library bank transfers. Run after tests/host/build.sh
// and build/host/library_test (for build/host/library_vectors.bin).
//   node tests/host/workbench_test.mjs [path/to/WebUpdate.js]
import {readFileSync} from 'node:fs';
import {spawn} from 'node:child_process';
import vm from 'node:vm';
const root = new URL('../../', import.meta.url);
const client = process.argv[2] ?? new URL('sdk/WebUpdate.js', root).pathname;
vm.runInThisContext(readFileSync(client, 'utf8'));
const {Library} = globalThis.FM1Web;

const device = spawn(new URL('build/host/workbench_bridge', root).pathname, [], {stdio: ['pipe', 'pipe', 'inherit']});
const listeners = new Set();
let pending = [];
device.stdout.on('data', chunk => {
  for (const b of chunk) {
    if (b === 0xf0) pending = [];
    pending.push(b);
    if (b === 0xf7) { const data = Uint8Array.from(pending); listeners.forEach(f => f({data})); pending = []; }
  }
});
const port = {name: 'FM1', manufacturer: '', state: 'connected', open: async () => {}, close: async () => {}};
const input = {...port, addEventListener: (_, f) => listeners.add(f), removeEventListener: (_, f) => listeners.delete(f)};
const output = {...port, send: bytes => device.stdin.write(Uint8Array.from(bytes))};
const access = {inputs: new Map([['in', input]]), outputs: new Map([['out', output]])};

let failures = 0;
const check = (ok, what) => { if (!ok) { console.log('FAIL ' + what); failures++; } };
const equal = (a, b) => a.length === b.length && a.every((v, i) => v === b[i]);
const rejects = async (promise, what) => { try { await promise; check(false, what); } catch { /* expected */ } };
try {
  const library = new Library(access);
  const info = await library.connect();
  check(info[1] === 16 && library.tuningBytes === 4264, 'connects, command 21 unchanged');
  const project = await library.download(0);
  await library.upload(0, 0, project);
  check(equal(await library.download(0), project), 'project round trip');
  if (!library.layout) {
    console.log('workbench_test: client without layout support connected and moved a project');
  } else {
    const {sampleLimit, banks, programs} = library.layout;
    check(sampleLimit === 446464 && banks === 4 && programs === 128, 'layout 446464 / 4 / 128');
    const vectors = new Uint8Array(readFileSync(new URL('build/host/library_vectors.bin', root)));
    const body = globalThis.FM1Web.bankBody(vectors.subarray(0, 4104));
    await library.upload(4, 2, body);
    check(equal(await library.download(4, 2), body), 'bank write and read back');
    check(equal(globalThis.FM1Web.bankFile(body), vectors.subarray(0, 4104)), 'bank file matches the .syx');
    check((await library.download(4, 0)).every(b => b === 255), 'other banks stay empty');
    check(equal(await library.download(0), project), 'project untouched by a bank write');
    const bad = body.slice(); bad[5] = 0x90;
    await rejects(library.upload(4, 1, bad), 'invalid bank refused');
    await rejects(library.upload(4, banks, body), 'bank index out of range refused');
    console.log('workbench_test: layout, bank transfer and project isolation');
  }
  await library.disconnect();
} catch (error) {
  console.log('FAIL ' + error.message); failures++;
} finally {
  device.stdin.end();
}
console.log(failures ? `workbench_test: ${failures} failure(s)` : 'workbench_test: ok');
process.exit(failures ? 1 : 0);
