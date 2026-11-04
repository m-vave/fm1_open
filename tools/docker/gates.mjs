#!/usr/bin/env node
// SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
//
// Host-side half of the Docker build: a dependency-free Node port of everything in
// sdk/Build.ps1 that does not need the cross toolchain to run, plus the 453,921
// byte app-image cap from sdk/Package.cs.
//
//   node gates.mjs inputs   <src> <toolchain>   SDK payload, DSP contract, toolchain identity
//   node gates.mjs prepare  <src> <out>         sources.txt, build_identity.c, linker script
//   node gates.mjs firmware <src> <out>         Test-Firmware symbol gates + image cap + report
//
// Exit status 1 with `error: ...` on the first failed gate, like Build.ps1's throw.
import { createHash } from 'node:crypto';
import { readFileSync, writeFileSync, mkdirSync, readdirSync, existsSync } from 'node:fs';
import { join, relative, dirname, sep } from 'node:path';
import { fileURLToPath } from 'node:url';
import { portedFromProblem } from '../portable/guard.mjs';

const HERE = dirname(fileURLToPath(import.meta.url));
const PROFILE = 'dx7';
const VARIANT = { userBytes: 536576, frozen: 'sdk/lib/libfm1_engine.a', updateFamily: 7 };

class GateError extends Error {}
const fail = (message) => { throw new GateError(message); };
const sha256 = (data) => createHash('sha256').update(data).digest('hex');
const sha256File = (file) => sha256(readFileSync(file));
const readJson = (file) => JSON.parse(readFileSync(file, 'utf8').replace(/^﻿/, ''));
const readText = (file) => readFileSync(file, 'utf8').replace(/^﻿/, '');

// Get-ChildItem -Recurse -File; hidden (dot) names are skipped unless `force`.
function walk(dir, { force = false } = {}) {
  const found = [];
  const visit = (current) => {
    for (const entry of readdirSync(current, { withFileTypes: true })) {
      if (!force && entry.name.startsWith('.')) continue;
      const full = join(current, entry.name);
      if (entry.isDirectory()) visit(full);
      else if (entry.isFile()) found.push(full);
    }
  };
  visit(dir);
  return found;
}
const posixRel = (root, file) => relative(root, file).split(sep).join('/');
// Sort-Object FullName: case-insensitive, '\' separators. This approximates the
// culture-aware PowerShell order; it only feeds the 32-byte build id and the
// object order, and is identical to a byte sort for the baseline file names.
const windowsKey = (rel) => rel.replaceAll('/', '\\').toLowerCase();
const byWindowsOrder = (a, b) => (windowsKey(a) < windowsKey(b) ? -1 : windowsKey(a) > windowsKey(b) ? 1 : 0);

function readVersion(src) {
  const text = readText(join(src, 'app/version.h'));
  const version = /^#define FM1_RELEASE_VERSION "(V(?:0[1-9]|[1-9][0-9]{1,2}))"\s*$/m.exec(text);
  if (!version) fail('Set FM1_RELEASE_VERSION in app/version.h.');
  const product = /^#define FM1_UPDATE_PRODUCT "(FM-1_8[0-9]{6})"\s*$/m.exec(text);
  if (!product) fail('Set a compatible FM1_UPDATE_PRODUCT in app/version.h.');
  return { release: version[1], product: product[1] };
}

// The cap is computed from sdk/Package.cs, so it cannot drift from the packager.
function appImageCap(src) {
  const text = readText(join(src, 'sdk/Package.cs'));
  const constant = (name, text2) => {
    const m = new RegExp(`\\b${name}\\s*=\\s*(0x[0-9a-fA-F]+|\\d+)`).exec(text2);
    if (!m) fail(`Cannot find ${name} in sdk/Package.cs.`);
    return Number(m[1]);
  };
  const expression = /app\.Length > FlashLimit-UserSize-BtSize-MinimumVm-Area-AppData-(\d+)-(\d+)\s*\|\|/.exec(text);
  if (!expression) fail('Package.cs image-size check changed shape; re-derive the cap in gates.mjs.');
  return constant('FlashLimit', text) - constant('UserSize', text) - constant('BtSize', text) -
    constant('MinimumVm', text) - constant('Area', text) - constant('AppData', text) -
    Number(expression[1]) - Number(expression[2]);
}

// ---------------------------------------------------------------- inputs
function checkInputs(src, toolchain) {
  const drift = portedFromProblem(src, ['sdk/Build.ps1', 'sdk/Package.cs']);
  if (drift) fail(drift);
  const manifest = readJson(join(src, 'sdk/manifest.json'));
  if (manifest.abi !== 131072) fail('This application requires the matching 128-frame SDK.');
  const contract = readJson(join(HERE, 'toolchain.json'));
  if (contract.schema !== 1) fail('Invalid Linux toolchain contract.');
  const external = manifest.external_toolchain;
  if (!external || external.schema !== 1 || !external.files) fail('Invalid external toolchain manifest.');
  for (const required of ['bin/clang.exe', 'bin/pi32v2-ld.exe', 'bin/LLVMgold.dll', 'bin/llvm-nm.exe', 'bin/llvm-objcopy.exe']) {
    if (!(required in external.files)) fail(`Missing toolchain contract: ${required}`);
  }
  for (const [name, expected] of Object.entries(external.files)) {
    if (!/^(bin|lib)\/[A-Za-z0-9_./-]+$/.test(name) || name.split('/').includes('..') || !/^[a-f0-9]{64}$/.test(expected)) {
      fail('Invalid external toolchain entry.');
    }
    if (name.startsWith('bin/')) {
      if (!(name in contract.windows_equivalents)) fail(`No Linux equivalent recorded for ${name}`);
      continue;
    }
    // Shared compiler headers: identical to Windows 2.5.3 once LF becomes CRLF.
    const file = join(toolchain, 'common', name);
    if (!existsSync(file)) fail(`External toolchain does not match the SDK: ${name}`);
    const crlf = readFileSync(file).toString('latin1').replace(/\r\n/g, '\n').replace(/\n/g, '\r\n');
    if (sha256(Buffer.from(crlf, 'latin1')) !== expected) fail(`External toolchain does not match the SDK: ${name}`);
  }
  for (const [name, expected] of Object.entries(contract.files)) {
    const file = join(toolchain, name);
    if (!existsSync(file) || sha256File(file) !== expected) {
      fail(`Linux toolchain does not match toolchain.json: ${name}. Expected ${contract.release}.`);
    }
  }

  const synth = readJson(join(src, 'sdk/synth.json'));
  if (synth.schema !== 4 || synth.api !== 8 || synth.kernel_abi !== 3 || synth.block_frames !== 128 || synth.sample_rate !== 44100 ||
      synth.sdk_block_sha256 !== sha256File(join(src, 'sdk/include/fm1_block.h')) ||
      synth.native_sha256 !== sha256File(join(src, 'sdk/lib/libfm1_engine.a'))) {
    fail('Native DSP kernels and SDK audio contracts do not match. Install a matching SDK package.');
  }
  if (!/^#define FM1_DSP_ABI 3U\s*$/m.test(readText(join(src, 'app/synth/engine.h')))) fail('Unsupported native DSP ABI.');
  for (const [name, expected] of Object.entries(synth.portable_contracts ?? {})) {
    if (!/^[A-Za-z0-9_]+\.inc$/.test(name) || expected !== sha256File(join(src, 'app/synth', name))) {
      fail('Fixed DSP coefficients differ from the native kernels. Matching kernels are required for this coefficient change.');
    }
  }
  if (!synth.sram_required?.length || !synth.sram_optional?.length) fail('Native DSP SRAM contract is missing.');
  for (const name of [...synth.sram_required, ...synth.sram_optional]) {
    if (!/^[A-Za-z_][\w.$]*$/.test(name)) fail('Invalid DSP SRAM symbol.');
  }

  const sdk = join(src, 'sdk');
  for (const [name, expected] of Object.entries(manifest.files)) {
    const full = join(src, name);
    if (!full.startsWith(sdk + sep) || !existsSync(full) || sha256File(full) !== expected) fail(`SDK payload changed: ${name}`);
  }
  for (const file of walk(sdk, { force: true })) {
    const rel = posixRel(src, file);
    if (rel === 'sdk/manifest.json' || rel in manifest.files) continue;
    if (rel.endsWith('/.DS_Store')) continue; // macOS Finder litter, not SDK content
    fail(`Unexpected SDK file: ${rel}`);
  }
  const { release, product } = readVersion(src);
  console.log(`inputs ok: FM1 Official Open ${release} (${product}), SDK payload, DSP contract, ${contract.release}`);
}

// --------------------------------------------------------------- prepare
function prepare(src, out) {
  const { release, product } = readVersion(src);
  const manifestHash = sha256File(join(src, 'sdk/manifest.json'));
  const appDir = join(src, 'app');
  const synthDir = join(appDir, 'synth') + sep;
  const appFiles = walk(appDir).map((file) => posixRel(src, file)).sort(byWindowsOrder);
  const sources = appFiles.filter((rel) => /\.(c|S)$/.test(rel) && !join(src, rel).startsWith(synthDir));
  mkdirSync(join(out, 'generated'), { recursive: true });
  writeFileSync(join(out, 'generated/sources.txt'), sources.join('\n') + '\n');

  const identityInputs = [PROFILE, manifestHash, ...appFiles.map((rel) => `${rel}:${sha256File(join(src, rel))}`)];
  const buildId = sha256(Buffer.from(identityInputs.join('\n'), 'utf8'));
  const idBytes = Array.from({ length: 32 }, (_, i) => '0x' + buildId.slice(i * 2, i * 2 + 2)).join(',');
  if (VARIANT.updateFamily !== 7 || VARIANT.userBytes < 4096 || VARIANT.userBytes % 4096) fail('Invalid fixed firmware layout.');
  writeFileSync(join(out, 'generated/build_identity.c'),
    '#include "layout.h"\n' +
    `const struct fm1_firmware_layout fm1_layout = {${VARIANT.userBytes}U,${VARIANT.updateFamily}U};\n` +
    `const unsigned char fm1_build_id[32] = {${idBytes}};\n` +
    `const unsigned char fm1_update_identity[27] = "${product}";\n`);
  writeFileSync(join(out, 'build-identity.json'), JSON.stringify({ profile: PROFILE, family: 'release', release_version: release, build_id: buildId }, null, 2));
  const linker = readText(join(src, 'sdk/config/linker.ld.in'))
    .replaceAll('@SBC_DECODER@', 'sbc_decoder\n').replaceAll('@ACL_TX_EXTRA@', ' + 1024').replaceAll('@ACL_RX_EXTRA@', ' + 4096');
  writeFileSync(join(out, 'generated/linker'), linker);
  console.log(`prepare ok: ${sources.length} sources, build id ${buildId.slice(0, 16)}...`);
}

// -------------------------------------------------------------- firmware
const SRAM_LO = 0x01c00000;
const SRAM_HI = 0x01c80000;
const MIN_HEAP = 49152;

function testFirmware(src, out) {
  const elfPath = join(out, 'sdk.elf');
  const reportPath = join(out, 'firmware-report.json');
  writeFileSync(reportPath, '{"passed":false}');
  const synth = readJson(join(src, 'sdk/synth.json'));
  const nm = readText(join(out, 'nm.txt'));
  const info = new Map();
  const heap = {};
  let usbMidi = false;
  for (const line of nm.split(/\r?\n/)) {
    const m = /^([0-9a-fA-F]+)[ \t]+(([0-9a-fA-F]+)[ \t]+)?([A-Za-z])[ \t]+(.+)$/.exec(line);
    if (!m) continue;
    const name = m[5];
    const address = parseInt(m[1], 16);
    const size = m[3] ? parseInt(m[3], 16) : null;
    const kind = m[4];
    info.set(name, { address, size, kind });
    if (/^(__fat_|fat_sdfile_fat_ops$|jl_fat_vfs_ops$|nor_sdfile_vfs_ops$|sdfile_ext_vfs_ops$)/.test(name)) {
      fail(`Unused filesystem entered the fixed Flash runtime: ${name}`);
    }
    if (/HEAP/.test(name) || (/heap/.test(name) && kind === 'A')) heap[name] = address;
    if (/^lv_/.test(name)) fail(`Unexpected LVGL runtime: ${name}`);
    if (/usb.*midi|midi.*usb/.test(name)) usbMidi = true;
  }
  const has = (name) => info.has(name);

  // Table entries are discovered by ranges, so native archives must retain them.
  const registrations = [['initcall', 4, 4], ['early_initcall', 16, 4], ['late_initcall', 4, 4],
    ['platform_initcall', 4, 4], ['module_initcall', 4, 4], ['sys_event_handler', 12, 12],
    ['syscfg_ops', 84, 28], ['lp_target', 72, 8], ['sys_cpu_timer', 16, 16],
    ['clock_critical_handler', 16, 8], ['vfs_ops', 120, 120], ['audio_decoder', 56, 56], ['audio_hwaccel', 32, 32]];
  for (const [name, minimum, stride] of registrations) {
    let begin = `_${name}_begin`, end = `_${name}_end`;
    if (!has(begin) || !has(end)) { begin = `${name}_begin`; end = `${name}_end`; }
    if (!has(begin) || !has(end)) fail(`Runtime registration range missing: ${name}`);
    const size = info.get(end).address - info.get(begin).address;
    if (size < minimum || size % stride) fail(`Runtime registration table invalid: ${name}`);
    if (name === 'vfs_ops' && size !== 120) fail('Expected only the internal Flash filesystem registration.');
  }
  const required = ['fm1_r_7919135c9963', 'fm1_r_1f449c2c7b8f', 'vSoftwareInterruptISR', 'fm1_r_e7d3da61dac2', 'fm1_r_1993f9b103e3', 'fm1_r_05431712fb7b',
    'fm1_update_init', 'fm1_update_message', 'fm1_update_busy', 'fm1_build_id', 'fm1_flash_init',
    'sdfile_vfs_ops', 'sdfile_init', 'syscfg_vm_init', 'syscfg_btif_init',
    'wifi_get_xosc', 'wifi_get_mcs_dgain', 'wifi_get_pa_trim_data',
    'connectivity_wl82_bluetooth_start', 'ble_profile_init', 'att_server_init',
    'fm1_ble_set_role', 'fm1_ble_scan', 'fm1_ble_connect', 'fm1_ble_disconnect', 'fm1_ble_get_status', 'fm1_ble_get_device',
    'audio_app_usb_microphone_enable', 'audio_app_usb_speaker_enable',
    'connectivity_audio_usb_microphone_enable', 'connectivity_audio_usb_speaker_enable',
    'audio_app_bluetooth_open', 'sbc_decoder', 'sbc_hwaccel', 'avdtp_packet_handler', 'bt_get_local_name', 'bt_get_mac_addr',
    'fm1_bluetooth_configure', 'fm1_bluetooth_get_status',
    'groove_tuning_capture', 'groove_tuning_apply', 'groove_project_apply_edit'];
  for (const name of required) if (!has(name)) fail(`Required runtime symbol absent: ${name}`);
  // Incoming connections also work with inert client stubs. Require the
  // fixed SDK's real discovery paths for A2DP reconnect and BLE HOST.
  for (const name of ['sdp_send_cmd_iotl', 'sdp_master_packet_handler', 'gatt_client_discover_primary_services_by_uuid128']) {
    const s = info.get(name);
    if (!s || !'TtNn'.includes(s.kind) || !(s.size > 16)) fail(`Bluetooth discovery implementation missing or stubbed: ${name}`);
  }
  // The instrument and UI must retain their real callbacks.
  for (const name of ['fm1_r_dee1ea411566', 'fm1_r_d16405f329ed', 'fm1_r_f03cc16d330d', 'fm1_r_1993f9b103e3',
    'fm1_io_trs_midi_enabled', 'fm1_r_05431712fb7b', 'fm1_r_0aa010fe1de1', 'fm1_r_33d5e5b63b6b', 'fm1_app_sysex',
    'fm1_app_midi_received']) {
    const s = info.get(name);
    // Build.ps1 notes the vendor nm prints SRAM-section functions as N, Flash text as T.
    if (!s || !'TtNn'.includes(s.kind) || !s.size) fail(`Application callback was lost or replaced by a weak default: ${name}`);
  }
  if (info.get('fm1_build_id')?.size !== 32) fail('Browser update build identity must be exactly 32 bytes.');
  if (info.get('fm1_update_identity')?.size !== 27) fail('FM1 update identity must be exactly 27 bytes.');
  if (!usbMidi) fail('USB MIDI runtime is missing.');
  for (const name of ['CurrentTCB', 'highCurrentTCB', 'xIdleTaskHandle']) {
    if (info.get(name)?.size !== 4) fail(`Platform RTOS ABI is invalid: ${name}`);
  }
  for (const name of ['__rt_local_irq_disable', '__rt_local_irq_enable', 'bt_lock_cnt',
    'fm1_r_83b2de623a96', 'fm1_r_c327ea18da09', 'cpuSuspend', 'audio_app_worker', 'edr_name', 'ble_name',
    'a2dp_source_event_handler', 'a2dp_source_sbc_codec', 'hfp_profile_init', 'hfp_ag_profile_init',
    'spp_user_send_data', 'avrcp_player_event', 'hid_diy_regiest_callback', 'obex_connection_req',
    'pan_cmd_io_ctrl', 'gatt_client_discover_primary_services', 'ancs_client_init', 'ams_client_init',
    'sdio_host_init', 'wl30_sdio_claim_irq', 'wl30_sdio_release_irq', 'wifi_on', 'wifi_off',
    'wifi_rx_task', 'wifi_tx_task', 'ui_yuv420_to_rgb565_pairs', 'ui_tiny_ttf_pack_scanline']) {
    if (has(name)) fail(`Unsupported platform runtime entered FM1: ${name}`);
  }
  const map = readText(join(out, 'sdk.map'));
  if (!map.includes(VARIANT.frozen + '(')) fail('Fixed native SDK archive missing.');
  if (map.includes('libfm1_runtime.a(')) fail('Only the fixed music/BLE SDK may be linked.');
  for (const item of ['/system.a(', 'audio_server.a(', 'media_app.a(', 'btstack.a(', 'lib_ui.a(', 'libjpeg_decode_only.a(']) {
    if (map.includes(item)) fail(`Unsupported FM1 link input: ${item}`);
  }
  for (const member of ['a2dp_source', 'ams_client', 'ancs_client', 'avctp', 'avrcp', 'bip', 'bnep', 'bt_audio',
    'client_user', 'hfp_ag_profile', 'hfp_profile', 'hid', 'iap_profile', 'obex', 'opp', 'pan',
    'pbg_profile', 'rfcomm', 'spp_profile', 'spp_update_profile', 'spp_user', 'vCard']) {
    if (map.includes(`.a(${member}.c.o)`)) fail(`Unsupported FM1 link member: ${member}`);
  }
  const acl = {};
  for (const direction of ['tx', 'rx']) {
    const begin = `acl_${direction}_pool`, end = `${begin}_end`;
    if (!has(begin) || !has(end)) fail(`Required controller pool missing: ${begin}`);
    acl[direction] = info.get(end).address - info.get(begin).address;
  }
  if (acl.tx < 1024 || acl.rx < 4096) fail('Classic ACL pools too small; need 1024/4096 bytes.');
  const heapMatch = /0x([0-9a-fA-F]+)[^\r\n]*_MALLOC_SIZE[ ]*=/.exec(map);
  if (!heapMatch) fail('Cannot find runtime heap in link map.');
  const heapBytes = parseInt(heapMatch[1], 16);
  if (heapBytes < MIN_HEAP) fail(`FM1 runtime heap is ${heapBytes} bytes; need at least ${MIN_HEAP}.`);

  const extra = ['groove_sequence_render', 'groove_sequence_clock', 'groove_sequence_position', 'groove_sequence_transport',
    'fm1_sea_decode_groups', 'fm1_codec_sea_mono_s16_run', ...synth.sram_required,
    'synth_dx7_render', 'synth_dx7_midi', 'synth_dx7_key', 'fm1_dx7_panic', 'fm_exp2_fixed', 'bass303_bank_render', 'bass303_bank_key',
    'bass303_bank_set', 'drum808_kit_render', 'drum808_kit_trigger', 'psg_render', 'psg_key', 'psg_midi', 'psg_panic', 'psg_set', 'psg_get',
    'pluck_render', 'pluck_key', 'pluck_midi', 'pluck_panic', 'pluck_set', 'pluck_get', 'fm1_mix128', 'fm1_mix_mono_stereo128',
    'fm1_bus_output128', 'fm1_master_mix128', 'fm1_master_gain', 'fm1_declick128', 'fm1_render_begin', 'fm1_render_end',
    'synth_dx7_active', 'bass303_bank_active', 'drum808_kit_active', 'psg_active', 'pluck_active', 'sampledrum_active',
    'synth_dx7_limit', 'psg_limit', 'pluck_limit', 'sampledrum_limit', 'bass303_advance_idle', 'fm1_send128', 'fm1_fx_render',
    'fm1_fx_clear', 'fm1_fx_active', 'sampledrum_render', 'sampledrum_trigger', 'sampledrum_panic', 'psg_active_chips'];
  const callbacks = ['fm1_r_e7d3da61dac2', 'fm1_r_1993f9b103e3', 'fm1_r_05431712fb7b', 'fm1_r_ffe3f82683f6', 'fm1_audio_irq', 'fm1_flash_worker_prepare',
    'fm1_r_7e12ed19990d', 'fm1_r_74310ca1fed9',
    'fm1_engine_render', 'fm1_engine_output_level', 'fm1_audio_copy', 'fm1_output_mix',
    'fm1_usb_audio_read', 'fm1_bluetooth_audio_read', 'fm1_usb_audio_write', 'connectivity_audio_get_speaker_volume', 'connectivity_audio_get_mic_volume',
    ...extra];
  for (const name of [...(synth.sram_optional ?? []), ...(synth.native_sram_optional ?? [])]) if (has(name)) callbacks.push(name);
  callbacks.push('cpu_enter_sfc_critical', 'cpu_exit_sfc_critical', 'enter_spi_code', 'exit_spi_code', 'fm1_r_da3931a5b7a8', 'sfc_drop_cache');
  // emu2149 helpers can inline into the mandatory psg_* SRAM callbacks.
  for (const name of ['audio_app_usb_mix', 'audio_app_usb_record', 'audio_app_usb_replace_input',
    'render_stereo', 'effects_render', 'effects_sample', 'sequence_event', 'clock_publish',
    'groove_arp_event', 'groove_arp_running', 'engine_note']) if (has(name)) callbacks.push(name);
  const engineReady = has('fm1_r_0aa010fe1de1') && info.get('fm1_r_0aa010fe1de1').address > 0;
  if (engineReady) {
    callbacks.push('fm1_r_0aa010fe1de1');
    if (has('fm1_r_33d5e5b63b6b') && info.get('fm1_r_33d5e5b63b6b').address > 0) callbacks.push('fm1_r_33d5e5b63b6b');
  }
  const inSram = (name) => info.get(name).address >= SRAM_LO && info.get(name).address < SRAM_HI;
  for (const name of callbacks) {
    if (!has(name)) fail(`Audio callback absent: ${name}`);
    if (!inSram(name)) fail(`Audio callback is outside internal SRAM: ${name}`);
  }
  for (const name of ['fm1_input_tick', 'fm1_panel_irq', 'fm1_scan_decode', 'fm1_key_pin', 'fm1_encoder_pin', 'fm1_quadrature']) {
    if (!has(name) || !inSram(name)) fail(`Panel scan code or wiring table is outside internal SRAM: ${name}`);
  }

  // ELF section walk.
  const bytes = readFileSync(elfPath);
  const range = (offset, count) => {
    if (offset < 0 || count < 0 || offset > bytes.length || count > bytes.length - offset) fail('Truncated ELF.');
  };
  const u16 = (o) => { range(o, 2); return bytes.readUInt16LE(o); };
  const u32 = (o) => { range(o, 4); return bytes.readUInt32LE(o); };
  range(0, 52);
  if (bytes.subarray(0, 6).join(',') !== '127,69,76,70,1,1') fail('Expected ELF32 little endian.');
  const entry = u32(24);
  if (entry !== 0x02000120 || info.get('_start')?.address !== entry || info.get('_text_rodata_begin')?.address !== entry) {
    fail('Firmware startup must match the fixed ROM entry and the first packaged instruction.');
  }
  const table = u32(32), stride = u16(46), count = u16(48), stringsIndex = u16(50);
  if (stride < 40 || !count || stringsIndex >= count) fail('Invalid ELF section table.');
  range(table, stride * count);
  const stringsHeader = table + stringsIndex * stride;
  const stringsOffset = u32(stringsHeader + 16), stringsBytes = u32(stringsHeader + 20);
  range(stringsOffset, stringsBytes);
  const sections = [];
  const imageParts = new Map();
  let payload = 0, appImage = 0;
  for (let i = 0; i < count; i++) {
    const header = table + i * stride;
    const nameOffset = u32(header), type = u32(header + 4), flags = u32(header + 8);
    const address = u32(header + 12), offset = u32(header + 16), size = u32(header + 20);
    if (type !== 8) range(offset, size);
    if (!(flags & 2) || !size) continue;
    if (nameOffset >= stringsBytes) fail('ELF section name is outside string table.');
    const start = stringsOffset + nameOffset;
    let end = start;
    while (end < stringsOffset + stringsBytes && bytes[end]) end++;
    if (end >= stringsOffset + stringsBytes) fail('Unterminated ELF section name.');
    const name = bytes.toString('ascii', start, end);
    if (!/^\.(text|data|bss|ram0_data|ram0_bss|boot_info|cache_ram_data|cache_ram_bss)$/.test(name)) fail(`Unsupported allocated section: ${name}`);
    const nobits = type === 8;
    if (!nobits) {
      payload += size;
      if (/^\.(text|data|ram0_data|cache_ram_data)$/.test(name)) { appImage += size; imageParts.set(name, bytes.subarray(offset, offset + size)); }
      else if (name !== '.boot_info') fail(`Allocated section is not covered by FM1 packaging: ${name}`);
    }
    sections.push({ name, address, bytes: size, nobits });
  }

  // sdk/Package.cs: Create() rejects an application larger than this.
  const cap = appImageCap(src);
  if (appImage > cap) fail(`App image is ${appImage} bytes; the FM1 layout allows at most ${cap} (sdk/Package.cs).`);

  // The packaged app image (objcopy -O binary of these sections, in Build.ps1's order). Unlike the
  // ELF it carries no path or tool metadata, so it is the hash to compare against a Windows build.
  const appImageSha = sha256(Buffer.concat(['.text', '.data', '.ram0_data', '.cache_ram_data'].filter((n) => imageParts.has(n)).map((n) => imageParts.get(n))));

  const report = {
    passed: true, elf_sha256: sha256(bytes), app_image_sha256: appImageSha, allocated_image_bytes: payload, app_image_bytes: appImage,
    app_image_cap_bytes: cap, app_image_headroom_bytes: cap - appImage,
    startup_entry_verified: true, startup_entry: entry, instrument_ready: engineReady,
    image_definition: 'Sum of ELF allocated non-NOBITS sections; excludes bootloader/filesystem packaging',
    sections, heap_symbols: heap, runtime_heap_bytes: heapBytes, minimum_runtime_heap_bytes: MIN_HEAP,
    classic_acl_tx_bytes: acl.tx, classic_acl_rx_bytes: acl.rx, registration_tables_verified: true, rtos_abi_verified: true,
    audio_callbacks_in_sram: true, extra_audio_symbols_in_sram: extra, bluetooth_and_usb_midi_linked: true,
    usb_audio_playback_and_capture_linked: true, bluetooth_audio_absent: false, ble_roles: 'peripheral/central', lvgl_absent: true,
    active_bluetooth_discovery_linked: true, application_callbacks_verified: true, board_validation: 'not performed',
    toolchain: 'jieli-linux-toolchains (see toolchain.json)',
  };
  writeFileSync(reportPath, JSON.stringify(report, null, 2));
  console.log(`FM1: ${appImage} bytes app image (cap ${cap}, headroom ${cap - appImage}); heap ${heapBytes} bytes (min ${MIN_HEAP}); SRAM/BLE/USB checks passed`);
}

const [command, a, b] = process.argv.slice(2);
try {
  if (command === 'inputs' && a && b) checkInputs(a, b);
  else if (command === 'prepare' && a && b) prepare(a, b);
  else if (command === 'firmware' && a && b) testFirmware(a, b);
  else {
    console.error('usage: gates.mjs inputs <src> <toolchain> | prepare <src> <out> | firmware <src> <out>');
    process.exit(2);
  }
} catch (error) {
  if (error instanceof GateError) { console.error(`error: ${error.message}`); process.exit(1); }
  throw error;
}
