/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* Patch library: the device DEFAULT project, the factory voice bank, VMEM
 * packing, INIT VOICE, sample bank overlap, program addressing, and library
 * bank transfer (kind 4) with the layout query (command 22). Exits non-zero on
 * failure. Writes build/host/library_vectors.bin for library_readvoices.mjs
 * and workbench_test.mjs.
 *
 * transfer.c is #included with its external symbols renamed so the test can
 * call command() directly; presets.c is #included a second time without the
 * web define, renamed, to check the device image's factory code. */
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
#undef FM1_OPEN_WEB
#define groove_factory_name device_factory_name
#define groove_factory_build device_factory_build
#define GROOVE_FACTORY_PROJECTS 1
#include "../../app/presets.c"
#undef groove_factory_name
#undef groove_factory_build
#undef GROOVE_FACTORY_PROJECTS
#include <stdio.h>
#include <stdlib.h>

void sim_boot(unsigned clear);
const float *sim_render(void);
uint8_t *sim_flash(void);
uint8_t *sim_file(void);
int sim_factory_bank(void);
extern unsigned groove_library_test_count;

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)
int t_midi_send(const uint8_t *data, size_t bytes) { (void)data; (void)bytes; return 0; }

static uint32_t rng = 12345;
static unsigned rnd(unsigned n) { rng = rng * 1103515245U + 12345U; return (rng >> 16) % n; }
/* A random voice inside the workbench's validateVoice ranges. */
static void random_patch(uint8_t p[156])
{
    static const uint8_t op[21] = {99,99,99,99,99,99,99,99,99,99,99,3,3,7,3,7,99,1,31,99,14};
    static const uint8_t global[19] = {99,99,99,99,99,99,99,99,31,7,1,99,99,99,99,1,5,7,48};
    for (unsigned i = 0; i < 156; ++i)
        p[i] = (uint8_t)rnd((i < 126 ? op[i % 21] : i < 145 ? global[i - 126] : i == 155 ? 63 : 127) + 1U);
    for (unsigned i = 145; i < 155; ++i) p[i] = (uint8_t)(32 + rnd(95));
    p[155] = 63;
}
static void name_of(unsigned track, char out[11])
{
    uint8_t p[156];
    memset(out, 0, 11);
    if (!groove_fm_copy(track, p, NULL)) memcpy(out, p + 145, 10);
}
static unsigned program_of(unsigned track)
{
    struct groove_view v = {0};
    groove_get_view(track, 0, &v);
    return v.program;
}
static void midi(uint8_t a, uint8_t b) { uint8_t m[2] = {a, b}; fm1_web_midi(m, 2); sim_render(); }
static void fresh(void) { sim_boot(3); groove_library_test_count = GROOVE_LIBRARY_VOICES; groove_library_refresh(); }
static const uint8_t *bank_flash(unsigned bank)
{
    return sim_flash() + GROOVE_TUNING_OFFSET - GROOVE_LIBRARY_BANK_BYTES * (bank + 1U);
}

/* ---- factory content ---- */
static void factory(void)
{
    /* Web build: all 16 demos, as before. */
    CHECK(groove_factory_name(15) && !groove_factory_name(16));
    /* Device image: one DEFAULT with the engine's start-up sources. */
    CHECK(!strcmp(device_factory_name(0), "DEFAULT") && !device_factory_name(1));
    struct groove_project *p = calloc(1, sizeof *p), *empty = calloc(1, sizeof *p);
    CHECK(device_factory_build(1, p) < 0);
    CHECK(device_factory_build(0, p) == 0 && groove_project_valid(p));
    static const uint8_t types[GROOVE_TRACKS] = {GROOVE_DX7, GROOVE_303, GROOVE_PSG, GROOVE_PLUCK,
                                                 GROOVE_SAMPLE, GROOVE_DX7, GROOVE_303, GROOVE_808};
    for (unsigned t = 0; t < GROOVE_TRACKS; ++t) CHECK(p->preset[t].type == types[t] && !p->preset[t].modified);
    CHECK(!memcmp(p->name, "DEFAULT     ", 12) && p->tempo == 120 && p->mute == 0x60);
    unsigned playing = 0;
    for (unsigned t = 0; t < GROOVE_TRACKS; ++t)
        for (unsigned i = 0; i < GROOVE_STEPS; ++i) playing |= p->step[t][i].velocity | p->step[t][i].drums;
    CHECK(!playing); /* no steps */
    /* Byte for byte what a freshly started device exports, apart from the name. */
    fresh();
    struct groove_project *boot = empty;
    CHECK(groove_project_export((uint8_t *)boot, sizeof *boot) == 0);
    memcpy(boot->name, p->name, sizeof p->name);
    for (unsigned t = 0; t < GROOVE_TRACKS; ++t)
        if (memcmp(&p->preset[t], &boot->preset[t], sizeof p->preset[t]))
            printf("DEFAULT differs from start-up on track %u\n", t + 1);
    CHECK(!memcmp(p, boot, sizeof *p));
    free(p); free(empty);
}
static void factory_bank(void)
{
    fresh();
    CHECK(sim_factory_bank() == 0);
    uint8_t bank[GROOVE_LIBRARY_BANK_BYTES], want[156], got[156];
    memcpy(bank, sim_file(), sizeof bank);
    for (unsigned v = 0; v < 32; ++v) {
        groove_library_unpack(bank + 128U * v, got);
        if (v < GROOVE_FACTORY_VOICES) groove_factory_voice(v, want);
        else groove_fm_program(want, GROOVE_LIBRARY_VOICES); /* INIT VOICE */
        CHECK(!memcmp(got, want, 156));
    }
    /* Installed as bank 0, programs 0..15 are the former built-in programs. */
    CHECK(groove_library_write(0, bank) == 0);
    groove_fm_program(got, 0);
    groove_factory_voice(0, want);
    CHECK(!memcmp(got, want, 156) && !memcmp(got + 145, "NEON KEYS ", 10));
    groove_fm_program(got, 15);
    CHECK(!memcmp(got + 145, "SOFT LEAD ", 10));
    CHECK(!memcmp(groove_fm_program_name(15), "SOFT LEAD ", 10));
    CHECK(!memcmp(groove_fm_program_name(16), "INIT VOICE", 10));
}

/* ---- storage ---- */
static void pack_unpack(void)
{
    uint8_t bank[GROOVE_LIBRARY_BANK_BYTES], patches[32][156], got[156];
    for (unsigned v = 0; v < 32; ++v) {
        random_patch(patches[v]);
        groove_library_pack(patches[v], bank + 128U * v);
        groove_library_unpack(bank + 128U * v, got);
        CHECK(!memcmp(got, patches[v], 156));
    }
    for (unsigned i = 0; i < sizeof bank; ++i) CHECK(bank[i] < 128);
    /* Vectors for the workbench cross-check: a 4104-byte .syx, then 32 x 156. */
    FILE *f = fopen("build/host/library_vectors.bin", "wb");
    CHECK(f != NULL);
    if (f) {
        uint8_t head[6] = {0xf0, 0x43, 0x00, 0x09, 0x20, 0x00};
        unsigned sum = 0;
        for (unsigned i = 0; i < sizeof bank; ++i) sum += bank[i];
        uint8_t tail[2] = {(uint8_t)(-sum & 127U), 0xf7};
        fwrite(head, 1, 6, f); fwrite(bank, 1, sizeof bank, f); fwrite(tail, 1, 2, f);
        fwrite(patches, 1, sizeof patches, f);
        uint8_t init[156], packed[128];          /* then INIT VOICE, packed */
        groove_fm_program(init, GROOVE_LIBRARY_VOICES);
        groove_library_pack(init, packed);
        fwrite(packed, 1, sizeof packed, f);
        fclose(f);
    }
}
static void empty_slots(void)
{
    fresh();
    uint8_t p[156];
    groove_fm_program(p, 5);                     /* erased library */
    CHECK(!memcmp(p + 145, "INIT VOICE", 10) && p[134] == 0 && p[5 * 21 + 16] == 99 && p[16] == 0);
    uint8_t bank[GROOVE_LIBRARY_BANK_BYTES], want[156];
    for (unsigned v = 0; v < 32; ++v) { random_patch(want); groove_library_pack(want, bank + 128U * v); }
    memset(bank + 128U * 7, 255, 128);           /* an empty slot is allowed */
    CHECK(groove_library_write(1, bank) == 0);
    groove_fm_program(p, 32 + 7);
    CHECK(!memcmp(p + 145, "INIT VOICE", 10));
    groove_library_unpack(bank + 128U * 8, want);
    groove_fm_program(p, 32 + 8);
    CHECK(!memcmp(p, want, 156));
    bank[128U * 9 + 40] = 0x80;                  /* a stray high bit is not */
    CHECK(groove_library_write(1, bank) == GROOVE_PROJECT_INVALID);
    uint8_t *raw = sim_flash() + GROOVE_TUNING_OFFSET - 2U * GROOVE_LIBRARY_BANK_BYTES;
    raw[128U * 9 + 40] |= 0x80;                  /* flipped in Flash: that slot reads INIT */
    groove_fm_program(p, 32 + 9);
    CHECK(!memcmp(p + 145, "INIT VOICE", 10));
    groove_fm_program(p, 32 + 8);
    CHECK(!memcmp(p, want, 156));
    CHECK(groove_library_write(GROOVE_LIBRARY_BANKS, bank) == GROOVE_PROJECT_INVALID);
}
static void banks_independent(void)
{
    fresh();
    uint8_t a[GROOVE_LIBRARY_BANK_BYTES], b[GROOVE_LIBRARY_BANK_BYTES], p[156];
    for (unsigned v = 0; v < 32; ++v) {
        random_patch(p); groove_library_pack(p, a + 128U * v);
        random_patch(p); groove_library_pack(p, b + 128U * v);
    }
    CHECK(groove_library_write(0, a) == 0);
    CHECK(groove_library_write(1, b) == 0);
    CHECK(!memcmp(bank_flash(0), a, sizeof a) && !memcmp(bank_flash(1), b, sizeof b));
    uint8_t got[GROOVE_LIBRARY_BANK_BYTES];
    CHECK(groove_library_read(0, got) == 0 && !memcmp(got, a, sizeof a));
    CHECK(groove_library_read(3, got) == 0);
    for (unsigned i = 0; i < sizeof got; ++i) CHECK(got[i] == 255);
    /* Tuning sector and projects untouched. */
    CHECK(sim_flash()[GROOVE_TUNING_OFFSET] == 255);
}

/* ---- sample bank overlap ---- */
static unsigned install_bank(unsigned bytes)
{
    uint8_t *b = sim_flash() + GROOVE_SAMPLE_OFFSET;
    uint32_t h[3] = {0x53314d46U, 1, bytes};     /* magic, version, bytes */
    memset(b, 0x5a, bytes);
    memcpy(b, h, sizeof h);
    groove_samples_init();
    for (unsigned i = 0; i < 4096 && groove_samples_status() == 1; ++i) groove_samples_poll();
    return groove_samples_status();
}
static void sample_overlap(void)
{
    CHECK(GROOVE_SAMPLE_LIMIT == 446464);
    CHECK(GROOVE_LIBRARY_OFFSET == GROOVE_SAMPLE_OFFSET + GROOVE_SAMPLE_LIMIT);
    fresh();
    CHECK(groove_samples_begin(GROOVE_SAMPLE_LIMIT + 1) < 0);
    CHECK(groove_samples_begin(GROOVE_SAMPLE_LIMIT) == 0);
    groove_samples_cancel();
    /* The built-in demo bank leaves the whole library free. */
    unsigned demo = 0;
    (void)sampledrum_factory(&demo);
    CHECK(demo && demo <= GROOVE_SAMPLE_LIMIT);

    uint8_t bank[GROOVE_LIBRARY_BANK_BYTES], p[156];
    for (unsigned v = 0; v < 32; ++v) { random_patch(p); groove_library_pack(p, bank + 128U * v); }
    for (unsigned b = 0; b < GROOVE_LIBRARY_BANKS; ++b) CHECK(groove_library_write(b, bank) == 0);
    /* An older 454,232-byte bank reaches banks 2 and 3 (the lowest sectors). */
    fresh();
    install_bank(454232);
    for (unsigned b = 0; b < GROOVE_LIBRARY_BANKS; ++b) CHECK(groove_library_available(b) == (b < 2));
    CHECK(groove_library_write(2, bank) == GROOVE_PROJECT_LIBRARY_SPACE);
    CHECK(groove_library_write(1, bank) == 0);
    groove_fm_program(p, 64);
    CHECK(!memcmp(p + 145, "INIT VOICE", 10));
    uint8_t got[GROOVE_LIBRARY_BANK_BYTES];
    CHECK(groove_library_read(3, got) == 0 && got[0] == 255 && got[4095] == 255);
    /* Tuning still saves: that bank stops below the tuning sector. */
    struct groove_tuning t;
    memset(&t, 0, sizeof t);
    t.main[4] = -50;
    CHECK(groove_tuning_set(0, &t) == 0);
    CHECK(groove_project_save_slot(1) == 0);
    /* A full 462,848-byte bank from older firmware empties every bank. */
    fresh();
    install_bank(462848);
    for (unsigned b = 0; b < GROOVE_LIBRARY_BANKS; ++b) CHECK(groove_library_available(b) == 0);
    CHECK(groove_library_write(0, bank) == GROOVE_PROJECT_LIBRARY_SPACE);
    groove_fm_program(p, 0);
    CHECK(!memcmp(p + 145, "INIT VOICE", 10));
}

/* ---- program addressing ---- */
static void addressing(void)
{
    fresh();
    uint8_t bank[GROOVE_LIBRARY_BANK_BYTES], p[156], want[156];
    for (unsigned v = 0; v < 32; ++v) {
        random_patch(p);
        memcpy(p + 145, "BANK3 V   ", 10);
        p[152] = (uint8_t)('0' + v / 10); p[153] = (uint8_t)('0' + v % 10);
        groove_library_pack(p, bank + 128U * v);
    }
    CHECK(groove_library_write(3, bank) == 0);
    CHECK(groove_source_type(0) == GROOVE_DX7);
    char name[11];
    midi(0xc0, 100);                             /* 100 = bank 3, voice 4 */
    CHECK(program_of(0) == 100);
    name_of(0, name);
    CHECK(!strcmp(name, "BANK3 V04 "));
    groove_library_unpack(bank + 128U * 4, want);
    CHECK(groove_fm_copy(0, p, NULL) == 0 && !memcmp(p, want, 155));
    midi(0xc0, 127);
    CHECK(program_of(0) == 127);
    groove_control_edit(0, 1, 0, 0);             /* PRESETS wraps 127 -> 0 */
    CHECK(program_of(0) == 0);
    groove_control_edit(0, -1, 0, 0);
    CHECK(program_of(0) == 127);
    /* A 64-program app ignores 100 (no wrap). */
    groove_library_test_count = 64;
    midi(0xc0, 10);
    CHECK(program_of(0) == 10);
    midi(0xc0, 100);
    CHECK(program_of(0) == 10);
    /* No app count: 16 programs, wrapped as before. */
    groove_library_test_count = 0;
    midi(0xc0, 20);
    CHECK(program_of(0) == 4);
    groove_control_edit(0, 12, 0, 0);
    CHECK(program_of(0) == 0);
    groove_library_test_count = GROOVE_LIBRARY_VOICES;
    /* Projects keep their own patch: replacing the slot changes nothing loaded. */
    midi(0xc0, 100);
    CHECK(groove_project_save_slot(4) == 0);
    for (unsigned i = 0; i < 128; ++i) bank[128U * 4 + i] = 0;
    memcpy(bank + 128U * 4 + 118, "REPLACED  ", 10);
    CHECK(groove_library_write(3, bank) == 0);
    CHECK(groove_project_load_slot(4) == 0);
    name_of(0, name);
    CHECK(!strcmp(name, "BANK3 V04 ") && program_of(0) == 100);
}

/* ---- transfer ---- */
static int cmd(unsigned c, const uint8_t *data, unsigned n, uint8_t *out, unsigned *count)
{
    return command(c, data, n, out, count);
}
static int send_bank(unsigned target, const uint8_t *bytes)
{
    uint8_t out[96], d[66];
    unsigned count;
    uint8_t begin[4] = {4, (uint8_t)target, 0, 16};
    int r = cmd(3, begin, 4, out, &count);
    if (r) return r;
    for (unsigned at = 0; at < GROOVE_LIBRARY_BANK_BYTES; at += 64) {
        d[0] = (uint8_t)at; d[1] = (uint8_t)(at >> 8);
        memcpy(d + 2, bytes + at, 64);
        if ((r = cmd(4, d, 66, out, &count))) return r;
    }
    uint32_t c = ~groove_crc32(~0U, bytes, GROOVE_LIBRARY_BANK_BYTES);
    uint8_t crc[4] = {(uint8_t)c, (uint8_t)(c >> 8), (uint8_t)(c >> 16), (uint8_t)(c >> 24)};
    r = cmd(5, crc, 4, out, &count);
    transfer_cancel();
    return r;
}
static int fetch_bank(unsigned target, uint8_t *bytes)
{
    uint8_t out[96];
    unsigned count;
    uint8_t begin[2] = {4, (uint8_t)target};
    int r = cmd(1, begin, 2, out, &count);
    if (r) return r;
    if (count != 2 || get16(out) != GROOVE_LIBRARY_BANK_BYTES) return -100;
    for (unsigned at = 0; at < GROOVE_LIBRARY_BANK_BYTES; at += 64) {
        uint8_t q[3] = {(uint8_t)at, (uint8_t)(at >> 8), 64};
        if ((r = cmd(2, q, 3, out, &count)) || count != 64) return r ? r : -101;
        memcpy(bytes + at, out, 64);
    }
    transfer_cancel();
    return 0;
}
static void bank_transfer(void)
{
    fresh();
    uint8_t out[96], bank[GROOVE_LIBRARY_BANK_BYTES], got[GROOVE_LIBRARY_BANK_BYTES], p[156];
    unsigned count = 0;
    CHECK(cmd(22, NULL, 0, out, &count) == 0 && count == 8);
    CHECK(get32(out) == 446464 && out[4] == 4 && get16(out + 5) == 4096 && out[7] == 128);
    CHECK(cmd(21, NULL, 0, out, &count) == 0 && count == 7);
    CHECK(get16(out + 1) == GROOVE_PROJECT_TUNED_BYTES && get32(out + 3) == 462848);
    for (unsigned v = 0; v < 32; ++v) { random_patch(p); groove_library_pack(p, bank + 128U * v); }
    static uint8_t before[GROOVE_PROJECT_TUNED_BYTES], after[GROOVE_PROJECT_TUNED_BYTES];
    CHECK(groove_project_export(before, sizeof before) == 0);
    CHECK(send_bank(2, bank) == 0);
    CHECK(groove_project_export(after, sizeof after) == 0);
    CHECK(!memcmp(before, after, sizeof before));  /* the project is untouched */
    CHECK(fetch_bank(2, got) == 0 && !memcmp(got, bank, sizeof bank));
    CHECK(send_bank(GROOVE_LIBRARY_BANKS, bank) < 0);
    bank[100] = 0x81;
    CHECK(send_bank(1, bank) < 0);
    CHECK(fetch_bank(1, got) == 0 && got[0] == 255);
}

int main(void)
{
    factory();
    factory_bank();
    pack_unpack();
    empty_slots();
    banks_independent();
    sample_overlap();
    addressing();
    bank_transfer();
    if (failures) { printf("library_test: %d failure(s)\n", failures); return 1; }
    printf("library_test: ok\n");
    return 0;
}
