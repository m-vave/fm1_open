#!/usr/bin/env node
// SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
//
// Assemble the workbench page: the complete one with the firmware inside
// (build/official-web/FM1-Official-Open-Source.html) or the local preview (build/preview/index.html).
//
// A port, for machines without PowerShell, of tools/Build-Web.ps1 and of the package-JSON step of
// sdk/Build.ps1 -Package. It compiles nothing itself: it takes the firmware package (fwsc.mjs), the build
// identity (build-identity.json) and the browser core (simulator.wasm, from browser-core.sh) and fills
// sdk/Update.html.in with them, exactly as Build-Web.ps1 does.
//
//   node page.mjs --root SRC --fwsc FILE.fwsc --identity build-identity.json --wasm simulator.wasm \
//                 --source-url https://.../archive/<commit>.zip -o OUTDIR
//   node page.mjs --root SRC --wasm simulator.wasm --preview -o OUTDIR       (like Build-Web.ps1 -LocalPreview)
//
// --root is the source tree the firmware and the wasm were built from. OUTDIR gets
// FM1-Official-Open-Source.html and SHA256SUMS.txt (preview: index.html). Run ./build.sh --package or
// ./build.sh --web to do the whole chain. If tools/Build-Web.ps1 or sdk/Build.ps1 changes, this refuses to
// run until the port has been reviewed (tools/portable/ported-from.json).
import { createHash } from 'node:crypto';
import { existsSync, mkdirSync, readFileSync, readdirSync, rmSync, statSync, writeFileSync } from 'node:fs';
import { basename, join, relative, resolve, sep } from 'node:path';
import { dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { portedFromProblem } from './guard.mjs';

const fail = (message) => { throw new Error(message); };
const sha256 = (data) => createHash('sha256').update(data).digest('hex');
const shaFile = (path) => sha256(readFileSync(path));
const readText = (path) => readFileSync(path, 'utf8').replace(/^﻿/, '');
const b64 = (data) => Buffer.from(data).toString('base64');

/** System.Net.WebUtility.HtmlEncode: & < > " ' and U+00A0..U+00FF, and non-BMP characters, numerically. */
function htmlEncode(text) {
  let out = '';
  for (const ch of text) {
    const c = ch.codePointAt(0);
    if (ch === '&') out += '&amp;';
    else if (ch === '<') out += '&lt;';
    else if (ch === '>') out += '&gt;';
    else if (ch === '"') out += '&quot;';
    else if (ch === "'") out += '&#39;';
    else if ((c >= 160 && c <= 255) || c >= 0x10000) out += `&#${c};`;
    else out += ch;
  }
  return out;
}

function walk(dir) {
  const files = [];
  for (const entry of readdirSync(dir, { withFileTypes: true })) {
    const full = join(dir, entry.name);
    if (entry.isDirectory()) files.push(...walk(full));
    else if (entry.isFile()) files.push(full);
  }
  return files;
}

/** The hash Build-Web.ps1 gives the browser sources (Build-Simulator) and records in the page. */
function sourceHash(root) {
  const inputs = new Map();
  for (const top of ['app', 'web', 'third_party/synth']) {
    for (const file of walk(join(root, top))) {
      const rel = relative(root, file).split(sep).join('/');
      const ext = file.slice(file.lastIndexOf('.'));
      if (!rel.startsWith('app/board/') && ['.c', '.h', '.inc'].includes(ext)) inputs.set(rel, shaFile(file));
    }
  }
  const extra = ['tools/Build-Web.ps1', ...readdirSync(join(root, 'sdk/include')).filter((n) => n.endsWith('.h')).map((n) => `sdk/include/${n}`)];
  for (const rel of extra) inputs.set(rel, shaFile(join(root, rel)));
  const names = [...inputs.keys()].sort();
  return sha256(names.map((n) => `${n}:${inputs.get(n)}`).join('\n'));
}

const compact = (value) => JSON.stringify(value);

function parseArgs(argv) {
  const opts = { preview: false };
  const flags = new Set(['--root', '--fwsc', '--identity', '--wasm', '--source-url', '-o']);
  for (let i = 0; i < argv.length; i++) {
    if (argv[i] === '--preview') opts.preview = true;
    else if (flags.has(argv[i])) opts[argv[i]] = argv[++i];
    else return null;
  }
  const need = opts.preview ? ['--root', '--wasm', '-o'] : ['--root', '--fwsc', '--identity', '--wasm', '--source-url', '-o'];
  return need.every((k) => opts[k]) ? opts : null;
}

function build(opts) {
  const root = resolve(opts['--root']);
  const problem = portedFromProblem(root, ['tools/Build-Web.ps1', 'sdk/Build.ps1']);
  if (problem) fail(problem);
  const preview = opts.preview;
  const sourceUrl = opts['--source-url'] ?? '';
  if (!preview) {
    let url;
    try { url = new URL(sourceUrl); } catch { fail('FM1_SOURCE_URL must be an HTTPS source URL without credentials.'); }
    if (url.protocol !== 'https:' || url.username || url.password || /[\r\n]/.test(sourceUrl)) fail('FM1_SOURCE_URL must be an HTTPS source URL without credentials.');
  }
  const versionH = readText(join(root, 'app/version.h'));
  const release = /^#define FM1_RELEASE_VERSION "(V(?:0[1-9]|[1-9][0-9]{1,2}))"[ \t]*$/m.exec(versionH)?.[1];
  const product = /^#define FM1_UPDATE_PRODUCT "(FM-1_8[0-9]{6})"[ \t]*$/m.exec(versionH)?.[1];
  if (!release || !product) fail('Set FM1_RELEASE_VERSION and a compatible FM1_UPDATE_PRODUCT in app/version.h.');

  const notices = [`APP: GPLv3 with the FM1 SDK linking exception; no warranty. SDK and firmware resources have separate redistribution terms in THIRD_PARTY.md. Source: ${sourceUrl}`];
  let packageJson = null, fwsc = null;
  if (!preview) {
    const identity = JSON.parse(readText(opts['--identity']));
    if (!/^[0-9a-f]{64}$/.test(identity.build_id) || identity.release_version !== release) {
      fail('Firmware and web release versions differ, or the source identifier is invalid.');
    }
    fwsc = readFileSync(opts['--fwsc']);
    // The original V15 recovery firmware, separate from the Open release, as Build.ps1 -Package embeds it.
    const stockPath = join(root, 'sdk/firmware/FM-1_015.fwsc');
    const stockHash = 'db1642b2b6fa5c2cccb11ffd13878068bb28601678d3644049f99dc40e7edb8a';
    if (!existsSync(stockPath) || shaFile(stockPath) !== stockHash) fail('Matching SDK original V15 recovery firmware is missing or invalid.');
    const stockBytes = readFileSync(stockPath);
    const stock = { schema: 1, kind: 'stock', product: 'FM-1_015', profile: 'M-VAVE Original V15', family: 7, release_version: 'V15',
      build_id: stockHash, sha256: stockHash, bytes: stockBytes.length,
      loader_sha256: 'ef1f2db89b011fa53e71df363f0122de0ea896618c53fb53e8da676322004541', data: b64(stockBytes) };
    packageJson = compact({ schema: 1, product, profile: identity.profile ?? 'dx7', family: 7, release_version: release,
      build_id: identity.build_id, sha256: sha256(fwsc), bytes: fwsc.length, loader_sha256: shaFile(join(root, 'sdk/tools/pack/ota.bin')),
      hardware_validated: false, data: b64(fwsc), stock });
    notices.push('FM-1 V15 recovery firmware: https://www.m-vave.com/download (PC Firmware / FM-1 V15). Distributed by M-VAVE under Apache-2.0. Retain applicable licenses and notices; components with separate licenses retain their terms. See THIRD_PARTY.md.');
  }

  const wasm = readFileSync(opts['--wasm']);
  const simulation = compact({ schema: 1, release_version: release, sha256: sha256(wasm), source_sha256: sourceHash(root),
    sample_bank: 'synthesized-demo', data: b64(wasm) });
  let web = readText(join(root, 'sdk/Update.html.in'));
  if (preview) web = web.replace(/<a\b[^>]*href="\/\*FM1_SOURCE_URL\*\/"[^>]*>(.*?)<\/a>/g, '<span>$1 (local preview)</span>');
  for (const name of ['THIRD_PARTY.md', 'LICENSE.md']) {
    const text = readText(join(root, name));
    if (name === 'THIRD_PARTY.md' && !text.startsWith('# Third-party notices')) fail('Use the public distribution notices.');
    notices.push(`\n===== ${name} =====\n${text}`);
  }
  const replacements = [
    ['/*FM1_SOURCE_URL*/', htmlEncode(sourceUrl)],
    ['/*FM1_PAYLOAD*/', `const FM1_PACKAGE = ${packageJson ?? 'null'};`],
    ['/*FM1_SIMULATOR*/', `const FM1_SIMULATOR = ${simulation};`],
    ['/*FM1_I18N*/', readText(join(root, 'sdk/I18n.js'))],
    ['/*FM1_SCRIPT*/', readText(join(root, 'sdk/WebUpdate.js'))],
    ['/*FM1_SIM_SCRIPT*/', readText(join(root, 'sdk/Simulator.js'))],
    ['/*FM1_SAMPLE_SCRIPT*/', readText(join(root, 'sdk/Samples.js'))],
    ['/*FM1_NOTICES*/', htmlEncode(notices.join('\n'))],
  ];
  for (const [key, value] of replacements) web = web.split(key).join(value);
  if (/\/\*FM1_[A-Z_]+\*\//.test(web)) fail('Unexpanded HTML placeholder.');

  const outdir = opts['-o'];
  mkdirSync(outdir, { recursive: true });
  const page = join(outdir, preview ? 'index.html' : 'FM1-Official-Open-Source.html');
  for (const stale of [page, join(outdir, 'SHA256SUMS.txt')]) if (existsSync(stale)) rmSync(stale);
  writeFileSync(page, web, 'utf8');
  if (preview) { console.log(`Preview: ${page}`); return; }
  const digest = shaFile(page);
  writeFileSync(join(outdir, 'SHA256SUMS.txt'), `${digest}  ${basename(page)}\n`);
  console.log(`Integrated FM1 workbench: ${page}`);
  console.log(`sha256 ${digest}   (firmware ${sha256(fwsc).slice(0, 16)}..., simulator ${sha256(wasm).slice(0, 16)}...)`);
}

const opts = parseArgs(process.argv.slice(2));
if (!opts) { console.error('usage: page.mjs --root SRC --fwsc F --identity build-identity.json --wasm W --source-url URL -o OUTDIR | --root SRC --wasm W --preview -o OUTDIR'); process.exit(2); }
try { build(opts); } catch (error) { console.error(`error: ${error.message}`); process.exit(1); }
