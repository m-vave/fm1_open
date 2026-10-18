/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#include "instrument.h"
#include "fm_patch.h"
#include "drum808.h"
#include "sampledrum.h"
#include <string.h>

#if defined(FM1_OPEN_WEB)
/* FM1 arrangements. Small musical recipes expand into the normal
 * editable project format, avoiding sixteen 4 KiB images in program Flash.
 * No random generation, engine allocation or live-state changes happen here.
 * Notes are MIDI pitches; masks read from bit 0 (the first sixteenth note).
 * Unused tracks stay empty, muted, and use the small PSG engine. */
enum { KEYS, BASS, ARP, LEAD, PAD, BEAT, PERC, STAB };
enum { ELECTRIC, BELL, ORGAN, ROUND_BASS, WARM_PAD, REED,
       SOFT_EP, CLAV, GUITAR, MARIMBA, VIBES, BRASS, STRINGS, SUB_BASS, METAL, SOFT_LEAD };
struct part {
    uint8_t source : 3, role : 3;
    uint8_t tone, level, sends;
};
struct scene {
    char name[13];
    uint8_t bpm, root, minor, swing, rhythm, motif, count;
    uint8_t decay, delay_steps, feedback, fx_level;
    struct part part[5];
};
#define FM(tone, role, level, rev, dly) {GROOVE_DX7, role, tone, level, rev + 16*dly}
#define AC(tone, role, level, rev, dly) {GROOVE_303, role, tone, level, rev + 16*dly}
#define PS(tone, role, level, rev, dly) {GROOVE_PSG, role, tone, level, rev + 16*dly}
#define PL(tone, role, level, rev, dly) {GROOVE_PLUCK, role, tone, level, rev + 16*dly}
#define DR(level) {GROOVE_808, BEAT, 0, level, 0}
#define SA(role, level) {GROOVE_SAMPLE, role, 0, level, 1}
/* Sixteen genre sketches. Sends are intentional per part: bass stays
 * dry, keys get a small room, sparse leads and arps get tempo-related echoes.
 * delay_steps counts sixteenths at the scene's initial tempo. */
static const struct scene scenes[GROOVE_FACTORY_PROJECTS] = {
    {"NEON KEYS",   104,45,1, 8,0,0,4, 64,2,34,76, {FM(ELECTRIC,KEYS,96,4,1), FM(ROUND_BASS,BASS,92,0,0), PL(0,ARP,76,3,3), SA(BEAT,82)}},
    {"ACID ALLEY",  130,38,1, 0,1,1,3, 38,2,42,68, {AC(1,BASS,42,0,1), DR(86), FM(SOFT_LEAD,LEAD,77,2,4)}},
    {"VELVET DUSK",  82,41,1,18,2,2,4, 72,1,28,78, {FM(SOFT_EP,KEYS,102,5,0), FM(SUB_BASS,BASS,92,0,0), SA(BEAT,76), FM(VIBES,LEAD,76,4,3)}},
    {"GLASS TIDE",   72,38,0, 0,0,3,3, 98,1,52,94, {FM(BELL,LEAD,94,7,5), PL(1,ARP,85,7,4), FM(WARM_PAD,PAD,89,7,0)}},
    {"CHIP PARADE", 142,36,0, 0,3,4,4, 40,2,24,66, {PS(0,LEAD,64,2,2), PS(1,BASS,80,0,0), DR(76), FM(MARIMBA,ARP,76,2,1)}},
    {"WOODLAND",     96,43,0, 5,4,5,4, 68,1,30,82, {PL(0,KEYS,105,5,1), FM(REED,LEAD,90,5,2), SA(PERC,68), FM(ROUND_BASS,BASS,87,0,0)}},
    {"NIGHT DRIVE", 116,36,1, 0,1,0,4, 70,2,38,76, {FM(ROUND_BASS,BASS,96,0,0), FM(STRINGS,PAD,75,5,0), AC(2,ARP,28,2,3), DR(84)}},
    {"DUB HARBOUR",  76,38,1,10,5,2,4, 74,1,70,84, {FM(SUB_BASS,BASS,99,0,0), FM(ORGAN,STAB,84,4,6), DR(82), PL(1,LEAD,74,3,5)}},
    {"METRO FUNK",  112,40,1,12,6,5,4, 42,1,24,68, {FM(CLAV,STAB,97,2,0), AC(0,BASS,37,0,0), SA(BEAT,83), FM(BRASS,LEAD,73,3,1)}},
    {"ORBIT BELLS",  92,45,1, 0,3,3,4, 86,2,48,78, {FM(BELL,LEAD,83,5,4), FM(MARIMBA,ARP,68,3,2), FM(SUB_BASS,BASS,82,0,0), DR(72)}},
    {"DESERT STEP", 108,38,1, 8,7,1,4, 62,2,35,76, {PL(2,LEAD,87,4,3), AC(3,BASS,37,0,0), DR(79), FM(REED,PAD,60,4,0)}},
    {"SOFT CURRENT",88,41,0,10,2,2,4, 62,1,27,76, {FM(SOFT_EP,KEYS,103,4,0), FM(ROUND_BASS,BASS,86,0,0), SA(PERC,65), FM(GUITAR,LEAD,81,3,2)}},
    {"BROKEN GRID", 134,36,1, 6,6,4,4, 46,2,42,70, {AC(2,BASS,38,0,1), FM(ORGAN,STAB,78,2,3), SA(BEAT,82), FM(METAL,LEAD,65,3,4)}},
    {"SUNRISE POP", 122,43,0, 0,0,5,5, 68,2,32,76, {FM(WARM_PAD,PAD,72,5,0), FM(ROUND_BASS,BASS,91,0,0), PL(0,ARP,75,3,3), SA(BEAT,84), FM(SOFT_LEAD,LEAD,82,3,2)}},
    {"MINIMAL ROOM",124,45,1, 4,1,1,3, 46,2,45,72, {AC(1,BASS,39,0,1), DR(89), FM(CLAV,STAB,70,2,4)}},
    {"AFTERGLOW",    68,36,0, 0,0,3,2, 90,1,42,80, {FM(SOFT_EP,KEYS,108,5,1), FM(VIBES,LEAD,90,6,3)}}
};
#undef FM
#undef AC
#undef PS
#undef PL
#undef DR
#undef SA

/* Kick, backbeat, closed hat, open hat and a small percussion response. */
static const uint16_t beats[8][5] = {
    {0x0101,0x1010,0x5555,0x4000,0x0080},
    {0x1111,0x1010,0x4444,0x0400,0x8000},
    {0x0401,0x1010,0x4545,0x0000,0x8000},
    {0x0141,0x1010,0x5555,0x4000,0x0080},
    {0x0101,0x1010,0x0505,0x0000,0x4400},
    {0x0100,0x0100,0x5555,0x0000,0x4080},
    {0x0841,0x1010,0x5555,0x4000,0x0080},
    {0x0409,0x1010,0x4545,0x0400,0x2080}
};
static const uint16_t bass_masks[6] = {0x1525,0x6d6b,0x0501,0x1101,0x5555,0x4929};
static const int8_t melodies[6][16] = {
    {0,-1,2,-1,4,-1,2,1, 0,-1,4,-1,5,4,2,-1},
    {0,0,-1,4,0,-1,2,0, 5,-1,4,2,-1,0,2,-1},
    {-1,-1,2,-1,4,-1,-1,2, -1,1,-1,-1,0,-1,-1,-1},
    {4,-1,-1,-1,2,-1,1,-1, 0,-1,-1,2,5,-1,-1,-1},
    {0,2,4,5,4,-1,2,-1, 1,2,4,-1,5,4,2,-1},
    {-1,0,-1,2,-1,4,2,-1, 5,-1,4,2,-1,1,0,-1}
};
static unsigned bar_root(const struct scene *s, unsigned bar)
{
    static const uint8_t progression[4][4] = {{0,7,9,5},{0,8,3,10},{0,9,2,7},{0,5,8,7}};
    return s->root + progression[s->minor + 2*(s->motif & 1U)][bar];
}
static unsigned bar_minor(const struct scene *s, unsigned bar)
{
    static const uint8_t quality[4][4] = {{0,0,1,0},{1,0,0,0},{0,1,1,0},{1,1,0,0}};
    return quality[s->minor + 2*(s->motif & 1U)][bar];
}
static const char fm_names[GROOVE_FACTORY_VOICES][11] = {
    "NEON KEYS ","ORBIT BELL","SOFT ORGAN","ROUND BASS","WARM AIR  ","WOOD REED ",
    "SOFT EP   ","DRY CLAV  ","FM GUITAR ","MARIMBA   ","VIBES     ","SOFT BRASS",
    "FM STRINGS","SUB BASS  ","METAL HIT ","SOFT LEAD "
};
/* Three independently voiced operator pairs: body, colour and transient.
 * Unequal carrier levels avoid three identical full-level voices stacking. */
/* Pack recipe fields without changing the serialized patch layout. */
struct fm_pair { uint8_t level, coarse : 2, detune : 4, mod, ratio, fine, decay, body; };
_Static_assert(sizeof(struct part) == 4 && sizeof(struct fm_pair) == 7, "compact recipes");
struct fm_voice {
    uint8_t attack, decay, settle, release, body, sustain, mod_attack, mod_release, feedback;
    struct fm_pair pair[3];
};
static const struct fm_voice voices[GROOVE_FACTORY_VOICES] = {
    {96,52,30,62,84,0,99,66,0, {{98,1,7,54,1,0,65,38},{91,1,8,60,1,0,57,58},{79,1,6,48,7,0,80,0}}},
    {99,50,28,53,70,0,99,62,0, {{96,1,7,58,3,50,59,0},{82,2,7,48,7,0,68,0},{69,3,7,35,11,0,80,0}}},
    {95,65,45,78,99,99,95,78,0, {{90,1,7,0,1,0,65,0},{87,2,7,0,1,0,65,0},{74,3,7,0,1,0,65,0}}},
    {97,72,46,82,91,84,99,86,0, {{99,1,7,42,1,0,76,32},{74,2,7,32,1,0,82,20},{0,1,7,0,1,0,80,0}}},
    {53,47,36,52,96,92,49,55,0, {{90,1,5,42,1,0,55,82},{84,1,9,34,2,0,53,76},{73,2,7,27,1,0,52,78}}},
    {84,61,46,72,94,89,78,77,1, {{95,1,7,61,2,0,63,86},{82,1,8,38,1,0,68,75},{70,3,7,29,1,0,72,60}}},
    {94,49,28,60,86,0,96,64,0, {{98,1,7,44,1,0,60,35},{86,1,8,38,3,0,66,20},{72,2,6,31,1,0,72,0}}},
    {99,75,47,80,42,0,99,83,1, {{96,1,7,67,1,0,82,18},{81,2,7,55,2,0,86,0},{0,1,7,0,1,0,80,0}}},
    {97,57,32,66,69,0,99,74,0, {{97,1,7,55,1,0,72,18},{83,2,7,46,3,0,81,0},{66,3,7,38,5,0,85,0}}},
    {99,67,37,72,34,0,99,81,0, {{98,1,7,60,3,0,85,0},{74,2,7,46,5,0,88,0},{58,3,7,32,7,0,91,0}}},
    {98,52,25,55,78,0,99,65,0, {{94,1,7,56,4,0,76,0},{76,2,8,39,3,0,81,0},{66,3,6,28,2,0,84,0}}},
    {81,62,42,72,96,90,67,75,1, {{95,1,7,67,1,0,62,90},{84,1,8,46,1,0,65,83},{70,2,6,40,2,0,68,75}}},
    {61,48,39,54,96,94,57,58,0, {{89,1,5,54,1,0,55,89},{83,1,9,36,2,0,55,83},{72,2,7,30,4,0,58,76}}},
    {95,72,44,84,96,94,99,86,0, {{99,1,7,0,1,0,80,0},{0,1,7,0,1,0,80,0},{0,1,7,0,1,0,80,0}}},
    {99,65,40,72,48,0,99,76,0, {{89,1,7,65,7,31,67,0},{70,2,7,58,11,47,73,0},{56,3,7,48,13,79,78,0}}},
    {88,60,40,71,96,90,82,75,1, {{96,1,7,45,1,0,65,83},{80,1,8,31,2,0,70,72},{71,2,6,24,1,0,74,66}}}
};
void groove_factory_voice(unsigned tone, uint8_t p[156])
{
    tone %= GROOVE_FACTORY_VOICES;
    const struct fm_voice *sound = &voices[tone];
    memset(p, 0, 156);
    memcpy(p + 145, fm_names[tone], 10);
    for (unsigned op = 0; op < 6; ++op) {
        uint8_t *v = p + 21 * op;
        const struct fm_pair *pair = &sound->pair[op / 2];
        unsigned carrier = op & 1U;
        v[0] = carrier ? sound->attack : sound->mod_attack;
        v[1] = carrier ? sound->decay : pair->decay;
        v[2] = sound->settle; v[3] = carrier ? sound->release : sound->mod_release;
        v[4] = 99; v[5] = carrier ? sound->body : pair->body;
        v[6] = carrier ? sound->sustain : sound->sustain ? pair->body : 0;
        v[8] = 50; v[10] = carrier ? 8 : 18; v[13] = 1;
        v[15] = carrier ? 2 : 4;
        v[16] = carrier ? pair->level : pair->mod;
        v[18] = carrier ? pair->coarse : pair->ratio;
        v[19] = carrier ? 0 : pair->fine; v[20] = pair->detune;
        if (tone == ORGAN) {
            static const uint8_t levels[6] = {89,77,84,68,59,52};
            static const uint8_t ratios[6] = {1,2,3,4,6,8};
            v[16] = levels[op]; v[18] = ratios[op]; v[19] = 0;
            v[5] = v[6] = 99; v[15] = 1; v[20] = 7;
        }
    }
    for (unsigned i = 126; i < 130; ++i) p[i] = 99;
    for (unsigned i = 130; i < 134; ++i) p[i] = 50;
    p[134] = tone == ORGAN ? 31 : 4; p[135] = sound->feedback;
    p[136] = 1; p[137] = 28; p[138] = 35; p[141] = 1; p[142] = 4;
    if (tone == WARM_PAD || tone == STRINGS || tone == REED) { p[139] = 2; p[143] = 2; }
    p[144] = 24; p[155] = 63;
}
#define FACTORY_VOICE(tone, data) groove_factory_voice(tone, data)
#else
/* Device image: the demos and DX7 recipes above are built only for the web
 * workbench, which can send them to the device. The device keeps one blank
 * project with the engine's start-up sources. */
enum { BASS = 1, LEAD = 3 };
struct part {
    uint8_t source : 3, role : 3;
    uint8_t tone, level, sends;
};
#define FACTORY_VOICE(tone, data) groove_fm_program(data, tone)
#endif
static void preset(struct groove_preset *p, const struct part *v)
{
    p->type = v->source; p->volume = v->level; p->modified = 1;
    p->controls.envelope[2] = 100;
    if (v->source == GROOVE_DX7) {
        FACTORY_VOICE(v->tone, p->data); p->program = v->tone; p->modified = 0;
    }
    else if (v->source == GROOVE_303) {
        static const uint8_t bass[4][BASS303_NPARAMS] = {
            {45,24,32,52,62,0,64,96,0,1,14,5},
            {60,66,73,56,74,0,64,94,6,1,26,10},
            {65,44,58,39,65,0,64,92,0,1,14,8},
            {34,18,20,62,42,1,64,100,0,1,20,5}
        };
        memcpy(p->data, bass[v->tone], BASS303_NPARAMS);
        p->program = p->data[BASS303_WAVE]; p->controls.mono = 1;
    } else if (v->source == GROOVE_808) {
        /* Public D8_PRESET_BYTES format: selectors, then two parameter sets.
         * Sound selectors must match each set for a lossless store/load. */
        unsigned at = D8_NUM;
        for (unsigned variant = 0; variant < 2; ++variant)
            for (unsigned part = 0; part <= D8_NUM; ++part)
                for (int i = 0; i < drum808_nparams((int)part); ++i) {
                    const fm1_param_t *d = drum808_param((int)part, i);
                    static const uint8_t levels[D8_NUM] = {70,52,46,42,38,36,46,28,25,30,29};
                    static const uint8_t pans[D8_NUM] = {64,64,49,64,79,54,72,80,47,77,72};
                    unsigned value = d->def;
                    if (!strcmp(d->name, "Sound")) value = variant;
                    if (part < D8_NUM) {
                        if (!strcmp(d->name, "Level")) value = levels[part];
                        else if (!strcmp(d->name, "Pan")) value = pans[part];
                        else if (!strcmp(d->name, "Rev")) value = part == D8_SD || part == D8_CP ? 24 : 9;
                        else if (!strcmp(d->name, "Dly")) value = part == D8_RS ? 12 : 0;
                        else if (!strcmp(d->name, "Decay") && part == D8_SD) value = 84;
                    }
                    p->data[at++] = (uint8_t)value;
                }
    } else if (v->source == GROOVE_SAMPLE) {
        p->sampler_pads = SAMPLE_DRUM_PADS;
        for (unsigned pad = 0; pad < SAMPLE_DRUM_PADS; ++pad) {
            uint8_t *pot = p->data + pad * SAMPLE_DRUM_PARAMS;
            for (unsigned i = 0; i < SAMPLE_DRUM_PARAMS; ++i) pot[i] = sampledrum_param(i)->def;
            static const uint8_t levels[SAMPLE_DRUM_PADS] = {105,83,75,83,72,73,58,62};
            pot[SAMPLE_DRUM_LEVEL] = levels[pad];
            pot[SAMPLE_DRUM_VELOCITY_LPF] = pad == 0 ? 12 : 30;
            if (pad == 6 || pad == 7) pot[SAMPLE_DRUM_CHOKE] = 1;
        }
    } else if (v->source == GROOVE_PLUCK) {
        static const uint8_t pluck[3][4] = {{88,70,24,48},{103,52,24,66},{79,89,24,42}};
        memcpy(p->data, pluck[v->tone], 4);
    } else {
        p->program = p->data[0] = v->tone; p->data[1] = 16;
        p->data[2] = 24; p->data[3] = v->role == BASS ? 20 : 38;
    }
    p->data[209] = v->sends;
}
#if defined(FM1_OPEN_WEB)
const char *groove_factory_name(unsigned index)
{
    return index < GROOVE_FACTORY_PROJECTS ? scenes[index].name : NULL;
}
int groove_factory_build(unsigned index, struct groove_project *p)
{
    if (!p || index >= GROOVE_FACTORY_PROJECTS) return GROOVE_PROJECT_INVALID;
    const struct scene *s = &scenes[index];
    memset(p, 0, sizeof *p);
    memset(p->name, ' ', sizeof p->name); memcpy(p->name, s->name, strlen(s->name));
    p->tempo = s->bpm; p->mute = (uint8_t)(255U << s->count);
    unsigned delay = FM1_SAMPLE_RATE * 60U * s->delay_steps / (s->bpm * 32U);
    p->effects[0] = s->decay;
    p->effects[1] = (uint8_t)((delay - 166U + 7U) / 14U);
    p->effects[2] = s->feedback; p->effects[3] = s->fx_level;
    groove_sequence_defaults(&p->sequence); p->sequence.swing = s->swing;
    static const uint8_t open_chords[2][8] = {{4,4,11,14},{4,3,10,14}};
    memcpy(p->sequence.chord[12], open_chords, sizeof open_chords);
    groove_project_arp_defaults(p);
    for (unsigned t = 0; t < GROOVE_TRACKS; ++t) {
        const struct part empty = {GROOVE_PSG,LEAD,0,100,0};
        const struct part *v = t < s->count ? &s->part[t] : &empty;
        preset(&p->preset[t], v);
        if (t >= s->count) continue;
        for (unsigned bar = 0; bar < 4; ++bar) {
            unsigned root = bar_root(s, bar), minor = bar_minor(s, bar);
            for (unsigned i = 0; i < 16; ++i) {
                struct groove_step *n = &p->step[t][bar * 16 + i];
                unsigned bit = 1U << i, chord = 0, gate = 7, tie = 0, pitch = root + 24;
                if (v->role == BEAT || v->role == PERC) {
                    const uint16_t *b = beats[s->rhythm];
                    unsigned sample = v->source == GROOVE_SAMPLE;
                    if (v->role == BEAT && (b[0] & bit)) n->drums |= 1U;
                    if (b[1] & bit) n->drums |= (uint16_t)(1U << (v->role == PERC && sample ? 2 : 1));
                    if (b[2] & bit) n->drums |= (uint16_t)(1U << (sample ? 6 : 10));
                    if (b[3] & bit) {
                        n->drums &= (uint16_t)~(1U << (sample ? 6 : 10));
                        n->drums |= (uint16_t)(1U << (sample ? 7 : 9));
                    }
                    if ((b[4] & bit) && (bar & 1U)) n->drums |= (uint16_t)(1U << (sample ? 2 : 5));
                    if (bar == 3 && i == 15) n->drums |= (uint16_t)(1U << (sample ? 4 : 3));
                    if (n->drums) n->velocity = (uint8_t)((n->drums & 3U) ? 100 : i % 4 == 0 ? 74 : 59 + (bar & 1U)*4);
                    continue;
                }
                if (v->role == BASS) {
                    if (!(bass_masks[s->motif] & bit)) continue;
                    pitch = root + (i == 14 ? 12U : i == 7 || i == 11 ? 7U : 0U);
                    gate = 12;
                } else if (v->role == KEYS || v->role == PAD || v->role == STAB) {
                    unsigned mask = v->role == STAB ? 0x0444 : v->role == KEYS ? 0x0909 : 0x0001;
                    if (!(mask & bit)) continue;
                    pitch = root + 12; chord = minor ? 2 : 1;
                    if (v->role == KEYS && v->source == GROOVE_DX7) chord = 13U + minor;
                    if (v->role == PAD) { gate = 15; tie = 1; }
                    else if (v->role == KEYS) { gate = 12; tie = i == 0 || i == 8; }
                    else gate = 9;
                } else if (v->role == ARP) {
                    if (i & 1U) continue;
                    const unsigned offsets[4] = {0,minor ? 3U : 4U,7,12};
                    pitch = root + 24 + offsets[(i / 2 + bar) % 4];
                    gate = 9;
                } else {
                    int degree = melodies[s->motif][i];
                    if (degree < 0) continue;
                    static const uint8_t scale[2][7] = {{0,2,4,7,9,12,14},{0,3,5,7,10,12,15}};
                    pitch = root + 24 + scale[minor][degree]; gate = 11;
                }
                n->note = (uint8_t)pitch;
                n->velocity = (uint8_t)(i % 4 == 0 ? 100 : i % 2 == 0 ? 88 : 76);
                n->drums = (uint16_t)(chord | (gate << 5) | (tie << 12));
            }
        }
    }
    return groove_project_valid(p) ? 0 : GROOVE_PROJECT_INVALID;
}
#else
const char *groove_factory_name(unsigned index)
{
    return index < GROOVE_FACTORY_PROJECTS ? "DEFAULT" : NULL;
}
int groove_factory_build(unsigned index, struct groove_project *p)
{
    if (!p || index >= GROOVE_FACTORY_PROJECTS) return GROOVE_PROJECT_INVALID;
    static const struct part parts[GROOVE_TRACKS] = {
        {GROOVE_DX7,LEAD,0,100,0}, {GROOVE_303,BASS,0,100,0}, {GROOVE_PSG,LEAD,0,100,0},
        {GROOVE_PLUCK,LEAD,0,100,0}, {GROOVE_SAMPLE,LEAD,0,100,0}, {GROOVE_DX7,LEAD,0,100,0},
        {GROOVE_303,BASS,0,100,0}, {GROOVE_808,LEAD,0,100,0}
    };
    static const uint8_t effects[4] = {64, 64, 48, 100};
    memset(p, 0, sizeof *p);
    memset(p->name, ' ', sizeof p->name); memcpy(p->name, "DEFAULT", 7);
    p->tempo = 120; p->mute = (1U << 5) | (1U << 6);
    memcpy(p->effects, effects, sizeof effects);
    groove_sequence_defaults(&p->sequence);
    groove_project_arp_defaults(p);
    for (unsigned t = 0; t < GROOVE_TRACKS; ++t) preset(&p->preset[t], &parts[t]);
    return groove_project_valid(p) ? 0 : GROOVE_PROJECT_INVALID;
}
#endif
