/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* Tuning persistence (tasks 5.1-5.4 of engine-tuning-hook): capture/apply, the
 * 462,848-byte sample cap, the tuning sector, and SysEx project transfer with
 * the optional tuning block. Exits non-zero on failure.
 *
 * transfer.c is #included with its external symbols renamed so the test can
 * capture replies (the simulator's fm1_midi_send discards them). */
#include "internal.h"
#define fm1_app_sysex t_app_sysex
#define groove_transfer_init t_transfer_init
#define groove_transfer_poll t_transfer_poll
#define fm1_midi_send t_midi_send
int t_midi_send(const uint8_t *data, size_t bytes);
#include "../../app/transfer.c"
#undef fm1_app_sysex
#undef groove_transfer_init
#undef groove_transfer_poll
#undef fm1_midi_send
#include <stdio.h>
#include <stdlib.h>

void sim_boot(unsigned clear);
const float *sim_render(void);
uint8_t *sim_flash(void);

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static uint8_t last_reply[256];
static unsigned last_reply_length;
int t_midi_send(const uint8_t *data, size_t bytes)
{
    if (!data || !bytes || bytes > sizeof last_reply) return -1;
    memcpy(last_reply, data, bytes);
    last_reply_length = (unsigned)bytes;
    return 0;
}

typedef struct groove_tuning tun8[GROOVE_TRACKS];
enum { TRACK = 0, TUNING_SECTOR = 4096 };

static void fill(tun8 t, int base)
{
    for (unsigned i = 0; i < GROOVE_TRACKS; ++i)
        for (unsigned k = 0; k < GROOVE_TUNING_KEYS; ++k) {
            t[i].main[k] = (int8_t)(base + (int)i * 3 + (int)k);
            t[i].alt[k] = (int8_t)(-base - (int)i * 2 - (int)k);
        }
}
static void set_all(const tun8 t) { for (unsigned i = 0; i < GROOVE_TRACKS; ++i) CHECK(groove_tuning_set(i, &t[i]) == 0); }
static int tables_are(const tun8 t)
{
    tun8 got;
    CHECK(groove_tuning_capture(got) == 0);
    return !memcmp(got, t, sizeof got);
}
static int tables_zero(void)
{
    tun8 z;
    memset(z, 0, sizeof z);
    return tables_are(z);
}
static void zero_tables(void)
{
    tun8 z;
    memset(z, 0, sizeof z);
    CHECK(groove_tuning_apply(z) == 0);
}
static void fresh(void) /* clear storage, melodic track 0 */
{
    sim_boot(3);
    CHECK(groove_source_set(TRACK, GROOVE_PLUCK) == 0);
}
/* Reboot the engine on the same Flash (no clear, no screen). */
static void reboot(void) { sim_boot(2); }
static uint32_t render_hash(void)
{
    uint32_t crc = ~0U;
    groove_note(TRACK, 60, 100, 0);
    for (unsigned b = 0; b < 48; ++b) crc = groove_crc32(crc, sim_render(), FM1_BLOCK_WORDS * sizeof(float));
    return ~crc;
}

/* ---- 5.1 ---- */
static void capture_apply(void)
{
    fresh();
    tun8 a, got, z;
    fill(a, -40);
    memset(z, 0, sizeof z);
    CHECK(tables_zero());
    set_all(a);
    memset(got, 0x55, sizeof got);
    CHECK(groove_tuning_capture(got) == 0);
    CHECK(!memcmp(got, a, sizeof a));                       /* set -> capture */
    zero_tables();
    CHECK(tables_zero());                                   /* zero */
    CHECK(groove_tuning_apply(got) == 0);                   /* apply */
    for (unsigned i = 0; i < GROOVE_TRACKS; ++i) {
        struct groove_tuning one;
        groove_tuning_get(i, &one);
        CHECK(!memcmp(&one, &a[i], sizeof one));            /* get */
    }
    /* -128 anywhere (last track, last key; main and alt) changes nothing. */
    tun8 b;
    fill(b, 10);
    b[GROOVE_TRACKS - 1].alt[GROOVE_TUNING_KEYS - 1] = -128;
    CHECK(groove_tuning_apply(b) < 0);
    CHECK(tables_are(a));
    fill(b, 10);
    b[5].main[0] = -128;
    CHECK(groove_tuning_apply(b) < 0);
    CHECK(tables_are(a));
    CHECK(groove_tuning_apply(NULL) < 0);
    CHECK(groove_tuning_capture(NULL) < 0);
    CHECK(tables_are(a));
    /* +127 / -127 are legal. */
    fill(b, 0);
    b[0].main[0] = 127; b[7].alt[11] = -127;
    CHECK(groove_tuning_apply(b) == 0 && tables_are(b));
}

/* ---- sample bank helpers ---- */
static uint8_t *make_bank(unsigned chunks, unsigned *bytes_out)
{
    unsigned bytes = 512 + chunks * 228;
    uint8_t *b = calloc(1, bytes);
    uint32_t *h = (uint32_t *)(void *)b; /* magic, version, bytes, crc, commit, rate */
    h[0] = 0x53314d46U; h[1] = 1; h[2] = bytes; h[5] = 32000;
    unsigned next = 512, left = chunks;
    for (unsigned i = 0; i < 16; ++i) {
        uint32_t *e = (uint32_t *)(void *)(b + 32 + i * 28); /* offset, frames, bytes, name[16] */
        unsigned n = i < 8 ? (i == 7 ? left : chunks / 8) : 0;
        left -= n;
        e[0] = next; e[1] = n * 320; e[2] = n * 228;
        if (n) { b[32 + i * 28 + 12] = 'P'; b[32 + i * 28 + 13] = (uint8_t)('0' + i); }
        next += n * 228;
    }
    for (unsigned p = 512; p < bytes; p += 228) { b[p] = 1; b[p + 1] = 0x45; b[p + 2] = 20; b[p + 3] = 0x5a; }
    uint32_t crc = ~groove_crc32(~0U, b, bytes); /* crc and commit fields are still zero */
    memcpy(b + 12, &crc, 4);
    *bytes_out = bytes;
    return b;
}
/* Write a bank straight into the simulated Flash and let the driver validate it. */
static unsigned install_bank(unsigned chunks, unsigned *bytes_out)
{
    unsigned bytes;
    uint8_t *b = make_bank(chunks, &bytes);
    memcpy(sim_flash() + GROOVE_SAMPLE_OFFSET, b, bytes);
    free(b);
    groove_samples_init();
    for (unsigned i = 0; i < 4096 && groove_samples_status() == 1; ++i) groove_samples_poll();
    *bytes_out = bytes;
    return groove_samples_status();
}

/* ---- 5.2 ---- */
static void sample_cap(void)
{
    CHECK(GROOVE_SAMPLE_LIMIT == 462848 && GROOVE_SAMPLE_BYTES == 466944);
    CHECK(GROOVE_TUNING_OFFSET == GROOVE_FLASH_BYTES - 4096);
    CHECK(GROOVE_TUNING_OFFSET == GROOVE_SAMPLE_OFFSET + GROOVE_SAMPLE_LIMIT);
    fresh();
    CHECK(groove_samples_begin(462849) < 0);
    CHECK(groove_samples_begin(GROOVE_SAMPLE_BYTES) < 0);
    CHECK(groove_samples_begin(511) < 0);
    CHECK(groove_samples_begin(462848) == 0);               /* exactly the cap */
    CHECK(groove_samples_status() == 3);
    groove_samples_cancel();
    /* SysEx cmd 11 still reports the full region: SDK clients require it. */
    uint8_t out[16];
    unsigned count = 0;
    CHECK(command(11, NULL, 0, out, &count) == 0 && count == 9);
    CHECK(get32(out + 5) == GROOVE_SAMPLE_BYTES);

    /* A bank at the largest size that fits validates and reads back. */
    fresh();
    unsigned bytes;
    CHECK(install_bank(2027, &bytes) == 2 && bytes <= GROOVE_SAMPLE_LIMIT);
    CHECK(groove_samples_size() == bytes);
    /* An existing over-size bank (pre-cap) still validates and reads. */
    fresh();
    CHECK(install_bank(2028, &bytes) == 2 && bytes > GROOVE_SAMPLE_LIMIT);
    CHECK(groove_samples_size() == bytes);
    uint8_t got[64];
    CHECK(groove_samples_read(bytes - 64, got, 64) == 0);
    CHECK(!memcmp(got, sim_flash() + GROOVE_SAMPLE_OFFSET + bytes - 64, 64));
}

/* ---- 5.3 ---- */
static const uint8_t *tuning_sector(void) { return sim_flash() + GROOVE_TUNING_OFFSET; }
static void sector_persistence(void)
{
    fresh();
    tun8 a, b;
    fill(a, 20);
    fill(b, -30);
    set_all(a);
    CHECK(groove_project_save_slot(2) == 0);
    set_all(b);
    CHECK(groove_project_save_slot(5) == 0);
    /* Layout: 'FM1T', version 1, slot entries of 200 B starting at +8. */
    CHECK(!memcmp(tuning_sector(), "FM1T", 4) && tuning_sector()[4] == 1);
    CHECK(!memcmp(tuning_sector() + 8 + 2 * 200 + 4, a, sizeof a));
    CHECK(!memcmp(tuning_sector() + 8 + 5 * 200 + 4, b, sizeof b));
    reboot();
    CHECK(tables_zero());                                   /* tables are not project state */
    CHECK(groove_project_load_slot(2) == 0 && tables_are(a));
    CHECK(groove_project_load_slot(5) == 0 && tables_are(b));
    CHECK(groove_project_load_slot(2) == 0 && tables_are(a));

    /* Sound check: a loaded tuned slot renders differently from the zero one. */
    uint32_t tuned = (reboot(), groove_project_load_slot(2), render_hash());
    uint32_t again = (reboot(), groove_project_load_slot(2), render_hash());
    CHECK(tuned == again);                                  /* deterministic */
    CHECK(groove_project_save_slot(9) == 0);                /* current tables a (loaded) */
    reboot();
    CHECK(groove_project_load_slot(9) == 0 && tables_are(a));

    /* Same slot, identical project content, different tables: the entry is
     * rewritten with the tables at save time (the project record is unchanged). */
    set_all(b);
    CHECK(groove_project_save_slot(2) == 0);
    reboot();
    CHECK(groove_project_load_slot(2) == 0 && tables_are(b));

    /* Replacing the slot with different content rewrites the entry with the
     * current tables. */
    tun8 c;
    fill(c, 7);
    set_all(c);
    groove_tempo_edit(5);
    CHECK(groove_project_save_slot(2) == 0);
    reboot();
    CHECK(groove_project_load_slot(2) == 0 && tables_are(c));
    zero_tables();
    groove_tempo_edit(5);
    CHECK(groove_project_save_slot(2) == 0);                /* saved with zero tables */
    set_all(a);
    CHECK(groove_project_load_slot(2) == 0 && tables_zero());

    /* A replacement that did not touch the sector (e.g. an older firmware) leaves a
     * stale entry; its generation no longer matches, so it loads as zero. */
    set_all(a);
    groove_tempo_edit(5);
    CHECK(groove_project_save_slot(3) == 0);
    uint8_t keep[4096];
    memcpy(keep, tuning_sector(), sizeof keep);
    groove_tempo_edit(-5);
    set_all(b);
    CHECK(groove_project_save_slot(3) == 0);
    CHECK(fm1_flash_erase(GROOVE_TUNING_OFFSET) == 0);
    CHECK(fm1_flash_write(GROOVE_TUNING_OFFSET, keep, sizeof keep) == 0);
    set_all(c);
    CHECK(groove_project_load_slot(3) == 0 && tables_zero());
    CHECK(groove_project_load_slot(5) == 0 && tables_are(b)); /* other slots unaffected */

    /* Corrupting one byte of one entry only zeros that slot. */
    fresh();
    set_all(a);
    CHECK(groove_project_save_slot(4) == 0);
    set_all(b);
    CHECK(groove_project_save_slot(6) == 0);
    uint8_t zero = 0;
    CHECK(tuning_sector()[8 + 4 * 200 + 4 + 13] != 0);
    CHECK(fm1_flash_write(GROOVE_TUNING_OFFSET + 8 + 4 * 200 + 4 + 13, &zero, 1) == 0);
    set_all(c);
    CHECK(groove_project_load_slot(4) == 0 && tables_zero());
    CHECK(groove_project_load_slot(6) == 0 && tables_are(b));

    /* Factory projects always get zero tables. */
    set_all(a);
    CHECK(groove_factory_load(0) == 0 && tables_zero());
}
static void pre_change_projects(void)
{
    /* A project saved before this change has no tuning sector (blank Flash) or
     * a header that is not ours: zero tables, and the render is untouched. */
    fresh();
    zero_tables();
    CHECK(groove_project_save_slot(7) == 0);
    reboot();
    CHECK(groove_project_load_slot(7) == 0);
    uint32_t reference = render_hash();
    tun8 a;
    fill(a, 25);
    CHECK(fm1_flash_erase(GROOVE_TUNING_OFFSET) == 0);      /* blank: no sector */
    reboot();
    set_all(a);
    CHECK(groove_project_load_slot(7) == 0 && tables_zero());
    CHECK(render_hash() == reference);
    /* Wrong magic / wrong version over a previously valid sector. */
    set_all(a);
    CHECK(groove_project_save_slot(7) == 0);
    uint8_t zero = 0;
    CHECK(fm1_flash_write(GROOVE_TUNING_OFFSET + 1, &zero, 1) == 0);
    set_all(a);
    CHECK(groove_project_load_slot(7) == 0 && tables_zero());
    CHECK(fm1_flash_erase(GROOVE_TUNING_OFFSET) == 0);
    set_all(a);
    CHECK(groove_project_save_slot(7) == 0);                /* rebuilds the sector */
    CHECK(fm1_flash_write(GROOVE_TUNING_OFFSET + 4, &zero, 1) == 0);
    set_all(a);
    CHECK(groove_project_load_slot(7) == 0 && tables_zero());
    /* Tuned project loads differently from the pre-change one, same engine. */
    fresh();
    zero_tables();
    CHECK(groove_project_save_slot(1) == 0);
    set_all(a);
    CHECK(groove_project_save_slot(2) == 0);
    reboot();
    CHECK(groove_project_load_slot(1) == 0);
    uint32_t h1 = render_hash();
    reboot();
    CHECK(groove_project_load_slot(2) == 0 && tables_are(a));
    CHECK(render_hash() != h1);
}
static void bank_overlap(void)
{
    fresh();
    tun8 a, b;
    fill(a, 12);
    fill(b, -9);
    set_all(a);
    CHECK(groove_project_save_slot(3) == 0);                /* valid entry exists */
    uint8_t before[4096];
    unsigned bytes;
    CHECK(install_bank(2028, &bytes) == 2 && bytes > GROOVE_SAMPLE_LIMIT);
    memcpy(before, tuning_sector(), sizeof before);         /* now bank data */
    set_all(b);
    CHECK(groove_project_save_slot(4) == 0);                /* project save still succeeds */
    CHECK(!memcmp(before, tuning_sector(), sizeof before)); /* sector untouched */
    CHECK(groove_samples_status() == 2 && groove_samples_size() == bytes);
    set_all(b);
    CHECK(groove_project_load_slot(3) == 0 && tables_zero()); /* loads as zero */
    CHECK(groove_project_load_slot(4) == 0 && tables_zero());
    /* A bank within the cap does not block. */
    fresh();
    CHECK(install_bank(2027, &bytes) == 2);
    set_all(a);
    CHECK(groove_project_save_slot(4) == 0);
    set_all(b);
    CHECK(groove_project_load_slot(4) == 0 && tables_are(a));
    CHECK(groove_samples_status() == 2);
}

/* ---- 5.4: SysEx transfer ---- */
static unsigned token;
/* Send one framed request; return its status and payload (response[1..]). */
static int sysex(unsigned cmd, const uint8_t *data, unsigned n, uint8_t *out, unsigned *out_n)
{
    uint8_t m[192];
    unsigned at = 0, bits = 0, value = 0, sum = 0;
    m[at++] = 0xf0; m[at++] = 0x7d; m[at++] = 'F'; m[at++] = 'M'; m[at++] = 2;
    m[at++] = (uint8_t)cmd;
    ++token;
    m[at++] = token & 127U; m[at++] = (token >> 7) & 127U;
    for (unsigned i = 0; i < n; ++i) {
        value |= (unsigned)data[i] << bits;
        bits += 8;
        while (bits >= 7) { m[at++] = value & 127U; value >>= 7; bits -= 7; }
    }
    if (bits) m[at++] = value & 127U;
    for (unsigned i = 1; i < at; ++i) sum += m[i];
    m[at++] = (uint8_t)((0U - sum) & 127U);
    m[at++] = 0xf7;
    last_reply_length = 0;
    t_app_sysex(m, at);
    t_transfer_poll();
    if (!last_reply_length) return -999;
    /* Decode the reply payload. */
    uint8_t dec[96];
    unsigned d = 0;
    bits = value = 0;
    for (unsigned i = 8; i + 2 < last_reply_length; ++i) {
        value |= (unsigned)last_reply[i] << bits;
        bits += 7;
        if (bits >= 8) { dec[d++] = (uint8_t)value; value >>= 8; bits -= 8; }
    }
    if (out_n) *out_n = d - 1;
    if (out && d > 1) memcpy(out, dec + 1, d - 1);
    return dec[0] >= 64 ? 64 - dec[0] : dec[0];
}
static int tx_export(unsigned kind, uint8_t *buf, unsigned *size)
{
    uint8_t req[2] = {(uint8_t)kind, 0}, out[96];
    unsigned n;
    int r = sysex(1, req, 2, out, &n);
    if (r) return r;
    *size = out[0] | (unsigned)out[1] << 8;
    for (unsigned at = 0; at < *size; at += 64) {
        unsigned chunk = *size - at < 64 ? *size - at : 64;
        uint8_t q[3] = {(uint8_t)at, (uint8_t)(at >> 8), (uint8_t)chunk};
        r = sysex(2, q, 3, out, &n);
        if (r || n != chunk) return r ? r : -998;
        memcpy(buf + at, out, chunk);
    }
    return 0;
}
static int tx_import(unsigned kind, const uint8_t *buf, unsigned size, unsigned crc_xor)
{
    uint8_t out[96], req[4] = {(uint8_t)kind, 0, (uint8_t)size, (uint8_t)(size >> 8)};
    unsigned n;
    int r = sysex(3, req, 4, out, &n);
    if (r) return r;
    for (unsigned at = 0; at < size; at += 64) {
        unsigned chunk = size - at < 64 ? size - at : 64;
        uint8_t q[66] = {(uint8_t)at, (uint8_t)(at >> 8)};
        memcpy(q + 2, buf + at, chunk);
        r = sysex(4, q, 2 + chunk, out, &n);
        if (r) return r;
    }
    uint32_t crc = ~groove_crc32(~0U, buf, size) ^ crc_xor;
    uint8_t c[4] = {(uint8_t)crc, (uint8_t)(crc >> 8), (uint8_t)(crc >> 16), (uint8_t)(crc >> 24)};
    return sysex(5, c, 4, out, &n);
}
static uint32_t block_checksum(const uint8_t *block) { return ~groove_crc32(~0U, block, 196); }
static void put_block_crc(uint8_t *block) { uint32_t c = block_checksum(block); memcpy(block + 196, &c, 4); }

static void sysex_transfer(void)
{
    static uint8_t full[GROOVE_PROJECT_TUNED_BYTES], plain[GROOVE_PROJECT_BYTES], bad[GROOVE_PROJECT_TUNED_BYTES];
    unsigned size;
    fresh();
    t_transfer_init();
    CHECK(GROOVE_PROJECT_TUNED_BYTES == 4264);
    tun8 a, b, z;
    fill(a, 33);
    fill(b, -17);
    memset(z, 0, sizeof z);
    set_all(a);
    CHECK(tx_export(3, full, &size) == 0 && size == 4264);
    CHECK(!memcmp(full + 4064, "FM1T", 4));
    CHECK(!memcmp(full + 4068, a, sizeof a));
    CHECK(!memcmp(full + 4260, (uint32_t[]){block_checksum(full + 4064)}, 4));
    /* Old export stays block-less and byte-identical to the project part. */
    CHECK(tx_export(0, plain, &size) == 0 && size == 4064);
    CHECK(!memcmp(plain, full, 4064));
    /* Round trip: the tables come back on a different instance. */
    set_all(b);
    CHECK(tx_import(3, full, sizeof full, 0) == 0);
    CHECK(tables_are(a));
    set_all(b);
    CHECK(tx_import(0, full, sizeof full, 0) == 0 && tables_are(a)); /* kind 0 accepts the new size */
    /* Block-less import: zero tables. */
    set_all(b);
    CHECK(tx_import(0, plain, sizeof plain, 0) == 0);
    CHECK(tables_zero());
    /* Kind 3 demands the new size; export and import sizes outside the set fail. */
    CHECK(tx_import(3, plain, sizeof plain, 0) != 0);
    /* A corrupt block (bad CRC, bad magic, -128 entry) is rejected as a whole. */
    set_all(b);
    memcpy(bad, full, sizeof bad);
    bad[4070] ^= 1;
    CHECK(tx_import(3, bad, sizeof bad, 0) == GROOVE_PROJECT_INVALID && tables_are(b));
    memcpy(bad, full, sizeof bad);
    bad[4064] ^= 1; put_block_crc(bad + 4064);
    CHECK(tx_import(3, bad, sizeof bad, 0) == GROOVE_PROJECT_INVALID && tables_are(b));
    memcpy(bad, full, sizeof bad);
    bad[4068 + 5] = 0x80; put_block_crc(bad + 4064);
    CHECK(tx_import(3, bad, sizeof bad, 0) == GROOVE_PROJECT_INVALID && tables_are(b));
    /* Transport CRC mismatch changes nothing either. */
    CHECK(tx_import(3, full, sizeof full, 1) != 0 && tables_are(b));
    /* The saved-slot entry and the transferred tables agree after saving. */
    CHECK(tx_import(3, full, sizeof full, 0) == 0);
    CHECK(groove_project_save_slot(8) == 0);
    set_all(b);
    CHECK(groove_project_load_slot(8) == 0 && tables_are(a));
    t_transfer_init();
}

int main(void)
{
    capture_apply();
    sample_cap();
    sector_persistence();
    pre_change_projects();
    bank_overlap();
    sysex_transfer();
    if (failures) { printf("storage_test: %d failure(s)\n", failures); return 1; }
    printf("storage_test: ok\n");
    return 0;
}
