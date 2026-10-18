/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* Browser platform adapter: in-memory display, storage and MIDI input. */
#include "internal.h"
#include "sampledrum.h"
#include "fm_patch.h"
#include "fm1_sea.h"
#include "fm1_engine_control.h"
#include "audio_output.h"
#include "fm1_audio_io.h"
#include <string.h>
#include <stdlib.h>

#ifdef __wasm__
#define EXPORT(name) __attribute__((export_name(name)))
#else
#define EXPORT(name)
#endif

enum { FLASH_BYTES = GROOVE_FLASH_BYTES, MIDI_QUEUE = 64, STATE_BYTES = 225 };
static struct fm1_controls controls;
static uint16_t pixels[240 * 240];
static uint8_t leds[41], flash[FLASH_BYTES], state[STATE_BYTES];
static uint8_t file_buffer[GROOVE_PROJECT_TUNED_BYTES];
static uint32_t now_ms, frame_revision, frame_dirty, flash_revision;
static uint64_t frames;
static unsigned gate, midi_read, midi_write, midi_reset;
static uint8_t radio_features, radio_role;
static struct { uint8_t data[3], count; } midi[MIDI_QUEUE];
static int32_t previous[FM1_BLOCK_WORDS];
static float output[FM1_BLOCK_WORDS];
static int16_t *sample_input;
static uint8_t *sample_encoded;
static unsigned sample_frames, sample_bytes;
extern void groove_screen_snapshot(uint8_t state[8]);
extern int groove_screen_open(unsigned target);
extern int groove_screen_preset(unsigned slot);
extern unsigned groove_screen_preset_position(void);

void fm1_web_pause(void) { ++gate; }
void fm1_web_resume(void) { if (gate) --gate; }
void fm1_engine_stats_read(struct fm1_engine_stats *stats)
{
    if (!stats) return;
    memset(stats, 0, sizeof *stats);
    stats->submitted = stats->completed = (uint32_t)(frames / FM1_BLOCK_FRAMES);
}
void fm1_control_request(void) {}
void fm1_audio_stats_read(struct fm1_audio_stats *stats)
{
    if (!stats) return;
    memset(stats, 0, sizeof *stats);
    stats->blocks = (uint32_t)(frames / FM1_BLOCK_FRAMES);
}
uint32_t fm1_millis(void) { return now_ms; }
void fm1_controls_init(void) {}
void fm1_controls_read(struct fm1_controls *value)
{
    *value = controls;
    memset(controls.encoders, 0, sizeof controls.encoders);
}
void fm1_led_set(unsigned key, int on) { if (key < sizeof leds) leds[key] = on != 0; }
/* Track 16-row bands until the browser consumes the next display update. */
static void screen_changed(unsigned y, unsigned h)
{
    if (!h) return;
    unsigned first = y / 16, bands = (y + h - 1) / 16 - first + 1;
    frame_dirty |= ((1U << bands) - 1U) << first;
    ++frame_revision;
}
void fm1_screen_init(void) { memset(pixels, 0, sizeof pixels); screen_changed(0, 240); }
void fm1_screen_fill(unsigned x, unsigned y, unsigned w, unsigned h, uint16_t color)
{
    if (x > 240 || y > 240 || w > 240 - x || h > 240 - y) return;
    for (unsigned r = 0; r < h; ++r)
        for (unsigned c = 0; c < w; ++c) pixels[(y + r) * 240 + x + c] = color;
    if (w) screen_changed(y, h);
}
int fm1_screen_blit(unsigned x, unsigned y, unsigned w, unsigned h,
                    const uint16_t *source, size_t count)
{
    if (!source || x > 240 || y > 240 || w > 240 - x || h > 240 - y || count < w * h) return -1;
    for (unsigned r = 0; r < h; ++r)
        memcpy(pixels + (y + r) * 240 + x, source + r * w, w * 2);
    if (w) screen_changed(y, h);
    return 0;
}
/* Browser pixel copies complete immediately. */
int fm1_screen_pending(void) { return 0; }
int fm1_screen_submit(unsigned x, unsigned y, unsigned w, unsigned h,
                      uint16_t *source, size_t count)
{
    if (!w || !h || w > 240U || h > 240U || w * h > FM1_SCREEN_DMA_PIXELS) return -1;
    int result = fm1_screen_blit(x, y, w, h, source, count);
    if (!result)
        for (unsigned i = 0; i < w * h; ++i)
            source[i] = (uint16_t)((source[i] << 8) | (source[i] >> 8));
    return result;
}
/* Settings work in the preview; discovery and links require the real radio. */
int fm1_bluetooth_configure(unsigned features)
{
    if (features & ~(FM1_BT_AUDIO | FM1_BT_MIDI)) return -1;
    radio_features = (uint8_t)features;
    return 0;
}
void fm1_bluetooth_get_status(struct fm1_bluetooth_status *value)
{
    if (!value) return;
    memset(value, 0, sizeof *value);
    value->features = radio_features;
}
int fm1_ble_set_role(enum fm1_ble_role role)
{
    if (role != FM1_BLE_PERIPHERAL && role != FM1_BLE_CENTRAL) return -1;
    radio_role = (uint8_t)role;
    return 0;
}
int fm1_ble_scan(void) { return -3; }
int fm1_ble_connect(unsigned index) { (void)index; return -3; }
int fm1_ble_disconnect(void) { return -3; }
int fm1_ble_forget_bonds(void) { return -3; }
void fm1_ble_get_status(struct fm1_ble_status *value)
{
    if (!value) return;
    memset(value, 0, sizeof *value);
    value->role = radio_role;
}
int fm1_ble_get_device(unsigned index, struct fm1_ble_device *value)
{ (void)index; if (value) memset(value, 0, sizeof *value); return -1; }
uint32_t fm1_flash_size(void) { return sizeof flash; }
int fm1_flash_read(uint32_t at, void *data, size_t bytes)
{
    if (!data || at > sizeof flash || bytes > sizeof flash - at) return FM1_FLASH_INVALID;
    memcpy(data, flash + at, bytes); return 0;
}
int fm1_flash_write(uint32_t at, const void *data, size_t bytes)
{
    if (!data || at > sizeof flash || bytes > sizeof flash - at) return FM1_FLASH_INVALID;
    const uint8_t *source = data;
    for (size_t i = 0; i < bytes; ++i)
        if ((flash[at + i] & source[i]) != source[i]) return FM1_FLASH_NEEDS_ERASE;
    memcpy(flash + at, data, bytes); ++flash_revision; return 0;
}
int fm1_flash_erase(uint32_t at)
{
    if (at % FM1_FLASH_SECTOR_BYTES || at > sizeof flash - FM1_FLASH_SECTOR_BYTES) return FM1_FLASH_INVALID;
    memset(flash + at, 255, FM1_FLASH_SECTOR_BYTES); ++flash_revision; return 0;
}
const uint8_t *fm1_flash_map(uint32_t at, size_t bytes)
{ return at <= sizeof flash && bytes <= sizeof flash - at ? flash + at : NULL; }
int fm1_flash_invalidate(uint32_t at, size_t bytes)
{ return at <= sizeof flash && bytes <= sizeof flash - at ? 0 : FM1_FLASH_INVALID; }
int fm1_midi_send(const uint8_t *data, size_t bytes) { return data && bytes && bytes <= 128 ? 0 : -1; }
int fm1_midi_post(const uint8_t *data, size_t bytes)
{
    if (!data || !bytes || bytes > 3 || data[0] < 128) return -1;
    unsigned status = data[0], count = 0;
    if (status < 0xf0) count = (status & 0xe0) == 0xc0 ? 2 : 3;
    else if (status == 0xf1 || status == 0xf3) count = 2;
    else if (status == 0xf2) count = 3;
    else if (status == 0xf6 || status == 0xf8 || status == 0xfa || status == 0xfb ||
             status == 0xfc || status == 0xfe || status == 0xff) count = 1;
    if (bytes != count) return -1;
    for (size_t i = 1; i < bytes; ++i) if (data[i] >= 128) return -1;
    unsigned next = (midi_write + 1) % MIDI_QUEUE;
    if (next == midi_read) { midi_read = midi_write; midi_reset = 1; return -1; }
    memcpy(midi[midi_write].data, data, bytes); midi[midi_write].count = (uint8_t)bytes;
    midi_write = next; return 0;
}
static void send(const uint8_t *data, unsigned bytes)
{ fm1_web_midi(data, bytes); }
EXPORT("sim_boot") void sim_boot(unsigned clear)
{
    /* Bit 0 clears storage; bit 1 previews the device startup animation. */
    if (clear & 1U) {
        memset(flash, 255, sizeof flash);
        unsigned bytes;
        const uint8_t *factory = sampledrum_factory(&bytes);
        if (bytes <= GROOVE_SAMPLE_BYTES) memcpy(flash + GROOVE_SAMPLE_OFFSET, factory, bytes);
        ++flash_revision;
    }
    memset(&controls, 0, sizeof controls); controls.battery = 900; controls.knob = 1023;
    memset(leds, 0, sizeof leds); memset(previous, 0, sizeof previous);
    now_ms = 0; frames = 0; gate = midi_read = midi_write = midi_reset = 0;
    fm1_output_reset();
    radio_features = FM1_BT_AUDIO | FM1_BT_MIDI;
    radio_role = FM1_BLE_PERIPHERAL;
    fm1_web_init();
    if (!(clear & 2U)) groove_screen_open(0);
    fm1_web_control();
    while (groove_samples_status() == 1) groove_samples_poll();
}
EXPORT("sim_control") void sim_control(unsigned kind, unsigned index, int value)
{
    if (kind == 0 && index < 14) {
        if (value) controls.buttons |= 1U << index; else controls.buttons &= ~(1U << index);
    } else if (kind == 1 && index < 27) {
        if (value) controls.keys |= 1U << index; else controls.keys &= ~(1U << index);
    } else if (kind == 2 && index < 7 && value >= -127 && value <= 127)
        controls.encoders[index] = (int16_t)value;
    else if (kind == 3 && index == 0 && value >= 0 && value <= 1023)
        controls.knob = (uint16_t)value;
    else return;
    fm1_web_control();
}
EXPORT("sim_release") void sim_release(void)
{
    controls.keys = controls.buttons = 0;
    memset(controls.encoders, 0, sizeof controls.encoders); fm1_web_control();
}
EXPORT("sim_clear") void sim_clear(void)
{
    /* Clear project slots and their staging sector, not the sample bank.
     * Keep the monitor setting when restarting the browser instruments. */
    unsigned level = controls.knob;
    memset(flash, 255, GROOVE_SAMPLE_OFFSET); ++flash_revision;
    sim_boot(0);
    sim_control(3, 0, (int)level);
}
EXPORT("sim_panic") void sim_panic(void)
{
    sim_release(); midi_read = midi_write = midi_reset = 0;
    const uint8_t reset = 255; send(&reset, 1); memset(previous, 0, sizeof previous);
}
static void tap(unsigned button) { sim_control(0, button, 1); sim_control(0, button, 0); }
EXPORT("sim_open") int sim_open(unsigned target)
{
    sim_release();
    int result = groove_screen_open(target);
    fm1_web_control();
    return result;
}
EXPORT("sim_preset") int sim_preset(unsigned slot)
{
    sim_release();
    int result = groove_screen_preset(slot);
    fm1_web_control();
    return result;
}
extern const char *groove_screen_web_knobs(void);
extern unsigned groove_screen_web_first(void);
extern int groove_screen_web_step(unsigned step, unsigned toggle);
extern const void *groove_screen_web_targets(void);
extern unsigned groove_screen_web_target_count(void), groove_screen_web_context(void), groove_screen_web_mask(void);
extern int groove_screen_web_adjust(unsigned context, unsigned action, unsigned index, unsigned owner, int delta);
extern void groove_screen_web_fine(unsigned fine);
EXPORT("sim_lcd_targets") const void *sim_lcd_targets(void) { return groove_screen_web_targets(); }
EXPORT("sim_lcd_count") unsigned sim_lcd_count(void) { return groove_screen_web_target_count(); }
EXPORT("sim_lcd_context") unsigned sim_lcd_context(void) { return groove_screen_web_context(); }
EXPORT("sim_lcd_mask") unsigned sim_lcd_mask(void) { return groove_screen_web_mask(); }
EXPORT("sim_lcd") int sim_lcd(unsigned context, unsigned action, unsigned index, unsigned owner, int delta)
{
    int result = groove_screen_web_adjust(context, action, index, owner, delta);
    if (result < 0) return result;
    groove_screen_web_fine(1);
    if (result) sim_control(2, (unsigned)result - 1, delta);
    else fm1_web_control();
    groove_screen_web_fine(0);
    return 0;
}
EXPORT("sim_knobs") const char *sim_knobs(void) { return groove_screen_web_knobs(); }
EXPORT("sim_step_first") unsigned sim_step_first(void) { return groove_screen_web_first(); }
EXPORT("sim_step") int sim_step(unsigned step, unsigned toggle)
{
    sim_release();
    int result = groove_screen_web_step(step, toggle);
    fm1_web_control(); return result;
}
EXPORT("sim_project_name") const char *sim_project_name(void)
{
    static char name[13]; groove_project_name(name); return name;
}
EXPORT("sim_preset_position") unsigned sim_preset_position(void) { return groove_screen_preset_position(); }
EXPORT("sim_project_dirty") unsigned sim_project_dirty(void) { return groove_project_dirty(); }
EXPORT("sim_edit_revision") uint32_t sim_edit_revision(void) { return groove_edit_revision(); }
EXPORT("sim_project_revision") uint32_t sim_project_revision(void) { return groove_project_revision(); }
EXPORT("sim_count_in") unsigned sim_count_in(void) { return groove_sequence_count_in(); }
EXPORT("sim_select") int sim_select(unsigned track, unsigned source)
{
    if (track >= GROOVE_TRACKS || source > GROOVE_SOURCES) return -1;
    uint8_t view[8]; groove_screen_snapshot(view);
    /* Keep FX visible when selecting another track from the web selector. */
    if (source == GROOVE_SOURCES && view[1] == 10U) {
        sim_release(); sim_control(2, 0, (int)track - view[0]); return 0;
    }
    sim_open(0); groove_screen_snapshot(view);
    sim_control(2, 0, (int)track - view[0]);
    if (source < GROOVE_SOURCES) {
        int delta = (int)source - (int)groove_source_type(track);
        if (delta) sim_control(2, 3, delta);
        if (delta) tap(9);
        if (groove_source_type(track) != source) return -1;
    }
    return 0;
}
EXPORT("sim_block_frames") unsigned sim_block_frames(void) { return FM1_BLOCK_FRAMES; }
/* Read-only acceptance state; note 255 means the arpeggiator gate is closed. */
extern unsigned groove_arp_test_note(unsigned track);
EXPORT("sim_arp") unsigned sim_arp(unsigned track)
{
    return groove_arp_test_note(track) | (groove_arp_enabled(track) << 8) |
           (groove_arp_setting(track, 0) << 9) | (groove_arp_setting(track, 1) << 11) |
           (groove_arp_setting(track, 2) << 13) | (groove_arp_setting(track, 3) << 15) |
           (groove_arp_setting(track, 4) << 17) | (groove_arp_setting(track, 5) << 23);
}
EXPORT("sim_render") const float *sim_render(void)
{
    /* Preserve one block of output latency. */
    for (unsigned i = 0; i < FM1_BLOCK_WORDS; ++i) output[i] = (float)previous[i] * (1.0f / 8388608.0f);
    if (midi_reset) { const uint8_t reset = 255; send(&reset, 1); midi_reset = 0; }
    while (midi_read != midi_write) {
        send(midi[midi_read].data, midi[midi_read].count); midi_read = (midi_read + 1) % MIDI_QUEUE;
    }
    (void)fm1_engine_render(previous, FM1_BLOCK_FRAMES);
    fm1_output_mix(previous);
    frames += FM1_BLOCK_FRAMES; uint32_t next = (uint32_t)(frames * 1000U / 44100U);
    unsigned poll = now_ms / 10U != next / 10U; now_ms = next;
    if (poll) { fm1_web_tick(); fm1_web_control(); }
    return output;
}
EXPORT("sim_state") const uint8_t *sim_state(void)
{
    groove_screen_snapshot(state);
    for (unsigned t = 0; t < GROOVE_TRACKS; ++t) {
        struct groove_view v; groove_get_view(t, state[2], &v); unsigned at = 8 + t * 20;
        state[at] = v.type; state[at + 1] = v.program; state[at + 2] = v.volume; state[at + 3] = v.level;
        state[at + 4] = v.muted; state[at + 5] = v.solo; state[at + 6] = v.playing; state[at + 7] = v.play_step;
        state[at + 8] = v.clock_mode; state[at + 9] = v.clock_state;
        state[at + 10] = (uint8_t)v.tempo; state[at + 11] = (uint8_t)(v.tempo >> 8);
        memcpy(state + at + 12, &v.active_steps, 8);
        state[209 + t] = v.sequence_length;
        state[217 + t] = v.next_length;
    }
    memcpy(state + 168, leds, sizeof leds); return state;
}
EXPORT("sim_pixels") const uint16_t *sim_pixels(void) { return pixels; }
EXPORT("sim_frame_revision") uint32_t sim_frame_revision(void) { return frame_revision; }
EXPORT("sim_frame_changes") unsigned sim_frame_changes(void)
{ unsigned dirty = frame_dirty; frame_dirty = 0; return dirty; }
EXPORT("sim_flash") uint8_t *sim_flash(void) { return flash; }
EXPORT("sim_flash_revision") uint32_t sim_flash_revision(void) { return flash_revision; }
EXPORT("sim_file") uint8_t *sim_file(void) { return file_buffer; }
EXPORT("sim_factory") const uint8_t *sim_factory(void)
{ return sampledrum_factory(NULL); }
EXPORT("sim_factory_size") unsigned sim_factory_size(void)
{ unsigned bytes; (void)sampledrum_factory(&bytes); return bytes; }
EXPORT("sim_sample_alloc") int16_t *sim_sample_alloc(unsigned frames)
{
    free(sample_input); free(sample_encoded); sample_encoded = NULL; sample_input = NULL;
    sample_bytes = sample_frames = 0;
    if (!frames || frames > SAMPLE_DRUM_MAX_FRAMES) return NULL;
    sample_input = malloc(((frames + 319U) / 320U) * 640U);
    if (sample_input) sample_frames = frames;
    return sample_input;
}
EXPORT("sim_sample_encode") const uint8_t *sim_sample_encode(void)
{
    free(sample_encoded);
    sample_encoded = sampledrum_encode(sample_input, sample_frames, &sample_bytes);
    return sample_encoded;
}
EXPORT("sim_sample_bytes") unsigned sim_sample_bytes(void) { return sample_bytes; }
EXPORT("sim_sample_bank") int sim_sample_bank(unsigned bytes)
{
    /* Browser worker writes only the sample portion of its private Flash array. */
    if (bytes < 512 || bytes > GROOVE_SAMPLE_BYTES) return -1;
    ++flash_revision;
    groove_samples_init();
    while (groove_samples_status() == 1) groove_samples_poll();
    return groove_samples_status() == 2 ? 0 : -1;
}
EXPORT("sim_export") int sim_export(void) { return groove_project_export(file_buffer, sizeof file_buffer); }
EXPORT("sim_import") int sim_import(unsigned bytes) { return groove_project_import(file_buffer, bytes); }
EXPORT("sim_sample_limit") unsigned sim_sample_limit(void) { return GROOVE_SAMPLE_LIMIT; }
EXPORT("sim_factory_name") const char *sim_factory_name(unsigned index) { return groove_factory_name(index); }
EXPORT("sim_factory_project") int sim_factory_project(unsigned index)
{
    struct groove_project *p = groove_project_scratch();
    int r = groove_factory_build(index, p);
    if (!r) {
        memcpy(file_buffer, p, sizeof *p);
        uint8_t *tail = file_buffer + sizeof *p;
        memset(tail, 0, GROOVE_TUNING_BLOCK_BYTES);
        memcpy(tail, "FM1T", 4);
        uint32_t crc = ~groove_crc32(~0U, tail, GROOVE_TUNING_BLOCK_BYTES - 4);
        memcpy(tail + GROOVE_TUNING_BLOCK_BYTES - 4, &crc, sizeof crc);
    }
    return r;
}
/* The former built-in DX7 programs as a 4096-byte library bank: factory voices
 * first, INIT VOICE after (no library slot reaches past GROOVE_LIBRARY_VOICES). */
EXPORT("sim_factory_bank") int sim_factory_bank(void)
{
    uint8_t patch[156];
    for (unsigned v = 0; v < 32; ++v) {
        if (v < GROOVE_FACTORY_VOICES) groove_factory_voice(v, patch);
        else groove_fm_program(patch, GROOVE_LIBRARY_VOICES);
        groove_library_pack(patch, file_buffer + 128U * v);
    }
    return 0;
}
EXPORT("sim_patch_export") int sim_patch_export(void)
{ uint8_t v[8]; groove_screen_snapshot(v); return groove_fm_copy(v[0], file_buffer, NULL); }
EXPORT("sim_patch_import") int sim_patch_import(void)
{ uint8_t v[8]; groove_screen_snapshot(v); return groove_fm_import(v[0], file_buffer); }
EXPORT("sim_timbre_export") int sim_timbre_export(void)
{ uint8_t v[8]; groove_screen_snapshot(v); return groove_timbre_export(v[0], file_buffer, sizeof file_buffer); }
EXPORT("sim_timbre_import") int sim_timbre_import(void)
{ uint8_t v[8]; groove_screen_snapshot(v); return groove_timbre_import(v[0], file_buffer, sizeof(struct groove_preset)); }
EXPORT("sim_midi") int sim_midi(unsigned bytes)
{
    if (!bytes || bytes > 1024) return -1;
    if (bytes <= 3) return fm1_midi_post(file_buffer, bytes);
    if (file_buffer[0] != 0xf0 || file_buffer[bytes - 1] != 0xf7) return -1;
    fm1_app_sysex(file_buffer, bytes); fm1_web_control(); return 0;
}

/* The browser has no hardware streams; the same APP mixer still runs. */
int fm1_usb_audio_read(int32_t *pcm, unsigned frames, uint16_t gains[2])
{
    if (!pcm || !gains || frames != FM1_BLOCK_FRAMES) return -1;
    gains[0] = gains[1] = 0;
    return 0;
}
int fm1_bluetooth_audio_read(int16_t *pcm, unsigned frames)
{ return pcm && frames == FM1_BLOCK_FRAMES ? 0 : -1; }
int fm1_usb_audio_write(const int32_t *pcm, unsigned frames)
{ return pcm && frames == FM1_BLOCK_FRAMES ? 0 : -1; }
