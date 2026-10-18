/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#include "instrument.h"
#include "fm_patch.h"
#include "version.h"
#include "fm1_engine_control.h"
#include <string.h>
enum {
    BACK = 0x0000,
    PANEL = 0x18e3,
    TEXT = 0xef5d,
    ACCENT = 0x07ff,
    DIM = 0x738e,
    GREEN = 0x07e0,
    AMBER = 0xfd20
};
/* Five columns per glyph, bit 0 at the top. Digits followed by uppercase. */
static const uint8_t font[36][5] = {
    {0x3e, 0x51, 0x49, 0x45, 0x3e}, {0, 0x42, 0x7f, 0x40, 0},       {0x42, 0x61, 0x51, 0x49, 0x46},
    {0x21, 0x41, 0x45, 0x4b, 0x31}, {0x18, 0x14, 0x12, 0x7f, 0x10}, {0x27, 0x45, 0x45, 0x45, 0x39},
    {0x3c, 0x4a, 0x49, 0x49, 0x30}, {0x01, 0x71, 0x09, 0x05, 0x03}, {0x36, 0x49, 0x49, 0x49, 0x36},
    {0x06, 0x49, 0x49, 0x29, 0x1e}, {0x7e, 0x11, 0x11, 0x11, 0x7e}, {0x7f, 0x49, 0x49, 0x49, 0x36},
    {0x3e, 0x41, 0x41, 0x41, 0x22}, {0x7f, 0x41, 0x41, 0x22, 0x1c}, {0x7f, 0x49, 0x49, 0x49, 0x41},
    {0x7f, 0x09, 0x09, 0x09, 0x01}, {0x3e, 0x41, 0x49, 0x49, 0x7a}, {0x7f, 0x08, 0x08, 0x08, 0x7f},
    {0, 0x41, 0x7f, 0x41, 0},       {0x20, 0x40, 0x41, 0x3f, 0x01}, {0x7f, 0x08, 0x14, 0x22, 0x41},
    {0x7f, 0x40, 0x40, 0x40, 0x40}, {0x7f, 0x02, 0x0c, 0x02, 0x7f}, {0x7f, 0x04, 0x08, 0x10, 0x7f},
    {0x3e, 0x41, 0x41, 0x41, 0x3e}, {0x7f, 0x09, 0x09, 0x09, 0x06}, {0x3e, 0x41, 0x51, 0x21, 0x5e},
    {0x7f, 0x09, 0x19, 0x29, 0x46}, {0x46, 0x49, 0x49, 0x49, 0x31}, {0x01, 0x01, 0x7f, 0x01, 0x01},
    {0x3f, 0x40, 0x40, 0x40, 0x3f}, {0x1f, 0x20, 0x40, 0x20, 0x1f}, {0x3f, 0x40, 0x38, 0x40, 0x3f},
    {0x63, 0x14, 0x08, 0x14, 0x63}, {0x07, 0x08, 0x70, 0x08, 0x07}, {0x61, 0x51, 0x49, 0x45, 0x43}};
/* Compose the final pixels before touching the LCD. Sixteen rows cost 7,680
 * bytes; unchanged bands and unchanged horizontal margins are not transferred.
 * A page change never exposes the temporary background clear on the panel. */
enum { PAINT_ROWS = 16, PAINT_BANDS = FM1_SCREEN_HEIGHT / PAINT_ROWS };
_Static_assert(FM1_SCREEN_WIDTH * PAINT_ROWS <= FM1_SCREEN_DMA_PIXELS, "LCD band capacity");
static uint16_t paint_pixels[FM1_SCREEN_WIDTH * PAINT_ROWS];
static uint8_t paint_left[PAINT_BANDS], paint_right[PAINT_BANDS];
static unsigned paint_rendering, paint_top, paint_next, paint_x, paint_end;
static uint32_t paint_priority;
static struct fm1_ble_status frame_ble;
static struct fm1_bluetooth_status frame_radio;
static unsigned frame_radio_flags, frame_radio_role;
static void paint_mark(unsigned x, unsigned y, unsigned w, unsigned h)
{
    for (unsigned b = y / PAINT_ROWS; b <= (y + h - 1) / PAINT_ROWS; ++b) {
        paint_priority |= 1U << b;
        if (x < paint_left[b]) paint_left[b] = (uint8_t)x;
        if (x + w > paint_right[b]) paint_right[b] = (uint8_t)(x + w);
    }
}
/* Many LCD pages share this bounded fill; keep a single native copy. */
static __attribute__((noinline)) void paint_fill(unsigned x, unsigned y, unsigned w, unsigned h, uint16_t color)
{
    if (!w || !h || x >= FM1_SCREEN_WIDTH || y >= FM1_SCREEN_HEIGHT) return;
    if (w > FM1_SCREEN_WIDTH - x) w = FM1_SCREEN_WIDTH - x;
    if (h > FM1_SCREEN_HEIGHT - y) h = FM1_SCREEN_HEIGHT - y;
    if (!paint_rendering) { paint_mark(x, y, w, h); return; }
    if (x >= paint_end || x + w <= paint_x) return;
    unsigned right = x + w < paint_end ? x + w : paint_end;
    if (x < paint_x) x = paint_x;
    unsigned first = y < paint_top ? paint_top : y;
    unsigned end = y + h < paint_top + PAINT_ROWS ? y + h : paint_top + PAINT_ROWS;
    for (unsigned row = first; row < end; ++row)
        for (unsigned col = x; col < right; ++col)
            paint_pixels[(row - paint_top) * (paint_end - paint_x) + col - paint_x] = color;
}
static const uint8_t punctuation[][6] = {
    {33,0,0,0x5f,0,0}, {34,0,7,0,7,0}, {36,0x24,0x2a,0x7f,0x2a,0x12},
    {38,0x36,0x49,0x55,0x22,0x50}, {39,0,5,3,0,0}, {40,0,0x1c,0x22,0x41,0},
    {41,0,0x41,0x22,0x1c,0}, {42,0x14,8,0x3e,8,0x14}, {44,0,0x50,0x30,0,0},
    {46,0,0x60,0x60,0,0}, {58,0,0x36,0x36,0,0}, {59,0,0x56,0x36,0,0},
    {60,8,0x14,0x22,0x41,0}, {61,0x14,0x14,0x14,0x14,0x14},
    {64,0x32,0x49,0x79,0x41,0x3e}, {91,0,0x7f,0x41,0x41,0},
    {92,1,6,0x18,0x60,0}, {93,0,0x41,0x41,0x7f,0}, {94,4,2,1,2,4},
    {95,0x40,0x40,0x40,0x40,0x40}, {96,0,1,2,4,0}, {123,0,8,0x36,0x41,0},
    {124,0,0,0x7f,0,0}, {125,0,0x41,0x36,8,0}, {126,8,4,8,0x10,8}
};
static void text(unsigned x, unsigned y, const char *s, unsigned scale, uint16_t color,
                 uint16_t background)
{
    if (!s || !scale || scale > 4 || x >= FM1_SCREEN_WIDTH ||
        y >= FM1_SCREEN_HEIGHT || 7U * scale > FM1_SCREEN_HEIGHT - y) return;
    unsigned w = 6U * scale, h = 7U * scale;
    if (paint_rendering && (y >= paint_top + PAINT_ROWS || y + h <= paint_top)) return;
    if (!paint_rendering) {
        unsigned n = (unsigned)strlen(s), available = (FM1_SCREEN_WIDTH - x) / w;
        if (n > available) n = available;
        if (n) paint_mark(x, y, n * w, h);
        return;
    }
    for (; *s && x + w <= FM1_SCREEN_WIDTH; ++s, x += w) {
        if (x >= paint_end) break;
        if (x + w <= paint_x) continue;
        char letter = *s >= 'a' && *s <= 'z' ? (char)(*s - 'a' + 'A') : *s;
        uint8_t special[5] = {0};
        const uint8_t *shape = special;
        if (*s >= '0' && *s <= '9')
            shape = font[*s - '0'];
        else if (letter >= 'A' && letter <= 'Z')
            shape = font[10 + letter - 'A'];
        else if (*s == '-' || *s == '+') {
            memset(special, 8, sizeof special);
            if (*s == '+')
                special[2] = 0x3e;
        } else if (*s == '/') {
            special[0] = 0x60;
            special[1] = 0x18;
            special[2] = 0x06;
            special[3] = 1;
        } else if (*s == '#') {
            special[0] = special[4] = 0x14;
            special[1] = special[3] = 0x7f;
            special[2] = 0x14;
        } else if (*s == '%') {
            special[0] = 0x63;
            special[1] = 0x13;
            special[2] = 8;
            special[3] = 0x64;
            special[4] = 0x63;
        } else if (*s == '>') {
            special[0] = 0x41;
            special[1] = 0x22;
            special[2] = 0x14;
            special[3] = 8;
        } else if (*s == '?') {
            special[0] = 2;
            special[1] = 1;
            special[2] = 0x51;
            special[3] = 9;
            special[4] = 6;
        } else
            for (unsigned i = 0; i < sizeof punctuation / sizeof punctuation[0]; ++i)
                if ((uint8_t)*s == punctuation[i][0]) {
                    shape = punctuation[i] + 1;
                    break;
                }
        unsigned first_row = y < paint_top ? paint_top : y;
        unsigned end_row = y + h < paint_top + PAINT_ROWS ? y + h : paint_top + PAINT_ROWS;
        unsigned first_col = x < paint_x ? paint_x : x;
        unsigned end_col = x + w < paint_end ? x + w : paint_end;
        for (unsigned row = first_row; row < end_row; ++row)
            for (unsigned col = first_col; col < end_col; ++col) {
                unsigned column = (col - x) / scale;
                paint_pixels[(row - paint_top) * (paint_end - paint_x) + col - paint_x] =
                    column < 5U && (shape[column] & (1U << ((row - y) / scale))) ? color : background;
            }
    }
}
/* Eight tracks stay visible on HOME. SEQ maps the sixteen white keys to one
 * quarter of the 64-step pattern. All drawing and edits run on app_control. */
enum { OVERVIEW, STEPS, FM_EDIT, FM_ENV, FM_LFO, TONE_EDIT, MIX, BLE, PROJECT, NAME, EFFECTS, SOURCE, ACTIONS, STEP_EDIT, RHYTHM, FM_QUICK, PRESET_BROWSER, ARP_EDIT, STARTUP };
enum { OCT_DOWN, OCT_UP, FX, SCL, ENV, LFO, EDIT, GLO, HOME, SAVE, ARP, SEQ, PLAY, REC };
#define BUTTON(b) (1U << (b))
static const uint16_t colors[8] = {0x3c9f, 0x07bf, 0x4eae, 0x06d4, 0xac9f, 0xed18, 0xff06, 0xfba5};
static const uint8_t white_keys[16] = {0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26};
/* FM1 EDIT key IDs 15,17,19,22,24,27,29,31,34,36 minus 14. */
static const uint8_t edit_keys[10] = {1, 3, 5, 8, 10, 13, 15, 17, 20, 22};
static const struct fm_page {
    const char *title, *label[4];
    uint8_t parameter[4], per_operator;
} fm_pages[] = {
    /* Preserve the FM edit page and knob order. */
    {"TUNE/LVL", {"DETUNE", "COARSE", "FINE", "OUT LVL"}, {20, 18, 19, 16}, 1},
    {"OP RATES", {"RATE 1", "RATE 2", "RATE 3", "RATE 4"}, {0, 1, 2, 3}, 1},
    {"OP LEVELS", {"LEVEL 1", "LEVEL 2", "LEVEL 3", "LEVEL 4"}, {4, 5, 6, 7}, 1},
    {"SENS/OSC", {"AM SENS", "VELOC", "MODE", ""}, {14, 15, 17, 255}, 1},
    {"SCALING", {"L DEPTH", "R DEPTH", "BREAKPT", ""}, {9, 10, 8, 255}, 1},
    {"CURVES", {"L CURVE", "R CURVE", "RATE SC", ""}, {11, 12, 13, 255}, 1},
    {"PITCH RATES", {"RATE 1", "RATE 2", "RATE 3", "RATE 4"}, {126, 127, 128, 129}, 0},
    {"PITCH LEVELS", {"LEVEL 1", "LEVEL 2", "LEVEL 3", "LEVEL 4"}, {130, 131, 132, 133}, 0},
    {"COMMON", {"ALG", "FDBACK", "OSC SYNC", "TRANS"}, {134, 135, 136, 144}, 0},
    {"LFO", {"WAVE", "SPEED", "PMD", "AMD"}, {142, 137, 139, 140}, 0},
    {"LFO MOD", {"DELAY", "P SENS", "LFO SYNC", "OSC SYNC"}, {138, 143, 141, 136}, 0}};
static const char *const source_names[] = {"DX7 FM", "BASS 303", "DRUM 808", "PSG", "PLUCK", "SAMPLER"};
static const char *const actions[] = {"STEP DETAILS", "RHYTHM / RECORD", "COPY STEP", "COPY PAGE", "COPY TRACK",
    "PASTE STEP", "PASTE PAGE", "PASTE TRACK", "CLEAR STEP", "CLEAR PAGE", "CLEAR TRACK",
    "REPEAT TO 32", "REPEAT TO 48", "REPEAT TO 64", "UNDO / REDO", "COPY TIMBRE", "PASTE TIMBRE",
    "DX7 PATCH NAME", "BLUETOOTH / CLOCK", "STOP ALL / REWIND"};
enum { ACTION_COUNT = sizeof actions / sizeof actions[0] };
static unsigned source_preview, action, action_confirm, tone_clip_ready;
static struct groove_preset tone_clip;
static uint32_t midi_seen[8], midi_until[8];
static uint8_t midi_active[8];
static unsigned track, cursor, selected_note = 60, drum, writing, page, fm_group, op;
static unsigned music_page, subpage_return, project_return, rec_before_press, effects_page;
static unsigned seq_page, arp_page;
static unsigned boot_active, boot_phase;
static uint32_t boot_started;
static const char *const seq_pages[] = {"1/5 BASIC", "2/5 STEP DETAILS", "3/5 RHYTHM/REC", "4/5 LOCKS", "5/5 SYNC / LOOP"};
static unsigned rec_down, rec_used;
static uint32_t rec_ms;
static int octave;
/* The editor cursor and the visible playback page are independent. Manual
 * paging/editing pins the view; SEQ resumes follow without moving the cursor. */
static unsigned step_follow = 1, transport_seen;
static const char *notice;
static uint32_t notice_until;
static char master_notice[24];
static unsigned step_first(const struct groove_view *v)
{
    return ((step_follow && v->playing ? v->play_step : cursor) % v->sequence_length) & ~15U;
}
static uint8_t operator_page[GROOVE_TRACKS][8], operator_selected[GROOVE_TRACKS],
    lfo_page[GROOVE_TRACKS];
static uint8_t tone_pages[GROOVE_TRACKS][GROOVE_PADS + 1], drum_parts[GROOVE_TRACKS];
static uint32_t operator_time[6], operator_held, operator_long;
static uint8_t key_note[27], key_track[27], retry_reset;
static uint32_t previous_buttons, previous_keys, blocked_keys, frame_ms, glo_ms;
static uint32_t encoder_time[7];
static int8_t encoder_direction[7];
static uint8_t encoder_speed[7];
static unsigned glo_down, glo_used, ble_device, ble_was_linked, ble_view, ble_action;
static int ble_result, source_result;
static unsigned ble_forget, ble_stage, ble_seconds;
static uint32_t ble_stage_since;
static uint32_t health_at, health_underruns, health_dropped, health_adapted, health_dma;
static int project_result;
static unsigned project_reload, project_operation;
static unsigned project_slot, project_factory, name_cursor;
static unsigned preset_slot, preset_bank, preset_loaded;
static unsigned preset_current_slot, preset_current_bank;
static uint32_t preset_project_revision;
static unsigned advanced_edit, edit_down, edit_long;
static uint32_t edit_ms;
static unsigned seq_down;
static uint32_t seq_ms;
static unsigned arp_down, arp_long;
static uint32_t arp_ms;
static char project_stored[13];
static int project_stored_result;
static uint32_t project_drawn_revision;
static void project_refresh(void)
{
    if (project_factory) {
        const char *name = groove_factory_name(project_slot);
        memset(project_stored, ' ', 12);
        if (name) memcpy(project_stored, name, strlen(name));
        project_stored[12] = 0;
        project_stored_result = name ? 0 : GROOVE_PROJECT_INVALID;
    } else project_stored_result = groove_project_info(project_slot, project_stored);
    project_drawn_revision = groove_project_revision();
}
static unsigned preset_slots(unsigned bank) { return bank ? GROOVE_FACTORY_PROJECTS : GROOVE_PROJECT_SLOTS; }
static void preset_refresh(void)
{
    project_slot = preset_slot;
    project_factory = preset_bank;
    project_refresh();
}
static char edit_character(char old, int delta)
{
    static const char alphabet[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-";
    unsigned at = 0;
    if (old >= 'a' && old <= 'z')
        old = (char)(old - 'a' + 'A');
    for (unsigned i = 0; i < sizeof alphabet - 1; ++i)
        if (alphabet[i] == old)
            at = i;
    int n = (int)sizeof alphabet - 1;
    return alphabet[((int)at + delta % n + n) % n];
}
struct draw_cache {
    unsigned shown_page;
    uint32_t header_cache, title_cache, middle_cache, hint_cache, hold_cache, seq_cache;
    uint32_t foot_cache[4], row_cache[8], middle_shape, cell_cache[8][16], tile_cache[16];
    unsigned meter_cache[8];
};
static struct draw_cache saved_cache, band_cache, *cache = &saved_cache;
static uint32_t led_cache;
static uint16_t button_led_cache;
static struct groove_view frame_views[GROOVE_TRACKS];
static uint8_t frame_step_notes[16], frame_effects[4];
static uint8_t frame_patch[156];
static struct groove_fm_controls frame_controls;
static struct fm1_ble_device frame_devices[FM1_BLE_MAX_DEVICES];
static uint32_t frame_time;

static int clamp(int value, int low, int high)
{
    return value < low ? low : value > high ? high : value;
}
static void move_step(const struct groove_view *view, int delta, int pages)
{
    if (step_follow && view->playing) cursor = view->play_step;
    if (pages) cursor = (unsigned)clamp((int)(cursor / 16) + pages, 0,
        (int)view->sequence_length / 16 - 1) * 16 + (cursor & 15U);
    cursor = (unsigned)clamp((int)cursor + delta, 0, (int)view->sequence_length - 1);
    step_follow = 0;
}
static unsigned navigation_mode(void)
{
    return page == BLE || page == PROJECT || page == PRESET_BROWSER || page == MIX || page == NAME ||
        (page >= SOURCE && page <= RHYTHM) || (page == STEPS && !writing) || glo_down;
}
static unsigned encoder_active(const int16_t values[7])
{
    for (unsigned i = 0; i < 7; ++i)
        if (values[i])
            return 1;
    return 0;
}
static void read_encoders(const struct fm1_controls *controls, uint32_t now, unsigned boundary)
{
    /* Apply the first detent immediately. A continuing fast turn accelerates
     * only continuous parameters; page/option selectors retain raw detents.
     * Reversal, a pause or a new control context returns to fine adjustment. */
    if (boundary) memset(encoder_direction, 0, sizeof encoder_direction);
    for (unsigned i = 0; i < 7; ++i) {
        int direction = (controls->encoders[i] > 0) - (controls->encoders[i] < 0);
        if (!direction) continue;
        encoder_speed[i] = direction == encoder_direction[i] && now - encoder_time[i] <= 40U
            ? (uint8_t)clamp(encoder_speed[i] + 1, 1, 4) : 1;
        encoder_direction[i] = (int8_t)direction;
        encoder_time[i] = now;
    }
}
#ifdef FM1_HOST_TEST
static unsigned web_fine;
void groove_screen_web_fine(unsigned fine) { web_fine = fine; }
#endif
static int fm_encoder_delta(unsigned encoder, int16_t value)
{
#ifdef FM1_HOST_TEST
    if (web_fine) return value;
#endif
    int amount = value < 0 ? -(int)value : value;
    int gain = amount < 2 ? 1 : amount < 4 ? 2 : amount == 4 ? 3 : 4;
    if (gain < encoder_speed[encoder]) gain = encoder_speed[encoder];
    return value * gain;
}
static unsigned parameter_page(unsigned value)
{
    return value == FM_ENV || value == FM_LFO || value == EFFECTS;
}
static void open_subpage(unsigned target)
{
    if (page == target) page = subpage_return;
    else {
        if (!parameter_page(page)) subpage_return = page;
        page = target;
    }
}
static int post(uint8_t command, uint8_t a, uint8_t b, unsigned bytes)
{
    const uint8_t message[3] = {command, a, b};
    if (!fm1_midi_post(message, bytes))
        return 1;
    retry_reset = 1;
    return 0;
}
/* Reuse the formatter across panel fields instead of duplicating it at every call. */
static __attribute__((noinline)) void decimal(char out[12], int value, unsigned digits)
{
    unsigned n = value < 0 ? (unsigned)-value : (unsigned)value, length;
    /* All displayed values are within -24..300; minimum width is optional. */
    length = 1;
    for (unsigned v = n; v >= 10; v /= 10)
        ++length;
    if (length < digits)
        length = digits;
    unsigned start = value < 0;
    if (start)
        out[0] = '-';
    out[start + length] = 0;
    while (length) {
        out[start + --length] = (char)('0' + n % 10);
        n /= 10;
    }
}
/* Share numeric painting across pages instead of expanding it at every call. */
static __attribute__((noinline)) void number(unsigned x, unsigned y, int value, unsigned digits, unsigned scale,
                   uint16_t foreground, uint16_t background)
{
    char out[12];
    decimal(out, value, digits);
    text(x, y, out, scale, foreground, background);
}
static void note_name(unsigned note, char out[12])
{
    static const char names[] = "CCDDEFFGGAAB";
    static const uint16_t sharps = (1U << 1) | (1U << 3) | (1U << 6) | (1U << 8) | (1U << 10);
    unsigned pitch = note % 12, n = 1;
    out[0] = names[pitch];
    if (sharps & (1U << pitch))
        out[n++] = '#';
    char oct[12];
    decimal(oct, (int)(note / 12) - 1, 1);
    strcpy(out + n, oct);
}
static uint32_t signature(uint32_t seed, const void *bytes, unsigned n)
{
    const uint8_t *p = bytes;
    for (unsigned i = 0; i < n; ++i)
        seed = seed * 33U + p[i];
    return seed;
}
static void frame(unsigned x, unsigned y, unsigned w, unsigned h, uint16_t color)
{
    paint_fill(x, y, w, 1, color);
    paint_fill(x, y + h - 1, w, 1, color);
    paint_fill(x, y, 1, h, color);
    paint_fill(x + w - 1, y, 1, h, color);
}
static void line(int x, int y, int end_x, int end_y, uint16_t color)
{
    if (paint_rendering && ((y < end_y ? y : end_y) >= (int)(paint_top + PAINT_ROWS) ||
        (y > end_y ? y : end_y) < (int)paint_top)) return;
    /* Algorithm wires are axis-aligned: one LCD window covers the whole wire. */
    if (x == end_x) {
        paint_fill((unsigned)x, (unsigned)(y < end_y ? y : end_y), 1,
                        (unsigned)(y < end_y ? end_y - y + 1 : y - end_y + 1), color);
        return;
    }
    if (y == end_y) {
        paint_fill((unsigned)(x < end_x ? x : end_x), (unsigned)y,
                        (unsigned)(x < end_x ? end_x - x + 1 : x - end_x + 1), 1, color);
        return;
    }
    int dx = end_x > x ? end_x - x : x - end_x, dy = end_y > y ? y - end_y : end_y - y;
    int sx = x < end_x ? 1 : -1, sy = y < end_y ? 1 : -1, error = dx + dy;
    for (;;) {
        paint_fill((unsigned)x, (unsigned)y, 1, 1, color);
        if (x == end_x && y == end_y)
            break;
        int twice = 2 * error;
        if (twice >= dy) {
            error += dy;
            x += sx;
        }
        if (twice <= dx) {
            error += dx;
            y += sy;
        }
    }
}
#ifdef FM1_HOST_TEST
static char web_knobs[4][2][16];
#endif
static void knob(unsigned col, const char *label, const char *value, uint16_t color)
{
#ifdef FM1_HOST_TEST
    if (col < 4) {
        memset(web_knobs[col], 0, sizeof web_knobs[col]);
        unsigned n = (unsigned)strlen(label); if (n > 15) n = 15;
        memcpy(web_knobs[col][0], label, n);
        n = (unsigned)strlen(value); if (n > 15) n = 15;
        memcpy(web_knobs[col][1], value, n);
    }
#endif
    if (paint_rendering && (paint_top >= 227 || paint_top + PAINT_ROWS <= 195)) return;
    uint32_t sig =
        signature(signature(color, label, (unsigned)strlen(label)), value, (unsigned)strlen(value));
    if (cache->foot_cache[col] == sig)
        return;
    cache->foot_cache[col] = sig;
    unsigned x = 6 + 59 * col;
    paint_fill(x, 195, 54, 32, BACK);
    paint_fill(x, 195, 54, 2, color);
    text(x, 200, label, 1, TEXT, BACK);
    text(x, 212, value, strlen(value) <= 4 ? 2 : 1, color, BACK);
}
static __attribute__((noinline)) void header(const struct groove_view *v)
{
    if (paint_rendering && paint_top >= 30) return;
    unsigned paused = !v->playing && !groove_sequence_rewound();
    unsigned count = (groove_sequence_count_in() + 3U) / 4U;
    if (count > 4) count = 4;
    uint32_t sig = v->tempo | ((uint32_t)v->play_step << 10) |
        ((uint32_t)v->playing << 16) | (writing << 17) |
        ((uint32_t)v->clock_mode << 18) | ((uint32_t)v->clock_state << 19) | (step_follow << 22) | (groove_arp_enabled(track) << 23) |
        ((uint32_t)(v->sequence_length >> 4) << 24) | (groove_project_dirty() << 27) | (paused << 28) | (count << 29);
    if (sig == cache->header_cache)
        return;
    uint32_t previous = cache->header_cache, changed = sig ^ previous;
    unsigned first = previous == ~0U;
    cache->header_cache = sig;
    if (first) {
        paint_fill(0, 0, 240, 30, BACK);
        text(122, 7, "/", 1, DIM, BACK);
    }
    if (first || (changed & (1U << 27)))
        text(165, 19, groove_project_dirty() ? "*" : " ", 1, AMBER, BACK);
    if (first || (changed & (7U << 24)))
        number(128, 7, v->sequence_length, 2, 1, DIM, BACK);
    /* Fixed-width glyphs replace their own background; a moving playhead
     * only updates its two digits and, at a page boundary, two indicators. */
    if (first || (changed & 1023U)) {
        if (v->clock_mode && !v->tempo)
            text(8, 8, "---", 2, DIM, BACK);
        else
            number(8, 8, v->tempo, 3, 2, TEXT, BACK);
    }
    if (first || (changed & (7U << 18))) {
        paint_fill(45, 15, 32, 7, BACK);
        text(45, 15, !v->clock_mode ? "BPM" : v->clock_state == GROOVE_CLOCK_LOST
            ? "LOST" : v->clock_state == GROOVE_CLOCK_LOCKED ? "EXT" : "WAIT",
            1, v->clock_state == GROOVE_CLOCK_LOST ? AMBER : DIM, BACK);
    }
    if (first || (changed & ((1U << 16) | (1U << 28))))
        text(80, 7, v->playing ? "PLAY " : paused ? "PAUSE" : "STOP ", 1,
             v->playing ? GREEN : paused ? AMBER : DIM, BACK);
    if (first || (changed & (63U << 10)))
        number(110, 7, v->play_step + 1, 2, 1, TEXT, BACK);
    for (unsigned i = 0; i < 4; ++i) {
        unsigned active = v->playing && i == v->play_step / 16;
        unsigned was_active = (previous & (1U << 16)) && i == ((previous >> 14) & 3U);
        if (first || active != was_active || (changed & (7U << 24)))
            paint_fill(80 + i * 16, 20, 12, 3, active ? GREEN : i * 16 < v->sequence_length ? PANEL : BACK);
    }
    if (first || (changed & (1U << 22)))
        text(142, 7, step_follow ? "FOLLOW" : "EDIT  ", 1, step_follow ? GREEN : AMBER, BACK);
    if (first || (changed & ((1U << 16) | (1U << 17) | (1U << 23) | (7U << 29)))) {
        uint16_t color = writing ? AMBER : PANEL;
        paint_fill(181, 7, 51, 16, color);
        char counting[] = "CNT 4"; counting[4] = (char)('0' + count);
        text(192, 12, writing ? count ? counting : v->playing ? "REC" : "STEP"
             : groove_arp_enabled(track) ? "ARP" : "LIVE", 1, writing ? BACK : TEXT, color);
    }
}
static __attribute__((noinline)) void overview(const struct groove_view v[8])
{
    if (paint_rendering && (paint_top >= 190 || paint_top + PAINT_ROWS <= 32)) return;
    unsigned first = step_first(&v[track]);
    char project_name[13]; groove_project_name(project_name);
    uint32_t title = signature(first / 16 | (step_follow << 3) | (cursor << 4) |
        ((unsigned)v[track].sequence_length << 10), project_name, 12);
    if (title != cache->title_cache) {
        cache->title_cache = title;
        paint_fill(0, 32, 240, 13, BACK);
        text(8, 35, project_name, 1, ACCENT, BACK);
        text(86, 35, "OPEN " FM1_RELEASE_VERSION, 1, DIM, BACK);
        text(146, 35, step_follow ? "PAGE" : "STEP", 1, DIM, BACK);
        number(178, 35, step_follow ? first + 1 : cursor + 1, 2, 1, TEXT, BACK);
        text(190, 35, step_follow ? "-" : "/", 1, DIM, BACK);
        number(196, 35, step_follow ? first + 16 : v[track].sequence_length, 2, 1, TEXT, BACK);
    }
    static const char *const chip_names[] = {"PSG YM", "PSG AY", "PSG NOIS", "PSG MIX"};
    for (unsigned t = 0; t < 8; ++t) {
        unsigned y = 48 + 18 * t;
        if (paint_rendering && (y >= paint_top + PAINT_ROWS || y + 16 <= paint_top)) continue;
        uint32_t meta = ((uint32_t)v[t].volume << 4) | ((t == track) << 11) |
                        ((uint32_t)v[t].modified << 12) | ((uint32_t)v[t].muted << 13) |
                        ((uint32_t)v[t].solo << 14) | ((uint32_t)v[t].ready << 15) |
                        ((uint32_t)v[t].type << 16) | ((uint32_t)midi_active[t] << 19) |
                        ((uint32_t)v[t].program << 20);
        if (meta != cache->row_cache[t]) {
            cache->row_cache[t] = meta;
            paint_fill(5, y, 71, 16, BACK);
            uint16_t bg = t == track ? colors[t] : PANEL;
            paint_fill(6, y, 18, 16, bg);
            number(12, y + 5, t + 1, 1, 1, t == track ? BACK : colors[t], bg);
            paint_fill(7, y + 12, 3, 3, midi_active[t] ? GREEN : bg);
            /* The track's own patch names it, whatever the library holds now. */
            char fm_name[8] = "DX7";
            uint8_t patch[156];
            if (v[t].type == GROOVE_DX7 && !groove_fm_copy(t, patch, NULL)) {
                memcpy(fm_name, patch + 145, 7); fm_name[7] = 0;
            }
            text(29, y + 1,
                 !v[t].ready                  ? "ERROR"
                 : v[t].type == GROOVE_DX7    ? fm_name
                 : v[t].type == GROOVE_303    ? (v[t].program ? "303 SQR" : "303 SAW")
                 : v[t].type == GROOVE_PSG    ? chip_names[v[t].program & 3U]
                 : v[t].type == GROOVE_PLUCK  ? "PLUCK"
                 : v[t].type == GROOVE_SAMPLE ? "SAMPLER"
                                              : "808 KIT",
                 1, colors[t], BACK);
            text(29, y + 9,
                 v[t].muted      ? "MUTE"
                 : v[t].solo     ? "SOLO"
                 : v[t].modified ? "EDITED"
                                 : "",
                 1, DIM, BACK);
            number(214, y + 1, v[t].volume, 3, 1, t == track ? TEXT : DIM, BACK);
        }
        for (unsigned i = 0; i < 16; ++i) {
            unsigned step = first + i;
            unsigned enabled = step < v[t].sequence_length;
            unsigned on = enabled && (unsigned)((v[t].active_steps >> step) & 1U);
            unsigned head = v[t].playing && v[t].play_step == step;
            unsigned selected = enabled && t == track && step == cursor;
            uint32_t sig = on | (head << 1) | (selected << 2) | ((uint32_t)v[t].audible << 3) | (enabled << 4);
            if (sig == cache->cell_cache[t][i])
                continue;
            cache->cell_cache[t][i] = sig;
            unsigned x = 80 + i * 8;
            paint_fill(x, y + 3, 6, 11, selected ? TEXT : BACK);
            paint_fill(x + 1, y + 5, 4, 8, on ? (v[t].audible ? colors[t] : DIM) : enabled ? PANEL : BACK);
            if (head)
                paint_fill(x, y + 3, 6, 2, GREEN);
        }
        unsigned meter = v[t].audible ? (v[t].level * 14 + 63) / 127 : 0;
        unsigned meter_signature = meter | (v[t].level >= 124 ? 16U : 0U);
        if (meter_signature != cache->meter_cache[t]) {
            cache->meter_cache[t] = meter_signature;
            paint_fill(216, y + 11, 14, 3, PANEL);
            if (meter)
                paint_fill(216, y + 11, meter, 3, v[t].level >= 124 ? AMBER : colors[t]);
        }
    }
}
static void step_knob(unsigned col, unsigned bank)
{
    static const char *const labels[] = {"VELOCITY", "GATE", "PROB", "CHORD", "TIE", "LOCK", "VALUE", "NOTE"};
    unsigned setting = bank * 4 + col, v = groove_step_setting(track, cursor, setting);
    char value[12];
    if (setting == 1) { decimal(value, v ? v == 15 ? 100 : (v * 100 + 8) / 16 : 50, 1); strcat(value, "%"); }
    else if (setting == 2) { decimal(value, v ? (v * 100 + 4) / 8 : 100, 1); strcat(value, "%"); }
    else if (setting == 3) strcpy(value, groove_sequence_chord_name(v));
    else if (setting == 4) strcpy(value, v ? "ON" : "OFF");
    else if (setting == 5 && !v) strcpy(value, "OFF");
    else if (setting < 7) decimal(value, v, 1);
    else { struct groove_view view = {0}; groove_get_view(track, cursor, &view); note_name(view.step.note, value); }
    if (groove_pad_count(track) && (setting == 1 || setting == 3 || setting == 4 || setting == 7)) strcpy(value, "--");
    knob(col, labels[setting], value, colors[col]);
}
static void step_adjust(unsigned bank, const int16_t *encoders, uint32_t now)
{
    for (unsigned i = 0; i < 4; ++i) if (encoders[2 + i]) {
        unsigned setting = bank * 4 + i;
        if (setting < 7) {
            if (groove_step_setting_edit(track, cursor, setting, encoders[2 + i])) {
                notice = "UNAVAILABLE / CHORD OR LOCK FULL"; notice_until = now + 2000U;
            }
        } else if (!groove_pad_count(track)) {
            struct groove_view v = {0}; groove_get_view(track, cursor, &v);
            groove_sequence_edit(track, cursor, (unsigned)clamp((int)v.step.note + encoders[2 + i], 0, 127), v.step.velocity);
        }
    }
}
static void sequence_length_adjust(int delta)
{
    if (!delta) return;
    struct groove_view v = {0}; groove_get_view(track, cursor, &v);
    unsigned length = (unsigned)clamp((int)(v.next_length / 16) + delta, 1, 4) * 16U;
    groove_sequence_length_set(track, length);
    cursor %= groove_sequence_length(track);
}
static __attribute__((noinline)) void steps(const struct groove_view *v)
{
    if (paint_rendering && (paint_top >= 184 || paint_top + PAINT_ROWS <= 32)) return;
    unsigned first = step_first(v);
    uint32_t title = track | (drum << 3) | (first << 7) | (writing << 13) |
        ((uint32_t)v->sequence_length << 14) | ((uint32_t)v->next_length << 21);
    if (title != cache->title_cache) {
        cache->title_cache = title;
        paint_fill(0, 32, 240, 34, BACK);
        text(8, 35, "T", 1, colors[track], BACK);
        number(14, 35, track + 1, 1, 1, colors[track], BACK);
        text(30, 35,
             groove_pad_count(track) != 0                ? groove_pad_name(track, drum)
             : groove_source_type(track) == GROOVE_303   ? "303 STEPS"
             : groove_source_type(track) == GROOVE_PSG   ? "PSG STEPS"
             : groove_source_type(track) == GROOVE_PLUCK ? "PLUCK STEPS"
                                                         : "DX7 STEPS",
             1, TEXT, BACK);
        unsigned pending = v->sequence_length != v->next_length;
        text(174, 35, pending ? "NEXT" : "LEN ", 1, pending ? AMBER : DIM, BACK);
        number(204, 35, v->next_length, 2, 1, pending ? AMBER : colors[track], BACK);
        for (unsigned p = 0; p < 4; ++p) {
            uint16_t bg = p == first / 16 ? colors[track] : p * 16 < v->sequence_length ? PANEL : BACK;
            paint_fill(8 + p * 57, 50, 53, 14, bg);
            number(19 + p * 57, 54, p * 16 + 1, 2, 1, p == first / 16 ? BACK : DIM, bg);
            text(31 + p * 57, 54, "-", 1, p == first / 16 ? BACK : DIM, bg);
            number(37 + p * 57, 54, p * 16 + 16, 2, 1, p == first / 16 ? BACK : DIM, bg);
        }
    }
    uint64_t lane = groove_pad_count(track) != 0 ? groove_drum_steps(track, drum) : v->active_steps;
    for (unsigned i = 0; i < 16; ++i) {
        unsigned step = first + i;
        unsigned x = 8 + (i % 4) * 57, y = 72 + (i / 4) * 29;
        if (paint_rendering && (y >= paint_top + PAINT_ROWS || y + 25 <= paint_top)) continue;
        unsigned on = (unsigned)((lane >> step) & 1U), head = v->playing && v->play_step == step;
        uint32_t sig = step | (on << 6) | (head << 7) | ((step == cursor) << 8) |
                       ((uint32_t)frame_step_notes[i] << 9) | (track << 16) | (drum << 19);
        if (sig == cache->tile_cache[i])
            continue;
        cache->tile_cache[i] = sig;
        uint16_t bg = on ? colors[track] : PANEL, fg = on ? BACK : TEXT;
        paint_fill(x, y, 53, 25, bg);
        if (step == cursor)
            frame(x, y, 53, 25, TEXT);
        if (head)
            paint_fill(x, y, 53, 3, GREEN);
        number(x + 5, y + 5, step + 1, 2, 1, fg, bg);
        char label[12];
        if (!on)
            strcpy(label, "-");
        else if (groove_pad_count(track) != 0)
            strcpy(label, "HIT");
        else
            note_name(frame_step_notes[i], label);
        text(x + 25, y + 14, label, 1, fg, bg);
    }
}
static unsigned fm_parameter(unsigned col)
{
    const struct fm_page *p = &fm_pages[fm_group];
    if (p->parameter[col] == 255)
        return 255;
    return p->parameter[col] + (p->per_operator ? (5 - op) * 21 : 0);
}
static void algorithm_draw(unsigned algorithm, unsigned muted);
static __attribute__((noinline, cold, minsize)) void fm_draw(const uint8_t patch[156], const struct groove_fm_controls *controls)
{
    if (paint_rendering && (paint_top >= 192 || paint_top + PAINT_ROWS <= 32)) return;
    const struct fm_page *p = &fm_pages[fm_group];
    /* Titles, diagrams and the summary row have independent damage. Editing
     * a number never retransmits an unchanged operator diagram. */
    uint32_t title = track | (op << 3) | (fm_group << 6) |
                     ((uint32_t)controls->operators << 10);
    uint32_t shape = patch[134] | (op << 5) | ((uint32_t)controls->operators << 8);
    if (page == FM_ENV)
        shape = signature(controls->enabled, controls->envelope, 4);
    else if (page == FM_EDIT &&
             (fm_group == 1 || fm_group == 2 || fm_group == 6 || fm_group == 7)) {
        unsigned offset = op == 6 ? 126 : (5 - op) * 21;
        shape = signature(op, patch + offset, 8);
    } else if (page == FM_LFO)
        memcpy(&shape, patch + 141, 3);
    if (cache->title_cache != title) {
        cache->title_cache = title;
        paint_fill(0, 32, 240, 40, BACK);
        text(8, 35, "T", 1, colors[track], BACK);
        number(14, 35, track + 1, 1, 1, colors[track], BACK);
        text(30, 35, "DX7", 1, TEXT, BACK);
        text(64, 35, page == FM_ENV ? "AMP ENV" : p->title, 1, colors[track], BACK);
        if (page == FM_EDIT) {
            number(206, 35, operator_page[track][op] + 1, 1, 1, TEXT, BACK);
            text(212, 35, op < 6 ? "/6" : op == 6 ? "/2" : "/1", 1, DIM, BACK);
        }
        for (unsigned i = 0; i < 6; ++i) {
            uint16_t bg = i == op && page == FM_EDIT ? colors[track] : PANEL;
            paint_fill(8 + i * 38, 51, 34, 16, bg);
            uint16_t fg = bg == colors[track] ? BACK : TEXT;
            text(13 + i * 38, 56, controls->operators & (1U << i) ? "X " : "OP", 1, fg, bg);
            number(25 + i * 38, 56, i + 1, 1, 1, fg, bg);
        }
    }
    uint32_t context = track | (fm_group << 3) | (page << 7) | ((op == 6U) << 11);
    if (cache->middle_shape != shape || cache->row_cache[0] != context) {
        cache->row_cache[0] = context;
        cache->middle_shape = shape;
        paint_fill(0, 72, 240, 108, BACK);
        if (page == FM_ENV) {
            text(8, 80, controls->enabled ? "ADSR ON" : "ADSR OFF", 1, GREEN, BACK);
            text(8, 95, controls->enabled ? "AMPLITUDE SHAPE" : "PREVIEW / BYPASSED", 1, DIM, BACK);
            /* Match adsr_config_set's millisecond/level mapping. Sustain has
             * an illustrative 400 ms hold; it lasts until the key is released. */
            unsigned a = 8U * controls->envelope[0], d = 100U + 8U * controls->envelope[1];
            unsigned r = 100U + 19U * controls->envelope[3], total = a + d + 400U + r;
            int x1 = 20 + a * 200U / total, x2 = 20 + (a + d) * 200U / total;
            int x3 = 20 + (a + d + 400U) * 200U / total;
            int y = 167 - (100U + 7U * controls->envelope[2]) * 53U / 1000U;
            unsigned color = controls->enabled ? colors[track] : DIM;
            line(20, 167, x1, 114, color); line(x1, 114, x2, y, color);
            line(x2, y, x3, y, color); line(x3, y, 220, 167, color);
        } else if (page == FM_EDIT &&
                   (fm_group == 1 || fm_group == 2 || fm_group == 6 || fm_group == 7)) {
            unsigned offset = op == 6 ? 126 : (5 - op) * 21;
            text(8, 80, "RATE / LEVEL PREVIEW", 1, DIM, BACK);
            text(8, 94, "HIGH RATE = FAST   L3 HOLDS", 1, DIM, BACK);
            if (op == 6)
                paint_fill(20, 134, 201, 1, PANEL);
            int x = 20, last_y = 166 - patch[offset + 7] * 64 / 99;
            /* Schematic widths keep all stages visible: higher RATE is faster.
             * L3 holds until release. This preview is not a millisecond trace. */
            for (unsigned i = 0; i < 4; ++i) {
                if (i == 3) { line(x, last_y, x + 30, last_y, colors[track]); x += 30; }
                int y = 166 - patch[offset + 4 + i] * 64 / 99;
                int next = x + 8 + (99 - patch[offset + i]) * 32 / 99;
                line(x, last_y, next, y, colors[track]);
                paint_fill((unsigned)next - 2, (unsigned)y - 2, 5, 5, TEXT);
                x = next;
                last_y = y;
            }
        } else if (page == FM_LFO) {
            static const char *const waves[6] = {"TRIANGLE", "SAW DOWN", "SAW UP",
                                                 "SQUARE",   "SINE",     "S/HOLD"};
            text(8, 91, "LFO WAVE", 1, DIM, BACK);
            text(8, 110, waves[patch[142] % 6], 2, colors[track], BACK);
            text(8, 146, patch[141] ? "KEY SYNC ON" : "KEY SYNC OFF", 1, TEXT, BACK);
            text(124, 146, "PM SENS", 1, DIM, BACK);
            number(178, 146, patch[143], 1, 1, TEXT, BACK);
        } else
            algorithm_draw(patch[134], controls->operators);
    }
    uint32_t sig = patch[134] | ((uint32_t)patch[135] << 5) | ((uint32_t)controls->mono << 8);
    sig = signature(sig, patch + 145, 10);
    if (cache->middle_cache == sig) return;
    cache->middle_cache = sig;
    paint_fill(0, 180, 240, 12, BACK);
    text(8, 181, "ALG", 1, DIM, BACK);
    number(32, 181, patch[134] + 1, 2, 1, TEXT, BACK);
    text(60, 181, "FB", 1, DIM, BACK);
    number(78, 181, patch[135], 1, 1, TEXT, BACK);
    text(104, 181, controls->mono ? "MONO" : "POLY 8", 1, DIM, BACK);
    char patch_name[11];
    for (unsigned i = 0; i < 10; ++i)
        patch_name[i] = patch[145 + i] >= 32 && patch[145 + i] <= 126 ? (char)patch[145 + i] : '?';
    patch_name[10] = 0;
    text(164, 181, page == FM_ENV ? "ENV OVERLAY" : patch_name, 1, DIM, BACK);
}
static unsigned tone_part(void) { return groove_pad_count(track) != 0 ? drum_parts[track] : 0; }
static unsigned tone_slot(void)
{
    return groove_pad_count(track) != 0 ? 1U + drum_parts[track] : 0;
}
/* ENV exposes each engine's actual time/shape controls, without changing
 * its synthesis model. A missing knob is blank rather than a dummy ADSR. */
static unsigned tone_parameter(unsigned col)
{
    unsigned type = groove_source_type(track);
    if (page != FM_ENV) return tone_pages[track][tone_slot()] * 4U + col;
    static const uint8_t bass[] = {BASS303_ENVMOD, BASS303_DECAY, BASS303_ACCDEC, BASS303_CUTOFF};
    static const uint8_t pluck[] = {0, 3, 1, 255};
    static const uint8_t sample[] = {2, 3, 255, 255};
    if (type == GROOVE_303) return bass[col];
    if (type == GROOVE_PSG) return col ? 255 : 3;
    if (type == GROOVE_PLUCK) return pluck[col];
    if (type == GROOVE_SAMPLE) return sample[col];
    const char *name = col == 0 ? "Decay" : col == 1 ? "Attack" : col == 2 ? "Tone" : "Tune";
    for (unsigned i = 0; i < groove_tone_count(track, tone_part()); ++i) {
        const fm1_param_t *p = groove_tone_parameter(track, tone_part(), i);
        if (p && !strcmp(p->name, name)) return i;
    }
    return 255;
}
static __attribute__((noinline)) void track_lfo_draw(void)
{
    const uint8_t *v = frame_patch;
    uint32_t sig = signature(track | (frame_views[track].type << 8), v, 4);
    if (cache->title_cache == sig) return;
    cache->title_cache = sig;
    paint_fill(0, 32, 240, 163, BACK);
    text(8, 40, "TRACK LFO", 2, colors[track], BACK);
    text(8, 62, "T", 1, colors[track], BACK);
    number(14, 62, track + 1, 1, 1, colors[track], BACK);
    text(32, 62, source_names[frame_views[track].type], 1, TEXT, BACK);
    text(8, 80, v[1] ? "MODULATION ON" : "DEPTH 0 = OFF", 1, v[1] ? GREEN : DIM, BACK);
    text(8, 100, v[2] ? "STEREO PAN" : "VOLUME TREMOLO", 2, TEXT, BACK);
    text(8, 140, "K1 RATE    K2 DEPTH", 1, DIM, BACK);
    text(8, 159, "K3 TARGET  K4 WAVE", 1, DIM, BACK);
    text(8, 181, "KEYS PLAY  SELECT TRACK", 1, DIM, BACK);
    static const char *const labels[] = {"RATE Hz", "DEPTH", "TARGET", "WAVE"};
    static const char *const waves[] = {"TRI", "SQUARE", "SAW UP", "SAW DOWN"};
    for (unsigned i = 0; i < 4; ++i) {
        char value[12];
        if (!i) {
            unsigned rate = groove_lfo_rate(v[0]);
            char fraction[12]; decimal(value, rate / 100, 1); decimal(fraction, rate % 100, 2);
            strcat(value, "."); strcat(value, fraction);
        } else if (i == 1) { decimal(value, v[i], 1); strcat(value, "%"); }
        else strcpy(value, i == 2 ? (v[i] ? "PAN" : "VOLUME") : waves[v[i]]);
        knob(i, labels[i], value, colors[i]);
    }
    text(8, 231, "LFO BACK  HOME TRACKS", 1, DIM, BACK);
}
static void tone_label(unsigned parameter, unsigned value, const fm1_param_t *p, char out[12])
{
    unsigned type = groove_source_type(track);
    if (p->names)
        strcpy(out, p->names[value]);
    else
        decimal(out,
                (int)value - (((type == GROOVE_PSG || type == GROOVE_PLUCK) && parameter == 2) ||
                                      (type == GROOVE_SAMPLE && parameter == 1)
                                  ? 24
                                  : 0),
                1);
}
static __attribute__((noinline)) void tone_draw(const uint8_t values[GROOVE_TONE_PARAMS])
{
    if (paint_rendering && (paint_top >= 195 || paint_top + PAINT_ROWS <= 32)) return;
    unsigned type = groove_source_type(track), part = tone_part(),
             pg = page == FM_ENV ? 0 : tone_pages[track][tone_slot()];
    const struct groove_view view = frame_views[track];
    uint32_t title = track | (part << 3) | (pg << 7) | (type << 12) | (view.mono << 16);
    if (cache->title_cache != title) {
        cache->title_cache = title;
        paint_fill(0, 32, 240, 163, BACK);
        text(8, 35,
             page == FM_ENV ? "SOURCE ENV" :
             type == GROOVE_303      ? "303 EDIT"
             : type == GROOVE_PSG    ? "PSG EDIT"
             : type == GROOVE_PLUCK  ? "PLUCK"
             : type == GROOVE_SAMPLE ? "SAMPLER"
                                     : "808 EDIT",
             2, colors[track], BACK);
        text(150, 38, "T", 1, colors[track], BACK);
        number(156, 38, track + 1, 1, 1, colors[track], BACK);
        number(192, 38, pg + 1, 1, 1, TEXT, BACK);
        text(198, 38, "/", 1, DIM, BACK);
        number(204, 38, page == FM_ENV ? 1 : (groove_tone_count(track, part) + 3) / 4, 1, 1, TEXT, BACK);
        static const char *const pages[] = {"FILTER", "OSCILLATOR / LEVEL", "DRIVE / SLIDE"};
        text(8, 56,
             page == FM_ENV && !groove_pad_count(track) ? source_names[type]
             : type == GROOVE_PSG                           ? "8 VOICES  YM / AY CHIPS"
             : type == GROOVE_PLUCK                       ? "8 VOICES  KARPLUS STRONG"
             : type == GROOVE_303                         ? pages[pg]
             : type == GROOVE_808 && part == GROOVE_DRUMS ? "KIT SETTINGS"
                                                          : groove_pad_name(track, part),
             1, TEXT, BACK);
        text(8, 181,
             page == FM_ENV ? (groove_pad_count(track) ? "PRESETS PART  KEYS PLAY" : "KEYS PLAY  SELECT TRACK")
             : type == GROOVE_PSG     ? "KEYS PLAY  PRESETS MODE"
             : type == GROOVE_PLUCK ? "KEYS PLAY  KNOBS EDIT"
             : type == GROOVE_303
                 ? (view.mono ? "MONO SLIDE  MONO/POLY KEYS" : "POLY 4      MONO/POLY KEYS")
             : type == GROOVE_SAMPLE ? "8 WHITE KEYS PLAY / SELECT"
                                     : page == FM_ENV ? "PRESETS DRUM  KEYS PLAY" : "PRESETS DRUM / KIT  KEYS PLAY",
             1, DIM, BACK);
        memset(cache->tile_cache, 0xff, sizeof cache->tile_cache);
    }
    if (!groove_pad_count(track)) {
        for (unsigned i = 0; i < 4; ++i) {
            unsigned parameter = tone_parameter(i);
            const fm1_param_t *p = groove_tone_parameter(track, 0, parameter);
            if (!p) continue;
            unsigned value = values[parameter];
            if (cache->tile_cache[i] == value) continue;
            cache->tile_cache[i] = value;
            unsigned y = 78 + i * 24;
            char label[12];
            tone_label(parameter, value, p, label);
            text(8, y, p->name, 1, DIM, BACK);
            paint_fill(56, y, 36, 7, BACK);
            text(56, y, label, 1, TEXT, BACK);
            paint_fill(100, y, 132, 8, PANEL);
            unsigned width = value * 132U / p->max;
            if (width)
                paint_fill(100, y, width, 8, colors[i]);
        }
    } else {
        unsigned count = type == GROOVE_SAMPLE ? groove_pad_count(track) : GROOVE_DRUMS + (page != FM_ENV);
        for (unsigned i = 0; i < count; ++i) {
            unsigned selected = i == part;
            if (cache->tile_cache[i] == selected)
                continue;
            cache->tile_cache[i] = selected;
            unsigned x = 8 + (i % 4) * 57, y = 74 + (i / 4) * 25;
            uint16_t bg = selected ? colors[track] : PANEL;
            paint_fill(x, y, 53, 22, bg);
            text(x + 4, y + 8, groove_pad_label(track, i), 1, selected ? BACK : TEXT, bg);
        }
    }
}
static __attribute__((noinline)) void mix_draw(const struct groove_view v[8])
{
    if (paint_rendering && (paint_top >= 184 || paint_top + PAINT_ROWS <= 32)) return;
    unsigned bank = track / 4;
    if (cache->title_cache != bank) {
        cache->title_cache = bank;
        paint_fill(0, 32, 240, 33, BACK);
        text(8, 35, "MIX  8 TRACKS", 1, TEXT, BACK);
        text(8, 51, "KEYS 1-8 MUTE  9-16 SOLO", 1, DIM, BACK);
    }
    for (unsigned i = 0; i < 16; ++i) {
        unsigned t = i & 7U, on = i < 8 ? v[t].muted : v[t].solo;
        uint32_t sig = on | ((t == track) << 1);
        if (sig == cache->tile_cache[i])
            continue;
        cache->tile_cache[i] = sig;
        unsigned x = 8 + (i % 4) * 57, y = 72 + (i / 4) * 29;
        uint16_t bg = on ? colors[t] : PANEL;
        paint_fill(x, y, 53, 25, bg);
        if (t == track)
            frame(x, y, 53, 25, TEXT);
        text(x + 7, y + 5, i < 8 ? "MUTE" : "SOLO", 1, on ? BACK : DIM, bg);
        number(x + 37, y + 5, t + 1, 1, 1, on ? BACK : colors[t], bg);
    }
}
static void radio_line(unsigned slot, unsigned y, const char *value, uint16_t color)
{
    uint32_t sig = signature(color, value, (unsigned)strlen(value));
    if (cache->tile_cache[slot] == sig) return;
    cache->tile_cache[slot] = sig;
    paint_fill(8, y, 224, 7, BACK);
    text(8, y, value, 1, color, BACK);
}
static __attribute__((noinline)) void draw_ble(const struct fm1_ble_status *status)
{
    static const char *const states[] = {"OFF", "IDLE", "WAITING FOR DEVICE",
        "SCANNING", "CONNECTING", "FINDING MIDI", "CONNECTED", "MIDI READY", "DISCONNECTING"};
    static const char *const errors[] = {
        "", "STACK BUSY", "TIMED OUT", "NOT BLE MIDI", "GATT FAILED", "LINK LOST"};
    unsigned on = (frame_radio_flags & GROOVE_RADIO_ON) != 0;
    unsigned midi = frame_radio_flags & GROOVE_RADIO_MIDI ? 1U + frame_radio_role : 0;
    if (cache->title_cache != ble_view) {
        cache->title_cache = ble_view;
        memset(cache->row_cache, 0xff, sizeof cache->row_cache);
        memset(cache->tile_cache, 0xff, sizeof cache->tile_cache);
        paint_fill(0, 0, 240, 240, BACK);
        paint_fill(0, 29, 240, 1, PANEL);
        text(8, 8, ble_view ? "MIDI HOST" : "BLUETOOTH", 2, ACCENT, BACK);
    }
    const char *state = !on ? "BLUETOOTH OFF" : !frame_radio.ready ? "RADIO AVAILABLE ON DEVICE"
        : status->pending ? "APPLYING SETTINGS"
        : !midi ? "MIDI OFF" : status->state == FM1_BLE_OFF ? "STARTING MIDI"
        : status->state <= FM1_BLE_DISCONNECTING ? states[status->state] : "UNKNOWN";
    char progress[40];
    if (on && frame_radio.ready && (status->pending || status->state == FM1_BLE_SCANNING ||
        status->state == FM1_BLE_CONNECTING || status->state == FM1_BLE_DISCOVERING)) {
        unsigned n = (unsigned)strlen(state);
        memcpy(progress, state, n);
        progress[n++] = ' ';
        decimal(progress + n, (int)ble_seconds, 1);
        n = (unsigned)strlen(progress);
        progress[n++] = 'S'; progress[n] = 0;
        state = progress;
    }
    radio_line(0, 39, state, on ? GREEN : DIM);
    if (!ble_view) {
        static const char *const labels[] = {"K1 BLUETOOTH", "K2 BT AUDIO", "K3 BLE ROLE", "K4 CLOCK"};
        unsigned values[] = {on, (frame_radio_flags & GROOVE_RADIO_AUDIO) != 0, midi, groove_clock_mode()};
        const char *const names[] = {on ? "ON" : "OFF", values[1] ? "ON" : "OFF",
            midi == 2 ? "HOST" : midi == 1 ? "DEVICE" : "OFF", values[3] ? "MIDI" : "INT"};
        for (unsigned i = 0; i < 4; ++i) {
            uint32_t sig = values[i] | (on << 8);
            if (cache->row_cache[i] == sig) continue;
            cache->row_cache[i] = sig;
            unsigned y = 61 + i * 30;
            paint_fill(8, y, 224, 20, BACK);
            text(8, y + 4, labels[i], 1, TEXT, BACK);
            text(146, y, names[i], 2, i && i < 3 && !on ? DIM : ACCENT, BACK);
        }
        const char *audio = !on || !values[1] ? "AUDIO OFF" : frame_radio.audio_playing ? "AUDIO PLAYING"
            : frame_radio.audio_connected ? "AUDIO CONNECTED" : "AUDIO WAITING FOR CONNECTION";
        radio_line(1, 184, audio, frame_radio.audio_playing ? GREEN : DIM);
        radio_line(2, 196, ble_action ? "> FORGET MIDI PAIRS" : "> MIDI DEVICE LIST (HOST)", ACCENT);
    } else {
        for (unsigned i = 0; i < FM1_BLE_MAX_DEVICES; ++i) {
            const struct fm1_ble_device *device = &frame_devices[i];
            uint32_t sig = signature(i == ble_device, device, sizeof *device);
            sig = sig * 33U + (i < status->count);
            if (!status->count) sig = sig * 33U + status->state;
            if (cache->row_cache[i] == sig) continue;
            cache->row_cache[i] = sig;
            unsigned y = 60 + i * 17;
            uint16_t bg = i < status->count && i == ble_device ? PANEL : BACK;
            paint_fill(8, y, 224, 15, bg);
            if (i < status->count) {
                number(12, y + 4, i + 1, 1, 1, ACCENT, bg);
                text(27, y + 4, device->name[0] ? device->name : "UNNAMED MIDI DEVICE", 1, TEXT, bg);
                char signal[12];
                decimal(signal, device->rssi, 1);
                text(204, y + 4, signal, 1, DIM, bg);
            } else if (!i)
                text(12, y + 4, status->state == FM1_BLE_SCANNING ? "LOOKING FOR MIDI DEVICES..."
                    : "NO DEVICES - SELECT SCAN BELOW", 1, DIM, bg);
        }
        static const char *const actions[] = {"> CONNECT SELECTED", "> SCAN", "> DISCONNECT", "> SETTINGS"};
        radio_line(1, 200, actions[ble_action], ACCENT);
    }
    const char *error = ble_forget == 1 ? "FORGET MIDI PAIRS? SAVE YES / HOME NO"
        : ble_forget == 2 && !status->pending && !status->error ? "PAIR AGAIN; FORGET FM1 ON PEER TOO"
        : groove_settings_status() ? "SETTINGS NOT SAVED - RETRY"
        : ble_result == -2 ? "BUSY - RETRY" : ble_result == -3 ? "RADIO AVAILABLE ON DEVICE"
        : ble_result ? "COMMAND UNAVAILABLE"
        : status->error <= FM1_BLE_LINK_LOST ? errors[status->error] : "";
    radio_line(3, 213, error, AMBER);
    radio_line(4, 229, "PRESETS ACTION  SAVE OK  HOME EXIT", DIM);
}
static __attribute__((noinline)) void project_draw(void)
{
    if (project_drawn_revision != groove_project_revision())
        project_refresh();
    char name[13];
    groove_project_name(name);
    uint32_t sig = signature(project_slot, project_stored, 12);
    sig = signature(sig, &project_stored_result, sizeof project_stored_result);
    if (cache->title_cache != sig) {
        cache->title_cache = sig;
        paint_fill(8, 40, 224, 7, BACK);
        text(8, 40, "USER", 1, DIM, BACK);
        number(38, 40, project_slot + 1, 2, 1, ACCENT, BACK);
        text(52, 40, "/16", 1, DIM, BACK);
        text(78, 40, project_stored_result < 0 ? "READ ERROR" : project_stored, 1, TEXT, BACK);
    }
    sig = signature(name_cursor, name, 12);
    if (cache->middle_cache != sig) {
        cache->middle_cache = sig;
        paint_fill(8, 77, 144, 18, BACK);
        text(8, 77, name, 2, ACCENT, BACK);
        paint_fill(8 + name_cursor * 12, 93, 10, 2, ACCENT);
    }
    sig = project_reload;
    if (cache->row_cache[0] != sig) {
        cache->row_cache[0] = sig;
        paint_fill(8, 126, 224, 26, BACK);
        text(8, 126, project_reload ? "OVERWRITE SELECTED SLOT?" : "SAVE WRITE PROJECT", 1, TEXT, BACK);
        text(8, 145, project_reload ? "SAVE CONFIRM  HOME CANCEL" : "K1 CURSOR  K2 LETTER", 1, DIM, BACK);
    }
    if (cache->hint_cache != 1U) {
        cache->hint_cache = 1U;
        text(8, 59, "WORKING NAME", 1, DIM, BACK);
        text(8, 105, "SELECT / PRESETS CHOOSE SLOT", 1, DIM, BACK);
        text(8, 224, "HOME CANCEL  EDIT SOUND", 1, DIM, BACK);
    }
    sig = (unsigned)(project_result + 16) | (project_operation << 16);
    if (cache->row_cache[1] == sig) return;
    cache->row_cache[1] = sig;
    const char *status = project_result == GROOVE_PROJECT_EMPTY     ? "NO VALID SAVE"
                         : project_result == GROOVE_PROJECT_TUNING_SPACE ? "REDUCE SAMPLE BANK"
                         : project_result == GROOVE_PROJECT_MEMORY  ? "NOT ENOUGH MEMORY"
                         : project_result == GROOVE_PROJECT_INVALID ? "INVALID PROJECT"
                         : project_result == FM1_FLASH_BUSY         ? "FLASH BUSY - RETRY"
                         : project_result == FM1_FLASH_UNAVAILABLE  ? "FLASH NOT READY"
                         : project_result                           ? "FLASH ERROR - RETRY"
                         : project_operation == 1                   ? "SAVED"
                         : project_operation == 3                   ? "SAVING"
                                                                    : "READY";
    paint_fill(8, 180, 224, 7, BACK);
    text(8, 180, status, 1, project_result ? AMBER : GREEN, BACK);
}
static __attribute__((noinline)) void preset_draw(void)
{
    uint32_t sig = signature(preset_slot | (preset_bank << 8),
                             project_stored, 12);
    sig = signature(sig, &project_result, sizeof project_result);
    if (cache->middle_cache == sig) return;
    cache->middle_cache = sig;
    paint_fill(0, 30, 240, 210, BACK);
    text(8, 42, preset_bank ? "FACTORY PRESETS" : "USER PROJECTS", 1, DIM, BACK);
    number(8, 65, preset_slot + 1, 2, 2, ACCENT, BACK);
    text(38, 65, "/", 2, DIM, BACK);
    number(50, 65, preset_slots(preset_bank), 2, 2, DIM, BACK);
    text(8, 97, project_stored_result < 0 ? "READ ERROR" : project_stored, 2, TEXT, BACK);
    text(8, 127, "SELECT / PRESETS LOAD", 1, TEXT, BACK);
    text(8, 148, "PLAY LOAD + RUN   SAVE PROJECT", 1, TEXT, BACK);
    text(8, 170, "WHITE KEYS 1-16 LOAD DIRECTLY", 1, DIM, BACK);
    text(8, 191, "K1 BANK: USER / FACTORY", 1, DIM, BACK);
    text(8, 213, project_result == GROOVE_PROJECT_EMPTY ? "EMPTY SLOT - CHOOSE ANOTHER" :
         project_result ? "LOAD FAILED - CURRENT KEPT" : "HOME BACK   HOME+SAVE UNDO", 1,
         project_result ? AMBER : DIM, BACK);
}
static __attribute__((noinline)) void name_draw(void)
{
    const uint8_t *patch = frame_patch;
    uint32_t sig = signature(name_cursor, patch + 145, 10);
    if (cache->middle_cache == sig)
        return;
    cache->middle_cache = sig;
    if (cache->title_cache != 1U) {
        cache->title_cache = 1U;
        text(8, 40, "DX7 PATCH NAME", 1, TEXT, BACK);
        text(8, 120, "SELECT CURSOR  K1 LETTER", 1, TEXT, BACK);
        text(8, 145, "SAVE STORES WITH PROJECT", 1, DIM, BACK);
        text(8, 171, "WEB IMPORT / EXPORT .SYX", 1, DIM, BACK);
        text(8, 225, "HOME EXIT  EDIT SOUND", 1, DIM, BACK);
    }
    paint_fill(8, 76, 120, 19, BACK);
    char name[11];
    for (unsigned i = 0; i < 10; ++i)
        name[i] = patch[145 + i] >= 32 && patch[145 + i] <= 126 ? (char)patch[145 + i] : '?';
    name[10] = 0;
    text(8, 76, name, 2, ACCENT, BACK);
    paint_fill(8 + name_cursor * 12, 93, 10, 2, ACCENT);
}
static __attribute__((noinline)) void effects_draw(void)
{
    const uint8_t *values = frame_effects;
    unsigned title = 1U + track + effects_page * 8U;
    if (cache->title_cache != title) {
        cache->title_cache = title;
        paint_fill(8, 40, 224, 36, BACK);
        if (effects_page) text(8, 40, "SHARED FX / ALL TRACKS", 1, TEXT, BACK);
        else {
            text(8, 40, "TRACK", 1, TEXT, BACK);
            number(44, 40, track + 1U, 1, 1, ACCENT, BACK);
            text(60, 40, source_names[frame_views[track].type], 1, TEXT, BACK);
        }
        text(8, 64, effects_page ? "PRESETS: BACK TO TRACK" : "PRESETS: SHARED SETTINGS", 1, DIM, BACK);
    }
    static const char *const shared[4] = {"REV TIME", "DLY TIME", "FEEDBACK", "RETURN"};
    static const char *const local[4] = {"REV SEND", "DLY SEND", "TRK VOL", "ALL RET"};
    const char *const *labels = effects_page ? shared : local;
    for (unsigned i = 0; i < 4; ++i) {
        unsigned value = effects_page && i == 1 ? (166U + values[i] * 14U) * 80U / 441U : values[i];
        if (cache->row_cache[i] != value) {
            unsigned first = cache->row_cache[i] == ~0U;
            cache->row_cache[i] = value;
            if (first) {
                text(8, 91 + i * 22, labels[i], 1, TEXT, BACK);
                if (effects_page && i == 1) text(190, 91 + i * 22, "MS", 1, DIM, BACK);
                if (!effects_page && i < 2) text(190, 91 + i * 22, "%", 1, DIM, BACK);
            }
            number(164, 91 + i * 22, value, 3, 1, ACCENT, BACK);
        }
        char number_text[12];
        decimal(number_text, (int)value, 1);
        knob(i, labels[i], number_text, colors[i]);
    }
}
static void leds(const struct groove_view *v, unsigned visible)
{
    uint32_t bits = 0;
    unsigned first = step_first(v), blink = (frame_time / 250U) & 1U;
    if (visible == STEPS && !writing) {
        uint64_t lane =
            groove_pad_count(track) != 0 ? groove_drum_steps(track, drum) : v->active_steps;
        for (unsigned i = 0; i < 16; ++i)
            if ((lane >> (first + i)) & 1U)
                bits |= 1U << white_keys[i];
        if (v->playing && (v->play_step & ~15U) == first)
            bits ^= 1U << white_keys[v->play_step & 15U];
    } else if (visible == PRESET_BROWSER) {
        bits = 1U << white_keys[preset_slot];
    } else if (visible == MIX) {
        for (unsigned t = 0; t < 8; ++t) {
            struct groove_view m = {0};
            groove_get_view(t, 0, &m);
            if (m.muted)
                bits |= 1U << white_keys[t];
            if (m.solo)
                bits |= 1U << white_keys[t + 8];
        }
    } else
        for (unsigned key = 0; key < 27; ++key)
            if (key_note[key] != 0xff)
                bits |= 1U << key;
    if (visible == FM_EDIT) {
        for (unsigned i = 0; i < 10; ++i) bits &= ~(1U << edit_keys[i]);
        for (unsigned i = 0; i < 6; ++i)
            if (!(frame_controls.operators & (1U << i))) bits |= 1U << edit_keys[i];
        if (op < 8) {
            bits &= ~(1U << edit_keys[op]);
            if (!blink) bits |= 1U << edit_keys[op];
        }
        bits |= 1U << edit_keys[frame_controls.mono ? 8 : 9];
    } else if (visible == TONE_EDIT && v->type == GROOVE_303) {
        bits &= ~((1U << edit_keys[8]) | (1U << edit_keys[9]));
        bits |= 1U << edit_keys[v->mono ? 8 : 9];
    }
    uint32_t changed = bits ^ led_cache;
    for (unsigned key = 0; key < 27; ++key)
        if (changed & (1U << key))
            fm1_led_set(key + 14, (bits >> key) & 1U);
    led_cache = bits;
    unsigned step_pages = visible == STEPS || visible == ACTIONS;
    bits = ((step_pages ? first > 0 : octave < 0) << OCT_DOWN)
        | ((step_pages ? first + 16 < v->sequence_length : octave > 0) << OCT_UP)
        | ((visible == OVERVIEW) << HOME) | ((visible == EFFECTS) << FX)
        | ((groove_arp_enabled(track) != 0) << ARP)
        | ((visible == FM_EDIT && (fm_group == 4 || fm_group == 5)) << SCL)
        | ((visible == STEPS || visible == ACTIONS || visible == STEP_EDIT || visible == RHYTHM) << SEQ)
        | ((writing != 0) << REC)
        | ((v->playing != 0) << PLAY) | ((visible == MIX) << GLO)
        | ((visible == FM_EDIT || visible == TONE_EDIT || visible == FM_QUICK || visible == STEP_EDIT) << EDIT)
        | ((visible == FM_ENV) << ENV) | ((visible == FM_LFO) << LFO)
        | ((visible == PROJECT || visible == SOURCE || (visible == ACTIONS && action_confirm)) << SAVE);
    changed = bits ^ button_led_cache;
    for (unsigned key = 0; key < 14; ++key)
        if (changed & (1U << key)) fm1_led_set(key, (bits >> key) & 1U);
    button_led_cache = (uint16_t)bits;
}
static __attribute__((noinline)) void edit_page_draw(unsigned visible)
{
    uint32_t sig = visible * 33U + action + (action_confirm << 8);
    sig = sig * 33U + cursor;
    sig = sig * 33U + source_preview;
    sig = sig * 33U + (unsigned)source_result;
    for (unsigned i = 0; i < 7; ++i) sig = sig * 33U + groove_step_setting(track, cursor, i);
    for (unsigned i = 0; i < 3; ++i) sig = sig * 33U + groove_sequence_setting(i);
    sig = sig * 33U + groove_sequence_count_in();
    sig = sig * 33U + frame_views[track].volume;
    if (notice) sig = signature(sig, notice, (unsigned)strlen(notice));
    if (visible == FM_QUICK) {
        sig = signature(sig, frame_patch, 156);
        sig = sig * 33U + frame_views[track].program;
        sig = sig * 33U + frame_views[track].modified;
    }
    if (visible == ARP_EDIT) {
        sig = sig * 33U + track + (groove_arp_enabled(track) << 8) + (arp_page << 9);
        for (unsigned i = 0; i < 6; ++i) sig = sig * 33U + groove_arp_setting(track, i);
    }
    if (cache->middle_cache == sig) return;
    cache->middle_cache = sig;
    paint_fill(0, 32, 240, 208, BACK);
    memset(cache->foot_cache, 0xff, sizeof cache->foot_cache);
    if (visible == SOURCE) {
        text(8, 42, "SELECT SOUND SOURCE", 1, TEXT, BACK);
        text(8, 78, source_names[source_preview], 2, ACCENT, BACK);
        text(8, 120, "SAVE APPLY  HOME CANCEL", 1, TEXT, BACK);
        text(8, 148, "NEW SOURCE RESETS ITS TIMBRE", 1, DIM, BACK);
        text(8, 165, "DRUM / PITCH CHANGE CLEARS STEPS", 1, AMBER, BACK);
        text(8, 196, source_result ? "NOT ENOUGH MEMORY - RETRY" : "UNDO IN HOLD SEQ MENU", 1, source_result ? AMBER : DIM, BACK);
        text(8, 231, "ALGO / K2 CHOOSE  SAVE APPLY", 1, DIM, BACK);
    } else if (visible == ACTIONS) {
        if (action_confirm) text(8, 38, "REPLACE PATTERN DATA?", 1, AMBER, BACK);
        else {
            text(8, 38, "T", 1, DIM, BACK); number(16, 38, track + 1, 1, 1, TEXT, BACK);
            text(36, 38, "STEP", 1, DIM, BACK); number(68, 38, cursor + 1, 2, 1, ACCENT, BACK);
            text(94, 38, "PAGE", 1, DIM, BACK); number(126, 38, cursor / 16 + 1, 1, 1, ACCENT, BACK);
            unsigned count = groove_edit_clipboard_count();
            text(154, 38, count == 1 ? "CLIP STEP" : count == 16 ? "CLIP PAGE" : count ? "CLIP TRACK" : "CLIP EMPTY", 1, DIM, BACK);
        }
        unsigned first = action < 4 ? 0 : action > ACTION_COUNT - 5 ? ACTION_COUNT - 8 : action - 3;
        for (unsigned i = 0; i < 8; ++i) {
            unsigned at = first + i, y = 57 + i * 19;
            uint16_t bg = at == action ? PANEL : BACK;
            paint_fill(6, y, 228, 17, bg);
            text(10, y + 5, actions[at], 1, at == action ? ACCENT : DIM, bg);
        }
        text(8, 213, notice ? notice : "SELECT ACTION  SAVE EXECUTE", 1, TEXT, BACK);
        text(8, 231, action_confirm ? "SAVE CONFIRM   HOME CANCEL" : "PRESETS STEP  OCT PAGE  SAVE DO", 1, DIM, BACK);
    } else if (visible == ARP_EDIT) {
        text(8, 40, "ARPEGGIATOR", 2, ACCENT, BACK);
        text(184, 40, "T", 2, colors[track], BACK);
        number(198, 40, track + 1, 1, 2, colors[track], BACK);
        text(8, 72, !groove_arp_enabled(track) ? "OFF - TAP ARP TO ENABLE" : groove_arp_setting(track, 5) ? "ON - HOLD LATCHES YOUR CHORD" : "ON - HOLD KEYS TO PLAY", 1, TEXT, BACK);
        text(8, 96, "SELECT TRACK  K1-K4 ADJUST", 1, DIM, BACK);
        text(8, 118, "RATE FOLLOWS BPM / MIDI CLOCK", 1, DIM, BACK);
        text(8, 140, arp_page ? "RELEASE ALL KEYS FOR A NEW CHORD" : groove_pad_count(track) ? "DRUMS: RANGE / GATE UNAVAILABLE" : "UP / DOWN / UP-DOWN / RANDOM", 1, DIM, BACK);
        text(8, 162, "SETTINGS SAVED WITH PROJECT", 1, DIM, BACK);
        text(8, 180, arp_page ? "2/2 SWING / HOLD  ALGO PAGE" : "1/2 NOTES  ALGO PAGE", 1, ACCENT, BACK);
        static const char *const rates[] = {"1/4", "1/8", "1/16", "1/32"};
        static const char *const modes[] = {"UP", "DOWN", "UP-DN", "RANDOM"};
        char range[12], gate[12];
        decimal(range, groove_arp_setting(track, 2) + 1, 1); strcat(range, " OCT");
        decimal(gate, (groove_arp_setting(track, 3) + 1) * 25, 1); strcat(gate, "%");
        if (!arp_page) {
            knob(0, "RATE", rates[groove_arp_setting(track, 0)], colors[0]);
            knob(1, "DIRECTION", modes[groove_arp_setting(track, 1)], colors[1]);
            knob(2, "RANGE", groove_pad_count(track) ? "--" : range, colors[2]);
            knob(3, "GATE", groove_pad_count(track) ? "--" : gate, colors[3]);
        } else {
            char swing[12]; decimal(swing, groove_arp_setting(track, 4), 1); strcat(swing, "%");
            knob(0, "SWING", swing, colors[0]);
            knob(1, "HOLD", groove_arp_setting(track, 5) ? "ON" : "OFF", colors[1]);
            knob(2, "-", "--", colors[2]); knob(3, "-", "--", colors[3]);
        }
        text(8, 231, notice ? notice : "ARP ON/OFF   HOME BACK", 1, DIM, BACK);
    } else {
        text(8, 40, "DX7 QUICK EDIT", 2, ACCENT, BACK);
        char name[11]; memcpy(name, frame_patch + 145, 10); name[10] = 0;
        number(8, 76, frame_views[track].program + 1, 3, 1, ACCENT, BACK);
        text(26, 76, "/", 1, DIM, BACK);
        number(32, 76, (int)GROOVE_ENGINE_PROGRAMS, 3, 1, DIM, BACK);
        text(50, 76, frame_views[track].modified ? "*" : " ", 1, AMBER, BACK);
        text(56, 72, name, 2, TEXT, BACK);
        text(8, 102, "PRESETS SELECT VOICE", 1, TEXT, BACK);
        text(8, 126, "HOLD EDIT OPERATOR PARAMETERS", 1, DIM, BACK);
        text(8, 148, "ENV ENVELOPE    LFO MODULATION", 1, DIM, BACK);
        text(8, 170, "SCL SCALING     KEYS PLAY", 1, DIM, BACK);
        static const char *const labels[] = {"ALGORITHM", "FEEDBACK", "TRANSPOSE", "LEVEL"};
        int values[] = {frame_patch[134] + 1, frame_patch[135], (int)frame_patch[144] - 24, frame_views[track].volume};
        for (unsigned i = 0; i < 4; ++i) { char value[12]; decimal(value, values[i], 1); knob(i, labels[i], value, colors[i]); }
        text(8, 231, "EDIT BACK  HOLD EDIT MORE", 1, DIM, BACK);
    }
}
static __attribute__((noinline, cold, minsize)) void boot_draw(void)
{
    if (cache->title_cache != 0) {
        cache->title_cache = 0;
        text(86, 48, "FM1", 4, TEXT, BACK);
        text(81, 98, "OFFICIAL OPEN", 1, ACCENT, BACK);
        text((240 - 12 * (sizeof FM1_RELEASE_VERSION - 1)) / 2, 120,
             FM1_RELEASE_VERSION, 2, TEXT, BACK);
        text(102, 208, "M-VAVE", 1, DIM, BACK);
    }
    if (cache->middle_cache == boot_phase) return;
    cache->middle_cache = boot_phase;
    static const uint8_t height[8] = {8, 18, 30, 42, 42, 30, 18, 8};
    for (unsigned i = 0; i < 8; ++i)
        paint_fill(58 + 16 * i, 166 - height[i] / 2, 12, height[i],
                   i < boot_phase ? colors[i] : PANEL);
}
static void draw_page_body(const struct fm1_ble_status *ble)
{
    unsigned visible = boot_active ? STARTUP : glo_down && glo_used ? MIX : page;
    if (visible != cache->shown_page) {
        cache->shown_page = visible;
        paint_fill(0, 0, 240, 240, BACK);
        if (visible != STARTUP) paint_fill(0, 29, 240, 1, PANEL);
        cache->header_cache = cache->title_cache = cache->middle_cache = cache->hint_cache = cache->hold_cache = cache->seq_cache = ~0U;
        memset(cache->foot_cache, 0xff, sizeof cache->foot_cache);
        memset(cache->row_cache, 0xff, sizeof cache->row_cache);
        memset(cache->cell_cache, 0xff, sizeof cache->cell_cache);
        memset(cache->tile_cache, 0xff, sizeof cache->tile_cache);
        memset(cache->meter_cache, 0xff, sizeof cache->meter_cache);
        if (visible == PROJECT || visible == PRESET_BROWSER)
            text(8, 8, visible == PROJECT ? "SAVE PROJECT" : "PRESETS", 2, ACCENT, BACK);
    }
    if (visible == STARTUP) { boot_draw(); return; }
    const struct groove_view *views = frame_views, *v = &views[track];
    if (!paint_rendering) leds(v, visible);
    if (visible == PRESET_BROWSER) {
        preset_draw();
        return;
    }
    if (visible == PROJECT) {
        project_draw();
        return;
    }
    if (visible == BLE) {
        fm1_bluetooth_get_status(&frame_radio);
        frame_radio_flags = groove_settings_radio_flags();
        frame_radio_role = groove_settings_role_value();
        draw_ble(ble);
        return;
    }
    header(v);
    if (visible >= SOURCE) {
        edit_page_draw(visible);
        return;
    }
    if (visible == NAME) {
        name_draw();
        return;
    }
    if (visible == EFFECTS) {
        effects_draw();
        if (cache->hint_cache != 1U) {
            cache->hint_cache = 1U;
            text(8, 231, "SELECT TRACK  PRESETS PAGE", 1, DIM, BACK);
        }
        return;
    }
    if (visible == FM_LFO && v->type != GROOVE_DX7) {
        track_lfo_draw();
        return;
    }
    const uint8_t *patch = frame_patch;
    const struct groove_fm_controls controls = frame_controls;
    if (visible == OVERVIEW)
        overview(views);
    else if (visible == STEPS)
        steps(v);
    else if (visible == MIX)
        mix_draw(views);
    else if (visible == TONE_EDIT || (visible == FM_ENV && v->type != GROOVE_DX7)) {
        tone_draw(patch);
    } else {
        fm_draw(patch, &controls);
    }
    if (visible == STEPS) {
        unsigned parameter = seq_page == 3 ? groove_step_setting(track, cursor, 5) : 0;
        uint32_t strip = parameter | (seq_page << 10) | ((unsigned)v->type << 13);
        if (cache->seq_cache != strip) {
            cache->seq_cache = strip;
            paint_fill(0, 184, 240, 11, BACK);
            text(8, 186, parameter ? groove_lock_parameter_name(track, parameter - 1) : seq_pages[seq_page], 1, ACCENT, BACK);
            if (!parameter) text(174, 186, "ALGO PAGE", 1, DIM, BACK);
        }
    }
    for (unsigned col = 0; col < 4; ++col) {
        char value[12];
        const char *label;
        uint16_t color = colors[col];
        if (visible == STEPS && (seq_page == 1 || seq_page == 3)) {
            step_knob(col, seq_page == 3); continue;
        } else if (visible == STEPS && seq_page == 2) {
            static const char *const labels[] = {"SWING", "CLICK", "COUNT IN", "LENGTH"};
            static const char *const clicks[] = {"OFF", "REC", "ON"};
            label = labels[col];
            if (!col) { decimal(value, groove_sequence_setting(0), 1); strcat(value, "%"); }
            else if (col == 1) strcpy(value, clicks[groove_sequence_setting(1)]);
            else if (col == 2) strcpy(value, groove_sequence_setting(2) ? "1 BAR" : "OFF");
            else decimal(value, v->next_length, 1);
        } else if (visible == STEPS && seq_page == 4) {
            static const char *const labels[] = {"CLOCK", "BPM", "LENGTH", "-"};
            label = labels[col];
            if (!col) strcpy(value, v->clock_mode ? "MIDI" : "INT");
            else if (col == 1) {
                if (v->clock_mode) {
                    label = "MIDI BPM";
                    if (v->tempo) decimal(value, v->tempo, 1);
                    else strcpy(value, v->clock_state == GROOVE_CLOCK_LOST ? "LOST" : "WAIT");
                }
                else decimal(value, v->tempo, 1);
            } else if (col == 2) decimal(value, v->next_length, 1);
            else strcpy(value, "--");
        } else if (visible == MIX) {
            unsigned t = (track & ~3U) + col;
            static const char *const labels[8] = {"TRACK 1", "TRACK 2", "TRACK 3", "TRACK 4",
                                                  "TRACK 5", "TRACK 6", "TRACK 7", "TRACK 8"};
            label = labels[t];
            color = colors[t];
            decimal(value, views[t].volume, 1);
        } else if (visible == TONE_EDIT || (visible == FM_ENV && v->type != GROOVE_DX7)) {
            unsigned parameter = tone_parameter(col);
            const fm1_param_t *p = groove_tone_parameter(track, tone_part(), parameter);
            label = p ? p->name : "";
            if (!p)
                value[0] = 0;
            else
                tone_label(parameter, patch[parameter], p, value);
        } else if (visible == FM_ENV) {
            static const char *const labels[] = {"ATTACK", "DECAY", "SUSTAIN", "RELEASE"};
            label = labels[col];
            decimal(value, controls.envelope[col], 1);
            strcat(value, "%");
        } else if (visible == FM_EDIT || visible == FM_LFO) {
            unsigned parameter = fm_parameter(col);
            label = fm_pages[fm_group].label[col];
            if (parameter == 255)
                value[0] = 0;
            else if (parameter < 126 && parameter % 21 == 17)
                strcpy(value, patch[parameter] ? "FIXED" : "RATIO");
            else if (parameter == 136 || parameter == 141)
                strcpy(value, patch[parameter] ? "ON" : "OFF");
            else if (parameter < 126 && parameter % 21 == 20)
                decimal(value, (int)patch[parameter] - 7, 1);
            else if (parameter < 126 && parameter % 21 == 8)
                note_name(patch[parameter] + 9, value);
            else if (parameter < 126 && (parameter % 21 == 11 || parameter % 21 == 12)) {
                static const char *const curves[] = {"-LN", "-EX", "+EX", "+LN"};
                strcpy(value, curves[patch[parameter] & 3]);
            } else if (parameter == 142) {
                static const char *const waves[] = {"TRI", "SAWDN", "SAWUP", "SQR", "SINE", "S/H"};
                strcpy(value, waves[patch[parameter] % 6]);
            } else
                decimal(value,
                        patch[parameter] + (parameter == 134   ? 1
                                            : parameter == 144 ? -24
                                                               : 0),
                        1);
        } else {
            static const char *const labels[4] = {"LEVEL", "STEP", "NOTE", "BPM"};
            label = labels[col];
            if (col == 0)
                decimal(value, v->volume, 1);
            else if (col == 1 && visible == OVERVIEW) {
                label = "SOURCE";
                strcpy(value, source_names[v->type]);
            } else if (col == (visible == OVERVIEW ? 2U : 1U)) {
                label = visible != OVERVIEW && step_follow && v->playing ? "PLAY" : "STEP";
                decimal(value, step_follow && v->playing ? v->play_step + 1U : cursor + 1U, 2);
            } else if (col == 2 && groove_pad_count(track) != 0) {
                label = v->type == GROOVE_SAMPLE ? "SLOT" : "DRUM";
                decimal(value, drum + 1, 2);
            } else if (col == 2)
                note_name(selected_note, value);
            else if (v->clock_mode) {
                label = "MIDI";
                if (!v->tempo)
                    strcpy(value, v->clock_state == GROOVE_CLOCK_LOST ? "LOST" : "WAIT");
                else
                    decimal(value, v->tempo, 1);
            } else
                decimal(value, v->tempo, 1);
        }
        knob(col, label, value, color);
    }
    if (notice && (int32_t)(frame_time - notice_until) >= 0) notice = NULL;
    const char *hint = notice ? notice : source_result ? "SOURCE MEMORY FULL - RETRY"
                       : v->clock_mode && v->clock_state != GROOVE_CLOCK_LOCKED
                           ? "WAIT MIDI CLOCK - SEQ SYNC: INT/MIDI"
                       : writing && groove_sequence_count_in() ? "COUNT IN - RECORD ON NEXT BAR"
                       : writing ? v->playing ? "LIVE REC - PLAY KEYS  REC TO FINISH"
                                                             : "STEP REC - PLAY KEYS  REC TO FINISH"
                       : visible == OVERVIEW && (previous_buttons & BUTTON(HOME))
                           ? "PRESETS SOURCE  EDIT PARAMETERS"
                       : visible == OVERVIEW ? "PRESETS PRESET  ALGO ENGINE"
                       : visible == STEPS    ? "PRESETS STEP  OCT PAGE  HOLD SEQ MORE"
                       : visible == MIX      ? "KEYS MUTE/SOLO  GLO / HOME BACK"
                       : visible == FM_EDIT  ? "BLACK OP  SELECT PAGE  EDIT BACK"
                       : visible == FM_ENV && v->type != GROOVE_DX7 ? "ENV BACK  SELECT TRACK"
                       : visible == FM_ENV   ? "PRESETS ENV ON/OFF  HOME TRACKS"
                       : visible == TONE_EDIT && !advanced_edit
                           ? "HOLD EDIT MORE  SELECT TRACK"
                           : "SEL PAGE  HOME TRACKS";
    uint32_t hint_sig = signature(0, hint, (unsigned)strlen(hint));
    if (hint_sig != cache->hint_cache) {
        cache->hint_cache = hint_sig;
        paint_fill(6, 231, 228, 7, BACK);
        text(6, 231, hint, 1, DIM, BACK);
    }
}
static void draw_page(const struct fm1_ble_status *ble)
{
    draw_page_body(ble);
    if (boot_active || (paint_rendering && paint_top >= 30)) return;
    uint32_t since = 0;
    unsigned active = 1;
    if (glo_down && !glo_used) since = glo_ms;
    else if (edit_down && !edit_long) since = edit_ms;
    else if (arp_down && !arp_long) since = arp_ms;
    else if (seq_down) since = seq_ms;
    else active = 0;
    unsigned width = active ? (unsigned)clamp((int)(frame_time - since), 0, 600) * 224U / 600U : 0;
    if (cache->hold_cache == width) return;
    cache->hold_cache = width;
    paint_fill(8, 27, 224, 2, BACK);
    if (width) paint_fill(8, 27, width, 2, ACCENT);
}
static void draw(const struct fm1_ble_status *ble)
{
    unsigned visible = glo_down && glo_used ? MIX : page;
    frame_time = fm1_millis();
    if (boot_active) {
        boot_phase = (frame_time - boot_started) / 120U;
        if (boot_phase > 8) boot_phase = 8;
    }
    if (notice && (int32_t)(frame_time - notice_until) >= 0) notice = NULL;
    for (unsigned t = 0; t < 8; ++t) {
        unsigned n = groove_midi_activity(t);
        if (n != midi_seen[t]) { midi_seen[t] = n; midi_until[t] = frame_time + 120U; }
        midi_active[t] = (int32_t)(midi_until[t] - frame_time) > 0;
    }
    frame_ble = *ble;
    if (visible == BLE) {
        memset(frame_devices, 0, sizeof frame_devices);
        for (unsigned i = 0; i < ble->count && i < FM1_BLE_MAX_DEVICES; ++i)
            fm1_ble_get_device(i, &frame_devices[i]);
    }
    for (unsigned t = 0; t < GROOVE_TRACKS; ++t)
        groove_get_view(t, cursor, &frame_views[t]);
    if (visible == STEPS) {
        unsigned first = step_first(&frame_views[track]);
        for (unsigned i = 0; i < 16; ++i) {
            struct groove_view cell = {0};
            groove_get_view(track, first + i, &cell);
            frame_step_notes[i] = cell.step.note;
        }
    }
    if (visible == EFFECTS) {
        groove_effects_get(frame_effects);
        if (!effects_page) {
            uint8_t sends[2]; groove_sends_get(track, sends);
            frame_effects[0] = (uint8_t)((sends[0] * 100U + 7U) / 15U);
            frame_effects[1] = (uint8_t)((sends[1] * 100U + 7U) / 15U);
            frame_effects[2] = frame_views[track].volume;
        }
    }
    memset(frame_patch, 0, sizeof frame_patch);
    memset(&frame_controls, 0, sizeof frame_controls);
    if (visible == TONE_EDIT || (visible == FM_ENV && groove_source_type(track) != GROOVE_DX7))
        groove_tone_copy(track, tone_part(), frame_patch);
    else if (visible == FM_LFO && groove_source_type(track) != GROOVE_DX7)
        groove_lfo_copy(track, frame_patch);
    else if ((visible >= FM_EDIT && visible <= FM_LFO) || visible == NAME || visible == FM_QUICK)
        groove_fm_copy(track, frame_patch, &frame_controls);
    /* Merge new damage with unfinished bands. Input can replace a pending
     * frame; no stale queue of old encoder positions needs to be displayed. */
    paint_priority = 0;
    cache = &saved_cache;
    draw_page(&frame_ble);
}
static void paint_flush(void)
{
    uint32_t started = fm1_millis();
    paint_rendering = 1;
    cache = &band_cache;
    for (unsigned visited = 0; visited < PAINT_BANDS; ++visited) {
        int pending = fm1_screen_pending();
        if (pending) {
            if (pending < 0) saved_cache.shown_page = ~0U;
            break;
        }
        unsigned band = paint_next;
        /* Fresh knob/key feedback goes ahead of the rest of a page repaint.
         * Rotate within that set so continuous motion cannot starve its text. */
        if (paint_priority)
            for (unsigned i = 0; i < PAINT_BANDS; ++i) {
                unsigned candidate = (paint_next + i) % PAINT_BANDS;
                if (paint_priority & (1U << candidate)) { band = candidate; break; }
            }
        paint_next = (band + 1U) % PAINT_BANDS;
        unsigned left = paint_left[band], width = paint_right[band] - left;
        if (left >= paint_right[band]) continue;
        paint_top = band * PAINT_ROWS;
        paint_x = left;
        paint_end = left + width;
        memset(cache, 0xff, sizeof *cache);
        draw_page(&frame_ble);
        /* Rasterize directly into the tightly packed DMA rectangle. */
        if (fm1_screen_submit(left, paint_top, width, PAINT_ROWS,
                            paint_pixels, width * PAINT_ROWS)) {
            saved_cache.shown_page = ~0U; /* Retry the whole page after a transport error. */
            break;
        }
        paint_left[band] = FM1_SCREEN_WIDTH;
        paint_right[band] = 0;
        paint_priority &= ~(1U << band);
        /* The DMA borrows paint_pixels. The next iteration/poll must observe
         * completion before composing another band into that same buffer. */
        if (fm1_millis() - started >= 4U) break;
    }
    paint_rendering = 0;
    cache = &saved_cache;
}
static int release_keys(void)
{
    for (unsigned key = 0; key < 27; ++key)
        if (key_note[key] != 0xff) {
            if (!post((uint8_t)(0x80 + groove_channel(key_track[key])), key_note[key], 0, 3))
                return 0;
            key_note[key] = 0xff;
        }
    return 1;
}
/* HOME and the list load immediately, including over unsaved edits.
 * Selecting a preset replaces the workspace without writing a project slot. */
static int preset_load(unsigned start, uint32_t now)
{
    if (!release_keys()) return -1;
    blocked_keys |= previous_keys;
    project_result = preset_bank ? groove_factory_load(preset_slot) : groove_project_load_slot(preset_slot);
    if (project_result) { page = PRESET_BROWSER; return project_result; }
    preset_loaded = 1;
    preset_project_revision = groove_project_revision();
    preset_current_slot = preset_slot; preset_current_bank = preset_bank;
    writing = cursor = drum = advanced_edit = 0; step_follow = 1;
    memset(operator_page, 0, sizeof operator_page);
    memset(operator_selected, 0, sizeof operator_selected);
    memset(lfo_page, 0, sizeof lfo_page);
    memset(tone_pages, 0, sizeof tone_pages);
    memset(drum_parts, 0, sizeof drum_parts);
    selected_note = groove_pad_count(track) ? groove_pad_note(track, 0) : 60;
    page = music_page = OVERVIEW;
    notice = start ? "PRESET LOADED - PLAYING" : "PRESET LOADED - PLAY TO START";
    notice_until = now + 2000U;
    if (start && !post(0xfa, 0, 0, 1)) return -1;
    return 0;
}
static void toggle_step(unsigned step)
{
    struct groove_view v = {0};
    groove_get_view(track, step, &v);
    unsigned enabled =
        groove_pad_count(track) != 0 ? (v.step.drums & (1U << drum)) != 0 : v.step.velocity != 0;
    if (!groove_pad_count(track) && enabled) selected_note = v.step.note;
    groove_sequence_edit(track, step, selected_note, enabled ? 0 : 100);
    cursor = step;
    step_follow = 0;
    if (page != TONE_EDIT && groove_pad_count(track)) drum_parts[track] = (uint8_t)drum;
}
void groove_screen_init(void)
{
    track = cursor = drum = writing = page = fm_group = op = 0;
    music_page = subpage_return = project_return = rec_before_press = OVERVIEW;
    rec_down = rec_used = 0;
    selected_note = 60;
    step_follow = 1;
    transport_seen = 0;
    notice = NULL;
    octave = 0;
    source_preview = action = action_confirm = tone_clip_ready = 0;
    seq_page = arp_page = 0;
    memset(midi_seen, 0, sizeof midi_seen);
    memset(midi_until, 0, sizeof midi_until);
    previous_buttons = previous_keys = blocked_keys = retry_reset = 0;
    glo_down = glo_used = ble_device = ble_was_linked = ble_view = ble_action = 0;
    ble_result = source_result = 0;
    ble_forget = ble_stage = ble_seconds = 0;
    ble_stage_since = fm1_millis();
    struct fm1_engine_stats health;
    fm1_engine_stats_read(&health);
    health_at = fm1_millis(); health_underruns = health.underruns; health_dropped = health.midi_dropped;
    struct fm1_audio_stats dma;
    fm1_audio_stats_read(&dma);
    health_dma = dma.overruns; health_adapted = 0;
    project_result = 0;
    project_reload = project_operation = 0;
    project_slot = project_factory = name_cursor = 0;
    preset_slot = preset_loaded = 0;
    preset_bank = 1;
    preset_current_slot = 0; preset_current_bank = 1;
    advanced_edit = edit_down = edit_long = 0;
    seq_down = 0;
    arp_down = arp_long = 0;
    memset(project_stored, 0, sizeof project_stored);
    cache->shown_page = ~0U;
    led_cache = button_led_cache = 0;
    memset(key_note, 0xff, sizeof key_note);
    memset(operator_page, 0, sizeof operator_page);
    memset(operator_selected, 0, sizeof operator_selected);
    memset(lfo_page, 0, sizeof lfo_page);
    memset(tone_pages, 0, sizeof tone_pages);
    memset(drum_parts, 0, sizeof drum_parts);
    operator_held = operator_long = 0;
    memset(paint_left, FM1_SCREEN_WIDTH, sizeof paint_left);
    memset(paint_right, 0, sizeof paint_right);
    paint_next = paint_rendering = paint_priority = 0;
    memset(encoder_direction, 0, sizeof encoder_direction);
    memset(encoder_speed, 0, sizeof encoder_speed);
    fm1_controls_init();
    for (unsigned key = 0; key < 41; ++key) fm1_led_set(key, 0);
    fm1_screen_init();
    boot_started = fm1_millis();
    boot_active = 1;
    struct fm1_ble_status ble = {0};
    fm1_ble_get_status(&ble);
    draw(&ble);
    paint_flush();
    frame_ms = fm1_millis();
}
static void select_track(int delta)
{
    unsigned before = track;
    track = (unsigned)clamp((int)track + delta, 0, 7);
    if (page == FM_QUICK && groove_source_type(track) != GROOVE_DX7) page = TONE_EDIT;
    if (page == TONE_EDIT && !advanced_edit && groove_source_type(track) == GROOVE_DX7) page = FM_QUICK;
    if ((groove_source_type(track) != GROOVE_DX7 && page == FM_EDIT) ||
        (groove_source_type(track) == GROOVE_DX7 && page == TONE_EDIT)) page = OVERVIEW;
    if (before != track) {
        cursor %= groove_sequence_length(track);
        cache->middle_cache = ~0U; source_result = 0;
        drum = drum_parts[track] < groove_pad_count(track) ? drum_parts[track] : 0;
        if (!groove_pad_count(track) && groove_pad_count(before)) selected_note = 60;
    }
    if (groove_pad_count(track)) {
        drum %= groove_pad_count(track);
        selected_note = groove_pad_note(track, drum);
    }
}
static int apply_controls(const struct fm1_controls *controls, uint32_t now,
                          const struct fm1_ble_status *ble)
{
    uint32_t pressed = controls->buttons & ~previous_buttons;
    uint32_t old_keys = previous_keys, key_down = controls->keys & ~old_keys;
    unsigned play_handled = 0;
    previous_buttons = controls->buttons;
    previous_keys = controls->keys;
    blocked_keys &= controls->keys;
    if (pressed || encoder_active(controls->encoders)) notice = NULL;
    if (retry_reset) {
        blocked_keys = controls->keys;
        if (!post(0xff, 0, 0, 1))
            return 0;
        retry_reset = 0;
        memset(key_note, 0xff, sizeof key_note);
    }
    unsigned old_page = page, old_mode = navigation_mode();
    unsigned arp_tap = arp_down && !(controls->buttons & BUTTON(ARP)) && !arp_long;
    if (!(controls->buttons & BUTTON(ARP))) arp_down = arp_long = 0;
    if (pressed & BUTTON(ARP)) {
        arp_down = 1; arp_long = 0; arp_ms = now;
    }
    if (!(controls->buttons & BUTTON(EDIT))) edit_down = edit_long = 0;
    if (!(controls->buttons & BUTTON(SEQ))) seq_down = 0;
    if (pressed & BUTTON(SEQ)) {
        seq_down = 1; seq_ms = now;
    }
    if (page == OVERVIEW || page == STEPS) music_page = page;
    if (pressed & BUTTON(REC)) rec_before_press = writing;
    /* HOME defers REC until release so REC+SELECT never starts a take.
     * A held gesture preserves the previous recording state. */
    if ((pressed & BUTTON(REC)) && page == OVERVIEW && !glo_down &&
        !(controls->buttons & ~BUTTON(REC))) {
        rec_down = 1; rec_used = 0; rec_ms = now;
    }
    if (rec_down) {
        if (page != OVERVIEW || (pressed & ~BUTTON(REC))) rec_down = 0;
        else if (!(controls->buttons & BUTTON(REC))) {
            if (!rec_used && now - rec_ms < 600U) writing ^= 1U;
            rec_down = 0;
        } else {
            pressed &= ~BUTTON(REC);
            if (controls->encoders[0]) {
                rec_used = 1;
                if (now - rec_ms >= 600U) {
                    cursor = (unsigned)clamp((int)cursor + controls->encoders[0], 0,
                        (int)groove_sequence_length(track) - 1);
                    groove_edit_seek(track, cursor);
                    step_follow = 0;
                }
                goto controls_done;
            }
        }
    }
    /* Emergency stop has the same meaning in every modal page. */
    if ((controls->buttons & (BUTTON(HOME) | BUTTON(PLAY))) ==
            (BUTTON(HOME) | BUTTON(PLAY)) && (pressed & (BUTTON(HOME) | BUTTON(PLAY)))) {
        blocked_keys |= controls->keys;
        if (!post(0xff, 0, 0, 1)) return 0;
        memset(key_note, 0xff, sizeof key_note);
        page = music_page = OVERVIEW;
        writing = glo_down = glo_used = project_reload = edit_down = edit_long = 0;
        arp_down = arp_long = seq_down = rec_down = 0;
        groove_edit_record(0, track, cursor);
        return 1;
    }
    /* A project received over MIDI can replace the current instrument while
     * its editor is open. Leave editors that cannot apply to the new source. */
    if (page == NAME && groove_source_type(track) != GROOVE_DX7)
        page = OVERVIEW;
    if ((controls->buttons & (BUTTON(HOME) | BUTTON(SAVE))) == (BUTTON(HOME) | BUTTON(SAVE)) &&
        (pressed & (BUTTON(HOME) | BUTTON(SAVE)))) {
        int r = groove_edit_undo();
        notice = r ? "NOTHING TO UNDO / STOP REC FIRST" : "UNDO / REDO APPLIED";
        notice_until = now + 2000U;
        page = music_page;
        goto controls_done;
    }
    if ((controls->buttons & (BUTTON(HOME) | BUTTON(GLO))) == (BUTTON(HOME) | BUTTON(GLO)) &&
        (pressed & (BUTTON(HOME) | BUTTON(GLO)))) {
        page = BLE; glo_down = glo_used = ble_view = ble_forget = ble_action = 0; ble_result = 0;
        goto controls_done;
    }
    /* Keep the original HOME+PRESETS shortcut, including after HOME opens
     * the browser. A source replacement still requires an explicit SAVE. */
    if ((controls->buttons & BUTTON(HOME)) && controls->encoders[6] &&
        !(controls->buttons & BUTTON(PLAY))) {
        source_preview = (unsigned)((int)groove_source_type(track) +
            controls->encoders[6] % GROOVE_SOURCES + GROOVE_SOURCES) % GROOVE_SOURCES;
        page = SOURCE; source_result = 0;
        goto controls_done;
    }
    if ((controls->buttons & (BUTTON(SEQ) | BUTTON(EDIT))) == (BUTTON(SEQ) | BUTTON(EDIT)) &&
        (pressed & (BUTTON(SEQ) | BUTTON(EDIT)))) {
        if (!release_keys()) return 0;
        blocked_keys |= controls->keys;
        page = ACTIONS; action_confirm = 0; edit_down = seq_down = 0;
        goto controls_done;
    }
    if (seq_down && page == STEPS && now - seq_ms >= 600U) {
        page = ACTIONS; action_confirm = 0; seq_down = edit_down = 0;
        goto controls_done;
    }
    if (arp_down && !arp_long && now - arp_ms >= 600U) {
        arp_long = 1; if (page != ARP_EDIT) arp_page = 0; page = ARP_EDIT;
        goto controls_done;
    }
    /* Panel shortcuts precede modal editors. A menu must not swallow a
     * navigation key, transport control or the release of a held gesture. */
    if (glo_down && !(controls->buttons & BUTTON(GLO))) {
        unsigned tapped = !glo_used && now - glo_ms < 600U;
        glo_down = 0;
        blocked_keys |= controls->keys;
        if (tapped) page = page == MIX ? music_page : MIX;
    }
    if ((controls->buttons & (BUTTON(EDIT) | BUTTON(REC))) == (BUTTON(EDIT) | BUTTON(REC)) &&
        (pressed & (BUTTON(EDIT) | BUTTON(REC))) && groove_source_type(track) == GROOVE_DX7) {
        writing = rec_before_press;
        edit_down = glo_down = 0;
        page = NAME; name_cursor = 0;
        goto controls_done;
    }
    if (pressed & (BUTTON(HOME) | BUTTON(SEQ) | BUTTON(FX) | BUTTON(SCL) |
                   BUTTON(EDIT) | BUTTON(ENV) | BUTTON(LFO))) {
        glo_down = glo_used = 0;
        if (pressed & BUTTON(HOME)) {
            if (page == ACTIONS && action_confirm) action_confirm = 0;
            else if (page == BLE && ble_forget == 1) ble_forget = 0;
            else if (page == OVERVIEW) {
                preset_refresh(); project_result = 0; page = PRESET_BROWSER;
            } else page = OVERVIEW;
        } else if (pressed & BUTTON(SEQ)) {
            page = STEPS; seq_page = 0; step_follow = 1; cache->shown_page = ~0U;
        } else if (pressed & BUTTON(FX)) {
            if (page != EFFECTS) effects_page = 0;
            open_subpage(EFFECTS);
        } else if (pressed & BUTTON(SCL)) {
            if (groove_source_type(track) == GROOVE_DX7) {
                op = operator_selected[track] < 6 ? operator_selected[track] : 0;
                operator_selected[track] = (uint8_t)op;
                operator_page[track][op] = (uint8_t)(page == FM_EDIT && fm_group == 4 ? 5 : 4);
                fm_group = operator_page[track][op]; page = FM_EDIT;
            } else {
                advanced_edit = 1; page = TONE_EDIT;
                tone_pages[track][tone_slot()] = groove_source_type(track) == GROOVE_303 ? 1 : 0;
            }
        } else if (pressed & BUTTON(EDIT)) {
            unsigned edit = groove_source_type(track) == GROOVE_DX7 ? FM_QUICK : TONE_EDIT;
            edit_down = 1; edit_long = 0; edit_ms = now;
            page = page == FM_QUICK || page == FM_EDIT || page == edit ? music_page : edit;
            advanced_edit = 0;
            if (page == TONE_EDIT) tone_pages[track][tone_slot()] = 0;
            if (page == TONE_EDIT && groove_pad_count(track)) drum_parts[track] = (uint8_t)drum;
        } else if (pressed & BUTTON(ENV)) {
            if (groove_pad_count(track)) drum_parts[track] = (uint8_t)drum;
            open_subpage(FM_ENV);
        } else open_subpage(FM_LFO);
        goto controls_done;
    }
    if (edit_down && !edit_long && now - edit_ms >= 600U &&
        !(controls->buttons & (BUTTON(SEQ) | BUTTON(REC) | BUTTON(HOME)))) {
        edit_long = advanced_edit = 1;
        page = groove_source_type(track) == GROOVE_DX7 ? FM_EDIT : TONE_EDIT;
        goto controls_done;
    }
    if (arp_tap) {
        if (!release_keys()) return 0;
        blocked_keys |= controls->keys;
        groove_arp_set(track, !groove_arp_enabled(track));
        notice = groove_arp_enabled(track) ? "ARP ON / HOLD ARP FOR SETTINGS" : "ARP OFF / LIVE KEYS";
        notice_until = now + 2000U;
    }
    if (pressed & BUTTON(REC)) {
        writing ^= 1U;
        glo_down = 0;
        if (writing && (page == BLE || page == PROJECT || page == PRESET_BROWSER ||
                       page == MIX || page == NAME || (page >= SOURCE && page <= RHYTHM) ||
                       (page == STEPS && (seq_page == 1 || seq_page == 3)))) {
            page = STEPS; seq_page = 0; step_follow = 1;
        }
        goto controls_done;
    }
    if (page != STEPS && page != ACTIONS)
        octave = clamp(octave + !!(pressed & BUTTON(OCT_UP)) - !!(pressed & BUTTON(OCT_DOWN)), -2, 2);
    if ((pressed & BUTTON(SAVE)) && page != PROJECT && page != BLE &&
        page != SOURCE && page != ACTIONS) {
        project_return = page; page = PROJECT; project_result = 0;
        project_reload = project_operation = 0;
        project_slot = groove_project_slot(); project_factory = name_cursor = 0;
        glo_down = 0; project_refresh();
        goto controls_done;
    }
    if (pressed & BUTTON(GLO)) {
        glo_down = 1; glo_used = 0; glo_ms = now; blocked_keys |= old_keys;
    }
    if (glo_down || page == MIX) {
        if (glo_down && (now - glo_ms >= 600U || key_down || encoder_active(controls->encoders)))
            glo_used = 1;
        select_track(controls->encoders[0]);
        for (unsigned i = 0; i < 4; ++i)
            if (controls->encoders[2 + i])
                groove_control_edit((track & ~3U) + i, 0, controls->encoders[2 + i], 0);
        for (unsigned i = 0; i < 16; ++i)
            if ((key_down & ~blocked_keys) & (1U << white_keys[i]))
                groove_mix_toggle(i & 7U, i >= 8);
        goto controls_done;
    }
    if (page == OVERVIEW && !glo_down && controls->encoders[6]) {
        unsigned slots = preset_slots(preset_current_bank);
        int current = preset_loaded ? (int)preset_current_slot : controls->encoders[6] > 0 ? -1 : (int)slots;
        unsigned slot = (unsigned)clamp(current + controls->encoders[6], 0, (int)slots - 1);
        if (!preset_loaded || slot != preset_current_slot) {
            struct groove_view view = {0}; groove_get_view(track, cursor, &view);
            preset_slot = slot; preset_bank = preset_current_bank; preset_refresh();
            preset_load(view.playing, now);
        }
        goto controls_done;
    }
    if (page == PRESET_BROWSER) {
        play_handled = 1;
        int delta = controls->encoders[0] + controls->encoders[6];
        unsigned bank = (unsigned)clamp((int)preset_bank + controls->encoders[2], 0, 1);
        unsigned slot = (unsigned)clamp((int)preset_slot + delta, 0, (int)preset_slots(bank) - 1);
        unsigned load = 0, start = 0;
        for (unsigned i = 0; i < preset_slots(bank); ++i)
            if ((key_down & ~blocked_keys) & (1U << white_keys[i])) {
                struct groove_view view = {0}; groove_get_view(track, cursor, &view);
                slot = i; load = 1; start = view.playing;
            }
        if (bank != preset_bank || slot != preset_slot) {
            preset_bank = bank; preset_slot = slot; project_result = 0;
            preset_refresh();
            struct groove_view view = {0}; groove_get_view(track, cursor, &view);
            load = 1; start = view.playing;
        }
        if (pressed & BUTTON(PLAY)) { load = 1; start = 1; }
        if (load) {
            preset_load(start, now);
            if (!(pressed & BUTTON(PLAY)) && !key_down) page = PRESET_BROWSER;
        }
        goto controls_done;
    }
    if (page == SOURCE) {
        int delta = controls->encoders[0] + controls->encoders[1] + controls->encoders[3] + controls->encoders[6];
        source_preview = (unsigned)((int)source_preview + delta % GROOVE_SOURCES + GROOVE_SOURCES) % GROOVE_SOURCES;
        if (pressed & BUTTON(SAVE)) {
            source_result = groove_source_set(track, source_preview);
            if (!source_result) {
                memset(tone_pages[track], 0, sizeof tone_pages[track]);
                drum_parts[track] = drum = 0;
                selected_note = groove_pad_count(track) ? groove_pad_note(track, 0) : 60;
                page = music_page;
            }
        }
        goto controls_done;
    }
    if (page == ACTIONS) {
        int move = controls->encoders[6] + controls->encoders[3];
        int paging = !!(pressed & BUTTON(OCT_UP)) - !!(pressed & BUTTON(OCT_DOWN));
        if (move || paging) {
            cursor = (unsigned)clamp((int)cursor + move + paging * 16, 0,
                                     (int)groove_sequence_length(track) - 1);
            step_follow = action_confirm = 0;
        }
        for (unsigned i = 0; i < 16; ++i)
            if ((key_down & ~blocked_keys) & (1U << white_keys[i])) {
                cursor = (cursor & ~15U) + i;
                step_follow = action_confirm = 0;
            }
        if (controls->encoders[0]) {
            action = (unsigned)clamp((int)action + controls->encoders[0], 0, ACTION_COUNT - 1);
            action_confirm = 0;
        }
        if (pressed & BUTTON(SAVE)) {
            int r = 0;
            unsigned operation = action;
            unsigned destructive = (action >= 7 && action <= 13) || action == 16;
            if (destructive && !action_confirm) { action_confirm = 1; goto controls_done; }
            action_confirm = 0;
            if (action <= 1) { page = STEPS; seq_page = action + 1; step_follow = 0; cache->shown_page = ~0U; }
            else if (action <= 4) {
                r = groove_edit_copy(track, cursor, action - 2);
                if (!r) action += 3; /* Select the matching paste command. */
            }
            else if (action <= 7) {
                unsigned wanted = action == 5 ? 1U : action == 6 ? 16U : 64U;
                r = groove_edit_clipboard_count() == wanted ? groove_edit_paste(track, cursor, action - 5) : -3;
            }
            else if (action <= 10) r = groove_edit_clear(track, cursor, action - 8);
            else if (action <= 13) r = groove_edit_repeat(track, (action - 9) * 16);
            else if (action == 14) r = groove_edit_undo();
            else if (action == 15) { r = groove_timbre_export(track, (uint8_t *)&tone_clip, sizeof tone_clip); tone_clip_ready = !r; }
            else if (action == 16) r = tone_clip_ready ? groove_timbre_import(track, (uint8_t *)&tone_clip, sizeof tone_clip) : -1;
            else if (action == 17) {
                if (groove_source_type(track) == GROOVE_DX7) { page = NAME; name_cursor = 0; }
                else r = -1;
            } else if (action == 18) { page = BLE; ble_view = ble_forget = ble_action = 0; ble_result = 0; }
            else {
                if (!post(0xff, 0, 0, 1)) return 0;
                memset(key_note, 0xff, sizeof key_note); writing = 0; page = OVERVIEW;
            }
            notice = r == -3 ? "COPY MATCHING STEP / PAGE / TRACK FIRST"
                : r == -2 ? "CHORD / LOCK TABLE FULL" : r ? "UNAVAILABLE / INCOMPATIBLE"
                : operation == 2 ? "STEP COPIED - CHOOSE DESTINATION"
                : operation == 3 ? "PAGE COPIED - CHOOSE DESTINATION"
                : operation == 4 ? "TRACK COPIED - CHOOSE DESTINATION"
                : operation == 5 ? "STEP PASTED - UNDO AVAILABLE"
                : operation == 6 ? "PAGE PASTED - UNDO AVAILABLE" : "DONE";
            notice_until = now + 2000U;
            cache->middle_cache = ~0U;
        }
        goto controls_done;
    }
    if (page == PROJECT) {
        if (old_page == PROJECT) {
            if (controls->encoders[0] || controls->encoders[6]) {
                project_slot = (unsigned)clamp((int)project_slot + controls->encoders[0] + controls->encoders[6], 0, GROOVE_PROJECT_SLOTS - 1);
                project_reload = project_operation = 0;
                project_refresh();
            }
            name_cursor = (unsigned)clamp((int)name_cursor + controls->encoders[2], 0, 11);
            if (controls->encoders[3] && !project_factory) {
                char name[13];
                groove_project_name(name);
                groove_project_rename(name_cursor, edit_character(name[name_cursor], controls->encoders[3]));
                project_reload = 0;
            }
            if (pressed & BUTTON(SAVE)) {
                unsigned overwrite = project_slot != groove_project_slot() && !project_stored_result;
                project_reload = overwrite && !project_reload ? 2 : 0;
                project_result = 0;
                if (!project_reload) {
                    project_operation = 3;
                    draw(ble);
                    paint_next = 180U / PAINT_ROWS;
                    paint_flush();
                    project_result = groove_project_save_slot(project_slot);
                    project_operation = 1;
                    project_refresh();
                }
            }
        }
        if (!project_result && !project_reload && project_operation == 1) {
            page = project_return;
            if (page == PRESET_BROWSER) preset_refresh();
            notice = "PROJECT SAVED";
            notice_until = now + 2000U;
            project_operation = 0;
        }
    } else if (page == NAME) {
        name_cursor = (unsigned)clamp((int)name_cursor + controls->encoders[0], 0, 9);
        if (controls->encoders[2]) {
            uint8_t patch[156];
            if (!groove_fm_copy(track, patch, NULL))
                groove_fm_name_edit(track, name_cursor,
                    edit_character((char)patch[145 + name_cursor], controls->encoders[2]));
        }
    } else if (page == BLE) {
        if (encoder_active(controls->encoders)) ble_forget = 0;
        ble_action = (unsigned)clamp((int)ble_action + controls->encoders[6], 0, ble_view ? 3 : 1);
        if (!ble_view) {
            unsigned flags = groove_settings_radio_flags(), role = groove_settings_role_value();
            for (unsigned i = 0; i < 2; ++i) {
                int delta = controls->encoders[2 + i];
                if (delta > 0) flags |= 1U << i;
                else if (delta < 0) flags &= ~(1U << i);
            }
            if (controls->encoders[4]) {
                role = (unsigned)clamp((int)role + controls->encoders[4],
                                       FM1_BLE_PERIPHERAL, FM1_BLE_CENTRAL);
            }
            if (flags != groove_settings_radio_flags() || role != groove_settings_role_value())
                ble_result = groove_settings_radio(flags, role);
            if (controls->encoders[5])
                groove_settings_clock(controls->encoders[5] > 0 ? GROOVE_CLOCK_MIDI : GROOVE_CLOCK_INTERNAL);
            if (pressed & BUTTON(SAVE)) {
                if (ble_action) {
                    if (ble_forget != 1) ble_forget = 1;
                    else { ble_result = fm1_ble_forget_bonds(); ble_forget = ble_result ? 0 : 2; }
                } else if ((flags & (GROOVE_RADIO_ON | GROOVE_RADIO_MIDI)) ==
                    (GROOVE_RADIO_ON | GROOVE_RADIO_MIDI) && role == FM1_BLE_CENTRAL) {
                    ble_view = 1; ble_action = ble->count ? 0 : 1; ble_forget = 0;
                    if (ble->state == FM1_BLE_IDLE) ble_result = fm1_ble_scan();
                } else ble_result = -1;
            }
        } else {
            unsigned count = ble->count > FM1_BLE_MAX_DEVICES ? FM1_BLE_MAX_DEVICES : ble->count;
            ble_device = (unsigned)clamp((int)ble_device + controls->encoders[0], 0,
                                         count ? (int)count - 1 : 0);
            if (pressed & BUTTON(SAVE)) {
                if (!ble_action) ble_result = fm1_ble_connect(ble_device);
                else if (ble_action == 1) {
                    ble_result = fm1_ble_scan();
                    if (!ble_result) ble_device = 0;
                } else if (ble_action == 2) ble_result = fm1_ble_disconnect();
                else { ble_view = ble_action = ble_forget = 0; ble_result = 0; }
            }
        }
    } else {
        if (page != BLE) {
            unsigned tracks = page == OVERVIEW || page == STEPS || page == FM_QUICK || page == ARP_EDIT ||
                page == EFFECTS || page == FM_ENV || (page == FM_LFO && groove_source_type(track) != GROOVE_DX7) ||
                (page == TONE_EDIT && !advanced_edit);
            select_track(tracks ? controls->encoders[0] : 0);
            {
                if (page == OVERVIEW && (controls->encoders[1] || controls->encoders[3])) {
                    /* Type and timbre are separate controls. Finish old note-offs
                     * before replacing the instance; held keys wait for release. */
                    blocked_keys |= controls->keys;
                    if (!release_keys())
                        return 0;
                    int type = (int)groove_source_type(track);
                    type = (type + (controls->encoders[1] + controls->encoders[3]) % GROOVE_SOURCES + GROOVE_SOURCES) %
                           GROOVE_SOURCES;
                    source_preview = (unsigned)type;
                    source_result = 0;
                    page = SOURCE;
                    cache->shown_page = ~0U;
                    operator_held = operator_long = 0;
                    goto controls_done;
                }
                int oct_delta = (int)((pressed >> OCT_UP) & 1U) - (int)((pressed >> OCT_DOWN) & 1U);
                if (page == STEPS) {
                    unsigned next = (unsigned)clamp((int)seq_page + controls->encoders[1], 0, 4);
                    struct groove_view v = {0}; groove_get_view(track, cursor, &v);
                    int move = controls->encoders[6] + (!next ? controls->encoders[3] : 0);
                    if (next != seq_page || move || oct_delta) {
                        move_step(&v, move, oct_delta);
                        if (move) writing = 0;
                    }
                    if (next != seq_page) { seq_page = next; cache->shown_page = ~0U; }
                    if (seq_page == 1 || seq_page == 3) step_adjust(seq_page == 3, controls->encoders, now);
                    else if (seq_page == 2) {
                        for (unsigned i = 0; i < 3; ++i) groove_sequence_setting_edit(i, controls->encoders[2 + i]);
                        sequence_length_adjust(controls->encoders[5]);
                    } else if (seq_page == 4) {
                        if (controls->encoders[2]) groove_settings_clock(controls->encoders[2] > 0
                            ? GROOVE_CLOCK_MIDI : GROOVE_CLOCK_INTERNAL);
                        groove_tempo_edit(controls->encoders[3]);
                        sequence_length_adjust(controls->encoders[4]);
                    }
                }
                if (page == ARP_EDIT) {
                    arp_page = (unsigned)clamp((int)arp_page + controls->encoders[1], 0, 1);
                    for (unsigned i = 0; i < 4; ++i) if (controls->encoders[2 + i]) {
                        if (arp_page) { if (i < 2) groove_arp_edit(track, i + 4, controls->encoders[2 + i]); }
                        else if (groove_pad_count(track) && i >= 2) {
                            notice = "RANGE / GATE FOR MELODIC TRACKS"; notice_until = now + 2000U;
                        } else groove_arp_edit(track, i, controls->encoders[2 + i]);
                    }
                } else if (page == NAME) {
                    name_cursor = (unsigned)clamp((int)name_cursor + controls->encoders[0], 0, 9);
                    if (controls->encoders[2]) {
                        uint8_t name_patch[156];
                        if (!groove_fm_copy(track, name_patch, NULL))
                            groove_fm_name_edit(track, name_cursor,
                                edit_character((char)name_patch[145 + name_cursor], controls->encoders[2]));
                    }
                } else if (page == FM_QUICK) {
                    static const uint8_t parameters[] = {134, 135, 144};
                    for (unsigned i = 0; i < 3; ++i) groove_fm_edit(track, parameters[i], controls->encoders[2 + i]);
                    groove_control_edit(track, 0, controls->encoders[5], 0);
                } else if (page == EFFECTS) {
                    unsigned next = (unsigned)clamp((int)effects_page + controls->encoders[6], 0, 1);
                    if (next != effects_page) { effects_page = next; cache->shown_page = ~0U; }
                    if (effects_page) {
                        for (unsigned i = 0; i < 4; ++i)
                            groove_effects_edit(i, controls->encoders[2 + i]);
                    } else {
                        for (unsigned i = 0; i < 2; ++i)
                            groove_sends_edit(track, i, controls->encoders[2 + i]);
                        groove_control_edit(track, 0, controls->encoders[4], 0);
                        groove_effects_edit(3, controls->encoders[5]);
                    }
                } else if (page == FM_LFO && groove_source_type(track) != GROOVE_DX7) {
                    for (unsigned i = 0; i < 4; ++i)
                        groove_lfo_edit(track, i, i < 2 ? fm_encoder_delta(2 + i, controls->encoders[2 + i]) : controls->encoders[2 + i]);
                } else if (page == TONE_EDIT || (page == FM_ENV && groove_source_type(track) != GROOVE_DX7)) {
                    if (page == TONE_EDIT && groove_source_type(track) == GROOVE_303)
                        for (unsigned i = 8; i < 10; ++i)
                            if ((key_down & ~blocked_keys) & (1U << edit_keys[i]))
                                groove_mono_set(track, i == 8);
                    if (groove_pad_count(track) != 0) {
                        drum_parts[track] = (uint8_t)clamp(
                            (int)drum_parts[track] + controls->encoders[6], 0,
                            groove_source_type(track) == GROOVE_808 ? GROOVE_DRUMS - (page == FM_ENV)
                                                                    : groove_pad_count(track) - 1);
                        for (unsigned key = 0; key < 27; ++key)
                            if ((key_down & ~blocked_keys) & (1U << key)) {
                                int pad =
                                    groove_pad_index(track, groove_key_note(track, key, octave));
                                if (pad >= 0)
                                    drum_parts[track] = (uint8_t)pad;
                            }
                        if (drum_parts[track] < groove_pad_count(track)) {
                            drum = drum_parts[track];
                            selected_note = groove_pad_note(track, drum);
                        }
                    }
                    unsigned slot = tone_slot(), count = groove_tone_count(track, tone_part());
                    int direction = page == TONE_EDIT && advanced_edit ? (controls->encoders[0] > 0) - (controls->encoders[0] < 0) : 0;
                    if (!advanced_edit) tone_pages[track][slot] = 0;
                    tone_pages[track][slot] = (uint8_t)clamp(
                        (int)tone_pages[track][slot] + direction, 0, (int)((count - 1) / 4));
                    for (unsigned col = 0; col < 4; ++col)
                        if (controls->encoders[2 + col]) {
                            unsigned parameter = tone_parameter(col);
                            const fm1_param_t *p =
                                groove_tone_parameter(track, tone_part(), parameter);
                            if (p)
                                groove_tone_edit(
                                    track, tone_part(), parameter,
                                    p->names ? controls->encoders[2 + col]
                                             : fm_encoder_delta(2 + col, controls->encoders[2 + col]));
                        }
                } else if (page >= FM_EDIT && page <= FM_LFO) {
                    if (page == FM_EDIT) {
                        for (unsigned i = 0; i < 10; ++i) {
                            uint32_t bit = 1U << edit_keys[i];
                            if ((key_down & ~blocked_keys) & bit) {
                                if (i < 8)
                                    operator_selected[track] = (uint8_t)i;
                                else
                                    groove_mono_set(track, i == 8);
                                if (i < 6) {
                                    operator_time[i] = now;
                                    operator_held |= 1U << i;
                                    operator_long &= ~(1U << i);
                                }
                            }
                            if (i < 6) {
                                if (!(controls->keys & bit) || (blocked_keys & bit))
                                    operator_held &= ~(1U << i);
                                if ((operator_held & ~operator_long & (1U << i)) &&
                                    now - operator_time[i] >= 500U) {
                                    groove_operator_toggle(track, i);
                                    operator_long |= 1U << i;
                                }
                            }
                        }
                        op = operator_selected[track];
                        unsigned limit = op < 6 ? 5 : op == 6 ? 1 : 0;
                        int direction = (controls->encoders[0] > 0) - (controls->encoders[0] < 0);
                        operator_page[track][op] = (uint8_t)clamp(
                            (int)operator_page[track][op] + direction, 0, (int)limit);
                        fm_group = op < 6    ? operator_page[track][op]
                                   : op == 6 ? 6 + operator_page[track][op]
                                             : 8;
                    } else if (page == FM_LFO) {
                        lfo_page[track] =
                            (uint8_t)clamp((int)lfo_page[track] + controls->encoders[0], 0, 1);
                        fm_group = 9 + lfo_page[track];
                    }
                    for (unsigned col = 0; col < 4; ++col)
                        if (controls->encoders[2 + col]) {
                            if (page == FM_ENV)
                                groove_adsr_edit(track, col,
                                                 fm_encoder_delta(2 + col, controls->encoders[2 + col]));
                            else
                                groove_fm_edit(track, fm_parameter(col),
                                               fm_encoder_delta(2 + col, controls->encoders[2 + col]));
                        }
                } else if (page != STEPS || !seq_page) {
                    if (page == OVERVIEW) {
                        if (controls->encoders[4]) {
                            struct groove_view view = {0};
                            groove_get_view(track, cursor, &view);
                            move_step(&view, controls->encoders[4], 0);
                            if (writing) groove_edit_seek(track, cursor);
                        }
                    } else if (groove_pad_count(track) != 0) {
                        drum = (unsigned)clamp((int)drum + controls->encoders[4], 0,
                                               (int)groove_pad_count(track) - 1);
                        selected_note = groove_pad_note(track, drum);
                    } else if (page == STEPS) {
                        struct groove_view selected = {0};
                        groove_get_view(track, cursor, &selected);
                        if (controls->encoders[4] && step_follow && selected.playing) {
                            cursor = selected.play_step;
                            groove_get_view(track, cursor, &selected);
                        }
                        /* STEP selects the existing cell; NOTE edits its pitch
                         * without replacing velocity, chord, gate or locks.
                         * On an empty cell it sets the next inserted note. */
                        if (selected.step.velocity) selected_note = selected.step.note;
                        if (controls->encoders[4]) {
                            step_follow = 0;
                            selected_note = (unsigned)clamp((int)selected_note + controls->encoders[4], 0, 127);
                            if (selected.step.velocity) {
                                groove_sequence_edit(track, cursor, selected_note, selected.step.velocity);
                                groove_get_view(track, cursor, &selected);
                                selected_note = selected.step.note; /* Respect chord pitch limits. */
                            }
                        }
                    }
                    if (controls->encoders[2])
                        groove_control_edit(track, 0, controls->encoders[2], 0);
                    if (controls->encoders[5]) {
                        groove_tempo_edit(controls->encoders[5]);
                        if (groove_clock_mode() == GROOVE_CLOCK_MIDI) {
                            notice = "EXTERNAL CLOCK - USE INT FOR BPM";
                            notice_until = now + 2000U;
                        }
                    }
                }
                if (page == FM_ENV && groove_source_type(track) == GROOVE_DX7 && controls->encoders[6])
                    groove_adsr_enable(track, controls->encoders[6] > 0);
                if (controls->encoders[6] && page != ARP_EDIT && page != STEPS && page != FM_ENV && page != FM_LFO && page != EFFECTS && page != NAME && !(page == TONE_EDIT && groove_pad_count(track) != 0))
                    groove_control_edit(track, controls->encoders[6], 0, 0);
                if (controls->encoders[1] && page != STEPS && page != ARP_EDIT) {
                    if (groove_source_type(track) == GROOVE_DX7)
                        groove_fm_edit(track, 134, fm_encoder_delta(1, controls->encoders[1]));
                    else
                        groove_control_edit(track, 0, 0, controls->encoders[1]);
                }
            }
        }
    }
controls_done:
    if (page != old_page) {
        glo_down = glo_used = action_confirm = ble_forget = project_reload = 0;
        if (!(pressed & BUTTON(EDIT))) edit_down = 0;
        if (page != STEPS) seq_down = 0;
    }
    if (page != OVERVIEW) rec_down = 0;
    if (page != old_page && page != ARP_EDIT) arp_down = arp_long = 0;
    /* Manual selection must not be overwritten by the recording cursor on
     * the next poll. Entering sequence editing finishes the recording take. */
    if (page == ACTIONS || page == STEP_EDIT || (page == STEPS && (seq_page == 1 || seq_page == 3))) writing = 0;
    groove_edit_record(writing, track, cursor);
    if ((pressed & BUTTON(PLAY)) && !play_handled) {
        struct groove_view view = {0};
        groove_get_view(track, cursor, &view);
        uint8_t command = view.playing ? 0xfc : (writing && groove_sequence_setting(2) && groove_sequence_rewound() ? 0xfa : 0xfb);
        if (!post(command, 0, 0, 1)) return 0;
    }
    unsigned mode = navigation_mode();
    if (page != FM_EDIT || glo_down)
        operator_held = operator_long = 0;
    if (mode != old_mode || page != old_page) {
        blocked_keys |= old_keys;
        if (!release_keys())
            return 0;
    }
    if (retry_reset) {
        blocked_keys |= controls->keys;
        if (!post(0xff, 0, 0, 1))
            return 0;
        retry_reset = 0;
        memset(key_note, 0xff, sizeof key_note);
    }
    uint32_t keys = controls->keys & ~blocked_keys;
    /* EDIT's labelled black keys select OP1..6 / Pitch / Global / Mono / Poly;
     * they must never also generate keyboard MIDI, including on page changes. */
    if (page == FM_EDIT)
        for (unsigned i = 0; i < 10; ++i)
            keys &= ~(1U << edit_keys[i]);
    if (page == TONE_EDIT && groove_source_type(track) == GROOVE_303)
        keys &= ~((1U << edit_keys[8]) | (1U << edit_keys[9]));
    if (page == STEPS && !writing && !glo_down) {
        struct groove_view view = {0};
        groove_get_view(track, cursor, &view);
        unsigned first = step_first(&view);
        for (unsigned i = 0; i < 16; ++i)
            if ((key_down & keys) & (1U << white_keys[i])) {
                if (seq_page == 1 || seq_page == 3) { cursor = first + i; step_follow = 0; }
                else toggle_step(first + i);
            }
    }
    for (unsigned key = 0; key < 27; ++key) {
        unsigned note = groove_key_note(track, key, octave);
        if (key_note[key] != 0xff &&
            (mode || !(keys & (1U << key)) || key_note[key] != note || key_track[key] != track)) {
            if (!post((uint8_t)(0x80 + groove_channel(key_track[key])), key_note[key], 0, 3))
                return 0;
            key_note[key] = 0xff;
        }
        if (!mode && note < 128 && (keys & (1U << key)) && key_note[key] == 0xff) {
            if (!post((uint8_t)(0x90 + groove_channel(track)), (uint8_t)note, 100, 3))
                return 0;
            key_note[key] = (uint8_t)note;
            key_track[key] = (uint8_t)track;
            selected_note = note;
            if (groove_pad_count(track) != 0)
                drum = (unsigned)groove_pad_index(track, note);

        }
    }
    if (page != TONE_EDIT && groove_pad_count(track)) drum_parts[track] = (uint8_t)drum;
    return 1;
}

void groove_screen_control(void)
{
    struct fm1_controls controls;
    fm1_controls_read(&controls);
    uint32_t now = fm1_millis(), key_down = controls.keys & ~previous_keys;
    unsigned master_changed = groove_master_update(controls.knob);
    if (master_changed && (!notice || notice == master_notice || (int32_t)(now - notice_until) >= 0)) {
        unsigned level = fm1_engine_output_level();
        if (level) {
            memcpy(master_notice, "MASTER ", 7);
            decimal(master_notice + 7, (int)((level * 100U + 63U) / 127U), 1);
            memcpy(master_notice + strlen(master_notice), " / 100", sizeof " / 100");
        }
        else memcpy(master_notice, "MASTER MUTED", sizeof "MASTER MUTED");
        notice = master_notice;
        notice_until = now + 1200U;
    }
    if (now - health_at >= 500U) {
        struct fm1_engine_stats health;
        fm1_engine_stats_read(&health);
        struct fm1_render_stats audio;
        groove_audio_stats(&audio);
        struct fm1_audio_stats dma;
        fm1_audio_stats_read(&dma);
        unsigned adapted = audio.adapted;
        if (health.underruns != health_underruns || dma.overruns != health_dma) {
            notice = "AUDIO OVERLOAD - REDUCE VOICES"; notice_until = now + 2500U;
        } else if (health.midi_dropped != health_dropped) {
            notice = "MIDI FULL - NOTES RESET"; notice_until = now + 2500U;
        } else if (adapted > health_adapted) {
            notice = "CPU GUARD - VOICES ADAPTED"; notice_until = now + 2500U;
        }
        health_adapted = adapted;
        health_dma = dma.overruns;
        health_at = now; health_underruns = health.underruns; health_dropped = health.midi_dropped;
    }
    unsigned input_changed = master_changed || controls.buttons != previous_buttons || controls.keys != previous_keys ||
                             encoder_active(controls.encoders);
    /* The logo never blocks control/audio or consumes the first gesture.
     * Ignore the initial MASTER ADC pickup when deciding whether to skip. */
    if (boot_active && (now - boot_started >= 1200U || controls.buttons || controls.keys ||
        encoder_active(controls.encoders) || (master_changed && now - boot_started >= 150U))) {
        boot_active = 0;
        input_changed = 1;
    }
    struct fm1_ble_status ble = {0};
    fm1_ble_get_status(&ble);
    unsigned stage = ble.state | (ble.pending << 8);
    if (stage != ble_stage) { ble_stage = stage; ble_stage_since = now; }
    ble_seconds = (now - ble_stage_since) / 1000U;
    if (ble_seconds > 999) ble_seconds = 999;
    unsigned linked = ble.state == FM1_BLE_CONNECTED || ble.state == FM1_BLE_READY ||
                      ble.state == FM1_BLE_DISCOVERING;
    if (ble_was_linked && !linked)
        retry_reset = 1;
    ble_was_linked = linked;
    writing = groove_edit_recording();
    if (writing) cursor = groove_edit_cursor();
    unsigned error = groove_edit_error();
    if (error && !notice) { notice = error == 1 ? "RECORD STOPPED - MIDI QUEUE FULL" : "CHORD LIMIT - LAST NOTES KEPT"; notice_until = now + 2000U; }
    struct groove_view transport = {0};
    groove_get_view(track, cursor, &transport);
    cursor %= transport.sequence_length;
    if (transport.playing && !transport_seen && !(page == STEPS && seq_page) && page != STEP_EDIT && page != ACTIONS) step_follow = 1;
    if (!transport.playing && transport_seen && step_follow && !writing)
        cursor = transport.play_step;
    transport_seen = transport.playing;
    unsigned old_page = page;
    unsigned boundary =
        controls.buttons != previous_buttons || controls.encoders[0] || controls.encoders[6];
    if (page == FM_EDIT && !glo_down) {
        for (unsigned i = 0; i < 10; ++i)
            boundary |= (key_down & (1U << edit_keys[i])) != 0;
    } else if (page == TONE_EDIT && groove_source_type(track) == GROOVE_303 && !glo_down) {
        boundary |= (key_down & ((1U << edit_keys[8]) | (1U << edit_keys[9]))) != 0;
    } else if (page == OVERVIEW || page == STEPS || glo_down ||
               ((page == TONE_EDIT || page == FM_ENV) && groove_pad_count(track) != 0))
        boundary |= key_down != 0;
    if (boundary) {
        if ((glo_down && !(controls.buttons & BUTTON(GLO))) ||
            (rec_down && !(controls.buttons & BUTTON(REC)))) {
            /* The encoder count includes motion before this release. Route
             * that motion through the held layer before closing it. */
            struct fm1_controls prior = controls;
            prior.keys = previous_keys;
            prior.buttons = previous_buttons;
            for (unsigned i = 0; i < 7; ++i) {
                controls.encoders[i] = 0;
            }
            if (encoder_active(prior.encoders) && !apply_controls(&prior, now, &ble))
                return;
        }
    }
    read_encoders(&controls, now, boundary);
    if (!apply_controls(&controls, now, &ble))
        return;
    /* Input changes do not wait for the animation timer. LCD work is bounded
     * separately and continues on later polls, even between animation frames. */
    if (now - frame_ms >= 33U || page != old_page || input_changed || saved_cache.shown_page == ~0U) {
        frame_ms = now;
        draw(&ble);
    }
    paint_flush();
}

void groove_screen_mmc(unsigned command)
{
    unsigned transport = command == 1 || command == 9 ? 0xfc :
                         command == 2 || command == 3 || command == 6 ? 0xfb : 0;
    if (!transport && command != 7) return;
    writing = command == 6;
    rec_down = 0;
    groove_edit_record(writing, track, cursor);
    if (transport) (void)post((uint8_t)transport, 0, 0, 1);
}
#ifdef FM1_HOST_TEST
int groove_screen_preset(unsigned slot)
{
    if (slot >= GROOVE_PROJECT_SLOTS) return GROOVE_PROJECT_INVALID;
    struct groove_view view = {0}; groove_get_view(track, cursor, &view);
    preset_slot = slot; preset_bank = 1; preset_refresh();
    return preset_load(view.playing, fm1_millis());
}
unsigned groove_screen_preset_position(void)
{
    unsigned current = preset_loaded && preset_project_revision == groove_project_revision();
    return preset_current_slot | (preset_current_bank << 8) | (current << 9);
}
/* Web shortcuts enter the same editor and use the same parameter handlers. */
int groove_screen_open(unsigned target)
{
    if (target > ARP_EDIT || target == PROJECT) return -1;
    boot_active = 0;
    unsigned quick = target == FM_QUICK;
    if (quick && groove_source_type(track) != GROOVE_DX7) target = TONE_EDIT;
    if ((target == FM_EDIT || target == NAME) &&
        groove_source_type(track) != GROOVE_DX7) {
        if (target == FM_EDIT) target = TONE_EDIT;
        else return -1;
    }
    if (!release_keys()) return -1;
    blocked_keys |= previous_keys;
    glo_down = glo_used = edit_down = seq_down = arp_down = arp_long = rec_down = 0;
    if (parameter_page(target)) {
        if (!parameter_page(page)) subpage_return = page;
    } else advanced_edit = !quick && (target == FM_EDIT || target == TONE_EDIT);
    page = target;
    if (target == STEP_EDIT || target == RHYTHM) { page = STEPS; seq_page = target == STEP_EDIT ? 1 : 2; step_follow = 0; }
    else if (page == STEPS) seq_page = 0;
    if (page == ARP_EDIT) arp_page = 0;
    if (page == ACTIONS || (page == STEPS && seq_page == 1)) {
        writing = 0;
        groove_edit_record(0, track, cursor);
    }
    if (quick && page == TONE_EDIT) tone_pages[track][tone_slot()] = 0;
    if (page == FM_ENV && groove_pad_count(track)) drum_parts[track] = (uint8_t)drum;
    action_confirm = name_cursor = 0;
    if (page == OVERVIEW || page == STEPS) music_page = page;
    if (page == SOURCE) { source_preview = groove_source_type(track); source_result = 0; }
    if (page == PRESET_BROWSER) { project_result = 0; preset_refresh(); }
    if (page == BLE) { ble_view = ble_forget = ble_action = 0; ble_result = 0; }
    cache->shown_page = ~0U;
    return 0;
}
/* Browser hit targets use the same cursor, record and step-edit handlers. */
/* Browser-only targets describe the real display, not an independent copy of
 * the synthesizer. Stale messages are rejected after a control-context change. */
struct web_target { int32_t x, y, w, h, action, index, owner, raw; char label[16], value[16]; };
static struct web_target web_targets[32];
static unsigned web_target_count;
unsigned groove_screen_web_context(void)
{
    unsigned context[] = {page, track, groove_source_type(track), op, fm_group,
        tone_part(), tone_pages[track][tone_slot()], effects_page,
        ble_view, advanced_edit, glo_down && glo_used,
        (page == STEP_EDIT || (page == STEPS && seq_page)) ? cursor : 0, seq_page, arp_page};
    return signature(2166136261U, context, sizeof context);
}
unsigned groove_screen_web_mask(void)
{
    unsigned mask = 0;
    if (glo_down && glo_used) return 0;
    if (page == OVERVIEW || page == STEPS || page == MIX || page == FM_EDIT ||
        page == FM_ENV || page == FM_LFO || page == TONE_EDIT || page == EFFECTS ||
        page == STEP_EDIT || page == RHYTHM || page == FM_QUICK || page == ARP_EDIT) {
        for (unsigned i = 0; i < 4; ++i)
            if (web_knobs[i][0][0] && strcmp(web_knobs[i][0], "-") &&
                web_knobs[i][1][0] && strcmp(web_knobs[i][1], "--")) mask |= 1U << i;
    }
    if ((page == OVERVIEW || (page == STEPS && !seq_page)) && groove_clock_mode()) mask &= ~8U;
    if (page == STEPS && seq_page == 4 && groove_clock_mode()) mask &= ~2U;
    if (page == RHYTHM) mask &= 7U; /* Fixed 1/16 quantization is a readout. */
    return mask;
}
static void web_target(unsigned x, unsigned y, unsigned w, unsigned h, unsigned action,
                       unsigned index, unsigned owner, const char *label, int value, const char *display)
{
    if (web_target_count >= 32) return;
    struct web_target *t = &web_targets[web_target_count++];
    memset(t, 0, sizeof *t);
    t->x = x; t->y = y; t->w = w; t->h = h; t->action = action;
    t->index = index; t->owner = owner; t->raw = value;
    unsigned n = (unsigned)strlen(label); if (n > 15) n = 15;
    memcpy(t->label, label, n);
    if (display) { n = (unsigned)strlen(display); if (n > 15) n = 15; memcpy(t->value, display, n); }
    else decimal(t->value, value, 1);
}
static void web_alias(unsigned x, unsigned y, unsigned w, unsigned h, unsigned col)
{
    if (groove_screen_web_mask() & (1U << col))
        web_target(x, y, w, h, 0, col, track, web_knobs[col][0], 0, web_knobs[col][1]);
}
const void *groove_screen_web_targets(void)
{
    /* Browser navigation can change page before the next 33 ms paint tick.
     * Refresh the shared draw metadata before exposing its edit targets. */
    struct fm1_ble_status status = {0}; fm1_ble_get_status(&status); draw(&status);
    web_target_count = 0;
    if (glo_down && glo_used) return web_targets;
    struct groove_view v; groove_get_view(track, cursor, &v);
    unsigned type = groove_source_type(track);
    if (page != BLE && page != PROJECT && page != PRESET_BROWSER && !v.clock_mode)
        web_target(6, 4, 65, 23, 1, 0, track, "BPM", v.tempo, NULL);
    if (page == OVERVIEW) {
        for (unsigned t = 0; t < 8; ++t) {
            struct groove_view row; groove_get_view(t, cursor, &row);
            char label[16] = "T1 LEVEL"; label[1] += t;
            web_target(210, 48 + t * 18, 27, 16, 2, 0, t, label, row.volume, NULL);
        }
    } else if (page == STEPS) {
        web_target(172, 31, 60, 17, 8, 0, track, "LOOP LENGTH", v.next_length, NULL);
        web_target(6, 181, 222, 12, 6, 1, track, "SEQ PAGE", seq_page + 1, seq_pages[seq_page]);
    } else if (page == ARP_EDIT) {
        web_target(6, 176, 222, 16, 6, 1, track, "ARP PAGE", arp_page + 1, arp_page ? "SWING / HOLD" : "NOTES");
    } else if (page == FM_EDIT || ((page == FM_ENV || page == FM_LFO) && type == GROOVE_DX7)) {
        uint8_t patch[156]; struct groove_fm_controls controls;
        groove_fm_copy(track, patch, &controls);
        web_target(6, 178, 47, 15, 3, 134, track, "ALGORITHM", patch[134] + 1, NULL);
        web_target(58, 178, 37, 15, 3, 135, track, "FEEDBACK", patch[135], NULL);
        web_target(101, 178, 56, 15, 4, 0, track, "VOICE MODE", controls.mono, controls.mono ? "MONO" : "POLY");
        if (page == FM_EDIT && op < 7)
            web_target(198, 31, 34, 17, 6, 0, track, "PARAMETER PAGE", operator_page[track][op] + 1, NULL);
        if (page == FM_ENV)
            web_target(6, 76, 65, 17, 5, 0, track, "ADSR", controls.enabled, controls.enabled ? "ON" : "OFF");
        if (page == FM_LFO) {
            static const char *const waves[] = {"TRI", "SAW DOWN", "SAW UP", "SQUARE", "SINE", "S/HOLD"};
            web_target(6, 106, 180, 22, 3, 142, track, "LFO WAVE", patch[142], waves[patch[142] % 6]);
            web_target(6, 142, 112, 17, 3, 141, track, "KEY SYNC", patch[141], patch[141] ? "ON" : "OFF");
            web_target(122, 142, 67, 17, 3, 143, track, "PM SENS", patch[143], NULL);
        }
    } else if (page == TONE_EDIT || (page == FM_ENV && type != GROOVE_DX7)) {
        if (!groove_pad_count(track))
            for (unsigned col = 0; col < 4; ++col) web_alias(6, 74 + col * 24, 228, 19, col);
        if (page == TONE_EDIT && advanced_edit && groove_tone_count(track, tone_part()) > 4)
            web_target(188, 33, 40, 17, 6, 0, track, "PARAMETER PAGE", tone_pages[track][tone_slot()] + 1, NULL);
    } else if (page == EFFECTS) {
        for (unsigned col = 0; col < 4; ++col) web_alias(6, 86 + col * 22, 204, 20, col);
        web_target(6, 59, 228, 18, 6, 6, track, "FX PAGE", effects_page, effects_page ? "SHARED" : "TRACK");
    } else if (page == MIX) {
        for (unsigned i = 0; i < 16; ++i) {
            struct groove_view row; groove_get_view(i & 7U, cursor, &row);
            unsigned on = i < 8 ? row.muted : row.solo;
            char label[16] = "T1 MUTE"; label[1] += i & 7U;
            if (i >= 8) memcpy(label + 3, "SOLO", 4);
            web_target(8 + i % 4 * 57, 72 + i / 4 * 29, 53, 25, 7, i >= 8, i & 7U, label, on, on ? "ON" : "OFF");
        }
    }
    return web_targets;
}
unsigned groove_screen_web_target_count(void) { return web_target_count; }
int groove_screen_web_adjust(unsigned context, unsigned action, unsigned index, unsigned owner, int delta)
{
    if (context != groove_screen_web_context()) return 0;
    if (!delta || delta < -127 || delta > 127) return -1;
    unsigned valid = action == 0 && owner == track && index < 4 &&
        (groove_screen_web_mask() & (1U << index));
    groove_screen_web_targets();
    for (unsigned i = 0; i < web_target_count; ++i) {
        struct web_target *t = &web_targets[i];
        if (t->action == (int)action && t->index == (int)index && t->owner == (int)owner) valid = 1;
    }
    if (!valid) return -1;
    if (!action) return (int)index + 3; /* Encoder index + 1. */
    if (action == 6) return (int)index + 1;
    if (action == 1) groove_tempo_edit(delta);
    else if (action == 2) groove_control_edit(owner, 0, delta, 0);
    else if (action == 3) groove_fm_edit(owner, index, delta);
    else if (action == 4) groove_mono_set(owner, delta > 0);
    else if (action == 5) groove_adsr_enable(owner, delta > 0);
    else if (action == 7) {
        struct groove_view v; groove_get_view(owner, cursor, &v);
        if ((index ? v.solo : v.muted) != (unsigned)(delta > 0)) groove_mix_toggle(owner, index);
    }
    else if (action == 8) sequence_length_adjust(delta);
    return 0;
}
const char *groove_screen_web_knobs(void) { return &web_knobs[0][0][0]; }
unsigned groove_screen_web_first(void)
{
    struct groove_view v; groove_get_view(track, cursor, &v); return step_first(&v);
}
int groove_screen_web_step(unsigned step, unsigned toggle)
{
    struct groove_view v; groove_get_view(track, cursor, &v);
    if ((page != OVERVIEW && page != STEPS) || step >= v.sequence_length || toggle > 1) return -1;
    if (!release_keys()) return -1;
    blocked_keys |= previous_keys;
    writing = 0; step_follow = 0; cursor = step;
    if (page == OVERVIEW) seq_page = 0;
    page = music_page = STEPS;
    groove_edit_record(0, track, cursor);
    if (toggle && seq_page != 1 && seq_page != 3) toggle_step(cursor);
    cache->shown_page = ~0U;
    return 0;
}
/* Read-only panel state for the browser simulator. */
void groove_screen_snapshot(uint8_t state[8])
{
    state[0] = (uint8_t)track;
    state[1] = (uint8_t)page;
    state[2] = (uint8_t)cursor;
    state[3] = (uint8_t)drum;
    state[4] = (uint8_t)writing;
    state[5] = (uint8_t)op;
    state[6] = (uint8_t)fm_group;
    state[7] = (uint8_t)(octave + 24);
}
#endif

/* FM1 algorithm layout drawn with bounded RGB565 primitives. */
static const uint8_t algorithms[32][12] = {
    {0x20, 0x00, 0x21, 0x00, 0x30, 0x00, 0x31, 0x00, 0x32, 0x00, 0x33, 0x01}, /* 1 */
    {0x20, 0x00, 0x21, 0x01, 0x30, 0x00, 0x31, 0x00, 0x32, 0x00, 0x33, 0x00}, /* 2 */
    {0x20, 0x00, 0x21, 0x00, 0x22, 0x00, 0x30, 0x00, 0x31, 0x00, 0x32, 0x01}, /* 3 */
    {0x20, 0x00, 0x21, 0x00, 0x22, 0x00, 0x30, 0x00, 0x31, 0x00, 0x32, 0x03}, /* 4 */
    {0x10, 0x00, 0x11, 0x00, 0x20, 0x00, 0x21, 0x00, 0x30, 0x00, 0x31, 0x01}, /* 5 */
    {0x10, 0x00, 0x11, 0x00, 0x20, 0x00, 0x21, 0x00, 0x30, 0x00, 0x31, 0x02}, /* 6 */
    {0x20, 0x00, 0x21, 0x00, 0x30, 0x00, 0x31, 0x00, 0x41, 0x10, 0x42, 0x01}, /* 7 */
    {0x20, 0x00, 0x21, 0x00, 0x30, 0x00, 0x31, 0x01, 0x41, 0x10, 0x42, 0x00}, /* 8 */
    {0x20, 0x00, 0x21, 0x01, 0x30, 0x00, 0x31, 0x00, 0x41, 0x10, 0x42, 0x00}, /* 9 */
    {0x20, 0x00, 0x21, 0x00, 0x22, 0x01, 0x30, 0x00, 0x31, 0x00, 0x41, 0x10}, /* 10 */
    {0x20, 0x00, 0x21, 0x00, 0x22, 0x00, 0x30, 0x00, 0x31, 0x00, 0x41, 0x11}, /* 11 */
    {0x10, 0x00, 0x11, 0x01, 0x30, 0x00, 0x21, 0x20, 0x31, 0x00, 0x41, 0x10}, /* 12 */
    {0x10, 0x00, 0x11, 0x00, 0x30, 0x00, 0x21, 0x20, 0x31, 0x00, 0x41, 0x11}, /* 13 */
    {0x20, 0x00, 0x21, 0x00, 0x30, 0x00, 0x31, 0x00, 0x32, 0x00, 0x42, 0x11}, /* 14 */
    {0x20, 0x00, 0x21, 0x01, 0x30, 0x00, 0x31, 0x00, 0x32, 0x00, 0x42, 0x10}, /* 15 */
    {0x20, 0x00, 0x11, 0x20, 0x21, 0x00, 0x22, 0x00, 0x31, 0x10, 0x32, 0x01}, /* 16 */
    {0x20, 0x00, 0x11, 0x21, 0x21, 0x00, 0x22, 0x00, 0x31, 0x10, 0x32, 0x00}, /* 17 */
    {0x20, 0x00, 0x11, 0x20, 0x21, 0x01, 0x31, 0x10, 0x32, 0x00, 0x33, 0x00}, /* 18 */
    {0x10, 0x00, 0x11, 0x00, 0x12, 0x00, 0x20, 0x00, 0x30, 0x00, 0x21, 0x31}, /* 19 */
    {0x10, 0x00, 0x20, 0x00, 0x11, 0x31, 0x30, 0x00, 0x31, 0x00, 0x41, 0x10}, /* 20 */
    {0x10, 0x00, 0x20, 0x00, 0x11, 0x31, 0x30, 0x00, 0x40, 0x00, 0x31, 0x30}, /* 21 */
    {0x10, 0x00, 0x11, 0x00, 0x20, 0x00, 0x30, 0x00, 0x40, 0x00, 0x21, 0x41}, /* 22 */
    {0x10, 0x00, 0x20, 0x00, 0x21, 0x00, 0x30, 0x00, 0x40, 0x00, 0x31, 0x31}, /* 23 */
    {0x10, 0x00, 0x20, 0x00, 0x30, 0x00, 0x40, 0x00, 0x50, 0x00, 0x31, 0x41}, /* 24 */
    {0x10, 0x00, 0x20, 0x00, 0x30, 0x00, 0x40, 0x00, 0x50, 0x00, 0x41, 0x31}, /* 25 */
    {0x10, 0x00, 0x20, 0x00, 0x21, 0x00, 0x30, 0x00, 0x31, 0x00, 0x41, 0x11}, /* 26 */
    {0x10, 0x00, 0x20, 0x00, 0x21, 0x01, 0x30, 0x00, 0x31, 0x00, 0x41, 0x10}, /* 27 */
    {0x20, 0x00, 0x21, 0x00, 0x30, 0x00, 0x31, 0x00, 0x32, 0x01, 0x40, 0x10}, /* 28 */
    {0x10, 0x00, 0x20, 0x00, 0x30, 0x00, 0x31, 0x00, 0x40, 0x00, 0x41, 0x01}, /* 29 */
    {0x10, 0x00, 0x20, 0x00, 0x30, 0x00, 0x31, 0x00, 0x32, 0x01, 0x40, 0x00}, /* 30 */
    {0x10, 0x00, 0x20, 0x00, 0x30, 0x00, 0x40, 0x00, 0x50, 0x00, 0x51, 0x01}, /* 31 */
    {0x00, 0x00, 0x10, 0x00, 0x20, 0x00, 0x30, 0x00, 0x40, 0x00, 0x50, 0x01}, /* 32 */
};
static void algorithm_draw(unsigned algorithm, unsigned muted)
{
    const uint8_t *layout = algorithms[algorithm & 31U];
    unsigned left = 239, right = 0;
    /* Draw wires first so every destination terminates behind an operator. */
    for (unsigned i = 0; i < 6; ++i) {
        unsigned column = layout[i * 2] >> 4, row = layout[i * 2] & 15;
        int x = 19 + (int)column * 36, y = 151 - (int)row * 15;
        unsigned branch = layout[i * 2 + 1] >> 4, feedback = layout[i * 2 + 1] & 15;
        int bottom = row ? y + 15 : 169;
        if (!row) {
            if ((unsigned)x < left)
                left = (unsigned)x;
            if ((unsigned)x > right)
                right = (unsigned)x;
        }
        if (branch == 0)
            line(x, y + 12, x, bottom, DIM);
        else {
            line(x, y + 12, x, y + 15, DIM);
            int first = branch == 1 ? x - 36 : branch == 2 ? x + 36 : x;
            int last = branch == 4 ? x + 72 : branch >= 2 ? x + 36 : first;
            line(first < x ? first : x, y + 15, last > x ? last : x, y + 15, DIM);
            for (int dest = first; dest <= last; dest += 36)
                line(dest, y + 15, dest, bottom, DIM);
        }
        if (feedback) {
            int end_y = y + (int)(feedback - 1) * 15 + 12;
            line(x, end_y, x + 17, end_y, AMBER);
            line(x + 17, end_y, x + 17, y - 4, AMBER);
            line(x + 17, y - 4, x, y - 4, AMBER);
            line(x, y - 4, x, y, AMBER);
        }
    }
    line((int)left, 169, (int)right, 169, DIM);
    text(104, 172, "OUT", 1, DIM, BACK);
    for (unsigned i = 0; i < 6; ++i) {
        unsigned x = 8 + (layout[i * 2] >> 4) * 36, y = 151 - (layout[i * 2] & 15) * 15;
        uint16_t bg = muted & (1U << i) ? PANEL : i == op ? colors[track] : DIM;
        paint_fill(x, y, 22, 12, bg);
        number(x + 8, y + 3, i + 1, 1, 1, muted & (1U << i) ? DIM : BACK, bg);
    }
}
