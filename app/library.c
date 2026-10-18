/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#include "instrument.h"
#include "fm_patch.h"
#include "fm1_device.h"
#include "fm1_engine_control.h"
#include <string.h>

/* DX7 patch library. Each bank is the 4096-byte body of a 32-voice DX7 bank
 * dump. Program n is voice n % 32 of bank n / 32. The audio path only reads
 * through a mapping and a bank mask published by the control task, and every
 * write pauses the engine, so a program change never sees a half-written bank. */
enum { SAMPLE_BANK_MAGIC = 0x53314d46U };
static FM1_DSP_DATA const uint8_t *library_map;
static FM1_DSP_DATA unsigned library_usable;

static uint32_t bank_offset(unsigned bank)
{
    return GROOVE_TUNING_OFFSET - GROOVE_LIBRARY_BANK_BYTES * (bank + 1U);
}
/* Bank mask not overlapped by a stored sample bank; negative on a read error. */
static int usable_banks(void)
{
    uint32_t header[3];
    if (fm1_flash_size() < GROOVE_FLASH_BYTES)
        return 0;
    int r = fm1_flash_read(GROOVE_SAMPLE_OFFSET, header, sizeof header);
    if (r)
        return r;
    unsigned mask = 0;
    for (unsigned b = 0; b < GROOVE_LIBRARY_BANKS; ++b)
        if (header[0] != SAMPLE_BANK_MAGIC || header[2] <= bank_offset(b) - GROOVE_SAMPLE_OFFSET)
            mask |= 1U << b;
    return (int)mask;
}
void groove_library_refresh(void)
{
    int mask = usable_banks();
    const uint8_t *map = fm1_flash_map(GROOVE_LIBRARY_OFFSET,
                                       GROOVE_LIBRARY_BANK_BYTES * GROOVE_LIBRARY_BANKS);
    fm1_engine_pause();
    library_map = mask > 0 ? map : NULL;
    library_usable = mask > 0 ? (unsigned)mask : 0;
    fm1_engine_resume();
}
int groove_library_available(unsigned bank)
{
    int mask = usable_banks();
    return mask < 0 ? mask : bank < GROOVE_LIBRARY_BANKS && ((unsigned)mask >> bank & 1U);
}
int groove_library_read(unsigned bank, uint8_t data[GROOVE_LIBRARY_BANK_BYTES])
{
    int r = groove_library_available(bank);
    if (r <= 0) {
        if (!r && bank < GROOVE_LIBRARY_BANKS)
            memset(data, 255, GROOVE_LIBRARY_BANK_BYTES);
        return r < 0 ? r : bank < GROOVE_LIBRARY_BANKS ? 0 : GROOVE_PROJECT_INVALID;
    }
    return fm1_flash_read(bank_offset(bank), data, GROOVE_LIBRARY_BANK_BYTES);
}
/* A voice is valid 7-bit data or fully erased (an empty slot). */
static int bank_valid(const uint8_t *data)
{
    for (unsigned v = 0; v < 32; ++v) {
        const uint8_t *p = data + 128U * v;
        unsigned high = 0, erased = 1;
        for (unsigned i = 0; i < 128; ++i) {
            high |= p[i] & 128U;
            erased &= p[i] == 255;
        }
        if (high && !erased)
            return 0;
    }
    return 1;
}
int groove_library_write(unsigned bank, const uint8_t data[GROOVE_LIBRARY_BANK_BYTES])
{
    if (bank >= GROOVE_LIBRARY_BANKS || !data || !bank_valid(data))
        return GROOVE_PROJECT_INVALID;
    int r = groove_library_available(bank);
    if (r <= 0)
        return r ? r : GROOVE_PROJECT_LIBRARY_SPACE;
    uint32_t at = bank_offset(bank);
    fm1_engine_pause();
    r = fm1_flash_erase(at);
    if (!r)
        r = fm1_flash_write(at, data, GROOVE_LIBRARY_BANK_BYTES);
    if (!r)
        r = fm1_flash_invalidate(at, GROOVE_LIBRARY_BANK_BYTES);
    fm1_engine_resume();
    groove_library_refresh();
    return r;
}

/* VMEM <-> VCED, as in the DX7 manual and the workbench's readVoices. */
FM1_DSP_CODE void groove_library_unpack(const uint8_t b[128], uint8_t p[156])
{
    for (unsigned o = 0; o < 6; ++o) {
        const uint8_t *i = b + 17U * o;
        uint8_t *j = p + 21U * o;
        memcpy(j, i, 11);
        j[11] = i[11] & 3U; j[12] = (i[11] >> 2) & 3U;
        j[13] = i[12] & 7U; j[20] = (i[12] >> 3) & 15U;
        j[14] = i[13] & 3U; j[15] = (i[13] >> 2) & 7U;
        j[16] = i[14]; j[17] = i[15] & 1U; j[18] = (i[15] >> 1) & 31U; j[19] = i[16];
    }
    memcpy(p + 126, b + 102, 8);
    p[134] = b[110] & 31U; p[135] = b[111] & 7U; p[136] = (b[111] >> 3) & 1U;
    memcpy(p + 137, b + 112, 4);
    p[141] = b[116] & 1U; p[142] = (b[116] >> 1) & 7U; p[143] = (b[116] >> 4) & 7U;
    memcpy(p + 144, b + 117, 11);
    p[155] = 63;
}
void groove_library_pack(const uint8_t p[156], uint8_t b[128])
{
    for (unsigned o = 0; o < 6; ++o) {
        const uint8_t *j = p + 21U * o;
        uint8_t *i = b + 17U * o;
        memcpy(i, j, 11);
        i[11] = (uint8_t)((j[11] & 3U) | (j[12] & 3U) << 2);
        i[12] = (uint8_t)((j[13] & 7U) | (j[20] & 15U) << 3);
        i[13] = (uint8_t)((j[14] & 3U) | (j[15] & 7U) << 2);
        i[14] = j[16]; i[15] = (uint8_t)((j[17] & 1U) | (j[18] & 31U) << 1); i[16] = j[19];
    }
    memcpy(b + 102, p + 126, 8);
    b[110] = p[134] & 31U; b[111] = (uint8_t)((p[135] & 7U) | (p[136] & 1U) << 3);
    memcpy(b + 112, p + 137, 4);
    b[116] = (uint8_t)((p[141] & 1U) | (p[142] & 7U) << 1 | (p[143] & 7U) << 4);
    memcpy(b + 117, p + 144, 11);
    for (unsigned i = 0; i < 128; ++i)
        b[i] &= 127U;
}
static FM1_DSP_CODE const uint8_t *voice(unsigned program)
{
    unsigned bank = program / 32U;
    if (!library_map || program >= GROOVE_LIBRARY_VOICES || !(library_usable >> bank & 1U))
        return NULL;
    const uint8_t *v = library_map + (GROOVE_LIBRARY_BANKS - 1U - bank) * GROOVE_LIBRARY_BANK_BYTES +
                       (program % 32U) * 128U;
    for (unsigned i = 0; i < 128; ++i)
        if (v[i] & 128U)
            return NULL;
    return v;
}
FM1_DSP_CODE void groove_fm_program(uint8_t p[156], unsigned program)
{
    const uint8_t *v = voice(program);
    if (v) {
        groove_library_unpack(v, p);
        return;
    }
    /* DX7 INIT VOICE: one full-level sine carrier, algorithm 1. */
    memset(p, 0, 156);
    for (unsigned op = 0; op < 6; ++op) {
        uint8_t *o = p + 21U * op;
        memset(o, 99, 7); o[7] = 0; o[8] = 39; o[18] = 1; o[20] = 7;
    }
    p[5 * 21 + 16] = 99;
    memset(p + 126, 99, 4); memset(p + 130, 50, 4);
    p[136] = 1; p[137] = 35; p[141] = 1; p[143] = 3; p[144] = 24;
    memcpy(p + 145, "INIT VOICE", 10);
    p[155] = 63;
}
const char *groove_fm_program_name(unsigned program)
{
    static char name[11];
    const uint8_t *v = voice(program);
    memcpy(name, v ? (const char *)v + 118 : "INIT VOICE", 10);
    return name;
}
#ifdef FM1_HOST_TEST
unsigned groove_library_test_count = GROOVE_LIBRARY_VOICES; /* tests vary the count */
FM1_DSP_CODE unsigned groove_fm_program_count(void) { return groove_library_test_count; }
#else
FM1_DSP_CODE unsigned groove_fm_program_count(void) { return GROOVE_LIBRARY_VOICES; }
#endif
