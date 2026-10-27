#include "ns_screen.h"
#include <math.h>
#include <string.h>

NsScreenCfg ns_screen_cfg_default(void) {
    NsScreenCfg c;
    memset(&c, 0, sizeof c);
    c.wave_gain = 1.0f; c.wave_space = 8.0f;
    c.bars_gain = 0.0f; c.bars_soft = 4.0f;
    c.scan_gain = 0.12f; c.scan_period = 2.0f; c.scan_cover = 0.5f;
    c.mode_amount = 0.30f;
    c.mode_a_rgb[0] = 0.20f; c.mode_a_rgb[1] = 1.00f; c.mode_a_rgb[2] = 0.30f;   /* visión A: verde */
    c.mode_b_rgb[0] = 1.00f; c.mode_b_rgb[1] = 0.35f; c.mode_b_rgb[2] = 0.20f;   /* visión B: rojizo */
    c.flash_gain = 0.55f; c.flash_slew = 6.0f; c.fade_slew = 0.0f;
    c.proj_h = 0.0f;
    return c;
}
void ns_screen_state_reset(NsScreenState *s) { memset(s, 0, sizeof *s); }

int ns_screen_invert34(const float m[12], float o[12]) {
    double a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
    double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (fabs(det) < 1e-20) return 0;
    double r[9] = { (e * i - f * h) / det, (c * h - b * i) / det, (b * f - c * e) / det,
                    (f * g - d * i) / det, (a * i - c * g) / det, (c * d - a * f) / det,
                    (d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det };
    for (int y = 0; y < 3; y++) {
        for (int x = 0; x < 3; x++) o[4 * y + x] = (float)r[3 * y + x];
        o[4 * y + 3] = (float)-(r[3 * y] * m[3] + r[3 * y + 1] * m[7] + r[3 * y + 2] * m[11]);
    }
    return 1;
}
void ns_screen_xform34(const float m[12], const float p[3], float o[3]) {
    for (int y = 0; y < 3; y++) o[y] = m[4 * y] * p[0] + m[4 * y + 1] * p[1] + m[4 * y + 2] * p[2] + m[4 * y + 3];
}

static float slew(float cur, float target, float rate, float dt) {
    if (rate <= 0.0f || dt <= 0.0f) return target;
    float d = target - cur, m = rate * dt;
    return d > m ? cur + m : (d < -m ? cur - m : target);
}
static float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

static NsGpuOverlay *add_ov(NsGpuScreen *o, unsigned kind, unsigned abr, float amount) {
    if (o->n_overlays >= NSG_MAX_OVERLAYS) return NULL;
    NsGpuOverlay *v = &o->ov[o->n_overlays++];
    memset(v, 0, sizeof *v);
    v->kind = kind; v->abr = abr; v->amount = amount;
    return v;
}

int ns_screen_build(const NsScreenFx *fx, const float to_vr[12], const float head_vr[3], float dt, const NsScreenCfg *cfgp, NsScreenState *st,
                    NsGpuScreen *out, float fromvr[12]) {
    NsScreenCfg cfg = cfgp ? *cfgp : ns_screen_cfg_default();
    NsScreenState tmp; if (!st) { memset(&tmp, 0, sizeof tmp); st = &tmp; }
    memset(out, 0, sizeof *out);
    if (fromvr) { static const float id[12] = {1,0,0,0, 0,1,0,0, 0,0,1,0}; memcpy(fromvr, id, sizeof id); }
    if (!fx) { st->flash = slew(st->flash, 0, cfg.flash_slew, dt); st->fade = slew(st->fade, 0, cfg.fade_slew, dt); return 0; }
    float inv[12]; int have_inv = ns_screen_invert34(to_vr, inv);
    if (have_inv && fromvr) memcpy(fromvr, inv, sizeof inv);
    const unsigned f = fx->flags;
    const float H = (cfg.proj_h > 8.0f && cfg.proj_h < 4096.0f) ? cfg.proj_h : 320.0f;   /* H real de la escena */
    /* agua: volumen en el mundo del motor; la cabeza dentro tiñe todo */
    if ((f & NS_SFX_WATER) && have_inv) {
        float he[3]; ns_screen_xform34(inv, head_vr, he);
        int inside = he[0] >= fx->water_min[0] && he[0] <= fx->water_max[0] && he[1] >= fx->water_min[1] && he[1] <= fx->water_max[1] &&
                     he[2] >= fx->water_min[2] && he[2] <= fx->water_max[2];
        for (int k = 0; k < 3; k++) { out->water_min[k] = fx->water_min[k]; out->water_max[k] = fx->water_max[k]; out->water_tint[k] = fx->water_tint[k]; }
        out->water_min[3] = 1.0f; out->water_tint[3] = inside ? 1.0f : 0.0f;
        int per = fx->water_wave_period > 0 ? fx->water_wave_period : 16;
        out->water_wave[0] = (float)fx->water_wave_rows * cfg.wave_gain;
        out->water_wave[1] = 6.2831853f / ((float)per * cfg.wave_space);
        out->water_wave[2] = 6.2831853f * (float)(fx->water_phase % (unsigned)per) / (float)per;
        if (inside && fx->water_bar_rows > 0 && cfg.bars_gain > 0) {
            NsGpuOverlay *o = add_ov(out, NSG_OV_BARS, 2, cfg.bars_gain);
            if (o) { o->rgb[0] = o->rgb[1] = o->rgb[2] = 1; o->p[0] = (float)fx->water_bar_rows; o->p[1] = cfg.bars_soft; o->p[3] = H; }
        }
    }
    /* modos de visión: tinte + líneas de exploración */
    unsigned modes = f & (NS_SFX_MODE_A | NS_SFX_MODE_B);
    if (modes) {
        const float *c = (modes & NS_SFX_MODE_B) ? cfg.mode_b_rgb : cfg.mode_a_rgb;
        NsGpuOverlay *o = add_ov(out, NSG_OV_COVER, 0, cfg.mode_amount);
        if (o) { o->rgb[0] = c[0]; o->rgb[1] = c[1]; o->rgb[2] = c[2]; }
        if (cfg.scan_gain > 0 && (o = add_ov(out, NSG_OV_SCAN, 2, cfg.scan_gain)) != NULL) {
            o->rgb[0] = o->rgb[1] = o->rgb[2] = 1; o->p[0] = cfg.scan_period; o->p[1] = 0; o->p[2] = cfg.scan_cover; o->p[3] = H;
        }
    }
    /* anillo (máscara de gas) */
    if ((f & NS_SFX_GAS) && fx->gas_intensity > 0) {
        NsGpuOverlay *o = add_ov(out, NSG_OV_RING, (unsigned)fx->gas_abr & 3u, clamp01(fx->gas_intensity));
        if (o) {
            for (int k = 0; k < 3; k++) { o->rgb[k] = fx->gas_in[k]; o->rgb2[k] = fx->gas_out[k]; }
            o->p[0] = fx->gas_inner_px; o->p[1] = fx->gas_outer_px; o->p[2] = 1.0f; o->p[3] = H;   /* viñeta circular: sin razón vertical (no gira con la cabeza) */
        }
    }
    /* destello (suma de blanco, limitado) */
    float flash_t = (f & NS_SFX_FLASH) ? cfg.flash_gain : 0.0f;
    st->flash = st->init ? slew(st->flash, flash_t, cfg.flash_slew, dt) : flash_t;
    if (st->flash > 0.001f) {
        NsGpuOverlay *o = add_ov(out, NSG_OV_COVER, 1, st->flash);
        if (o) o->rgb[0] = o->rgb[1] = o->rgb[2] = 1;
    }
    /* fundido a blanco (suma) o a negro (resta) */
    float fade_t = (f & NS_SFX_FADE) ? clamp01(fx->fade_amount) : 0.0f;
    st->fade = st->init ? slew(st->fade, fade_t, cfg.fade_slew, dt) : fade_t;
    st->init = 1;
    if (st->fade > 0.001f) {
        NsGpuOverlay *o = add_ov(out, NSG_OV_COVER, fx->fade_white ? 1u : 2u, st->fade);
        if (o) o->rgb[0] = o->rgb[1] = o->rgb[2] = 1;
    }
    return (int)out->n_overlays;
}

/* ---- máscara de visión de la caja (NSG_OV_FRAME) ---- */
int ns_screen_frame_build(float area_w, float area_h, float cx_col, float cy_row, float H, const float rgb[3], NsGpuOverlay out[2]) {
    if (!(area_w > 0.0f) || !(area_h > 0.0f) || !(H > 1.0f) || !out) return 0;
    for (int i = 0; i < 2; i++) {
        memset(&out[i], 0, sizeof out[i]);
        out[i].kind = NSG_OV_FRAME; out[i].abr = i == 0 ? 2u : 1u; out[i].amount = 1.0f;
        out[i].rgb2[0] = area_w; out[i].rgb2[1] = area_h; out[i].rgb2[2] = cy_row;
        out[i].p[0] = cx_col; out[i].p[1] = (float)i; out[i].p[3] = H;
    }
    out[0].rgb[0] = out[0].rgb[1] = out[0].rgb[2] = 1.0f;
    for (int k = 0; k < 3; k++) out[1].rgb[k] = rgb ? clamp01(rgb[k]) : 0.0f;
    return 2;
}
static float sstep(float e0, float e1, float x);
float ns_screen_frame_sub(const uint8_t *frame, int fw, int fh, const NsGpuOverlay *ov, float tx, float ty) {
    if (!frame || fw < 2 || fh < 2 || ov->rgb2[0] <= 0.0f || ov->rgb2[1] <= 0.0f) return 0.0f;
    const float H = ov->p[3] > 0 ? ov->p[3] : 320.0f;
    const float col = ov->p[0] + tx * H, row = ov->rgb2[2] - ty * H;     /* ty hacia arriba; las filas de la imagen crecen hacia abajo */
    const float u = col / ov->rgb2[0], v = row / ov->rgb2[1];
    if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) return 1.0f;       /* fuera de la imagen original: cerrado */
    float fx = u * (float)fw - 0.5f, fy = v * (float)fh - 0.5f;
    if (fx < 0) fx = 0; if (fy < 0) fy = 0;
    if (fx > (float)(fw - 1)) fx = (float)(fw - 1);
    if (fy > (float)(fh - 1)) fy = (float)(fh - 1);
    int x0 = (int)fx, y0 = (int)fy, x1 = x0 + 1 < fw ? x0 + 1 : x0, y1 = y0 + 1 < fh ? y0 + 1 : y0;
    float ax = fx - (float)x0, ay = fy - (float)y0;
    float a = (float)frame[y0 * fw + x0] * (1 - ax) + (float)frame[y0 * fw + x1] * ax, b = (float)frame[y1 * fw + x0] * (1 - ax) + (float)frame[y1 * fw + x1] * ax;
    return clamp01((a * (1 - ay) + b * ay) / 255.0f);
}
void ns_screen_frame_ref(const NsGpuOverlay *ov, float sub, const float b[3], float out[3]) {
    for (int k = 0; k < 3; k++) {
        float r = ov->p[1] < 0.5f ? b[k] - sub : b[k] + ov->rgb[k] * sstep(0.85f, 1.0f, sub);
        out[k] = clamp01(r);
    }
}

/* ---- referencias ---- */
static float sstep(float e0, float e1, float x) {
    if (e1 <= e0) return x >= e1 ? 1.0f : 0.0f;
    float t = clamp01((x - e0) / (e1 - e0)); return t * t * (3.0f - 2.0f * t);
}
void ns_screen_overlay_ref(const NsGpuOverlay *ov, float tx, float ty, float ydir, const float b[3], float out[3]) {
    float F[3] = { ov->rgb[0], ov->rgb[1], ov->rgb[2] };
    float a = ov->amount;                                     /* intensidad efectiva por píxel */
    const float H = ov->p[3] > 0 ? ov->p[3] : 320.0f;
    const float px = tx * H, py = ty * H;
    switch (ov->kind) {
    case NSG_OV_RING: {
        float r = sqrtf(px * px + py * py);
        float c = sstep(ov->p[0], ov->p[1], r);
        for (int k = 0; k < 3; k++) F[k] = ov->rgb[k] + (ov->rgb2[k] - ov->rgb[k]) * c;
        a *= c; break; }
    case NSG_OV_SCAN: {
        /* anclada al mundo: la fase sale de la ELEVACIÓN del rayo en el mundo VR (en filas del motor: ángulo * H), no de la fila de pantalla */
        float per = ov->p[0] > 0 ? ov->p[0] : 2.0f;
        float yd = ydir < -1.0f ? -1.0f : (ydir > 1.0f ? 1.0f : ydir);
        float ph = asinf(yd) * H / per + ov->p[1];
        float fr = ph - floorf(ph);
        a *= fr < ov->p[2] ? 1.0f : 0.0f; break; }
    case NSG_OV_BARS: {
        float lim = 120.0f - ov->p[0];                        /* media pantalla del motor = 120 filas; viñeta CIRCULAR (las barras del original no giran con la cabeza) */
        float r = sqrtf(px * px + py * py);
        a *= sstep(lim - 10.0f * ov->p[1], lim + 10.0f * ov->p[1], r); break; }
    default: break;
    }
    for (int k = 0; k < 3; k++) {
        float r;
        switch (ov->abr) {
        case 0: r = b[k] * (1.0f - 0.5f * a) + F[k] * 0.5f * a; break;
        case 1: r = b[k] + F[k] * a; break;
        case 2: r = b[k] - F[k] * a; break;
        default: r = b[k] + F[k] * a * 0.25f; break;
        }
        out[k] = clamp01(r);
    }
}

int ns_screen_water_ref(const float wmin[4], const float wmax[4], const float wtint[4], const float wwave[4], const float ew[3], float *du, float *dv, float col[3]) {
    *du = 0; *dv = 0;
    if (wmin[3] < 0.5f) return 0;
    int in = ew[0] >= wmin[0] && ew[0] <= wmax[0] && ew[1] >= wmin[1] && ew[1] <= wmax[1] && ew[2] >= wmin[2] && ew[2] <= wmax[2];
    if (!in && wtint[3] < 0.5f) return 0;
    for (int k = 0; k < 3; k++) col[k] *= wtint[k];
    /* onda horizontal que depende de la altura (como las filas de la pantalla del motor) y vertical suave que depende de la posición horizontal */
    *du = wwave[0] * sinf(wwave[1] * ew[1] + wwave[2]);
    *dv = 0.5f * wwave[0] * sinf(wwave[1] * (ew[0] + ew[2]) + wwave[2]);
    return 1;
}
