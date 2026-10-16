/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#include "instrument.h"
#include "tuning_hook.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* One sector per project, plus one shared staging sector. Device settings fit
 * after the last slot's project; changing settings preserves that project.
 * A replacement commits in staging before its destination is erased. Before
 * reusing staging, finish that copy. Reads can recover directly from staging;
 * boot never writes or starts a BLE connection. This is not a backup bank. */
enum { PROJECT_MAGIC = 0x35504d46U, SETTINGS_MAGIC = 0x35534d46U,
       LEGACY_PROJECT_MAGIC = 0x34504d46U, LEGACY_SETTINGS_MAGIC = 0x34534d46U,
       SETTINGS_SECTOR = GROOVE_PROJECT_SLOTS - 1, STAGING_SECTOR = GROOVE_PROJECT_SLOTS,
       USER_BYTES = (GROOVE_PROJECT_SLOTS + 1) * FM1_FLASH_SECTOR_BYTES };
struct header { uint32_t magic, generation, target, crc, commit; };
/* Device preferences are independent of project editing and undo. */
struct settings { uint8_t role, clock, slot, radio_off; };
static struct record {
    struct header header;
    struct groove_project project;
    struct settings settings;
} record;
_Static_assert(sizeof record == 4088, "single-sector project record");
_Static_assert(sizeof record <= FM1_FLASH_SECTOR_BYTES, "one record per sector");
static struct settings settings;
static char project_name[12] = {'U', 'N', 'T', 'I', 'T', 'L', 'E', 'D', ' ', ' ', ' ', ' '};
static unsigned restore_pending, role_pending, settings_dirty;
static uint32_t settings_changed;
static uint32_t project_revision;
static int settings_result;
struct groove_project *groove_project_scratch(void) { return &record.project; }

uint32_t groove_crc32(uint32_t crc, const void *data, unsigned bytes)
{
    static const uint32_t table[16] = {
        0x00000000U, 0x1db71064U, 0x3b6e20c8U, 0x26d930acU,
        0x76dc4190U, 0x6b6b51f4U, 0x4db26158U, 0x5005713cU,
        0xedb88320U, 0xf00f9344U, 0xd6d6a3e8U, 0xcb61b38cU,
        0x9b64c2b0U, 0x86d3d2d4U, 0xa00ae278U, 0xbdbdf21cU
    };
    const uint8_t *p = data;
    while (bytes--) {
        crc ^= *p++;
        crc = (crc >> 4) ^ table[crc & 15U];
        crc = (crc >> 4) ^ table[crc & 15U];
    }
    return crc;
}
static uint32_t checksum(void)
{
    uint32_t crc = groove_crc32(~0U, &record.header, offsetof(struct header, crc));
    crc = groove_crc32(crc, &record.settings, sizeof record.settings);
    if (record.header.magic == PROJECT_MAGIC)
        crc = groove_crc32(crc, &record.project, sizeof record.project);
    else if (record.header.magic == LEGACY_PROJECT_MAGIC)
        crc = groove_crc32(crc, &record.project, GROOVE_PROJECT_LEGACY_BYTES);
    return ~crc;
}
static int read_sector(unsigned sector)
{
    uint32_t address = sector * FM1_FLASH_SECTOR_BYTES;
    int r = fm1_flash_read(address, &record.header, sizeof record.header);
    if (r)
        return r;
    const struct header *h = &record.header;
    unsigned legacy = h->magic == LEGACY_PROJECT_MAGIC || h->magic == LEGACY_SETTINGS_MAGIC;
    unsigned project = h->magic == PROJECT_MAGIC || h->magic == LEGACY_PROJECT_MAGIC;
    if ((!project && h->magic != SETTINGS_MAGIC && h->magic != LEGACY_SETTINGS_MAGIC) || h->commit ||
        h->target >= GROOVE_PROJECT_SLOTS ||
        (sector != STAGING_SECTOR && h->target != sector) ||
        (!project && h->target != SETTINGS_SECTOR))
        return GROOVE_PROJECT_EMPTY;
    unsigned bytes = legacy ? GROOVE_PROJECT_LEGACY_BYTES : sizeof record.project;
    if (project)
        r = fm1_flash_read(address + sizeof record.header, &record.project, bytes);
    if (!r)
        r = fm1_flash_read(address + sizeof record.header + bytes,
                           &record.settings, sizeof record.settings);
    if (r)
        return r;
    const struct settings *s = &record.settings;
    if (h->crc != checksum() || s->role > FM1_BLE_CENTRAL || s->clock > GROOVE_CLOCK_MIDI ||
        s->slot >= GROOVE_PROJECT_SLOTS || (s->radio_off & ~GROOVE_RADIO_ALL) ||
        (h->target != SETTINGS_SECTOR && (s->role || s->clock || s->slot || s->radio_off)))
        return GROOVE_PROJECT_EMPTY;
    if (project && legacy) {
        if (record.project.sequence.version != 3) return GROOVE_PROJECT_EMPTY;
        record.project.sequence.version = 4;
        groove_project_arp_defaults(&record.project);
    }
    if (project && !groove_project_valid(&record.project)) return GROOVE_PROJECT_EMPTY;
    /* Verify the old CRC before migration. Normalize only in RAM; the next
     * explicit save/settings write uses the new record and staging protocol. */
    if (legacy) {
        record.header.magic = project ? PROJECT_MAGIC : SETTINGS_MAGIC;
        record.header.crc = checksum();
    }
    return 0;
}
static int latest(unsigned target, unsigned *sector, uint32_t *generation)
{
    if (fm1_flash_size() < USER_BYTES || target > SETTINGS_SECTOR)
        return FM1_FLASH_UNAVAILABLE;
    unsigned found = 0;
    for (unsigned i = 0; i < 2; ++i) {
        unsigned s = i ? STAGING_SECTOR : target;
        int r = read_sector(s);
        if (r < 0)
            return r;
        uint32_t d = record.header.generation - *generation;
        if (!r && record.header.target == target && (!found || (d && d < 0x80000000U))) {
            *sector = s;
            *generation = record.header.generation;
            found = 1;
        }
    }
    return found ? 0 : GROOVE_PROJECT_EMPTY;
}
static int read_project(unsigned sector)
{
    int r = read_sector(sector);
    return !r && record.header.magic != PROJECT_MAGIC ? GROOVE_PROJECT_EMPTY : r;
}
/* Tuning sector (last 4 KiB of Flash, GROOVE_TUNING_OFFSET): header, then one
 * entry per project slot. An entry is valid only for the exact project record
 * it was saved with: its CRC covers the record's generation, the tables and the
 * record's own CRC. Rewriting the sector (erase + write) is not atomic; a torn
 * write only invalidates entries, never projects. */
enum { TUNING_MAGIC = 0x54314d46U, TUNING_VERSION = 1, SAMPLE_BANK_MAGIC = 0x53314d46U };
struct tuning_entry { uint32_t generation; struct groove_tuning t[GROOVE_TRACKS]; uint32_t crc; };
struct tuning_sector { uint32_t magic, version; struct tuning_entry entry[GROOVE_PROJECT_SLOTS]; };
struct tuning_block { uint32_t magic; struct groove_tuning t[GROOVE_TRACKS]; uint32_t crc; };
_Static_assert(sizeof(struct groove_tuning) == 24, "tuning table layout");
_Static_assert(sizeof(struct tuning_entry) == 200, "tuning entry layout");
_Static_assert(sizeof(struct tuning_block) == GROOVE_TUNING_BLOCK_BYTES, "tuning block layout");
_Static_assert(sizeof(struct tuning_sector) <= FM1_FLASH_SECTOR_BYTES, "one tuning sector");
static uint32_t entry_crc(uint32_t generation, const struct groove_tuning *t, uint32_t project_crc)
{
    uint32_t crc = groove_crc32(~0U, &generation, sizeof generation);
    crc = groove_crc32(crc, t, sizeof(struct groove_tuning) * GROOVE_TRACKS);
    return ~groove_crc32(crc, &project_crc, sizeof project_crc);
}
static uint32_t block_crc(const struct tuning_block *b)
{ return ~groove_crc32(~0U, b, offsetof(struct tuning_block, crc)); }
static int tuning_tables_valid(const struct groove_tuning *t)
{
    for (unsigned i = 0; i < GROOVE_TRACKS; ++i)
        for (unsigned k = 0; k < GROOVE_TUNING_KEYS; ++k)
            if (t[i].main[k] == -128 || t[i].alt[k] == -128)
                return 0;
    return 1;
}
static void tuning_zero(void)
{
    static const struct groove_tuning zero[GROOVE_TRACKS];
    if (groove_tuning_apply)
        (void)groove_tuning_apply(zero);
}
/* Nonzero if the stored sample bank reaches into the tuning sector (a bank
 * from before the cap) or Flash cannot be read: tuning is then neither saved
 * nor loaded, and the bank is left alone. */
static int tuning_blocked(void)
{
    uint32_t bank[3];
    if (fm1_flash_size() < GROOVE_FLASH_BYTES ||
        fm1_flash_read(GROOVE_SAMPLE_OFFSET, bank, sizeof bank))
        return 1;
    return bank[0] == SAMPLE_BANK_MAGIC && bank[2] > GROOVE_SAMPLE_LIMIT;
}
/* Generation and CRC of the newest committed record for slot, in key[0..1]. */
static int tuning_key(unsigned slot, uint32_t key[2])
{
    unsigned sector = slot;
    uint32_t generation = 0;
    int r = latest(slot, &sector, &generation);
    if (!r)
        r = read_project(sector);
    if (!r) {
        key[0] = record.header.generation;
        key[1] = record.header.crc;
    }
    return r;
}
/* Entry always reflects the tables at the moment of this save. Best effort. */
static void tuning_save(unsigned slot)
{
    struct groove_tuning tables[GROOVE_TRACKS];
    uint32_t key[2];
    if (!groove_tuning_capture || groove_tuning_capture(tables) || tuning_blocked() ||
        tuning_key(slot, key))
        return;
    struct tuning_sector *s = malloc(2U * sizeof *s);
    if (!s)
        return;
    struct tuning_sector *old = s + 1;
    if (fm1_flash_read(GROOVE_TUNING_OFFSET, old, sizeof *old) == 0) {
        *s = *old;
        if (s->magic != TUNING_MAGIC || s->version != TUNING_VERSION) {
            memset(s, 0xff, sizeof *s);
            s->magic = TUNING_MAGIC;
            s->version = TUNING_VERSION;
        }
        struct tuning_entry *e = &s->entry[slot];
        e->generation = key[0];
        memcpy(e->t, tables, sizeof tables);
        e->crc = entry_crc(key[0], tables, key[1]);
        if (memcmp(s, old, sizeof *s) &&
            fm1_flash_erase(GROOVE_TUNING_OFFSET) == 0)
            (void)fm1_flash_write(GROOVE_TUNING_OFFSET, s, sizeof *s);
    }
    free(s);
}
/* Apply slot's tables if its entry matches the loaded record, else zero. */
static void tuning_load(unsigned slot, const uint32_t key[2])
{
    if (!groove_tuning_apply)
        return;
    struct tuning_entry e;
    uint32_t head[2];
    if (!tuning_blocked() &&
        fm1_flash_read(GROOVE_TUNING_OFFSET, head, sizeof head) == 0 &&
        head[0] == TUNING_MAGIC && head[1] == TUNING_VERSION &&
        fm1_flash_read(GROOVE_TUNING_OFFSET + offsetof(struct tuning_sector, entry) +
                       slot * sizeof e, &e, sizeof e) == 0 &&
        e.generation == key[0] && e.crc == entry_crc(key[0], e.t, key[1]) &&
        groove_tuning_apply(e.t) == 0)
        return;
    tuning_zero();
}
/* record holds a validated payload. Commit follows the complete header/body. */
static int program_record(unsigned sector)
{
    uint32_t address = sector * FM1_FLASH_SECTOR_BYTES;
    record.header.commit = ~0U;
    int r = fm1_flash_erase(address);
    if (!r)
        r = fm1_flash_write(address, &record,
                            record.header.magic == PROJECT_MAGIC ? sizeof record : sizeof record.header);
    if (!r && record.header.magic == SETTINGS_MAGIC)
        r = fm1_flash_write(address + offsetof(struct record, settings),
                            &record.settings, sizeof record.settings);
    if (!r) {
        uint32_t commit = 0;
        r = fm1_flash_write(address + offsetof(struct header, commit), &commit, sizeof commit);
    }
    if (!r)
        r = read_sector(sector);
    return r == GROOVE_PROJECT_EMPTY ? FM1_FLASH_IO : r;
}
/* Complete the last committed replacement before staging a different one.
 * Called only by writes, before capturing their payload into the shared RAM
 * record. Repeating recovery after another interruption is safe. */
static int recover_staging(void)
{
    if (fm1_flash_size() < USER_BYTES)
        return FM1_FLASH_UNAVAILABLE;
    int r = read_sector(STAGING_SECTOR);
    if (r)
        return r == GROOVE_PROJECT_EMPTY ? 0 : r;
    const struct header staged = record.header;
    r = read_sector(staged.target);
    if (r < 0)
        return r;
    uint32_t d = record.header.generation - staged.generation;
    if (!r && ((!d && record.header.crc == staged.crc) || (d && d < 0x80000000U)))
        return 0;
    r = read_sector(STAGING_SECTOR);
    return r ? (r == GROOVE_PROJECT_EMPTY ? FM1_FLASH_IO : r)
             : program_record(staged.target);
}
/* Payload is already in record. Unchanged contents never erase Flash. */
static int write_record(unsigned target, uint32_t generation, unsigned had_save)
{
    int r;
    if (had_save) {
        uint32_t magic;
        r = fm1_flash_read(target * FM1_FLASH_SECTOR_BYTES, &magic, sizeof magic);
        if (r)
            return r;
        unsigned begin = record.header.magic == PROJECT_MAGIC ? sizeof record.header
                         : offsetof(struct record, settings);
        uint8_t chunk[64];
        unsigned at = begin;
        while (magic == record.header.magic && at < sizeof record) {
            unsigned n = sizeof record - at;
            if (n > sizeof chunk)
                n = sizeof chunk;
            r = fm1_flash_read(target * FM1_FLASH_SECTOR_BYTES + at, chunk, n);
            if (r)
                return r;
            if (memcmp(chunk, (const uint8_t *)&record + at, n))
                break;
            at += n;
        }
        if (at == sizeof record)
            return 0;
    }
    record.header.generation = generation + 1U;
    record.header.target = target;
    record.header.crc = checksum();
    r = program_record(STAGING_SECTOR);
    return r ? r : program_record(target);
}
static void settings_mark(void)
{
    settings_dirty = 1;
    settings_changed = fm1_millis();
    settings_result = 0;
}
unsigned groove_project_slot(void) { return settings.slot; }
uint32_t groove_project_revision(void) { return project_revision; }
void groove_project_name(char name[13])
{
    memcpy(name, project_name, 12);
    name[12] = 0;
}
void groove_project_rename(unsigned character, char value)
{
    if (character < 12 && value >= 32 && value <= 126 && project_name[character] != value) {
        groove_edit_checkpoint(0x81000U);
        project_name[character] = value;
        groove_project_touch();
    }
}
void groove_project_metadata(const struct groove_project *p)
{
    memcpy(project_name, p->name, sizeof project_name);
    ++project_revision;
}
int groove_project_info(unsigned slot, char name[13])
{
    if (slot >= GROOVE_PROJECT_SLOTS || !name)
        return GROOVE_PROJECT_INVALID;
    unsigned sector = slot;
    uint32_t generation = 0;
    int r = latest(sector, &sector, &generation);
    if (!r)
        r = read_project(sector);
    memcpy(name, r ? "EMPTY       " : record.project.name, 12);
    name[12] = 0;
    return r;
}
int groove_project_save_slot(unsigned slot)
{
    if (slot >= GROOVE_PROJECT_SLOTS)
        return GROOVE_PROJECT_INVALID;
    int r = recover_staging();
    if (r)
        return r;
    unsigned sector = slot;
    uint32_t generation = 0;
    r = latest(slot, &sector, &generation);
    if (r < 0)
        return r;
    unsigned saved = r == 0;
    memset(&record.settings, 0, sizeof record.settings);
    if (slot == SETTINGS_SECTOR) {
        record.settings = settings;
        record.settings.slot = (uint8_t)slot;
    }
    record.header.magic = PROJECT_MAGIC;
    r = groove_project_capture(&record.project);
    if (!r)
        r = write_record(slot, generation, saved);
    if (!r) {
        tuning_save(slot);
        ++project_revision;
        if (settings.slot != slot) {
            settings.slot = (uint8_t)slot;
            settings_mark();
            settings_changed -= 1500U;
        }
        groove_project_clean();
    }
    return r;
}
int groove_project_load_slot(unsigned slot)
{
    if (slot >= GROOVE_PROJECT_SLOTS)
        return GROOVE_PROJECT_INVALID;
    unsigned sector = slot;
    uint32_t generation = 0;
    int r = latest(sector, &sector, &generation);
    if (!r)
        r = read_project(sector);
    uint32_t key[2] = {record.header.generation, record.header.crc};
    if (!r) {
        groove_edit_checkpoint(0x82000U + slot);
        r = groove_project_apply(&record.project);
        if (!r)
            tuning_load(slot, key);
    }
    if (!r && settings.slot != slot) {
        settings.slot = (uint8_t)slot;
        settings_mark();
        settings_changed -= 1500U;
    }
    if (!r)
        groove_project_clean();
    return r;
}

int groove_project_import(const uint8_t *bytes, unsigned length)
{
    /* Optional trailing tuning block; without it the tables become zero. */
    unsigned tuned = length == GROOVE_PROJECT_TUNED_BYTES;
    struct tuning_block block;
    if (tuned) {
        if (!bytes)
            return GROOVE_PROJECT_INVALID;
        memcpy(&block, bytes + GROOVE_PROJECT_BYTES, sizeof block);
        if (block.magic != TUNING_MAGIC || block.crc != block_crc(&block) ||
            !tuning_tables_valid(block.t))
            return GROOVE_PROJECT_INVALID;
        length = GROOVE_PROJECT_BYTES;
    }
    if (!bytes || (length != sizeof record.project && length != GROOVE_PROJECT_LEGACY_BYTES))
        return GROOVE_PROJECT_INVALID;
    memmove(&record.project, bytes, length);
    if (length == GROOVE_PROJECT_LEGACY_BYTES) {
        if (record.project.sequence.version != 3) return GROOVE_PROJECT_INVALID;
        record.project.sequence.version = 4;
        groove_project_arp_defaults(&record.project);
    }
    if (!groove_project_valid(&record.project))
        return GROOVE_PROJECT_INVALID;
    groove_edit_checkpoint(0x83000U);
    int r = groove_project_apply(&record.project);
    if (!r) {
        if (tuned && groove_tuning_apply)
            (void)groove_tuning_apply(block.t);
        else
            tuning_zero();
        groove_project_touch();
    }
    return r;
}
/* Appends the tuning block when capacity holds it; zero tables without the hook. */
int groove_project_export(uint8_t *bytes, unsigned capacity)
{
    if (!bytes || capacity < sizeof record.project)
        return GROOVE_PROJECT_INVALID;
    int r = groove_project_capture(&record.project);
    if (r)
        return r;
    memcpy(bytes, &record.project, sizeof record.project);
    struct tuning_block block;
    if (capacity >= GROOVE_PROJECT_TUNED_BYTES) {
        if (!groove_tuning_capture || groove_tuning_capture(block.t))
            memset(block.t, 0, sizeof block.t);
        block.magic = TUNING_MAGIC;
        block.crc = block_crc(&block);
        memcpy(bytes + sizeof record.project, &block, sizeof block);
    }
    return 0;
}
int groove_factory_load(unsigned index)
{
    int r = groove_factory_build(index, &record.project);
    if (r) return r;
    groove_edit_checkpoint(0x84000U + index);
    r = groove_project_apply(&record.project);
    if (!r) { tuning_zero(); groove_project_touch(); }
    return r;
}
/* BLE MIDI follows the radio master switch. Ignore the old separate MIDI-off
 * preference without changing the stored settings layout or writing at boot. */
unsigned groove_settings_radio_flags(void) { return (GROOVE_RADIO_ALL ^ settings.radio_off) | GROOVE_RADIO_MIDI; }
unsigned groove_settings_role_value(void) { return settings.role; }
int groove_settings_radio(unsigned flags, unsigned role)
{
    if ((flags & ~GROOVE_RADIO_ALL) || role > FM1_BLE_CENTRAL) return -1;
    flags |= GROOVE_RADIO_MIDI;
    if (flags == groove_settings_radio_flags() && role == settings.role) return 0;
    settings.radio_off = (uint8_t)(GROOVE_RADIO_ALL ^ flags);
    settings.role = (uint8_t)role;
    role_pending = 2;
    settings_mark();
    return 0;
}
void groove_settings_clock(unsigned mode)
{
    if (mode <= GROOVE_CLOCK_MIDI && settings.clock != mode) {
        groove_clock_set(mode);
        settings.clock = (uint8_t)mode;
        settings_mark();
    }
}
int groove_settings_status(void) { return settings_result; }
void groove_project_init(void)
{
    memset(&settings, 0, sizeof settings);
    memcpy(project_name, "UNTITLED    ", 12);
    restore_pending = 2;
    role_pending = 2;
    settings_dirty = 0;
    settings_result = 0;
}
void groove_project_poll(void)
{
    if (restore_pending == 2) {
        unsigned sector = SETTINGS_SECTOR;
        uint32_t generation = 0;
        int r = latest(sector, &sector, &generation);
        if (!r)
            r = read_sector(sector);
        if (r == FM1_FLASH_BUSY)
            return;
        if (!r) {
            settings = record.settings;
            groove_clock_set(settings.clock);
            role_pending = 2;
        }
        restore_pending = 1;
    }
    if (restore_pending && groove_project_load_slot(settings.slot) != FM1_FLASH_BUSY) {
        restore_pending = 0;
        groove_project_clean();
    }
    if (role_pending == 2) {
        unsigned flags = groove_settings_radio_flags();
        unsigned features = flags & GROOVE_RADIO_ON ? flags >> 1 : 0;
        if (!fm1_bluetooth_configure(features)) role_pending = 1;
    }
    if (role_pending == 1) {
        int r = fm1_ble_set_role(settings.role);
        if (!r || (r != -2 && r != -3))
            role_pending = 0;
    }
    if (settings_dirty && fm1_millis() - settings_changed >= 1500U) {
        unsigned sector = SETTINGS_SECTOR;
        uint32_t generation = 0;
        int r = recover_staging();
        if (!r)
            r = latest(SETTINGS_SECTOR, &sector, &generation);
        if (r >= 0) {
            unsigned saved = r == 0;
            if (saved)
                r = read_sector(sector); /* Retain slot 16's project, if present. */
            else {
                record.header.magic = SETTINGS_MAGIC;
                r = 0;
            }
            if (!r) {
                record.settings = settings;
                r = write_record(SETTINGS_SECTOR, generation, saved);
            }
        }
        settings_result = r;
        if (!r)
            settings_dirty = 0;
        else
            settings_changed = fm1_millis();
    }
}
