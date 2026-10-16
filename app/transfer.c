/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#include "instrument.h"
#include "fm1_engine_control.h"
#include <stdlib.h>
#include <string.h>

/* Project transfer protocol: F0 7D 'F' 'M' 02 cmd token-lo token-hi packed checksum F7.
 * Packing is an LSB-first stream of 8-bit bytes in 7-bit MIDI data. Checksum
 * makes the sum from 7D through checksum zero modulo 128. Replies set cmd bit
 * 6, and begin with status (0 success, 1 empty, 64-error for negative errors).
 * One request at a time; exact retries receive the cached reply. Each reply
 * fits the SDK's 128-byte outgoing queue. Audio never handles file transfers.
 */
#ifdef FM1_HOST_TEST
#include <stdatomic.h>
#define TRANSFER_WORD _Atomic(unsigned)
#else
#define TRANSFER_WORD volatile unsigned
#endif
static struct { uint8_t bytes[192]; TRANSFER_WORD length; } incoming;
static uint8_t request[192], reply[96];
static unsigned request_length, reply_length, reply_pending;
static uint32_t reply_started, last_activity;
static uint8_t *transfer;
static unsigned transfer_size, transferred, transfer_kind, transfer_target, transfer_writing;
static void barrier(void)
{
#ifndef FM1_HOST_TEST
    __asm__ volatile("csync" ::: "memory");
#endif
}
void fm1_app_sysex(const uint8_t *m, size_t n)
{
    if (!m || n < 6 || n > sizeof incoming.bytes || m[0] != 0xf0 || m[n - 1] != 0xf7 ||
        incoming.length)
        return;
    if (!(n >= 8 && m[1] == 0x7d && m[2] == 'F' && m[3] == 'M' && m[4] == 2) &&
        !(n == 6 && m[1] == 0x7f && (m[2] == 0 || m[2] == 0x7f) && m[3] == 6) &&
        !(n == 163 && m[1] == 0x43 && m[2] < 16 && m[3] == 0 && m[4] == 1 && m[5] == 27))
        return;
    memcpy(incoming.bytes, m, n);
    barrier();
    incoming.length = (unsigned)n;
    fm1_control_request();
}
static void transfer_cancel(void)
{
    free(transfer);
    transfer = NULL;
    transfer_size = transferred = transfer_writing = 0;
}
void groove_transfer_init(void)
{
    transfer_cancel();
    incoming.length = request_length = reply_length = reply_pending = 0;
}
static int transfer_begin(unsigned kind, unsigned target, unsigned size, unsigned writing)
{
    if (kind > 3 || (kind == 1 && (target >= GROOVE_TRACKS || size != 156)) ||
        (kind == 2 && (target >= GROOVE_TRACKS || size != sizeof(struct groove_preset))) ||
        (!kind && (target || (size != GROOVE_PROJECT_BYTES && size != GROOVE_PROJECT_LEGACY_BYTES &&
                              (!writing || size != GROOVE_PROJECT_TUNED_BYTES)))) ||
        (kind == 3 && (target || size != GROOVE_PROJECT_TUNED_BYTES)))
        return GROOVE_PROJECT_INVALID;
    if (transfer && transfer_size == size) {
        /* A new session replaces the old one; equal sizes need no new block. */
        memset(transfer, 0, size);
    } else {
        uint8_t *next = calloc(1, size);
        if (!next)
            return GROOVE_PROJECT_MEMORY;
        transfer_cancel();
        transfer = next;
    }
    transfer_size = size;
    transferred = 0;
    transfer_kind = kind;
    transfer_target = target;
    transfer_writing = writing;
    return 0;
}
static unsigned get16(const uint8_t *p) { return p[0] | (unsigned)p[1] << 8; }
static uint32_t get32(const uint8_t *p)
{ return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void put32(uint8_t *p, uint32_t value)
{ for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (i * 8U)); }
static int command(unsigned cmd, const uint8_t *data, unsigned n, uint8_t *out, unsigned *count)
{
    int r = GROOVE_PROJECT_INVALID;
    *count = 0;
    if (cmd == 20 && !n) {
        put32(out, groove_master_state());
        *count = 4; return 0;
    }
    if (cmd == 19 && !n) {
        struct fm1_audio_stats stats; fm1_audio_stats_read(&stats);
        put32(out, stats.blocks); put32(out+4, stats.short_intervals);
        put32(out+8, stats.overruns); put32(out+12, stats.peak_ticks);
        *count = 16; return 0;
    }
    if (cmd == 18 && !n) {
        struct fm1_render_stats s; groove_audio_stats(&s);
        put32(out, s.render_ticks); put32(out+4, s.peak_ticks);
        put32(out+8, s.midi_ticks); put32(out+12, s.adapted);
        put32(out+16, s.overloads); put32(out+20, s.voice_capacity);
        put32(out+24, s.master_gain); put32(out+28, s.deadline_ticks);
        *count = 32; return 0;
    }
    if (cmd == 17 && !n) {
        struct fm1_engine_stats stats;
        fm1_engine_stats_read(&stats);
        put32(out, stats.submitted); put32(out + 4, stats.completed);
        put32(out + 8, stats.underruns); put32(out + 12, stats.midi_dropped);
        out[16] = (uint8_t)groove_samples_status();
        out[17] = (uint8_t)groove_edit_recording();
        *count = 18; return 0;
    }
    if (cmd == 11 && !n) {
        out[0] = (uint8_t)groove_samples_status();
        put32(out + 1, groove_samples_size());
        /* Region size, not the new-bank cap: SDK clients require this exact value.
         * groove_samples_begin rejects banks over GROOVE_SAMPLE_LIMIT. */
        put32(out + 5, GROOVE_SAMPLE_BYTES);
        *count = 9; return 0;
    }
    if (cmd == 12 && n == 4) return groove_samples_begin(get32(data));
    if (cmd == 13 && n > 4 && n <= 132) return groove_samples_write(get32(data), data + 4, n - 4);
    if (cmd == 14 && !n) return groove_samples_finish();
    if (cmd == 15 && n == 5 && data[4] <= 64) {
        r = groove_samples_read(get32(data), out, data[4]);
        if (!r) *count = data[4];
        return r;
    }
    if (cmd == 16 && !n) { groove_samples_cancel(); return 0; }
    if (cmd == 0 && !n) {
        out[0] = 4;
        out[1] = GROOVE_PROJECT_SLOTS;
        out[2] = (uint8_t)groove_project_slot();
        out[3] = GROOVE_PROJECT_BYTES & 255;
        out[4] = GROOVE_PROJECT_BYTES >> 8;
        out[5] = (uint8_t)groove_clock_mode();
        for (unsigned t = 0; t < GROOVE_TRACKS; ++t)
            out[6 + t] = (uint8_t)groove_source_type(t);
        *count = 14;
        return 0;
    }
    if (cmd == 1 && n == 2) {
        unsigned size = data[0] == 2 ? sizeof(struct groove_preset) : data[0] == 1 ? 156
                        : data[0] == 3 ? GROOVE_PROJECT_TUNED_BYTES : GROOVE_PROJECT_BYTES;
        r = transfer_begin(data[0], data[1], size, 0);
        if (!r) {
            r = data[0] == 2 ? groove_timbre_export(data[1], transfer, size)
                : data[0] == 1 ? groove_fm_copy(data[1], transfer, NULL)
                        : groove_project_export(transfer, size);
            if (r)
                transfer_cancel();
            else {
                out[0] = (uint8_t)size;
                out[1] = (uint8_t)(size >> 8);
                *count = 2;
            }
        }
    } else if (cmd == 2 && n == 3 && transfer && !transfer_writing) {
        unsigned at = get16(data), bytes = data[2];
        if (bytes && bytes <= 64 && at <= transfer_size && bytes <= transfer_size - at) {
            memcpy(out, transfer + at, bytes);
            *count = bytes;
            r = 0;
        }
    } else if (cmd == 3 && n == 4)
        r = transfer_begin(data[0], data[1], get16(data + 2), 1);
    else if (cmd == 4 && n > 2 && n <= 66 && transfer && transfer_writing) {
        unsigned at = get16(data), bytes = n - 2;
        if (at == transferred && at <= transfer_size && bytes <= transfer_size - at) {
            memcpy(transfer + at, data + 2, bytes);
            transferred += bytes;
            r = 0;
        }
    } else if (cmd == 5 && n == 4 && transfer && transfer_writing && transferred == transfer_size) {
        if (get32(data) == ~groove_crc32(~0U, transfer, transfer_size)) {
            r = transfer_kind == 2 ? groove_timbre_import(transfer_target, transfer, transfer_size)
                : transfer_kind == 1 ? groove_fm_import(transfer_target, transfer)
                              : groove_project_import(transfer, transfer_size);
            if (!r)
                transfer_cancel();
        }
    } else if (cmd == 6 && !n) {
        transfer_cancel();
        r = 0;
    } else if (cmd == 7 && n == 1)
        r = groove_project_save_slot(data[0]);
    else if (cmd == 8 && n == 1)
        r = groove_project_load_slot(data[0]);
    else if (cmd == 9 && n == 1) {
        char name[13];
        r = groove_project_info(data[0], name);
        if (r >= 0) {
            memcpy(out, name, 12);
            *count = 12;
        }
    } else if (cmd == 10 && n == 12) {
        for (unsigned i = 0; i < n; ++i)
            if (data[i] < 32 || data[i] > 126)
                return GROOVE_PROJECT_INVALID;
        for (unsigned i = 0; i < n; ++i)
            groove_project_rename(i, (char)data[i]);
        r = 0;
    }
    return r;
}
void groove_transfer_poll(void)
{
    uint32_t now = fm1_millis();
    if (transfer && now - last_activity > 10000U)
        transfer_cancel();
    if (reply_pending) {
        if (!fm1_midi_send(reply, reply_length) || now - reply_started > 2000U)
            reply_pending = 0;
        else
            return;
    }
    unsigned n = incoming.length;
    if (!n)
        return;
    barrier();
    if (request_length == n && !memcmp(request, incoming.bytes, n) && reply_length) {
        incoming.length = 0;
        reply_pending = 1;
        reply_started = last_activity = now;
        if (!fm1_midi_send(reply, reply_length)) reply_pending = 0;
        return;
    }
    memcpy(request, incoming.bytes, n);
    barrier();
    incoming.length = 0;
    request_length = n;
    reply_length = 0;
    for (unsigned i = 1; i + 1 < n; ++i)
        if (request[i] >= 128)
            return;
    if (request[1] == 0x7f) {
        groove_screen_mmc(request[4]);
        return;
    }
    if (request[1] == 0x43) {
        unsigned sum = 0;
        for (unsigned i = 6; i < 162; ++i)
            sum += request[i];
        unsigned track = request[2] == 9 ? 7 : request[2];
        if (!(sum & 127U) && track < 8 && request[2] == groove_channel(track)) {
            uint8_t patch[156];
            memcpy(patch, request + 6, 155);
            patch[155] = 63;
            (void)groove_fm_import(track, patch);
        }
        return;
    }
    if (n < 10 || request[5] >= 64)
        return;
    unsigned sum = 0;
    for (unsigned i = 1; i + 1 < n; ++i)
        sum += request[i];
    if (sum & 127U)
        return;
    uint8_t data[132], response[65];
    unsigned bits = 0, value = 0, bytes = 0;
    for (unsigned i = 8; i + 2 < n; ++i) {
        value |= (unsigned)request[i] << bits;
        bits += 7;
        if (bits >= 8) {
            if (bytes == sizeof data)
                return;
            data[bytes++] = (uint8_t)value;
            value >>= 8;
            bits -= 8;
        }
    }
    if (value || bits >= 7)
        return;
    unsigned count = 0;
    int r = command(request[5], data, bytes, response + 1, &count);
    response[0] = (uint8_t)(r < 0 ? 64 - r : r);
    memcpy(reply, request, 8);
    reply[5] |= 64;
    unsigned at = 8;
    value = bits = 0;
    for (unsigned i = 0; i <= count; ++i) {
        value |= (unsigned)response[i] << bits;
        bits += 8;
        while (bits >= 7) {
            reply[at++] = value & 127U;
            value >>= 7;
            bits -= 7;
        }
    }
    if (bits)
        reply[at++] = value & 127U;
    sum = 0;
    for (unsigned i = 1; i < at; ++i)
        sum += reply[i];
    reply[at++] = (uint8_t)((0U - sum) & 127U);
    reply[at++] = 0xf7;
    reply_length = at;
    reply_pending = 1;
    reply_started = last_activity = now;
    if (!fm1_midi_send(reply, reply_length)) reply_pending = 0;
}
