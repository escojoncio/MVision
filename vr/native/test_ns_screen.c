/* Pruebas sintéticas de los efectos de pantalla nativos (ns_screen). Sin datos del juego. */
#include "ns_screen.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_fail, g_ok;
#define CHECK(c, ...) do { if (c) g_ok++; else { g_fail++; printf("FALLO %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static int near(float a, float b, float e) { return fabsf(a - b) <= e; }

static const float TOVR[12] = { 0.001f, 0, 0, 1.0f,   0, -0.001f, 0, 2.0f,   0, 0, 0.001f, -3.0f };   /* escala 1/1000, Y invertida, traslación */

static void base_fx(NsScreenFx *fx) { memset(fx, 0, sizeof *fx); }

static void test_invert(void) {
    float inv[12], p[3] = { 1234, -567, 89 }, v[3], b[3];
    CHECK(ns_screen_invert34(TOVR, inv), "inversa");
    ns_screen_xform34(TOVR, p, v); ns_screen_xform34(inv, v, b);
    CHECK(near(b[0], p[0], 0.05f) && near(b[1], p[1], 0.05f) && near(b[2], p[2], 0.05f), "ida y vuelta %f %f %f", b[0], b[1], b[2]);
    float sing[12] = {0};
    CHECK(!ns_screen_invert34(sing, inv), "singular");
}

static void test_none(void) {
    NsScreenFx fx; base_fx(&fx);
    NsGpuScreen o; float inv[12]; float head[3] = {1, 2, -3};
    CHECK(ns_screen_build(&fx, TOVR, head, 0.016f, NULL, NULL, &o, inv) == 0, "sin efectos no hay recubrimientos");
    CHECK(o.water_min[3] == 0 && o.water_tint[3] == 0, "sin agua");
    CHECK(ns_screen_build(NULL, TOVR, head, 0.016f, NULL, NULL, &o, inv) == 0, "NULL");
}

static void test_water(void) {
    NsScreenFx fx; base_fx(&fx);
    fx.flags = NS_SFX_WATER;
    fx.water_min[0] = -1000; fx.water_min[1] = 0; fx.water_min[2] = -1000;
    fx.water_max[0] = 1000; fx.water_max[1] = 500; fx.water_max[2] = 1000;
    fx.water_tint[0] = 0.5f; fx.water_tint[1] = 0.8f; fx.water_tint[2] = 1.0f;
    fx.water_wave_rows = 3; fx.water_wave_period = 16; fx.water_bar_rows = 10; fx.water_phase = 5;
    /* cabeza fuera del volumen: mundo VR de un punto con y_motor = 2000 */
    float out_e[3] = { 0, 2000, 0 }, in_e[3] = { 0, 250, 0 }, head_out[3], head_in[3];
    ns_screen_xform34(TOVR, out_e, head_out); ns_screen_xform34(TOVR, in_e, head_in);
    NsGpuScreen o; float inv[12];
    int n = ns_screen_build(&fx, TOVR, head_out, 0.016f, NULL, NULL, &o, inv);
    CHECK(n == 0 && o.water_min[3] == 1 && o.water_tint[3] == 0, "fuera: volumen sí, cabeza no, sin barras (n=%d)", n);
    float col[3] = { 1, 1, 1 }, du, dv, ew_in[3] = { 10, 100, 10 }, ew_out[3] = { 10, 900, 10 };
    CHECK(ns_screen_water_ref(o.water_min, o.water_max, o.water_tint, o.water_wave, ew_in, &du, &dv, col) == 1, "fragmento en el volumen afectado");
    CHECK(near(col[0], 0.5f, 1e-5f) && near(col[1], 0.8f, 1e-5f) && near(col[2], 1.0f, 1e-5f), "tinte %f %f %f", col[0], col[1], col[2]);
    float amp = 3.0f; CHECK(fabsf(du) <= amp + 1e-4f && fabsf(dv) <= amp * 0.5f + 1e-4f, "onda acotada %f %f", du, dv);
    float c2[3] = { 1, 1, 1 };
    CHECK(ns_screen_water_ref(o.water_min, o.water_max, o.water_tint, o.water_wave, ew_out, &du, &dv, c2) == 0 && c2[0] == 1 && du == 0 && dv == 0, "fragmento fuera: intacto");
    /* cabeza dentro: todo teñido, barras presentes */
    n = ns_screen_build(&fx, TOVR, head_in, 0.016f, NULL, NULL, &o, inv);
    CHECK(n == 1 && o.ov[0].kind == NSG_OV_BARS && o.water_tint[3] == 1, "dentro: barras y bandera de cabeza (n=%d)", n);
    float c3[3] = { 1, 1, 1 };
    CHECK(ns_screen_water_ref(o.water_min, o.water_max, o.water_tint, o.water_wave, ew_out, &du, &dv, c3) == 1 && near(c3[0], 0.5f, 1e-5f), "cabeza dentro tiñe incluso lo de fuera del volumen");
    /* INVARIANCIA con la cabeza: la onda sólo depende de la posición en el mundo y de la fase */
    float du2, dv2, c4[3] = { 1, 1, 1 };
    float head_in2[3]; float in2[3] = { 300, 100, -200 }; ns_screen_xform34(TOVR, in2, head_in2);
    ns_screen_build(&fx, TOVR, head_in2, 0.016f, NULL, NULL, &o, inv);
    ns_screen_water_ref(o.water_min, o.water_max, o.water_tint, o.water_wave, ew_in, &du2, &dv2, c4);
    float c5[3] = { 1, 1, 1 }, du1, dv1;
    ns_screen_build(&fx, TOVR, head_in, 0.016f, NULL, NULL, &o, inv);
    ns_screen_water_ref(o.water_min, o.water_max, o.water_tint, o.water_wave, ew_in, &du1, &dv1, c5);
    CHECK(du1 == du2 && dv1 == dv2, "la onda del mundo no depende de dónde esté la cabeza");
    /* la fase mueve la onda */
    fx.water_phase = 9;
    ns_screen_build(&fx, TOVR, head_in, 0.016f, NULL, NULL, &o, inv);
    float c6[3] = { 1, 1, 1 }, du3, dv3;
    ns_screen_water_ref(o.water_min, o.water_max, o.water_tint, o.water_wave, ew_in, &du3, &dv3, c6);
    CHECK(du3 != du1, "la fase cambia la onda");
    /* el periodo se repite */
    fx.water_phase = 5 + 16;
    ns_screen_build(&fx, TOVR, head_in, 0.016f, NULL, NULL, &o, inv);
    float c7[3] = { 1, 1, 1 }, du4, dv4;
    ns_screen_water_ref(o.water_min, o.water_max, o.water_tint, o.water_wave, ew_in, &du4, &dv4, c7);
    CHECK(near(du4, du1, 1e-4f) && near(dv4, dv1, 1e-4f), "periodo");
    /* sin bandera de agua no hay nada */
    fx.flags = 0;
    ns_screen_build(&fx, TOVR, head_in, 0.016f, NULL, NULL, &o, inv);
    CHECK(o.water_min[3] == 0, "sin bandera no hay agua");
}

static void test_bars(void) {
    NsGpuOverlay ov; memset(&ov, 0, sizeof ov);
    ov.kind = NSG_OV_BARS; ov.abr = 2; ov.amount = 1; ov.rgb[0] = ov.rgb[1] = ov.rgb[2] = 1; ov.p[0] = 10; ov.p[1] = 2; ov.p[3] = 160;
    float b[3] = { 0.8f, 0.8f, 0.8f }, o[3];
    ns_screen_overlay_ref(&ov, 0, 0, b, o);
    CHECK(near(o[0], 0.8f, 1e-5f), "centro intacto");
    ns_screen_overlay_ref(&ov, 0, 118.0f / 160.0f, b, o);
    CHECK(near(o[0], 0.0f, 1e-5f), "barra negra arriba (%f)", o[0]);
    ns_screen_overlay_ref(&ov, 0.3f, -118.0f / 160.0f, b, o);
    CHECK(near(o[0], 0.0f, 1e-5f), "barra negra abajo");
    ns_screen_overlay_ref(&ov, 0, 100.0f / 160.0f, b, o);
    CHECK(near(o[0], 0.8f, 1e-5f), "justo antes de la barra");
}

static void test_ring(void) {
    NsScreenFx fx; base_fx(&fx);
    fx.flags = NS_SFX_GAS; fx.gas_intensity = 1; fx.gas_inner_px = 60; fx.gas_outer_px = 120; fx.gas_aspect = 0.75f; fx.gas_abr = 1;
    fx.gas_in[0] = 0; fx.gas_in[1] = 0.2f; fx.gas_in[2] = 0; fx.gas_out[0] = 0.6f; fx.gas_out[1] = 0.6f; fx.gas_out[2] = 0;
    NsGpuScreen o; float inv[12], head[3] = {0};
    CHECK(ns_screen_build(&fx, TOVR, head, 0.016f, NULL, NULL, &o, inv) == 1 && o.ov[0].kind == NSG_OV_RING && o.ov[0].abr == 1, "anillo");
    float b[3] = { 0.1f, 0.1f, 0.1f }, r[3];
    ns_screen_overlay_ref(&o.ov[0], 0, 0, b, r);
    CHECK(near(r[0], 0.1f, 1e-5f) && near(r[1], 0.1f, 1e-5f), "centro libre");
    ns_screen_overlay_ref(&o.ov[0], 130.0f / 160.0f, 0, b, r);
    CHECK(near(r[0], 0.7f, 1e-4f) && near(r[1], 0.7f, 1e-4f), "borde exterior (%f)", r[0]);
    /* eje vertical: el radio se divide por la razón */
    float ty = 0.75f * 130.0f / 160.0f;
    ns_screen_overlay_ref(&o.ov[0], 0, ty, b, r);
    CHECK(near(r[0], 0.7f, 1e-3f), "razón vertical (%f)", r[0]);
    /* simetría izquierda/derecha y arriba/abajo (no gira con nada) */
    float r2[3]; ns_screen_overlay_ref(&o.ov[0], -130.0f / 160.0f, 0, b, r2);
    CHECK(r[0] == r2[0] || near(r2[0], 0.7f, 1e-4f), "simetría");
    fx.gas_intensity = 0; CHECK(ns_screen_build(&fx, TOVR, head, 0.016f, NULL, NULL, &o, inv) == 0, "intensidad 0 no dibuja");
}

static void test_fade_flash(void) {
    NsScreenFx fx; base_fx(&fx);
    NsGpuScreen o; float inv[12], head[3] = {0};
    NsScreenCfg cfg = ns_screen_cfg_default();
    fx.flags = NS_SFX_FADE; fx.fade_amount = 0.5f; fx.fade_white = 0;
    CHECK(ns_screen_build(&fx, TOVR, head, 0.016f, &cfg, NULL, &o, inv) == 1 && o.ov[0].abr == 2, "fundido a negro resta");
    float b[3] = { 0.8f, 0.6f, 0.4f }, r[3];
    ns_screen_overlay_ref(&o.ov[0], 0.2f, 0.1f, b, r);
    CHECK(near(r[0], 0.3f, 1e-5f) && near(r[1], 0.1f, 1e-5f) && near(r[2], 0.0f, 1e-5f), "negro 50 %%: %f %f %f", r[0], r[1], r[2]);
    fx.fade_amount = 1; ns_screen_build(&fx, TOVR, head, 0.016f, &cfg, NULL, &o, inv);
    ns_screen_overlay_ref(&o.ov[0], 0.2f, 0.1f, b, r);
    CHECK(r[0] == 0 && r[1] == 0 && r[2] == 0, "negro total");
    fx.fade_white = 1; ns_screen_build(&fx, TOVR, head, 0.016f, &cfg, NULL, &o, inv);
    CHECK(o.ov[0].abr == 1, "fundido a blanco suma");
    ns_screen_overlay_ref(&o.ov[0], 0.2f, 0.1f, b, r);
    CHECK(r[0] == 1 && r[1] == 1 && r[2] == 1, "blanco total");
    /* el fundido es uniforme en toda la pantalla (no depende de la dirección de la vista) */
    float r2[3]; ns_screen_overlay_ref(&o.ov[0], -0.9f, 0.7f, b, r2);
    CHECK(r2[0] == r[0], "uniforme");
    /* destello */
    fx.flags = NS_SFX_FLASH;
    NsScreenState st; ns_screen_state_reset(&st);
    CHECK(ns_screen_build(&fx, TOVR, head, 0.016f, &cfg, &st, &o, inv) == 1 && o.ov[0].abr == 1 && near(o.ov[0].amount, cfg.flash_gain, 1e-5f), "destello suma con la ganancia");
    fx.flags = 0;
    ns_screen_build(&fx, TOVR, head, 0.016f, &cfg, &st, &o, inv);
    CHECK(st.flash < cfg.flash_gain && st.flash > 0, "decae con pendiente limitada (%f)", st.flash);
    for (int i = 0; i < 100; i++) ns_screen_build(&fx, TOVR, head, 0.016f, &cfg, &st, &o, inv);
    CHECK(st.flash == 0 && o.n_overlays == 0, "se apaga");
    /* destello: ganancia limitada (no deslumbra) */
    CHECK(cfg.flash_gain < 1.0f, "ganancia < 1");
}

static void test_modes(void) {
    NsScreenFx fx; base_fx(&fx);
    NsGpuScreen o; float inv[12], head[3] = {0};
    fx.flags = NS_SFX_MODE_A;
    int n = ns_screen_build(&fx, TOVR, head, 0.016f, NULL, NULL, &o, inv);
    CHECK(n == 2 && o.ov[0].kind == NSG_OV_COVER && o.ov[1].kind == NSG_OV_SCAN, "modo A: tinte y líneas (n=%d)", n);
    NsScreenCfg cfg = ns_screen_cfg_default();
    float b[3] = { 0.5f, 0.5f, 0.5f }, r[3];
    ns_screen_overlay_ref(&o.ov[0], 0, 0, b, r);
    CHECK(r[1] > r[0] && r[1] > r[2], "tinte verde (%f %f %f)", r[0], r[1], r[2]);
    fx.flags = NS_SFX_MODE_B; ns_screen_build(&fx, TOVR, head, 0.016f, NULL, NULL, &o, inv);
    ns_screen_overlay_ref(&o.ov[0], 0, 0, b, r);
    CHECK(r[0] > r[1] && r[0] > r[2], "tinte rojizo B (%f %f %f)", r[0], r[1], r[2]);
    /* líneas: alternan por filas del motor, nunca giran con la cabeza (dependen de ty) */
    float lit = 0, dark = 0;
    for (int i = 0; i < 40; i++) {
        float ty = (float)i * 0.25f / cfg.scan_period / 160.0f * cfg.scan_period; (void)ty;
        float t = ((float)i * 0.25f) / 160.0f;
        ns_screen_overlay_ref(&o.ov[1], 0, t, b, r);
        if (r[0] < 0.5f - 1e-6f) dark++; else lit++;
    }
    CHECK(dark > 5 && lit > 5, "líneas alternan (%g oscuras, %g claras)", dark, lit);
    float r2[3]; ns_screen_overlay_ref(&o.ov[1], 0.7f, 0.1f, b, r); ns_screen_overlay_ref(&o.ov[1], -0.7f, 0.1f, b, r2);
    CHECK(r[0] == r2[0], "las líneas no dependen de x");
    /* varias banderas a la vez: orden de dibujo estable */
    fx.flags = NS_SFX_MODE_A | NS_SFX_GAS | NS_SFX_FADE | NS_SFX_FLASH | NS_SFX_WATER; fx.gas_intensity = 0.5f; fx.fade_amount = 0.2f;
    fx.water_max[0] = fx.water_max[1] = fx.water_max[2] = 10;
    n = ns_screen_build(&fx, TOVR, head, 0.016f, NULL, NULL, &o, inv);
    CHECK(n <= NSG_MAX_OVERLAYS && o.ov[n - 1].kind == NSG_OV_COVER && o.ov[n - 1].abr == 2, "el fundido (a negro) va el último (abr=%u)", n ? o.ov[n - 1].abr : 9u);
}

int main(void) {
    test_invert(); test_none(); test_water(); test_bars(); test_ring(); test_fade_flash(); test_modes();
    printf("%s: %d comprobaciones, %d fallos\n", g_fail ? "FALLO" : "OK", g_ok + g_fail, g_fail);
    return g_fail ? 1 : 0;
}
