/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#include "instrument.h"
#include "tuning_hook.h"
#include "fm_patch.h"
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
static struct { uint8_t bytes[192], source; TRANSFER_WORD length; } incoming;
static struct reply_cache {
    uint8_t request[192], reply[96];
    unsigned request_length, reply_length, pending;
    uint32_t started;
} replies[3];
static unsigned session_source;
static uint32_t last_activity;
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
    if (n == 6 && m[1] == 0x7d && m[2] == 'F' && m[3] == 'S') {
        groove_screen_mirror_hello(m[4]); /* a big-screen app is listening (1: send the whole screen) */
        return;
    }
    if (!(n >= 8 && m[1] == 0x7d && m[2] == 'F' && m[3] == 'M' && m[4] == 2) &&
        !(n == 6 && m[1] == 0x7f && (m[2] == 0 || m[2] == 0x7f) && m[3] == 6) &&
        !(n == 163 && m[1] == 0x43 && m[2] < 16 && m[3] == 0 && m[4] == 1 && m[5] == 27))
        return;
    incoming.source = (uint8_t)fm1_midi_input_source();
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
    incoming.length = 0; memset(replies,0,sizeof replies);
    session_source = FM1_MIDI_USB;
}
static int transfer_begin(unsigned kind, unsigned target, unsigned size, unsigned writing)
{
    if (kind > 4 || (kind == 4 && (target >= 4 || size != 4096)) || (kind == 1 && (target >= GROOVE_TRACKS || size != 156)) ||
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
    /* Additive capability reply; command 21 retains its old exact layout. */
    if (cmd == 22 && !n) {
        out[0]=1; out[1]=128; out[2]=4; out[3]=(uint8_t)groove_fm_library_available();
        put32(out+4,GROOVE_SAMPLE_LIMIT); out[8]=3; /* session roles and extended/synchronized delay */
        *count=9; return 0;
    }
    if (cmd == 21 && !n) {
        out[0] = groove_tuning_capture && groove_tuning_apply ? 1 : 0;
        out[1] = GROOVE_PROJECT_TUNED_BYTES & 255;
        out[2] = GROOVE_PROJECT_TUNED_BYTES >> 8;
        put32(out + 3, GROOVE_SAMPLE_BYTES - 4096);
        *count = 7; return 0;
    }
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
                        : data[0] == 4 ? 4096 : data[0] == 3 ? GROOVE_PROJECT_TUNED_BYTES : GROOVE_PROJECT_BYTES;
        r = transfer_begin(data[0], data[1], size, 0);
        if (!r) {
            r = data[0] == 4 ? groove_fm_library_read(data[1],transfer)
                : data[0] == 2 ? groove_timbre_export(data[1], transfer, size)
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
            r = transfer_kind == 4 ? groove_fm_library_write(transfer_target,transfer)
                : transfer_kind == 2 ? groove_timbre_import(transfer_target, transfer, transfer_size)
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
    /* Retry each port independently. A blocked BLE reply never delays USB
     * notes or its transfer ACK. Cached retries also belong to that port. */
    for (unsigned p=0;p<3;++p) if (replies[p].pending) {
        int r=fm1_midi_send_port(p,replies[p].reply,replies[p].reply_length);
        if (r!=-2 || now-replies[p].started>2000U) replies[p].pending=0;
    }
    unsigned n = incoming.length;
    if (!n) return;
    barrier();
    unsigned source=incoming.source;
    struct reply_cache *c=&replies[source];
    if (c->pending) return;
    if (c->request_length == n && !memcmp(c->request, incoming.bytes, n) && c->reply_length) {
        incoming.length = 0;
        c->started=now;
        if (source==session_source || (!transfer && groove_samples_status()!=3)) last_activity=now;
        c->pending=fm1_midi_send_port(source,c->reply,c->reply_length)==-2;
        return;
    }
    /* Keep the last file reply across MMC, voice dumps and rejected frames.
     * These can arrive while the sender is retrying a lost chunk ACK. Hold
     * the incoming mailbox until validation has consumed all of its bytes. */
    const uint8_t *message = incoming.bytes;
    for (unsigned i = 1; i + 1 < n; ++i)
        if (message[i] >= 128)
            goto discard;
    if (message[1] == 0x7f) {
        unsigned code = message[4];
        barrier();
        incoming.length = 0;
        if (groove_midi_clock_accept(source, code == 1 ? 0xfc : 0xfa)) groove_screen_mmc(code);
        return;
    }
    if (message[1] == 0x43) {
        unsigned sum = 0;
        for (unsigned i = 6; i < 162; ++i)
            sum += message[i];
        if (!(sum & 127U)) {
            uint8_t patch[156];
            unsigned channel = message[2];
            memcpy(patch, message + 6, 155);
            patch[155] = 63;
            barrier();
            incoming.length = 0;
            for (unsigned track=0;track<GROOVE_TRACKS;++track)
                if (groove_track_role(track) != GROOVE_TRACK_OFF && channel == groove_midi_input_channel(track))
                    (void)groove_fm_import(track, patch);
            return;
        }
        goto discard;
    }
    if (n < 10 || message[5] >= 64)
        goto discard;
    unsigned sum = 0;
    for (unsigned i = 1; i + 1 < n; ++i)
        sum += message[i];
    if (sum & 127U)
        goto discard;
    uint8_t data[132], response[65];
    unsigned bits = 0, value = 0, bytes = 0;
    for (unsigned i = 8; i + 2 < n; ++i) {
        value |= (unsigned)message[i] << bits;
        bits += 7;
        if (bits >= 8) {
            if (bytes == sizeof data)
                goto discard;
            data[bytes++] = (uint8_t)value;
            value >>= 8;
            bits -= 8;
        }
    }
    if (value || bits >= 7)
        goto discard;
    if ((transfer || groove_samples_status() == 3) && source != session_source) {
        uint8_t busy[12]; memcpy(busy,message,8); busy[5]|=64;
        busy[8]=64-FM1_FLASH_BUSY; busy[9]=0; unsigned sum=0;
        for (unsigned i=1;i<10;++i) sum+=busy[i];
        busy[10]=(uint8_t)((0U-sum)&127U); busy[11]=0xf7;
        (void)fm1_midi_send_port(source,busy,sizeof busy);
        goto discard;
    }
    memcpy(c->request, message, n);
    barrier();
    incoming.length = 0;
    c->request_length = n;
    c->reply_length = 0;
    unsigned count = 0;
    int r = command(c->request[5], data, bytes, response + 1, &count);
    if (!r && (transfer || groove_samples_status() == 3)) session_source = source;
    response[0] = (uint8_t)(r < 0 ? 64 - r : r);
    memcpy(c->reply, c->request, 8);
    c->reply[5] |= 64;
    unsigned at = 8;
    value = bits = 0;
    for (unsigned i = 0; i <= count; ++i) {
        value |= (unsigned)response[i] << bits;
        bits += 8;
        while (bits >= 7) {
            c->reply[at++] = value & 127U;
            value >>= 7;
            bits -= 7;
        }
    }
    if (bits)
        c->reply[at++] = value & 127U;
    sum = 0;
    for (unsigned i = 1; i < at; ++i)
        sum += c->reply[i];
    c->reply[at++] = (uint8_t)((0U - sum) & 127U);
    c->reply[at++] = 0xf7;
    c->reply_length = at;
    c->started = last_activity = now;
    c->pending = fm1_midi_send_port(source,c->reply,c->reply_length)==-2;
    return;
discard:
    barrier();
    incoming.length = 0;
}
