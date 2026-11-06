#!/usr/bin/env node
// SPDX-License-Identifier: MIT
// A port of sdk/Package.cs (Copyright (c) 2023 Andrey Grigoryev (kagaimiq), 2026 M-VAVE; JieLi formats and
// ciphers from jl-misctools 0a5b12d; FM1 layout and writer by M-VAVE; full MIT terms in THIRD_PARTY.md) and of
// the image step of Package-Firmware in sdk/Build.ps1, so that the firmware package can be made on a machine
// that has no PowerShell or .NET, from an ELF file that any build produced.
//
// Make the FM-1 firmware package (.fwsc) from a linked sdk.elf, the way sdk/Build.ps1 and sdk/Package.cs do:
//
//   node fwsc.mjs SDK.elf OUT.fwsc [--root SRC] [--product FM-1_8000014] [--resources sdk/tools/pack]
//
// --root is the source tree the ELF was built from (default: this repository): it supplies app/version.h,
// the boot resources and the drift check.
//
// The product defaults to FM1_UPDATE_PRODUCT in app/version.h. The result has been compared byte for byte with
// the package the Windows build makes (docs/BUILDING.md). The SDK's own packager stays the reference: if
// sdk/Package.cs changes, this refuses to run (tools/portable/ported-from.json).
import { readFileSync, writeFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { Elf } from './elf.mjs';
import { portedFromProblem } from './guard.mjs';

const AREA = 0x4000, ENTRY = 0x02000120, APP_DATA = 0x120;
const FLASH_LIMIT = 0xff000, USER_SIZE = 536576, BT_SIZE = 4096, MIN_VM = 32768;
const PUBLIC_CODE = 0x980f;

const CRC_TABLE = (() => {
  const table = [];
  for (let i = 0; i < 256; i++) {
    let value = i << 8;
    for (let b = 0; b < 8; b++) value = ((value << 1) ^ (value & 0x8000 ? 0x1021 : 0)) & 0xffff;
    table.push(value);
  }
  return table;
})();

function crc(data, begin = 0, length = data.length - begin) {
  let value = 0;
  for (let i = begin; i < begin + length; i++) value = ((value << 8) ^ CRC_TABLE[(value >> 8) ^ data[i]]) & 0xffff;
  return value;
}

function cipher(buf, begin, length, state) {
  for (let i = begin; i < begin + length; i++) {
    buf[i] ^= state & 0xff;
    state = ((state << 1) ^ (state & 0x8000 ? 0x1021 : 0)) & 0xffff;
  }
}

const roundUp = (size, alignment) => (size + alignment - 1) & -alignment;
const fail = (message) => { throw new Error(message); };

class Image {
  constructor(size, fill) { this.b = Buffer.alloc(size, fill); }
  number(offset, value, width) { for (let n = 0; n < width; n++) this.b[offset + n] = Math.floor(value / 2 ** (8 * n)) & 0xff; }
  copy(offset, data) { Buffer.from(data).copy(this.b, offset); }
  text(offset, value) { this.copy(offset, Buffer.from(value, 'ascii')); }
  checksum(field, offset, size) { this.number(field, crc(this.b, offset, size), 2); }
  record(offset, name, crcValue, address, size, attributes, last, extra = null) {
    this.b.fill(0, offset, offset + 32);
    this.number(offset + 2, crcValue, 2);
    this.number(offset + 4, address, 4);
    this.number(offset + 8, size, 4);
    this.number(offset + 12, attributes, 2);
    this.number(offset + 14, last, 2);
    this.b.fill(255, offset + 16, offset + 32);
    this.text(offset + 16, name);
    this.b[offset + 16 + name.length] = 0;
    if (extra !== null) this.number(offset + 28, extra, 4);
    this.checksum(offset, offset + 2, 30);
  }
}

function readResource(directory, name, size) {
  const data = readFileSync(join(directory, name));
  if (data.length !== size) fail(`Unexpected FM1 resource: ${name}`);
  return data;
}

function encodeBoot(plain) {
  const result = Buffer.from(plain);
  const banks = plain.readUInt16LE(0);
  if (banks < 1 || banks > Math.floor(plain.length / 16)) fail('Invalid boot directory.');
  let previousEnd = banks * 16;
  for (let i = 0; i < banks; i++) {
    const header = i * 16, length = plain.readUInt16LE(header + 2), start = plain.readInt32LE(header + 8);
    if (length === 0 || start < previousEnd || start > plain.length - length ||
        crc(plain, header, 14) !== plain.readUInt16LE(header + 14) || crc(plain, start, length) !== plain.readUInt16LE(header + 12)) {
      fail('Invalid boot bank.');
    }
    cipher(result, header, 16, 65535);
    cipher(result, start, length, 65535);
    previousEnd = start + length;
  }
  return result;
}

function buildFlash(application, directory, end, vmEnd) {
  const boot = readResource(directory, 'uboot.boot', 14488);
  const config = readResource(directory, 'boot-config.bin', 721);
  const tool = readResource(directory, 'cfg_tool.bin', 383);
  const image = new Image(end, 255), settings = new Image(34 + config.length, 0);
  // Deterministic jl-misctools chipkeybin descriptor for the public image.
  for (let bit = 0; bit < 16; bit++) {
    settings.b[bit] = 16;
    settings.b[16 + bit] = 16 ^ (((PUBLIC_CODE >> bit) & 1) === 0 ? 255 : 0);
  }
  settings.checksum(32, 0, 32);
  settings.copy(34, config);
  // AC791N boot header fields from the verified FM1 boot resources.
  image.copy(0, [0, 0, 0x20, 2, 0x2f, 0x10, 0x4c, 0xc9, 0, 0xf0, 0x0f, 0, 0x10, 0x10, 0xa4, 0xff,
    0x4e, 0x5d, 0x0b, 0x41, 0xe0, 0xcd, 0x78, 0x1d, 0xc8, 0x56, 0x41, 0x7f, 0xb3, 0x67, 0xcf, 0x9f]);
  image.checksum(0, 2, 30);
  const configAt = 160 + boot.length;
  image.record(32, 'uboot.boot', crc(boot), 160, boot.length, 0, 0);
  image.record(64, 'isd_config.ini', crc(settings.b), configAt, settings.b.length, 0x8002, 0);
  image.record(96, 'app_dir_head', 65535, AREA, 0xffffffff, 0xff81, 0);
  image.record(128, 'key_mac', 65535, FLASH_LIMIT, 4096, 0x0112, 65535);
  for (let p = 0; p < 160; p += 32) cipher(image.b, p, 32, 65535);
  image.copy(160, encodeBoot(boot));
  image.copy(configAt, settings.b);
  const payloadSize = APP_DATA + application.length + tool.length;
  const app = AREA + APP_DATA;
  image.copy(app, application);
  image.copy(app + application.length, tool);
  image.record(AREA + 32, 'app.bin', crc(application), APP_DATA, application.length, 0xff82, 0);
  image.record(AREA + 64, 'cfg_tool.bin', crc(tool), APP_DATA + application.length, tool.length, 0xff82, 0);
  image.record(AREA + 96, 'VM', 65535, end, vmEnd - end, 0x8112, 0, MIN_VM);
  image.record(AREA + 128, 'PRCT', 65535, 0, end, 0x8292, 0, 0x45444f43);
  image.record(AREA + 160, 'BTIF', 65535, vmEnd, BT_SIZE, 0x8192, 1, 0x4f545541);
  image.record(AREA, 'app_area_head', crc(image.b, AREA + 32, payloadSize - 32), ENTRY, payloadSize, 0xff83, 0);
  const reserved = AREA + payloadSize;
  image.record(reserved + 32, 'USER', 65535, FLASH_LIMIT - USER_SIZE, USER_SIZE, 0x8192, 1, 0x4f545541);
  image.record(reserved, 'EXT_RESERVED', crc(image.b, reserved + 32, 32), 32, 64, 0xff93, 1);
  for (let p = 0; p < payloadSize + 64; p += 32) {
    cipher(image.b, AREA + p, Math.min(32, payloadSize + 64 - p), PUBLIC_CODE ^ (p >> 2));
  }
  return image;
}

function wrap(flash, loader, product) {
  if (loader.length < 1 || loader.length >= 32768) fail('Invalid FM1 update loader.');
  const loaderAt = 1024 + roundUp(flash.b.length, 32), total = loaderAt + roundUp(loader.length, 32);
  const ufw = new Image(total, 255);
  ufw.b.fill(0, 0, 224);
  const files = [flash.b, loader], names = ['flash.bin', 'ota.bin'];
  files.forEach((data, i) => {
    const record = 64 + i * 80;
    ufw.number(record, i === 0 ? 0 : 100, 2);
    ufw.number(record + 2, i, 2);
    ufw.number(record + 4, crc(data), 2);
    ufw.number(record + 8, i === 0 ? 1024 : loaderAt, 4);
    ufw.number(record + 12, data.length, 4);
    ufw.number(record + 16, roundUp(data.length, 32), 4);
    ufw.text(record + 64, names[i]);
    cipher(ufw.b, record, 80, 65535);
  });
  ufw.number(4, total, 4);
  ufw.number(8, 2, 2);
  ufw.number(10, 4, 2);
  ufw.number(12, 512, 4);
  ufw.text(16, 'AC791N');
  ufw.checksum(2, 64, 160);
  ufw.checksum(0, 2, 62);
  cipher(ufw.b, 0, 64, 65535);
  ufw.copy(1024, flash.b);
  ufw.copy(loaderAt, loader);
  const out = Buffer.alloc(total + 20);
  let destination = 0;
  for (let source = 0; source < total; source++, destination++) {
    out[destination] = ufw.b[source];
    if (source < 940 && (source + 1) % 47 === 0) {
      const index = Math.floor(source / 47);
      out[++destination] = index < product.length ? (product.charCodeAt(index) + index + 1) & 0xff : 125;
    }
  }
  return out;
}

/** Make the package from an application image (the sections of sdk.elf that Build.ps1 hands to the packager). */
export function createPackage(application, resources, product) {
  if (!/^FM-1_8[0-9]{6}$/.test(product)) fail('Invalid FM1 update identity.');
  if (!application.length || application.length > FLASH_LIMIT - USER_SIZE - BT_SIZE - MIN_VM - AREA - APP_DATA - 383 - 64) {
    fail('Application does not fit the FM1 layout.');
  }
  const imageSize = roundUp(AREA + APP_DATA + application.length + 383 + 64, 4096);
  const vmEnd = FLASH_LIMIT - USER_SIZE - BT_SIZE;
  if (imageSize > vmEnd - MIN_VM) fail('Insufficient FM1 settings space.');
  const flash = buildFlash(application, resources, imageSize, vmEnd);
  const package_ = wrap(flash, readFileSync(join(resources, 'ota.bin')), product);
  return { package: package_, layout: { FLASH_BIN_SIZE: imageSize, VM_REAL_SIZE: vmEnd - imageSize, user_bytes: USER_SIZE } };
}

/** The application image Build.ps1 gives the packager: .text, .data, .ram0_data and .cache_ram_data, in that order. */
export function applicationImage(elfPath) {
  const elf = new Elf(elfPath);
  if (elf.data.readUInt32LE(24) !== ENTRY) fail('Package entry must match the verified FM1 firmware entry.');
  const parts = [];
  for (const name of ['.text', '.data', '.ram0_data', '.cache_ram_data']) {
    const s = elf.sections.find((x) => x.name === name);
    if (s && s.size) parts.push(elf.content(s));
  }
  return Buffer.concat(parts);
}

function main(argv) {
  let product = null, resources = null, root = null;
  const args = [];
  for (let i = 0; i < argv.length; i++) {
    if (argv[i] === '--product') product = argv[++i];
    else if (argv[i] === '--resources') resources = argv[++i];
    else if (argv[i] === '--root') root = resolve(argv[++i]);
    else args.push(argv[i]);
  }
  if (args.length !== 2) { console.error('usage: fwsc.mjs SDK.elf OUT.fwsc [--root SRC] [--product FM-1_8000014] [--resources sdk/tools/pack]'); return 2; }
  root = root ?? resolve(dirname(fileURLToPath(import.meta.url)), '..', '..');
  const problem = portedFromProblem(root, ['sdk/Package.cs', 'sdk/Build.ps1']);
  if (problem) fail(problem);
  resources = resources ?? join(root, 'sdk', 'tools', 'pack');
  if (!product) {
    const m = /^#define FM1_UPDATE_PRODUCT "(FM-1_8[0-9]{6})"[ \t]*$/m.exec(readFileSync(join(root, 'app', 'version.h'), 'utf8'));
    if (!m) fail('Set a compatible FM1_UPDATE_PRODUCT in app/version.h.');
    product = m[1];
  }
  const { package: pkg, layout } = createPackage(applicationImage(args[0]), resources, product);
  writeFileSync(args[1], pkg);
  console.log(`Offline package fits 1 MiB: image ${layout.FLASH_BIN_SIZE}, VM ${layout.VM_REAL_SIZE}, user ${layout.user_bytes} bytes`);
  console.log(`${args[1]}: ${pkg.length} bytes, product ${product}`);
  return 0;
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  try { process.exit(main(process.argv.slice(2))); } catch (error) { console.error(`error: ${error.message}`); process.exit(1); }
}
