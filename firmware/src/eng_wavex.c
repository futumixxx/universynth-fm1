/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments (style); UNIVERSYNTH's own engine */
/* WAVEX: UNIVERSYNTH's own wavetable-style engine. One phase accumulator crossfades continuously across
 * four classic single-cycle shapes (SAW, PULSE, TRI, SINE, in that order), so a sweep of POSITION morphs
 * smoothly through all four, PPG-style; MORPH adds a steady automatic drift of that position over the
 * held note's life (0 = still, higher = the shape keeps moving on its own), a sub oscillator an octave
 * down, a bitcrusher before the filter, a one-pole lowpass (shared dsp.c tsvf_*) and a drive stage.
 *
 * Parameters:
 *   E0 POSITION   start point of the crossfade: 0 SAW, 1/3 PULSE, 2/3 TRI, max SINE
 *   E1 MORPH      automatic drift rate of POSITION while a note is held (0 = none)
 *   E2 CRUSH      bit depth reduction before the filter (0 = clean)
 *   E3 SUB        a sine an octave down, mixed in
 *   E4 CUTOFF     lowpass cutoff
 *   E5 RESONANCE  lowpass resonance
 *   E6 DRIVE      soft-clip drive after the filter
 *   E7 KTR        key tracking of the cutoff
 */
static const int32_t WAVEX_POS_MAX = 3 << 8;   /* 3 crossfades x 256 steps */

static inline int32_t wavex_wave(uint32_t idx, uint32_t ph, uint32_t inc)
{
    switch (idx & 3u) {
    case 0: return osc_saw(ph, inc);
    case 1: return osc_pulse(ph, inc, 0x80000000u);
    case 2: return osc_tri(ph);
    default: return osc_sine(ph);
    }
}

static void wavex_note_on(track_t *t, voice_t *v)
{
    v->s[0] = (int32_t)((uint32_t)(uint16_t)t->p[P_E0] * (uint32_t)WAVEX_POS_MAX / 127u);
    v->s[3] = 0;                 /* drift direction: 0 forward, 1 bounced back */
    v->ph[0] = v->ph[1] = 0;
    (void)t;
}

static void wavex_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    int32_t drift = p[P_E1] * 6;
    int32_t sub_lvl = p[P_E3] * 200;
    int32_t bits = 15 - (p[P_E2] * 11) / 127;
    uint32_t mask = bits < 15 ? (0xFFFFFFFFu << (15 - (uint32_t)bits)) : 0xFFFFFFFFu;
    int32_t cut = (p[P_E4] << 8) + m->cutoff + ((p[P_E7] * (v->pitch16 - 60 * 16)) >> 4);
    int32_t drive = 32768 + p[P_E6] * 512;
    tsvf_t flt;
    int32_t ic1 = v->s[1], ic2 = v->s[2];
    int32_t pos = v->s[0], bounce = v->s[3];
    uint32_t ph0 = v->ph[0], ph1 = v->ph[1], i;

    tsvf_coef(&flt, cut, p[P_E5]);
    for (i = 0; i < n; i++) {
        int32_t seg, frac, a, b, s;
        ph0 += m->inc;
        ph1 += m->inc >> 1;
        if (bounce) {
            pos -= drift;
            if (pos <= 0) { pos = 0; bounce = 0; }
        } else {
            pos += drift;
            if (pos >= WAVEX_POS_MAX) { pos = WAVEX_POS_MAX; bounce = 1; }
        }
        seg = pos >> 8;
        frac = pos & 255;
        a = wavex_wave((uint32_t)seg, ph0, m->inc);
        b = wavex_wave((uint32_t)seg + 1u, ph0, m->inc);
        s = a + (((b - a) * frac) >> 8);
        if (sub_lvl)
            s += mulq15(osc_sine(ph1), sub_lvl);
        s = (int32_t)((uint32_t)s & mask);
        s = tsvf_lp(&flt, s, &ic1, &ic2);
        s = clamp(mulq15(s, drive), -32767, 32767);
        if (p[P_E6])
            s = softclip(s);
        out[i] += voice_amp(s, m, i);
    }
    v->ph[0] = ph0;
    v->ph[1] = ph1;
    v->s[0] = pos;
    v->s[1] = ic1;
    v->s[2] = ic2;
    v->s[3] = bounce;
}

static const preset_t WAVEX_PRESETS[] = {
    /* name, {POS, MORPH, CRUSH, SUB, CUTOFF, RES, DRIVE, KTR}, {A D S R}, fenv, mono */
    {"DIGITAL SWEEP", {32, 114, 0, 102, 102, 25, 0, 64}, {4, 64, 89, 38}, 25, 0, FX(0, 0, 20, 10), PAT(1)},
    {"MORPHING PAD", {44, 127, 0, 127, 76, 13, 0, 32}, {102, 121, 108, 100}, 0, 0, FX(0, 60, 38, 70), PAT(5)},
    {"METALLIC LEAD", {108, 64, 0, 25, 95, 51, 0, 64}, {3, 38, 89, 19}, 51, 0, FX(0, 20, 10, 25), PAT(4)},
    {"SUB WOBBLE", {0, 64, 0, 0, 28, 76, 0, 0}, {0, 13, 127, 13}, 13, 1, FX(0, 0, 0, 10), PAT(2)},
    {"ALIEN MOTION", {89, 51, 44, 127, 83, 76, 10, 64}, {13, 76, 89, 64}, 0, 0, FX(0, 40, 40, 45), PAT(1)},
    {"GLASS MORPH LEAD", {64, 114, 0, 38, 108, 19, 0, 64}, {4, 57, 95, 25}, 0, 0, FX(0, 35, 20, 30), PAT(4)},
    {"RESO SWEEP BASS", {38, 64, 0, 0, 15, 108, 5, 64}, {0, 10, 44, 10}, 63, 1, FX(0, 0, 8, 8), PAT(2)},
    {"CYBERPUNK DRONE", {76, 25, 57, 89, 57, 64, 15, 32}, {90, 100, 127, 115}, 0, 0, FX(0, 55, 45, 75), PAT(5)},
};

static const engine_t ENG_WAVEX = {
    .name = "WAVEX",
    .page_title = {"WAVE", "FLT"},
    .edit = {
        {"POSITION", F_PCT, 0, 127, 0, 0, 0},
        {"MORPH", F_PCT, 0, 127, 40, 0, 0},
        {"CRUSH", F_PCT, 0, 127, 0, 0, 0},
        {"SUB", F_PCT, 0, 127, 0, 0, 0},
        {"CUTOFF", F_CUTOFF, 0, 127, 90, 0, 0},
        {"RESONANCE", F_PCT, 0, 127, 20, 0, 0},
        {"DRIVE", F_PCT, 0, 127, 0, 0, 0},
        {"KTR", F_PCT, 0, 127, 64, 0, 0},
    },
    .presets = WAVEX_PRESETS,
    .npresets = NELEM(WAVEX_PRESETS),
    .note_on = wavex_note_on,
    .render = wavex_render,
    .knob = {P_E0, P_E1, P_E4, P_E5},
    .keep = 0x06,                /* the filter (s[1], s[2]) */
};
