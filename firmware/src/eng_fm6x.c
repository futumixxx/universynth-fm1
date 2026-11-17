/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments (style); UNIVERSYNTH's own engine */
/* FM6X: UNIVERSYNTH's own 6-operator phase modulation engine, distinct from Felucca's FM6 (Dexed-based
 * DX7 emulation): 8 fixed operator graphs of its own (not DX7 algorithms), one phase accumulator per
 * voice scaled per operator (Q16, 64-bit intermediate, exact mod 2^32 — six operators never need six
 * separate accumulators), and up to two self-feedback operators per graph using the two free state words
 * as one-sample delay registers.
 *
 * ownenv: FM6X drives its own six-operator envelopes (one attack-then-decay-to-target shaper per operator,
 * re-triggered fresh on every note_on) instead of the host ADSR; voice.c renders its block at 1.0 and
 * FM6X ends the voice itself (done()) once every carrier has decayed out and the gate is up.
 *
 * Parameters:
 *   E0 ALGORITHM  one of 8 operator graphs (name below)
 *   E1 DEPTH      FM index applied at every modulation edge (parent -> child)
 *   E2 FEEDBACK   FM index of each algorithm's self-feedback operator(s)
 *   E3 RATIO      stretches modulator ratios only (0.5x .. 2x); carriers keep their table ratio, so the
 *                 voice always tracks pitch correctly
 *   E4 ATTACK     operator envelope attack time
 *   E5 DECAY      operator envelope decay / release time
 *   E6 BRIGHT     how far modulators decay below full level while held (0 = constant brightness)
 *   E7 SUSTAIN    carrier level held while a key is down (0 = percussive pluck, full decay to silence)
 */
static const char *const N_FM6X_ALG[] = {
    "CHAIN6", "DUO-FM", "STAR", "TRIO-FM", "4+FM", "STACK4", "DUAL-FM", "WEB"
};

typedef struct {
    int8_t parent[6];    /* -1 = no modulator input, else the operator (< k) that phase-modulates it */
    uint8_t carrier;     /* bit k: operator k is summed to the part output */
    int8_t fb[2];        /* up to two operators with self-feedback, else -1 */
} fm6x_algo_t;

/* parent[k] < k always: a single forward pass (k = 0..5) has every parent already rendered. */
static const fm6x_algo_t FM6X_ALGO[8] = {
    /* CHAIN6:  0->1->2->3->4->5(carrier) */
    {{-1, 0, 1, 2, 3, 4}, 0x20, {0, -1}},
    /* DUO-FM:  0->1->2(carrier) | 3->4->5(carrier) */
    {{-1, 0, 1, -1, 3, 4}, 0x24, {0, 3}},
    /* STAR:    0 -> 1,2,3 (carriers) | 4->5(carrier) */
    {{-1, 0, 0, 0, -1, 4}, 0x2E, {0, 4}},
    /* TRIO-FM: (0->1)(2->3)(4->5), all three second ops carriers */
    {{-1, 0, -1, 2, -1, 4}, 0x2A, {0, 4}},
    /* 4+FM:    0,1,2,3 plain sine carriers (additive) | 4->5(carrier) */
    {{-1, -1, -1, -1, -1, 4}, 0x2F, {4, -1}},
    /* STACK4:  0->1->2->3(carrier) | 4(carrier) | 5(carrier) */
    {{-1, 0, 1, 2, -1, -1}, 0x38, {0, -1}},
    /* DUAL-FM: 0->1->2(carrier) | 3->4->5(carrier) (op3 is its own root) */
    {{-1, 0, 1, -1, 3, 4}, 0x24, {0, 3}},
    /* WEB:     0 -> 1,2 ; 1->3 ; 2->4(carrier) ; 3->5(carrier) */
    {{-1, 0, 0, 1, 2, 3}, 0x30, {0, -1}},
};

/* operator ratios, in sixteenths (16 = 1.0x); carriers keep this exactly, modulators are stretched by
 * E3 (RATIO). Values chosen per algorithm for a musical default, not derived from the graph. */
static const uint8_t FM6X_RATIO16[8][6] = {
    {8, 16, 16, 32, 48, 16},
    {32, 16, 16, 48, 16, 16},
    {16, 16, 32, 48, 8, 16},
    {16, 16, 32, 16, 48, 16},
    {16, 32, 48, 64, 16, 16},
    {8, 16, 32, 16, 16, 32},
    {8, 16, 16, 16, 32, 16},
    {16, 32, 48, 16, 16, 16},
};

static void fm6x_note_on(track_t *t, voice_t *v)
{
    uint32_t k;
    (void)t;
    v->ph[0] = 0;
    for (k = 0; k < 8u; k++)
        v->s[k] = 0;
}

static int fm6x_done(track_t *t, voice_t *v)
{
    const fm6x_algo_t *alg = &FM6X_ALGO[(uint32_t)t->p[P_E0] & 7u];
    uint32_t k;
    if (v->gate)
        return 0;
    for (k = 0; k < 6u; k++)
        if ((alg->carrier >> k & 1u) && v->s[k] > 48)
            return 0;
    return 1;
}

static void fm6x_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    const fm6x_algo_t *alg = &FM6X_ALGO[(uint32_t)p[P_E0] & 7u];
    const uint8_t *rat16 = FM6X_RATIO16[(uint32_t)p[P_E0] & 7u];
    int32_t depth_mul = 1 + (int32_t)p[P_E1] * 1968;
    int32_t fb_mul = 1 + (int32_t)p[P_E2] * 984;
    uint32_t ratio_scale = 32768u + (uint32_t)(p[P_E3] * 98304 / 127); /* Q16: 0.5x .. 2.0x */
    int32_t atk_shift = 2 + (p[P_E4] >> 4);
    int32_t dec_shift = 3 + (p[P_E5] >> 3);
    int32_t mod_sustain = 32767 - p[P_E6] * 258;
    int32_t car_sustain = p[P_E7] * 32767 / 127;
    uint32_t ratio_q16[6];
    int8_t fbslot[6] = {-1, -1, -1, -1, -1, -1};
    int32_t env[6], fb_reg[2];
    uint32_t ph0 = v->ph[0], i, k;

    if (mod_sustain < 0)
        mod_sustain = 0;
    if (alg->fb[0] >= 0)
        fbslot[alg->fb[0]] = 0;
    if (alg->fb[1] >= 0)
        fbslot[alg->fb[1]] = 1;
    for (k = 0; k < 6u; k++) {
        uint32_t base = (uint32_t)rat16[k] * 4096u;          /* x16 -> Q16 */
        ratio_q16[k] = (alg->carrier >> k & 1u) ? base
                     : (uint32_t)(((uint64_t)base * ratio_scale) >> 16);
    }
    for (k = 0; k < 6u; k++) {
        int32_t is_car = alg->carrier >> k & 1;
        int32_t tgt = v->gate ? (is_car ? car_sustain : mod_sustain) : 0;
        int32_t e = v->s[k];
        if (e < 32600 && v->gate)
            e += (32767 - e) >> atk_shift;
        else
            e += (tgt - e) >> dec_shift;
        env[k] = clamp(e, 0, 32767);
    }
    fb_reg[0] = v->s[6];
    fb_reg[1] = v->s[7];

    for (i = 0; i < n; i++) {
        int32_t opout[6];
        int32_t mix = 0;
        ph0 += m->inc;
        for (k = 0; k < 6u; k++) {
            uint32_t phk = (uint32_t)(((uint64_t)ph0 * ratio_q16[k]) >> 16);
            int32_t pmoff = 0;
            int8_t par = alg->parent[k];
            int32_t s;
            if (par >= 0)
                pmoff = mul_wrap(opout[(uint32_t)par], depth_mul);
            if (fbslot[k] >= 0)
                pmoff += mul_wrap(fb_reg[fbslot[k]], fb_mul);
            s = sine_i(phk + (uint32_t)pmoff);
            s = mulq15(s, env[k]);
            opout[k] = s;
            if (fbslot[k] >= 0)
                fb_reg[fbslot[k]] = s;
            if (alg->carrier >> k & 1u)
                mix += s;
        }
        out[i] += voice_amp(soft_knee(mix, 20000), m, i);
    }
    v->ph[0] = ph0;
    for (k = 0; k < 6u; k++)
        v->s[k] = env[k];
    v->s[6] = fb_reg[0];
    v->s[7] = fb_reg[1];
}

static const preset_t FM6X_PRESETS[] = {
    /* name, {ALG, DEPTH, FEEDBACK, RATIO, ATTACK, DECAY, BRIGHT, SUSTAIN}, {A D S R} (unused: ownenv), fenv, mono */
    {"DX E-PIANO", {0, 48, 0, 50, 10, 70, 90, 20}, {0, 0, 0, 0}, 0, 0, FX(0, 20, 20, 30), PAT(6)},
    {"METAL BELL", {2, 90, 20, 95, 0, 95, 110, 0}, {0, 0, 0, 0}, 0, 0, FX(0, 0, 30, 70), PAT(7)},
    {"FM BRASS", {6, 70, 55, 55, 30, 40, 40, 100}, {0, 0, 0, 0}, 0, 0, FX(10, 20, 10, 20), PAT(5)},
    {"GLASS PAD", {4, 35, 0, 60, 90, 110, 30, 115}, {0, 0, 0, 0}, 0, 0, FX(0, 60, 30, 70), PAT(2)},
    {"FM BASS", {1, 65, 25, 15, 0, 45, 70, 35}, {0, 0, 0, 0}, 0, 0, FX(0, 0, 0, 10), PAT(1)},
    {"ORGAN STACK", {5, 25, 0, 40, 0, 20, 10, 127}, {0, 0, 0, 0}, 0, 0, FX(0, 30, 0, 20), PAT(6)},
};

static const engine_t ENG_FM6X = {
    .name = "FM6X",
    .page_title = {"FM", "ENV"},
    .edit = {
        {"ALGORITHM", F_ENUM, 0, 7, 0, N_FM6X_ALG, 0},
        {"DEPTH", F_PCT, 0, 127, 48, 0, 0},
        {"FEEDBACK", F_PCT, 0, 127, 0, 0, 0},
        {"RATIO", F_PCT, 0, 127, 64, 0, 0},
        {"ATTACK", F_PCT, 0, 127, 0, 0, 0},
        {"DECAY", F_PCT, 0, 127, 60, 0, 0},
        {"BRIGHT", F_PCT, 0, 127, 64, 0, 0},
        {"SUSTAIN", F_PCT, 0, 127, 40, 0, 0},
    },
    .presets = FM6X_PRESETS,
    .npresets = NELEM(FM6X_PRESETS),
    .note_on = fm6x_note_on,
    .render = fm6x_render,
    .ownenv = 1,
    .done = fm6x_done,
    .knob = {P_E0, P_E1, P_E2, P_E3},
};
