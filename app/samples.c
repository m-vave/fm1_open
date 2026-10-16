/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#include "fm1_engine_control.h"
#include "fm1_device.h"
#include "instrument.h"
#include "sampledrum.h"
#include <string.h>

/* Single bank, no hidden second copy. CRC and a last-written commit word make
 * an interrupted import visibly incomplete. Projects occupy separate sectors.
 * The browser retains/downloads a complete bank before replacing one slot. */
struct sample_entry {
    uint32_t offset, frames, bytes;
    char name[16];
};
static struct sample_header {
    uint32_t magic, version, bytes, crc, commit, rate, reserved[2];
    struct sample_entry entry[16];
    uint8_t padding[32];
} header;
_Static_assert(sizeof header == 512, "sample bank header");
static unsigned state, total, position;
static uint32_t checksum, changed_at;
static uint32_t crc32(uint32_t crc, unsigned offset, const uint8_t *p, unsigned bytes)
{
    /* Treat the stored CRC and commit word as zero, including split writes. */
    static const uint8_t zero[8] = {0};
    if (offset < 12) {
        unsigned n = bytes < 12 - offset ? bytes : 12 - offset;
        crc = groove_crc32(crc, p, n);
        p += n; offset += n; bytes -= n;
    }
    if (offset < 20 && bytes) {
        unsigned n = bytes < 20 - offset ? bytes : 20 - offset;
        crc = groove_crc32(crc, zero, n);
        p += n; bytes -= n;
    }
    return groove_crc32(crc, p, bytes);
}
static void unbind(void)
{
    fm1_engine_pause();
    groove_samples_stop();
    for (unsigned i = 0; i < SAMPLE_DRUM_PADS; ++i)
        (void)sampledrum_bind(i, NULL, 0, 0, NULL);
    fm1_engine_resume();
}
static int valid(void)
{
    if (header.magic != 0x53314d46U || header.version != 1 || header.commit ||
        header.rate != 32000 || header.bytes < sizeof header ||
        header.bytes > GROOVE_SAMPLE_BYTES || header.reserved[0] || header.reserved[1])
        return 0;
    for (unsigned i = 0; i < sizeof header.padding; ++i)
        if (header.padding[i])
            return 0;
    unsigned next = sizeof header;
    for (unsigned i = 0; i < 16; ++i) {
        const struct sample_entry *s = &header.entry[i];
        if (s->offset != next || s->frames > SAMPLE_DRUM_MAX_FRAMES ||
            (i >= SAMPLE_DRUM_PADS && (s->frames || s->name[0])) ||
            s->bytes != ((s->frames + 319U) / 320U) * 228U || s->bytes > header.bytes - next ||
            s->name[15])
            return 0;
        for (unsigned n = 0; n < 15; ++n)
            if ((uint8_t)s->name[n] && (s->name[n] < 32 || s->name[n] > 126))
                return 0;
        next += s->bytes;
    }
    return next == header.bytes;
}
static const uint8_t *mapped_data(void)
{
    const uint8_t *mapped = fm1_flash_map(GROOVE_SAMPLE_OFFSET, total);
    if (!mapped)
        return NULL;
    for (unsigned p = sizeof header; p < total; p += 228U)
        if (mapped[p] != 1 || mapped[p + 1] != 0x45 || mapped[p + 2] != 20 || mapped[p + 3] != 0x5a)
            return NULL;
    return mapped;
}
static int bind(void)
{
    const uint8_t *mapped = mapped_data();
    if (!mapped)
        return GROOVE_PROJECT_INVALID;
    fm1_engine_pause();
    for (unsigned i = 0; i < SAMPLE_DRUM_PADS; ++i) {
        const struct sample_entry *s = &header.entry[i];
        (void)sampledrum_bind(i, s->frames ? mapped + s->offset : NULL, s->bytes, s->frames,
                              s->name);
    }
    fm1_engine_resume();
    state = 2;
    return 0;
}
void groove_samples_init(void)
{
    unbind();
    state = total = position = 0;
    checksum = ~0U;
    state = 1;
}
void groove_samples_poll(void)
{
    if (state == 3 && fm1_millis() - changed_at > 10000U)
        groove_samples_cancel();
    if (state != 1)
        return;
    if (!total) {
        if (fm1_flash_size() != GROOVE_FLASH_BYTES) {
            state = 4;
            return;
        }
        int r = fm1_flash_read(GROOVE_SAMPLE_OFFSET, &header, sizeof header);
        if (r == FM1_FLASH_BUSY)
            return;
        if (r) {
            state = 4;
            return;
        }
        if (!valid()) {
            state = 0;
            return;
        }
        total = header.bytes;
    }
    uint8_t bytes[512];
    for (unsigned budget = 0; budget < 8 && position < total; ++budget) {
        unsigned n = total - position;
        if (n > sizeof bytes)
            n = sizeof bytes;
        int r = fm1_flash_read(GROOVE_SAMPLE_OFFSET + position, bytes, n);
        if (r == FM1_FLASH_BUSY) return;
        if (r) {
            state = 4;
            return;
        }
        checksum = crc32(checksum, position, bytes, n);
        position += n;
    }
    if (position == total && (~checksum != header.crc || bind()))
        state = 4;
}
unsigned groove_samples_status(void)
{
    return state;
}
unsigned groove_samples_size(void)
{
    return state == 2 ? total : 0;
}
int groove_samples_begin(unsigned bytes)
{
    if (bytes < sizeof header || bytes > GROOVE_SAMPLE_LIMIT ||
        fm1_flash_size() != GROOVE_FLASH_BYTES)
        return GROOVE_PROJECT_INVALID;
    unbind();
    /* Erase the commit first, before any old sample bytes can be overwritten. */
    groove_edit_record(0, 0, 0);
    fm1_engine_pause();
    fm1_engine_stop();
    fm1_engine_resume();
    int r = fm1_flash_erase(GROOVE_SAMPLE_OFFSET);
    if (r) {
        state = 4;
        return r;
    }
    state = 3;
    total = bytes;
    position = 0;
    checksum = ~0U;
    changed_at = fm1_millis();
    memset(&header, 0, sizeof header);
    return 0;
}
int groove_samples_write(unsigned offset, const uint8_t *data, unsigned bytes)
{
    if (state != 3 || !data || !bytes || bytes > 128 || offset != position ||
        bytes > total - position)
        return GROOVE_PROJECT_INVALID;
    uint8_t copy[128];
    memcpy(copy, data, bytes);
    if (offset < sizeof header) {
        unsigned n = sizeof header - offset;
        if (n > bytes)
            n = bytes;
        memcpy((uint8_t *)&header + offset, data, n);
        for (unsigned i = 0; i < bytes; ++i)
            if (offset + i >= 16 && offset + i < 20)
                copy[i] = 255;
    }
    /* Requests may straddle a sector. Erase only the as-yet-unwritten sector. */
    unsigned first = offset / 4096U, last = (offset + bytes - 1U) / 4096U;
    int r = 0;
    if (offset && !(offset % 4096U))
        r = fm1_flash_erase(GROOVE_SAMPLE_OFFSET + first * 4096U);
    if (!r && last != first)
        r = fm1_flash_erase(GROOVE_SAMPLE_OFFSET + last * 4096U);
    if (!r)
        r = fm1_flash_write(GROOVE_SAMPLE_OFFSET + offset, copy, bytes);
    if (r) {
        state = 4;
        return r;
    }
    checksum = crc32(checksum, offset, data, bytes);
    position += bytes;
    changed_at = fm1_millis();
    return 0;
}
int groove_samples_finish(void)
{
    if (state != 3 || position != total || header.bytes != total || !valid() ||
        ~checksum != header.crc)
        return GROOVE_PROJECT_INVALID;
    /* Validate codec headers before committing, including data just written
     * through the raw Flash API and not yet visible in the mapped cache. */
    int r = fm1_flash_invalidate(GROOVE_SAMPLE_OFFSET, total);
    if (!r && !mapped_data())
        r = GROOVE_PROJECT_INVALID;
    uint32_t commit = 0;
    if (!r)
        r = fm1_flash_write(GROOVE_SAMPLE_OFFSET + 16, &commit, sizeof commit);
    if (!r)
        r = bind();
    if (r)
        state = 4;
    return r;
}
void groove_samples_cancel(void)
{
    if (state == 3)
        state = 4;
}
int groove_samples_read(unsigned offset, uint8_t *data, unsigned bytes)
{
    if (state != 2 || !data || !bytes || bytes > 64 || offset > total || bytes > total - offset)
        return GROOVE_PROJECT_INVALID;
    return fm1_flash_read(GROOVE_SAMPLE_OFFSET + offset, data, bytes);
}
