/* l2d_dump: reproduce sin conexión la capa 2D de un volcado (list.bin + vram.bin) y la compara con frame.png, el fotograma REAL mostrado.
 * Uso (desde la raíz del repo):  make -C vr/scene l2d-dump ARGS="<carpeta del volcado> [<otra carpeta> ...] [--max-pct P] [--png <carpeta de salida>]"
 * --png DIR: escribe, por volcado, la capa 2D que llega al panel del visor (UNA sola capa completa, sin trocear: la misma que vl2a/vrhost con el troceo apagado) como PNG sobre un fondo gris a cuadros (lo transparente se ve
 *   a cuadros): <volcado>_capa2d.png (1x, exacta) y <volcado>_hoja.png (x2: frame.png original | capa del panel | en vista de cine, además la capa completa opaca). Un volcado sin list.bin (los de «stage»: solo RAM/VRAM/GTE)
 *   NO tiene capa reproducible (la capa sale de las primitivas de la lista de dibujo): se avisa y se salta, no se aproxima.
 * La escena se ensambla como vrhost.c build_one en el modo nativo (opciones por defecto + screen_filter + drop_world2d, puente y registro de cámara del meta.txt, correspondencia mundo -> usuario, reensamblado sin filtro si no se conserva):
 *   op_dest == 2 = lo que va al panel. Con procedencia en el volcado (prov.bin) se clasifica como el visor (vr_prov.h, sin aprendizaje de la ventana). La garantía de cobertura nativa sale de la RAM del volcado
 *   (ns_fx: elemento exportable del paquete, anillo / fundido activos) dando por hecho un renderer nativo listo con escena apta (lo que el visor comprueba con vrhost_native_sfx); sin ram.bin nada es WORLD ni
 *   FX_SCREEN. Las primitivas de clase WORLD no salen de vr_scene (con drop_world2d) y, en la vista mundo, las clases sin capa 2D (FX_*, SCOPE_FRAME) se quitan del panel, igual que el reparto por capas de vrhost.c (aquí HUD + ventana + pantalla completa van juntas en una sola capa). Aproximaciones: cine_cand y last_scene3d se deducen del meta (rec_active == 0 con registro) y de las primitivas 3D de ESTA lista. El modo de la capa sale de view_mode del meta
 *   (= VrView.kind de vr_view.h: 0 UI, 1 mundo, 2 cinemática del motor, 3 vídeo), igual que vrhost.c: UI / vídeo -> VL_FULL (fotograma entero opaco), cinemática -> VL_PANEL_VERIFIED, mundo -> VL_PANEL.
 *   La VRAM del volcado es la del FINAL de la lista y se usa también como vram_start/vram_end (el visor usa la real de cada momento): para los modos con 2D semitransparente, realimentación (fbk) o texturas dibujadas dentro de la zona mostrada, y
 *   sobre todo para VL_PANEL_VERIFIED (que repite el mundo y lo compara con vram_end), la capa puede no ser idéntica a la del visor.
 * Mide, con vl2d a escala 1 (modo original, como la app):
 *   - pantalla completa (vl2d_run full=1): diferencia media por canal (0..255) y % de píxeles distintos frente a frame.png;
 *   - panel (full=0, lo que se pinta sobre el mundo): píxeles OPACOS de la capa frente a frame.png (los semitransparentes dependen del mundo de debajo).
 * Sale con 1 si la pantalla completa difiere más de P % (def. 0) o si algún píxel opaco del panel difiere.
 * Limitación: el volcado solo trae la VRAM AL FINAL de la lista; se usa como vram_start y como vram_end (la zona mostrada de partida es, pues, la final: mide la idempotencia del
 * rasterizado y las texturas, no los borrados ni las copias del propio fotograma: eso lo cubre test_layer2d con el oráculo gpu.c). frame.png: PNG RGBA sin compresión (el que escribe ns_dump). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu.h"
#include "gpu.h"
#include "hle.h"
#include "vr_scene.h"
#include "vr_layer2d.h"
#include "vr_prov.h"
#include "vr_feedback.h"
#include "ns_dump.h"
#include "ns_fx.h"
#include "vr_camera.h"
#include "vr_usermap.h"
#include "vr_view.h"

uint8_t emu_ram[EMU_RAM_SIZE];
uint32_t hle_frame_count(void) { return 0; }
void gte_hp_enable(int on) { (void)on; }
void gte_hp_frame(void) {}
int gte_hp_lookup(int sx, int sy, GpuHpInfo *o) { (void)sx; (void)sy; (void)o; return 0; }
int gte_hp_resolve_a(int n, const int *sx, const int *sy, const uint32_t *a, const uint32_t *w, GpuHpInfo *o, uint8_t *ok) { (void)n; (void)sx; (void)sy; (void)a; (void)w; (void)o; (void)ok; return 0; }
int gte_hp_resolve_addr(uint32_t addr, uint32_t word, GpuHpInfo *o) { (void)addr; (void)word; (void)o; return 0; }
void hle_invoke(Cpu *c, uint32_t fn, uint32_t arg) { (void)c; (void)fn; (void)arg; }
int rt_orig(Cpu *c, const char *name) { (void)c; (void)name; return 0; }
uint8_t mem_r8(Cpu *c, uint32_t a) { (void)c; return emu_ram[a & 0x1FFFFF]; }
uint16_t mem_r16(Cpu *c, uint32_t a) { (void)c; uint16_t v; memcpy(&v, &emu_ram[a & 0x1FFFFE], 2); return v; }
uint32_t mem_r32(Cpu *c, uint32_t a) { (void)c; uint32_t v; memcpy(&v, &emu_ram[a & 0x1FFFFC], 4); return v; }
void mem_w32(Cpu *c, uint32_t a, uint32_t v) { (void)c; memcpy(&emu_ram[a & 0x1FFFFC], &v, 4); }

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* PNG RGBA8 con bloques "stored" (sin compresión) -> RGBA. NULL si no es de ese tipo. */
static uint8_t *png_read_stored(const char *path, int *w, int *h) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = (uint8_t *)malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
    fclose(f);
    if (n < 33 || memcmp(b, "\x89PNG\r\n\x1a\n", 8)) { free(b); return NULL; }
    uint8_t *z = (uint8_t *)malloc((size_t)n); size_t zn = 0;
    int W = 0, H = 0;
    for (long o = 8; o + 12 <= n;) {
        uint32_t len = be32(b + o); const uint8_t *t = b + o + 4, *d = b + o + 8;
        if (o + 12 + len > (uint32_t)n) break;
        if (!memcmp(t, "IHDR", 4)) { W = (int)be32(d); H = (int)be32(d + 4); if (d[8] != 8 || d[9] != 6 || d[12] != 0) { free(b); free(z); return NULL; } }
        else if (!memcmp(t, "IDAT", 4)) { memcpy(z + zn, d, len); zn += len; }
        o += 12 + len;
    }
    free(b);
    size_t rowb = (size_t)W * 4 + 1, need = rowb * (size_t)H;
    uint8_t *raw = (uint8_t *)malloc(need + 16); size_t rn = 0;
    size_t i = 2;   /* cabecera zlib */
    while (i + 5 <= zn && rn < need) {
        int fin = z[i] & 1, bt = (z[i] >> 1) & 3;
        if (bt != 0) { free(z); free(raw); return NULL; }
        uint32_t L = z[i + 1] | (uint32_t)z[i + 2] << 8;
        i += 5;
        if (i + L > zn || rn + L > need + 16) { free(z); free(raw); return NULL; }
        memcpy(raw + rn, z + i, L); rn += L; i += L;
        if (fin) break;
    }
    free(z);
    if (rn < need) { free(raw); return NULL; }
    uint8_t *out = (uint8_t *)malloc((size_t)W * H * 4);
    for (int y = 0; y < H; y++) {   /* deshacer filtros */
        const uint8_t *s = raw + (size_t)y * rowb + 1; uint8_t *d = out + (size_t)y * W * 4; int ft = raw[(size_t)y * rowb];
        for (int x = 0; x < W * 4; x++) {
            int a = x >= 4 ? d[x - 4] : 0, up = y ? d[x - W * 4] : 0, c = (x >= 4 && y) ? d[x - W * 4 - 4] : 0, v = s[x];
            switch (ft) {
            case 1: v += a; break;
            case 2: v += up; break;
            case 3: v += (a + up) >> 1; break;
            case 4: { int p = a + up - c, pa = abs(p - a), pb = abs(p - up), pc = abs(p - c); v += (pa <= pb && pa <= pc) ? a : (pb <= pc ? up : c); break; }
            default: break;
            }
            d[x] = (uint8_t)v;
        }
    }
    free(raw);
    *w = W; *h = H;
    return out;
}

/* Garantía de cobertura nativa (VrProvLearn.covered) con la RAM del volcado: como prov_covered de vrhost.c, con el renderer nativo listo y la escena apta. */
static struct { const uint8_t *ram; NsFxScene *fx; int ok; int fb; } g_cov;
static int dump_covered(void *ctx, int what, uint32_t pkt_addr) {
    (void)ctx;
    if (!g_cov.ok) return 0;
    if (what == VPC_COV_FEEDBACK) return g_cov.fb;   /* realimentación de pantalla reproducible (vr_feedback.h): como vrhost.c con la escena nativa ya anotada */
    if (what == VPC_COV_GAS) return g_cov.fx->screen.gas.active && g_cov.fx->screen.gas.intensity > 0;
    if (what == VPC_COV_FADE) return g_cov.fx->screen.fade.active;
    return what == VPC_COV_PACKET && ns_fx_covers_packet(g_cov.ram, g_cov.fx, pkt_addr);
}

/* Ensamblado de la escena como vrhost.c build_one en modo nativo (ver la cabecera). 0 = bien. */
static int build_scene(const NsDump *d, const GpuDrawList *l, VrScene *sc) {
    VrSceneOpts o = vr_scene_opts_default();
    o.screen_filter = 1;   /* on_start */
    static VrProvLearn learn;   /* build_one con la procedencia encendida (sin aprendizaje de la ventana) */
    if (!g_cov.fx) g_cov.fx = ns_fx_scene_create(0);
    NsFxOpts fo = ns_fx_opts_default();
    g_cov.ram = d->ram; g_cov.ok = d->ram && g_cov.fx && ns_fx_extract(d->ram, &fo, g_cov.fx) == 0;
    { VrFeedback fb; int fbm = vrfb_build(l->ops, l->nops, NULL, NULL, d->vram, &fb);
      g_cov.fb = fbm != VRFB_NONE && getenv("L2D_SIN_REALIM") == NULL;
      if (fb.n) printf("realimentación de pantalla: %d primitivas, %s (a=%.3f b=%.3f, residuo %.1f px)\n", fb.n, fb.mode == VRFB_PREV ? "imagen anterior" : fb.mode == VRFB_SELF ? "misma imagen" : "sin modelo", (double)fb.a, (double)fb.b[0], (double)fb.err_px); }
    learn.covered = dump_covered;
    vpc_learn_note(&learn, l, VPC_ST_OTHER);   /* build_one anota cada lista (vrhost.c): fija bino (prismáticos); sin ventana aprendida (estado OTHER) */
    o.op_class = vpc_op_class_cb; o.op_class_ctx = &learn;
    o.drop_world2d = getenv("L2D_SIN_W2D") == NULL && ((int)ns_dump_meta_num(d, "view_mode", 1) == VRV_K_WORLD || (int)ns_dump_meta_num(d, "view_mode", 1) == VRV_K_ENGINE);    /* build_one: vista mundo y cinemática del motor (el meta no trae el motivo: selector/pausa no se distinguen); L2D_SIN_W2D=1 la apaga (comparar con/sin) */
    BridgeState bs; memset(&bs, 0, sizeof bs);
    VrCamRecord rec; memset(&rec, 0, sizeof rec);
    float M[9], eye[3], pref[3] = {0, 0, 0};
    int have_rec = ns_dump_meta_floats(d, "rec_M", M, 9) == 9 && ns_dump_meta_floats(d, "rec_eye", eye, 3) == 3;
    int rec_active = 0;
    if (have_rec) {
        ns_dump_meta_floats(d, "rec_p_ref", pref, 3);
        rec.serial = 1; rec.active = rec_active = (int)ns_dump_meta_num(d, "rec_active", 0) != 0;
        for (int i = 0; i < 3; i++) rec.eye[i] = (int32_t)lrintf(eye[i]);
        memcpy(rec.M, M, sizeof M); memcpy(rec.p_ref, pref, sizeof pref);
        rec.theta_play_deg = (float)ns_dump_meta_num(d, "rec_theta", 0); rec.units_per_m = (float)ns_dump_meta_num(d, "rec_units_per_m", 1000);
        bs.valid = BRIDGE_OK_EYE;
        for (int i = 0; i < 3; i++) { bs.eye_pos[i] = rec.eye[i]; for (int j = 0; j < 3; j++) bs.eye_axes[i][j] = (int16_t)lrintf(M[3 * i + j] * 4096.0f); }
        if ((int)ns_dump_meta_num(d, "game", 0)) {   /* has_player + núcleo del puente: world_valid y origen bajo el jugador */
            bs.valid = BRIDGE_OK_ALL_CORE; bs.has_player = 1;
            bs.player_pos.x = (int16_t)ns_dump_meta_num(d, "player_x", 0); bs.player_pos.y = (int16_t)ns_dump_meta_num(d, "player_y", 0); bs.player_pos.z = (int16_t)ns_dump_meta_num(d, "player_z", 0);
        }
        if (rec_active) {
            float org[3]; vr_scene_player_origin(&bs, &o, org);
            o.origin_mode = 1; o.origin_game[0] = (float)rec.eye[0]; o.origin_game[1] = org[1]; o.origin_game[2] = (float)rec.eye[2];
            vrcam_scene_user_map(&o, &rec, org[1]);
        }
    }
    int rc = vr_scene_build(l, &bs, &o, sc);
    if (rc == 0 && o.screen_filter && sc->filter.active && sc->filter.n_strips == 0 && sc->filter.n_polys == 0) {
        int cine_cand = have_rec && !rec_active, scene3d = cine_cand || sc->stats.n_tris3d >= 12;
        if (!vrhost_keep_filter(1, cine_cand, scene3d, sc->has_3d, sc->world_valid, sc->filter.n_scan, sc->filter.frame_on, sc->filter.n_strips)) {
            VrSceneOpts o2 = o; o2.screen_filter = 0;
            rc = vr_scene_build(l, &bs, &o2, sc);
        }
    }
    if (rc == 0 && sc->op_dest && sc->op_cls && (int)ns_dump_meta_num(d, "view_mode", 1) == VRV_K_WORLD) {   /* vista mundo: reparto del panel por clase (vrhost.c, vpc_split_dest); las clases sin capa no llegan al panel */
        static uint8_t *od;
        uint8_t *q = (uint8_t *)realloc(od, (size_t)sc->n_op_dest + 1);
        if (q) {
            od = q;
            for (uint32_t i = 0; i < sc->n_op_dest; i++) od[i] = sc->op_dest[i] == 2 && vpc_layer_of(sc->op_cls[i]) < 0 ? 0 : sc->op_dest[i];
            sc->op_dest = od;
        }
    }
    return rc;
}

/* capa RGBA no premultiplicada (w x h) sobre un fondo gris a cuadros (casillas de 8 px de la capa), replicada k x k: RGBA opaco en `out` (w*k x h*k). */
static void on_checker(const uint8_t *rgba, int w, int h, int k, uint8_t *out, int ow) {
    for (int y = 0; y < h * k; y++)
        for (int x = 0; x < w * k; x++) {
            const uint8_t *p = &rgba[((size_t)(y / k) * w + (size_t)(x / k)) * 4];
            int bg = (((x / k) / 8 + (y / k) / 8) & 1) ? 0xB4 : 0x78, a = p[3];
            uint8_t *q = &out[((size_t)y * ow + x) * 4];
            for (int c = 0; c < 3; c++) q[c] = (uint8_t)((p[c] * a + bg * (255 - a) + 127) / 255);
            q[3] = 255;
        }
}
static const char *base_name(const char *p) {
    size_t n = strlen(p);
    while (n > 1 && p[n - 1] == '/') n--;
    const char *b = p; for (size_t i = 0; i < n; i++) if (p[i] == '/') b = p + i + 1;
    return b;
}

/* Una capa (modo vl2d_run `mode`) del volcado: copia RGBA (w x h) en *out (a liberar). 0 = sin capa. */
static int layer_of(const GpuDrawList *l, const VrScene *sc, int mode, uint8_t **out, int *w, int *h, VrL2Info *info) {
    GpuDrawList lv = *l;
    if (mode == VL_PANEL_VERIFIED) lv.vram_end = l->vram_start;   /* la VRAM del volcado es la del final de la lista */
    VrLayer2D *L = vl2d_new();
    vl2d_set_options(L, 1, VL_SMOOTH_ORIGINAL);   /* lo que fija vl2a_options_native en el visor */
    int ok = vl2d_run(L, &lv, sc, mode);
    if (ok) {
        const uint8_t *rg = vl2d_rgba(L, w, h);
        *out = (uint8_t *)malloc((size_t)*w * *h * 4);
        if (*out) memcpy(*out, rg, (size_t)*w * *h * 4); else ok = 0;
        if (info) vl2d_info(L, info);
    }
    vl2d_free(L);
    return ok;
}
static void layer_stats(const char *name, const uint8_t *rg, int w, int h) {
    long op = 0, semi = 0, tr = 0;
    for (long i = 0; i < (long)w * h; i++) { int a = rg[i * 4 + 3]; if (a == 255) op++; else if (a) semi++; else tr++; }
    printf("  %-22s %dx%d: opacos %ld (%.1f%%), semitransparentes %ld, transparentes %ld\n", name, w, h, op, 100.0 * op / ((double)w * h), semi, tr);
}

/* ---- comprobación de la composición del panel: capa RGBA (no premultiplicada) sobre el MUNDO SIN 2D, frente a frame.png (la verdad del hardware) ----
 * El mundo sin 2D es el fotograma completo (vl2d_run full=1) de la lista sin las primitivas que van al panel (op_dest == 2): exactamente lo que hay debajo del panel. La fórmula es la del panel del visor (XRShaders.metal
 * xr_panel_color + mezcla one / oneMinusSrcAlpha): color premultiplicado rgb * a sobre el fondo; `linear` = en espacio lineal (destino sRGB del visor: la capa y el mundo se decodifican, se mezclan y se codifican) o en gamma (como la
 * aritmética de la PS1). */
static double s2l(double c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
static double l2s(double c) { return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055; }
static void compose_panel(const uint8_t *lay, const uint8_t *world, int w, int h, int linear, uint8_t *out) {
    for (long i = 0; i < (long)w * h; i++) {
        double a = lay[i * 4 + 3] / 255.0;
        for (int k = 0; k < 3; k++) {
            double c = lay[i * 4 + k] / 255.0, b = world[i * 4 + k] / 255.0, v;
            if (linear) v = l2s(s2l(c) * a + (1.0 - a) * s2l(b)); else v = c * a + (1.0 - a) * b;
            if (lay[i * 4 + 3] < 5) v = b;   /* alfa < 0,02 -> discard_fragment */
            out[i * 4 + k] = (uint8_t)(v <= 0 ? 0 : v >= 1 ? 255 : (int)(v * 255.0 + 0.5));
        }
        out[i * 4 + 3] = 255;
    }
}
/* diferencia con la verdad: media por canal (0..255) y % de píxeles distintos > 8/255 en un conjunto (sel: 0 todos, 1 con alfa en 1..254 = semitransparentes, 2 con alfa > 0) */
static int g_zone[4] = {0, 0, 1 << 20, 1 << 20};   /* x0,y0,x1,y1 inclusivo: L2D_ZONA=x0,y0,x1,y1 limita las estadísticas de composición (p. ej. los créditos) */
static void diff_stats(const char *name, const uint8_t *lay, const uint8_t *img, const uint8_t *ref, int w, int h, int sel, int rw) {
    long n = 0, bad = 0, sum = 0;
    for (int y = g_zone[1]; y < h && y <= g_zone[3]; y++) for (int x = g_zone[0]; x < w && x <= g_zone[2]; x++) {
        int a = lay[((size_t)y * w + x) * 4 + 3];
        if ((sel == 1 && (a == 0 || a == 255)) || (sel == 2 && a == 0)) continue;
        const uint8_t *p = &img[((size_t)y * w + x) * 4], *q = &ref[((size_t)y * rw + x) * 4];
        int d = 0, m = 0; for (int k = 0; k < 3; k++) { int e = abs(p[k] - q[k]); d += e; if (e > m) m = e; }
        n++; sum += d; if (m > 8) bad++;
    }
    printf("    %-34s px %ld: dif media %.2f / 255, distintos(>8) %ld (%.1f%%)\n", name, n, n ? sum / (3.0 * n) : 0.0, bad, n ? 100.0 * bad / n : 0.0);
}
/* mundo sin 2D (lo que hay bajo el panel). NULL si falla. */
static uint8_t *world_without_2d(const GpuDrawList *l, const VrScene *sc, int *w, int *h) {
    GpuOp *ops = (GpuOp *)malloc((size_t)(l->nops + 1) * sizeof *ops);
    if (!ops) return NULL;
    uint32_t n = 0;
    for (uint32_t i = 0; i < l->nops; i++) {
        int prim = l->ops[i].type == GPU_OP_POLY || l->ops[i].type == GPU_OP_RECT || l->ops[i].type == GPU_OP_LINE;
        if (prim && sc->op_dest && i < sc->n_op_dest && sc->op_dest[i] == 2) continue;
        ops[n++] = l->ops[i];
    }
    GpuDrawList l2 = *l; l2.ops = ops; l2.nops = n; l2.cap_ops = 0;
    uint8_t *img = NULL;
    VrLayer2D *L = vl2d_new();
    vl2d_set_options(L, 1, VL_SMOOTH_ORIGINAL);
    if (vl2d_run(L, &l2, NULL, VL_FULL)) {
        const uint8_t *rg = vl2d_rgba(L, w, h);
        img = (uint8_t *)malloc((size_t)*w * *h * 4);
        if (img) memcpy(img, rg, (size_t)*w * *h * 4);
    }
    vl2d_free(L); free(ops);
    return img;
}

/* Un volcado. rc: 0 bien, 1 diferencias, 2 sin capa. */
static int one_dump(const char *dir, double maxpct, const char *png_dir) {
    NsDump *d = ns_dump_open(dir);
    if (!d) { fprintf(stderr, "%s: no se pudo abrir\n", dir); return 2; }
    if (!d->has_list || !d->list.nops || !d->vram) {
        char pb[48]; const char *pt = ns_dump_meta(d, "point", pb, sizeof pb);
        printf("== %s\n  SIN list.bin/vram.bin (point=%s: solo RAM/VRAM/GTE): no hay capa 2D reproducible, la capa sale de las primitivas de la lista de dibujo; no se aproxima.\n", dir, pt ? pt : "?");
        ns_dump_close(d);
        return 2;
    }
    char path[600]; snprintf(path, sizeof path, "%s/frame.png", dir);
    int rw = 0, rh = 0;
    uint8_t *ref = png_read_stored(path, &rw, &rh);
    if (!ref) { fprintf(stderr, "%s: frame.png ilegible (se espera PNG RGBA sin compresión)\n", dir); ns_dump_close(d); return 2; }
    GpuDrawList l = d->list; l.vram_start = d->vram; l.vram_end = NULL;   /* sin vram_end: se mide el rasterizado, no el respaldo */
    VrScene sc;
    if (build_scene(d, &l, &sc) != 0) { fprintf(stderr, "%s: vr_scene_build falló\n", dir); free(ref); ns_dump_close(d); return 2; }
    printf("== %s\nlista: %u ops, zona mostrada (%d,%d) %dx%d marco %d aw=%d pad_l=%d; frame.png %dx%d; vista %s\n", dir, l.nops, l.disp.x, l.disp.y, l.disp.w, l.disp.h, l.disp.fw, l.disp.aw, l.disp.pad_l, rw, rh,
           (int)ns_dump_meta_num(d, "view_mode", 1) ? "mundo" : "cine");
    int rc = 0;
    VrL2Info in;
    int w = 0, h = 0;
    uint8_t *full = NULL, *panel = NULL, *ver = NULL;
    int fw_ = 0, fh_ = 0, pw = 0, ph = 0, vw = 0, vh = 0;
    {   /* pantalla completa (vl2d_run full=1): frente a frame.png */
        VrLayer2D *L = vl2d_new();
        vl2d_set_options(L, 1, VL_SMOOTH_ORIGINAL);
        if (vl2d_run(L, &l, &sc, 1)) {
            const uint8_t *rg = vl2d_rgba(L, &w, &h);
            VrL2Diff df;
            vl2d_info(L, &in);
            if (vl2d_compare(rg, w, h, 1, ref, rw, rh, &df)) {
                printf("completa  (escala 1): dif media %.4f / 255, píxeles distintos %.3f%% (%ld de %ld), max %d | primitivas %d, solo VRAM %d\n", df.mean, df.pct, df.bad, df.n, df.max, in.prims, in.mirror_prims);
                if (df.pct > maxpct) rc = 1;
            } else printf("completa: dimensiones de la capa %dx%d no encajan con frame.png %dx%d\n", w, h, rw, rh), rc = 1;
            full = (uint8_t *)malloc((size_t)w * h * 4);
            if (full) { memcpy(full, rg, (size_t)w * h * 4); fw_ = w; fh_ = h; }
        }
        vl2d_free(L);
    }
    if (layer_of(&l, &sc, VL_PANEL, &panel, &pw, &ph, &in)) {   /* panel (full=0, lo que se pinta sobre el mundo): píxeles OPACOS de la capa frente a frame.png (los semitransparentes dependen del mundo de debajo) */
        long op = 0, semi = 0, bad = 0, sum = 0;
        for (int y = 0; y < ph && y < rh; y++) for (int x = 0; x < pw && x < rw; x++) {
            const uint8_t *p = &panel[((size_t)y * pw + x) * 4], *q = &ref[((size_t)y * rw + x) * 4];
            if (p[3] == 255) { int e = abs(p[0] - q[0]) + abs(p[1] - q[1]) + abs(p[2] - q[2]); op++; if (e) bad++; sum += e; }
            else if (p[3]) semi++;
        }
        printf("panel     (escala 1): %ld píxeles opacos (dif media %.4f / 255, distintos %ld = %.3f%%), %ld semitransparentes | primitivas al panel %d, solo VRAM %d\n", op, op ? sum / (3.0 * op) : 0.0, bad, op ? 100.0 * bad / op : 0.0, semi, in.prims, in.mirror_prims);
        if (bad) rc = 1;
    }
    {   /* panel verificado (pantalla 3D del motor + panel): se compone sobre el mundo reproducido y se compara con la VRAM del volcado (= la imagen del hardware) */
        GpuDrawList lv = l; lv.vram_end = d->vram;
        VrLayer2D *L2 = vl2d_new();
        vl2d_set_options(L2, 1, VL_SMOOTH_ORIGINAL);
        if (vl2d_run(L2, &lv, &sc, VL_PANEL_VERIFIED)) {
            vl2d_info(L2, &in);
            printf("panel verificado: %s | píxeles del panel %d, distintos del hardware %d (dif media %.3f), sustituidos %d, mundo distinto fuera del panel %d, 2D perdidas %d, respaldo %s\n",
                   in.verify ? "aplicado" : "no aplicable", in.v_cov, in.v_bad, in.v_mean_milli / 1000.0, in.v_patched, in.v_out, in.v_lost2d, in.fallback ? "SI" : "no");
            const uint8_t *rg = vl2d_rgba(L2, &vw, &vh);
            ver = (uint8_t *)malloc((size_t)vw * vh * 4);
            if (ver) memcpy(ver, rg, (size_t)vw * vh * 4); else vw = vh = 0;
        }
        vl2d_free(L2);
    }
    if (png_dir) {
        const char *nm = base_name(dir);
        const int kind = (int)ns_dump_meta_num(d, "view_mode", VRV_K_WORLD);   /* VrView.kind */
        const int mode = kind == VRV_K_UI || kind == VRV_K_VIDEO ? VL_FULL : kind == VRV_K_ENGINE ? VL_PANEL_VERIFIED : VL_PANEL;   /* vrhost.c l2_full */
        const char *mname = mode == VL_FULL ? "VL_FULL (cine: UI / vídeo, fotograma entero opaco)" : mode == VL_PANEL_VERIFIED ? "VL_PANEL_VERIFIED (cinemática del motor: 2D encima de la pantalla 3D)" : "VL_PANEL (mundo)";
        const uint8_t *lay = mode == VL_FULL ? full : mode == VL_PANEL_VERIFIED ? ver : panel;
        const int lw = mode == VL_FULL ? fw_ : mode == VL_PANEL_VERIFIED ? vw : pw, lh = mode == VL_FULL ? fh_ : mode == VL_PANEL_VERIFIED ? vh : ph;
        printf("  view_mode(kind)=%d -> la capa que llega al panel es %s\n", kind, mname);
        if (lay) layer_stats("capa del panel", lay, lw, lh);
        if (mode == VL_PANEL_VERIFIED && panel) layer_stats("(sin verificar VL_PANEL)", panel, pw, ph);
        if (lay) {
            uint8_t *im = (uint8_t *)malloc((size_t)lw * lh * 4);
            if (im) {
                on_checker(lay, lw, lh, 1, im, lw);
                char o[800]; snprintf(o, sizeof o, "%s/%s_capa2d.png", png_dir, nm);
                printf("  PNG %s %s\n", o, ns_png_write_rgba(o, im, lw, lh) == 0 ? "(capa 2D completa del panel, 1x, sobre gris a cuadros)" : "ERROR");
                free(im);
            }
            /* hoja x2: frame.png original | capa del panel */
            const int K = 2;
            int cw = (rw > lw ? rw : lw) * K, chh = (rh > lh ? rh : lh) * K;
            int SW = cw * 2;
            uint8_t *sheet = (uint8_t *)malloc((size_t)SW * chh * 4), *tmp = (uint8_t *)malloc((size_t)cw * chh * 4);
            if (sheet && tmp) {
                for (size_t i = 0; i < (size_t)SW * chh; i++) { sheet[i * 4] = sheet[i * 4 + 1] = sheet[i * 4 + 2] = 0x30; sheet[i * 4 + 3] = 255; }
                on_checker(ref, rw, rh, K, tmp, cw);   /* frame.png es opaco: se ve tal cual */
                for (int y = 0; y < rh * K; y++) memcpy(&sheet[((size_t)y * SW) * 4], &tmp[(size_t)y * cw * 4], (size_t)rw * K * 4);
                on_checker(lay, lw, lh, K, tmp, cw);
                for (int y = 0; y < lh * K; y++) memcpy(&sheet[((size_t)y * SW + cw) * 4], &tmp[(size_t)y * cw * 4], (size_t)lw * K * 4);
                char o[800]; snprintf(o, sizeof o, "%s/%s_hoja.png", png_dir, nm);
                printf("  PNG %s %s\n", o, ns_png_write_rgba(o, sheet, SW, chh) == 0 ? "(izq: frame.png original | der: capa del panel, x2)" : "ERROR");
            }
            free(sheet); free(tmp);
            if (mode == VL_PANEL) {   /* composición del panel sobre el mundo SIN 2D frente a frame.png (la verdad): brillo de lo semitransparente / aditivo */
                int bw = 0, bh = 0;
                uint8_t *wd = world_without_2d(&l, &sc, &bw, &bh);
                if (wd && bw == lw && bh == lh && bw <= rw && bh <= rh) {
                    diff_stats("mundo sin 2D vs frame (alfa 0)", lay, wd, ref, bw, bh, 0, rw);   /* validez del fondo: fuera del 2D debe ser igual a frame.png */
                    uint8_t *cg = (uint8_t *)malloc((size_t)bw * bh * 4), *cl = (uint8_t *)malloc((size_t)bw * bh * 4);
                    if (cg && cl) {
                        printf("  composición del panel (capa sobre el mundo sin 2D) frente a frame.png:\n");
                        compose_panel(lay, wd, bw, bh, 0, cg); compose_panel(lay, wd, bw, bh, 1, cl);
                        diff_stats("gamma (PS1), px con alfa > 0", lay, cg, ref, bw, bh, 2, rw);
                        diff_stats("gamma (PS1), solo semitransp.", lay, cg, ref, bw, bh, 1, rw);
                        diff_stats("lineal (visor sRGB), px alfa > 0", lay, cl, ref, bw, bh, 2, rw);
                        diff_stats("lineal (visor sRGB), semitransp.", lay, cl, ref, bw, bh, 1, rw);
                        /* PNG: [frame.png | gamma | lineal | |gamma - frame| x4] x2 */
                        const int K = 2; int cw = bw * K, SW = cw * 4, chh = bh * K;
                        uint8_t *sh = (uint8_t *)malloc((size_t)SW * chh * 4);
                        if (sh) {
                            for (int y = 0; y < chh; y++) for (int x = 0; x < SW; x++) {
                                int col = x / cw, sx = (x % cw) / K, sy = y / K; uint8_t *q = &sh[((size_t)y * SW + x) * 4];
                                const uint8_t *r = &ref[((size_t)sy * rw + sx) * 4], *g = &cg[((size_t)sy * bw + sx) * 4], *li = &cl[((size_t)sy * bw + sx) * 4];
                                for (int k = 0; k < 3; k++) { int v = col == 0 ? r[k] : col == 1 ? g[k] : col == 2 ? li[k] : abs(g[k] - r[k]) * 4; q[k] = (uint8_t)(v > 255 ? 255 : v); }
                                q[3] = 255;
                            }
                            char o[800]; snprintf(o, sizeof o, "%s/%s_compuesta.png", png_dir, nm);
                            printf("  PNG %s %s\n", o, ns_png_write_rgba(o, sh, SW, chh) == 0 ? "(x2: frame.png | capa sobre mundo sin 2D en gamma | idem en lineal | |gamma - frame| x4)" : "ERROR");
                            free(sh);
                        }
                    }
                    const char *zs = getenv("L2D_ZONA");
                    if (zs && cg && cl && sscanf(zs, "%d,%d,%d,%d", &g_zone[0], &g_zone[1], &g_zone[2], &g_zone[3]) == 4) {
                        printf("  zona L2D_ZONA=%s:\n", zs);
                        diff_stats("gamma (PS1), px con alfa > 0", lay, cg, ref, bw, bh, 2, rw);
                        diff_stats("lineal (visor sRGB), px alfa > 0", lay, cl, ref, bw, bh, 2, rw);
                        long n = 0, sr = 0, sg = 0, sl = 0;   /* brillo medio (canal G) de la zona: frame.png / gamma / lineal, solo px con alfa > 0 */
                        for (int y = g_zone[1]; y < bh && y <= g_zone[3]; y++) for (int x = g_zone[0]; x < bw && x <= g_zone[2]; x++) {
                            size_t i = (size_t)y * bw + x;
                            if (lay[i * 4 + 3] == 0) continue;
                            n++; sr += ref[((size_t)y * rw + x) * 4 + 1]; sg += cg[i * 4 + 1]; sl += cl[i * 4 + 1];
                        }
                        if (n) printf("    brillo medio G de la zona (px alfa>0, %ld): frame.png %.1f | gamma %.1f | lineal %.1f\n", n, (double)sr / n, (double)sg / n, (double)sl / n);
                        g_zone[0] = g_zone[1] = 0; g_zone[2] = g_zone[3] = 1 << 20;
                    }
                    free(cg); free(cl);
                } else printf("  (composición no comprobable: mundo sin 2D %s)\n", wd ? "de otro tamaño" : "no reproducible");
                free(wd);
            }
            if (mode == VL_PANEL_VERIFIED && panel) {   /* respaldo informativo: solo las primitivas 2D (sin la comparación con el mundo reproducido) */
                uint8_t *im2 = (uint8_t *)malloc((size_t)pw * ph * 4);
                if (im2) { on_checker(panel, pw, ph, 1, im2, pw); char o[800]; snprintf(o, sizeof o, "%s/%s_capa2d_sin_verificar.png", png_dir, nm); printf("  PNG %s %s\n", o, ns_png_write_rgba(o, im2, pw, ph) == 0 ? "(informativo: VL_PANEL sin verificar)" : "ERROR"); free(im2); }
            }
        }
    }
    free(full); free(panel); free(ver);
    free(ref); ns_dump_close(d);
    return rc;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "uso: l2d_dump <carpeta del volcado> [<otra> ...] [--max-pct P] [--png <carpeta de salida>]\n"); return 2; }
    double maxpct = 0;
    const char *png_dir = NULL;
    const char *dirs[64]; int nd = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--max-pct") && i + 1 < argc) maxpct = atof(argv[++i]);
        else if (!strcmp(argv[i], "--png") && i + 1 < argc) png_dir = argv[++i];
        else if (nd < 64) dirs[nd++] = argv[i];
    }
    if (!nd) { fprintf(stderr, "falta la carpeta del volcado\n"); return 2; }
    int rc = 0;
    for (int i = 0; i < nd; i++) {
        int r = one_dump(dirs[i], maxpct, png_dir);
        if (nd == 1) rc = r;               /* un solo volcado: la salida de siempre (2 = sin lista) */
        else if (r == 1) rc = 1;           /* varios: las diferencias cuentan; un volcado sin lista (stage) se avisa y se salta */
    }
    printf(rc ? "RESULTADO: DIFERENCIAS\n" : "RESULTADO: OK\n");
    return rc;
}
