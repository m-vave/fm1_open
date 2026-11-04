#!/usr/bin/env node
// SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
//
// Compare the loadable content of PI32 ELF32 firmware files.
//
// Builds of the same source by different toolchain builds (JieLi's Windows 2.5.3 and the Linux one used by
// tools/docker) or on different machines are never byte-identical as ELF files: the symbol and string
// tables hold file names and build paths. What matters for a flash image is the content of the sections the
// chip loads (SHF_ALLOC), so this compares those, section by section and byte by byte, and the function
// and data symbols by name, address and size.
//
//   node elf-compare.mjs FILE.elf                 print a report: size and SHA-256 of every loadable section
//   node elf-compare.mjs A.elf B.elf              compare two files and say what differs, where
//   node elf-compare.mjs --symbols OUT FILE.elf   also write the sorted symbol list ("address size name")
//
// Exit status of the two-file form: 0 when every loadable section is identical, 1 otherwise.
import { createHash } from 'node:crypto';
import { writeFileSync } from 'node:fs';
import { Elf, SHT_NOBITS } from './elf.mjs';

const sha = (data) => createHash('sha256').update(data).digest('hex');
const hex8 = (n) => n.toString(16).padStart(8, '0');

const byAddress = (a, b) => a.addr - b.addr || (a.name < b.name ? -1 : a.name > b.name ? 1 : 0);

function loadableDigest(elf) {
  const h = createHash('sha256');
  for (const s of [...elf.loadable()].sort(byAddress)) {
    h.update(`${s.name}:${s.addr}:${s.size}:`);
    h.update(elf.content(s));
  }
  return h.digest('hex');
}

function report(elf, symbolsOut) {
  console.log(`file ${elf.path}`);
  console.log(`elf_bytes ${elf.data.length} (informational: debug and string tables differ between builds)`);
  for (const s of [...elf.loadable()].sort(byAddress)) {
    const digest = s.kind === SHT_NOBITS ? 'nobits' : sha(elf.content(s));
    console.log(`section ${s.name} addr=0x${hex8(s.addr)} size=${s.size} sha256=${digest}`);
  }
  console.log(`loadable_sha256 ${loadableDigest(elf)}`);
  const lines = [...elf.symbols()].map(([n, [v, z]]) => `${hex8(v)} ${z} ${n}`).sort();
  console.log(`symbols ${lines.length} sha256=${sha(lines.join('\n'))}`);
  if (symbolsOut) writeFileSync(symbolsOut, lines.join('\n') + '\n');
}

function compare(a, b) {
  let same = true;
  const key = (s) => `${s.name}@${s.addr}`;
  const sa = new Map(a.loadable().map((s) => [key(s), s])), sb = new Map(b.loadable().map((s) => [key(s), s]));
  const keys = [...new Set([...sa.keys(), ...sb.keys()])].map((k) => sa.get(k) ?? sb.get(k)).sort(byAddress).map(key);
  for (const k of keys) {
    const x = sa.get(k), y = sb.get(k), at = (x ?? y);
    const where = `section ${at.name} @0x${hex8(at.addr)}`;
    if (!x || !y) { console.log(`${where}: only in ${x ? 'A' : 'B'}`); same = false; continue; }
    if (x.size !== y.size) { console.log(`${where}: size differs, A=${x.size} B=${y.size}`); same = false; continue; }
    if (x.kind === SHT_NOBITS || y.kind === SHT_NOBITS) { console.log(`${where}: ${x.size} bytes, no file content (zero-initialised)`); continue; }
    const ca = a.content(x), cb = b.content(y);
    if (ca.equals(cb)) { console.log(`${where}: identical, ${x.size} bytes, sha256 ${sha(ca).slice(0, 16)}...`); continue; }
    let first = -1, last = -1, count = 0;
    for (let i = 0; i < ca.length; i++) if (ca[i] !== cb[i]) { if (first < 0) first = i; last = i; count++; }
    console.log(`${where}: DIFFERS in ${count} of ${ca.length} bytes, first at offset 0x${first.toString(16)} (address 0x${hex8(x.addr + first)}), last at 0x${last.toString(16)}`);
    same = false;
  }
  const ya = a.symbols(), yb = b.symbols();
  const both = [...ya.keys()].filter((n) => yb.has(n));
  const moved = both.filter((n) => ya.get(n)[0] !== yb.get(n)[0] || ya.get(n)[1] !== yb.get(n)[1]);
  console.log(`symbols: ${ya.size} in A, ${yb.size} in B, ${both.length} in both, ${moved.length} with a different address or size, ` +
    `${[...ya.keys()].filter((n) => !yb.has(n)).length} only in A, ${[...yb.keys()].filter((n) => !ya.has(n)).length} only in B`);
  for (const n of moved.sort((p, q) => ya.get(p)[0] - ya.get(q)[0]).slice(0, 10)) {
    console.log(`  ${n}: A=0x${hex8(ya.get(n)[0])}/${ya.get(n)[1]} B=0x${hex8(yb.get(n)[0])}/${yb.get(n)[1]}`);
  }
  console.log('loadable content: ' + (same ? 'IDENTICAL' : 'DIFFERENT'));
  return same;
}

const usage = () => {
  console.error('usage: elf-compare.mjs FILE.elf | A.elf B.elf | --symbols OUT FILE.elf');
  return 2;
};

function main(argv) {
  let symbolsOut = null;
  if (argv[0] === '--symbols' && argv.length >= 3) { symbolsOut = argv[1]; argv = argv.slice(2); }
  if (argv.length === 1) { report(new Elf(argv[0]), symbolsOut); return 0; }
  if (argv.length === 2) return compare(new Elf(argv[0]), new Elf(argv[1])) ? 0 : 1;
  return usage();
}

try { process.exit(main(process.argv.slice(2))); } catch (error) { console.error(`error: ${error.message}`); process.exit(1); }
