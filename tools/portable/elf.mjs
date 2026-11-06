// SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
//
// Minimal reader for the 32-bit little-endian ELF files the PI32 toolchain writes: sections and symbols.
import { readFileSync } from 'node:fs';

export const SHF_ALLOC = 2, SHT_NOBITS = 8, SHT_SYMTAB = 2, STT_OBJECT = 1, STT_FUNC = 2;

export class Elf {
  constructor(path) {
    this.path = path;
    const d = (this.data = readFileSync(path));
    if (d.length < 0x34 || d.readUInt32BE(0) !== 0x7f454c46 || d[4] !== 1 || d[5] !== 1) {
      throw new Error(`${path}: not a 32-bit little-endian ELF file`);
    }
    const shoff = d.readUInt32LE(0x20), shentsize = d.readUInt16LE(0x2e), shnum = d.readUInt16LE(0x30), shstrndx = d.readUInt16LE(0x32);
    const raw = [];
    for (let i = 0; i < shnum; i++) {
      const at = shoff + i * shentsize;
      const f = Array.from({ length: 10 }, (_, k) => d.readUInt32LE(at + 4 * k));
      raw.push(f);
    }
    const names = raw[shstrndx];
    const strings = d.subarray(names[4], names[4] + names[5]);
    this.sections = raw.map(([name, kind, flags, addr, offset, size, link]) => ({
      name: cString(strings, name), kind, flags, addr, offset, size, link,
    }));
  }
  loadable() { return this.sections.filter((s) => s.flags & SHF_ALLOC && s.size); }
  content(s) { return s.kind === SHT_NOBITS ? Buffer.alloc(0) : this.data.subarray(s.offset, s.offset + s.size); }
  symbols() {
    const out = new Map();
    for (const s of this.sections) {
      if (s.kind !== SHT_SYMTAB) continue;
      const table = this.sections[s.link];
      const strings = this.data.subarray(table.offset, table.offset + table.size);
      for (let i = 0; i < Math.floor(s.size / 16); i++) {
        const at = s.offset + i * 16;
        const name = this.data.readUInt32LE(at), value = this.data.readUInt32LE(at + 4), size = this.data.readUInt32LE(at + 8);
        const info = this.data[at + 12], shndx = this.data.readUInt16LE(at + 14);
        if (shndx === 0 || ![STT_OBJECT, STT_FUNC].includes(info & 15)) continue;
        out.set(cString(strings, name), [value, size]);
      }
    }
    return out;
  }
}

function cString(buffer, offset) {
  const end = buffer.indexOf(0, offset);
  return buffer.toString('latin1', offset, end < 0 ? buffer.length : end);
}
