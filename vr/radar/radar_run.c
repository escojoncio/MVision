/* Validación del radar de muñeca en el runtime real (MV_NOGEN, sin vr/host): ejecuta el juego con el guion de mando, captura las
 * listas de la GPU, separa el radar del HUD y escribe imágenes.
 *   radar_run <exe> <disco> <frames> <carpeta_salida> [--preset original|vr|exigente] [--max N]
 * Salidas (PPM; se convierten a PNG con radar_png.py):
 *   radar_<f>_tex.ppm       textura del radar (ventana recortada, 4x), solo conos del juego (sin ampliar)
 *   radar_<f>_ext.ppm       igual con los conos ampliados según el preset
 *   hud_<f>_full.ppm        HUD/pantalla completa tal cual (2x)
 *   hud_<f>_norad.ppm       lo mismo sin las primitivas del radar
 * Comprobaciones (código de salida != 0 si fallan): las primitivas marcadas son EXACTAMENTE las del entorno de dibujo del radar. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "bridge.h"
#include "cpu.h"
#include "gpu.h"
#include "hlebind.h"
#include "host.h"
#include "host_hooks.h"
#include "ref_raster.h"
#include "vr_perception.h"
#include "vr_radar.h"
#include "vr_radar_vrp.h"
#include "vr_scene.h"

void hle_vrradar_reset(void);
void vrradar_hle_stats(unsigned *calls, unsigned *marked, unsigned *fail);

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "FALLO %d: ", __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); fails++; } } while (0)

static const char *outdir = ".";
static VrpPreset preset = VRP_VR;
static int max_dumps = 12;
static int force_mode = -1;
static uint32_t force_from, force_to;

/* ---------------------------------------------------------------------------------------------- rasterizador 2D de la escena (referencia) */
typedef struct { int w, h; float s; float *px; } Img;

static Img img_new(int w, int h, float s, const float bg[3]) {
    Img im = {w, h, s, (float *)malloc((size_t)w * h * 3 * sizeof(float))};
    for (int i = 0; i < w * h; i++) { im.px[i * 3] = bg[0]; im.px[i * 3 + 1] = bg[1]; im.px[i * 3 + 2] = bg[2]; }
    return im;
}
static void img_write(const Img *im, const char *name) {
    char p[512];
    snprintf(p, sizeof p, "%s/%s", outdir, name);
    FILE *f = fopen(p, "wb");
    if (!f) { perror(p); return; }
    fprintf(f, "P6\n%d %d\n255\n", im->w, im->h);
    for (int i = 0; i < im->w * im->h * 3; i++) {
        float v = im->px[i];
        fputc((int)(v < 0 ? 0 : (v > 1 ? 255 : v * 255.0f + 0.5f)), f);
    }
    fclose(f);
}
static float expand5(unsigned v) { return (float)((v << 3) | (v >> 2)) / 255.0f; }

static int texel(const VrVtx *v, const VrState *st, int u, int w, unsigned *out) {
    int tx = (v->tpage & 15) * 64, ty = ((v->tpage >> 4) & 1) * 256, tp = (v->tpage >> 7) & 3;
    int clx = (v->clut & 63) * 16, cly = (v->clut >> 6) & 511;
    u &= 255; w &= 255;
    if (st && (st->twmx || st->twmy)) {
        u = (u & ~(st->twmx * 8)) | ((st->twox & st->twmx) * 8);
        w = (w & ~(st->twmy * 8)) | ((st->twoy & st->twmy) * 8);
    }
    int y = (ty + w) & 511;
    unsigned t;
    if (tp == 0) { unsigned x = gpu_vram[y][(tx + (u >> 2)) & 1023]; t = gpu_vram[cly][(clx + ((x >> ((u & 3) * 4)) & 15)) & 1023]; }
    else if (tp == 1) { unsigned x = gpu_vram[y][(tx + (u >> 1)) & 1023]; t = gpu_vram[cly][(clx + ((x >> ((u & 1) * 8)) & 255)) & 1023]; }
    else t = gpu_vram[y][(tx + u) & 1023];
    *out = t;
    return t != 0;
}

static void blend(float *d, const float f[3], int mode) {
    for (int k = 0; k < 3; k++) {
        float B = d[k], F = f[k], r;
        switch (mode) {
        case 0: r = 0.5f * B + 0.5f * F; break;
        case 1: r = B + F; break;
        case 2: r = B - F; break;
        default: r = B + 0.25f * F; break;
        }
        d[k] = r < 0 ? 0 : (r > 1 ? 1 : r);
    }
}

/* Triángulo con regla de relleno única (sin doble dibujo en la diagonal), color Gouraud, textura nearest, mezcla original. */
static void tri(Img *im, float ox, float oy, const VrVtx *a, const VrVtx *b, const VrVtx *c, int bl, const VrState *states, uint32_t ns) {
    const VrVtx *V[3] = {a, b, c};
    float X[3], Y[3];
    for (int i = 0; i < 3; i++) { X[i] = (V[i]->pos[0] - ox) * im->s; Y[i] = (V[i]->pos[1] - oy) * im->s; }
    float area = (X[1] - X[0]) * (Y[2] - Y[0]) - (Y[1] - Y[0]) * (X[2] - X[0]);
    if (area == 0) return;
    if (area < 0) { const VrVtx *t = V[1]; V[1] = V[2]; V[2] = t; float tx = X[1], ty = Y[1]; X[1] = X[2]; Y[1] = Y[2]; X[2] = tx; Y[2] = ty; area = -area; }
    int x0 = (int)floorf(fminf(X[0], fminf(X[1], X[2]))), x1 = (int)ceilf(fmaxf(X[0], fmaxf(X[1], X[2])));
    int y0 = (int)floorf(fminf(Y[0], fminf(Y[1], Y[2]))), y1 = (int)ceilf(fmaxf(Y[0], fmaxf(Y[1], Y[2])));
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > im->w) x1 = im->w;
    if (y1 > im->h) y1 = im->h;
    unsigned flags = V[0]->flags;
    int textured = (flags & GPU_F_TEXTURED) != 0, raw = (flags & GPU_F_RAW) != 0, rect = (flags & VR_F_RECT) != 0;
    const VrState *st = (V[0]->pad != 0xFFFF && V[0]->pad < ns) ? &states[V[0]->pad] : NULL;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            float px = x + 0.5f, py = y + 0.5f, w[3];
            int in = 1;
            for (int i = 0; i < 3; i++) {
                int j = (i + 1) % 3;
                float dx = X[j] - X[i], dy = Y[j] - Y[i];
                float e = dx * (py - Y[i]) - dy * (px - X[i]);
                w[(i + 2) % 3] = e;
                if (e < 0 || (e == 0 && !(dy > 0 || (dy == 0 && dx > 0)))) { in = 0; break; }
            }
            if (!in) continue;
            float l0 = w[0] / area, l1 = w[1] / area, l2 = w[2] / area;
            float col[3];
            for (int k = 0; k < 3; k++) {
                float cc[3];
                for (int i = 0; i < 3; i++) cc[i] = (float)((V[i]->rgba >> (8 * k)) & 255);
                col[k] = l0 * cc[0] + l1 * cc[1] + l2 * cc[2];
            }
            float rgb[3];
            int mode = bl;
            if (textured) {
                float u = l0 * V[0]->uv[0] + l1 * V[1]->uv[0] + l2 * V[2]->uv[0];
                float v = l0 * V[0]->uv[1] + l1 * V[1]->uv[1] + l2 * V[2]->uv[1];
                if (rect) { if (flags & GPU_F_FLIPX) u += 1.0f; if (flags & GPU_F_FLIPY) v += 1.0f; }
                unsigned t;
                if (!texel(V[0], st, (int)floorf(u), (int)floorf(v), &t)) continue;
                float tex[3] = {expand5(t & 31), expand5((t >> 5) & 31), expand5((t >> 10) & 31)};
                for (int k = 0; k < 3; k++) { rgb[k] = raw ? tex[k] : tex[k] * col[k] / 128.0f; if (rgb[k] > 1) rgb[k] = 1; }
                if (!(t & 0x8000)) mode = 0xFF;                /* texel sin bit 15: opaco */
            } else {
                for (int k = 0; k < 3; k++) { rgb[k] = col[k] / 255.0f; if (rgb[k] > 1) rgb[k] = 1; }
                if (!(flags & GPU_F_SEMI)) mode = 0xFF;
            }
            float *d = &im->px[((size_t)y * im->w + x) * 3];
            if (mode == 0xFF) { d[0] = rgb[0]; d[1] = rgb[1]; d[2] = rgb[2]; }
            else blend(d, rgb, mode & 3);
        }
}

static void raster_scene(Img *im, const VrVtx *v, const VrBatch *b, uint32_t nb, const VrState *st, uint32_t ns, int ox, int oy) {
    for (uint32_t i = 0; i < nb; i++) {
        if (b[i].kind != 2) continue;
        for (uint32_t k = 0; k + 2 < b[i].count; k += 3) {
            const VrVtx *a = &v[b[i].first + k];
            tri(im, (float)ox, (float)oy, a, a + 1, a + 2, b[i].blend, st, ns);
        }
    }
}

/* ---------------------------------------------------------------------------------------------- estado */
static BridgeState bs;
static int cap_on;
static RefR *r_full, *r_hud;
static uint32_t n_lists, n_present, n_cone_lists, n_ext_lists, n_state_lists, n_dumps;
static uint32_t d_nocone, d_cone, d_state;
static uint32_t ops_min = 0xFFFFFFFFu, ops_max;
static uint32_t n_frames_rect_changed;
static int last_rect[4];

static void save_ref(RefR *r, const GpuDrawList *l, const char *name) {
    int w, h;
    const uint8_t *p = ref_present(r, &l->disp, &w, &h);
    if (!p) return;
    char path[512];
    snprintf(path, sizeof path, "%s/%s", outdir, name);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    fwrite(p, 3, (size_t)w * h, f);
    fclose(f);
}

static int valid_op_for_checks(const GpuOp *o) { return o->type == GPU_OP_POLY || o->type == GPU_OP_RECT || o->type == GPU_OP_LINE; }

static void check_split(const GpuDrawList *l, const VrRadarSplit *s) {
    /* (1) todo lo marcado cae dentro de la ventana del radar; (2) nada fuera de las marcas usa el entorno de dibujo del radar */
    uint32_t nmark = 0, frame_rects = 0;
    for (uint32_t i = 0; i < l->nops; i++) {
        const GpuOp *o = &l->ops[i];
        if (!valid_op_for_checks(o)) continue;
        int mk = vrradar_seq_is_radar(o->seq);
        int same_env = o->dox == s->center[0] && o->doy == s->center[1];
        if (mk) {
            nmark++;
            if (!same_env) {   /* solo el marco negro usa el entorno base: rect opaco negro sobre el borde de la ventana */
                int ok = o->type == GPU_OP_RECT && o->v[0].r == 0 && o->v[0].g == 0 && o->v[0].b == 0 && !(o->flags & GPU_F_TEXTURED);
                int x = o->v[0].x, y = o->v[0].y;
                ok = ok && x >= s->rect[0] && y >= s->rect[1] && x + o->rw <= s->rect[0] + s->rect[2] && y + o->rh <= s->rect[1] + s->rect[3];
                CHECK(ok, "lista %u op %u marcada fuera del entorno del radar (tipo %d en %d,%d env %d,%d)", l->index, i, o->type, x, y, o->dox, o->doy);
                frame_rects++;
            }
        } else {
            CHECK(!same_env, "lista %u op %u usa el entorno del radar (%d,%d) y NO está marcada (tipo %d)", l->index, i, o->dox, o->doy, o->type);
        }
    }
    CHECK(nmark == s->n_ops, "n_ops %u != marcadas %u", s->n_ops, nmark);
    CHECK(frame_rects == 4, "lista %u: marco con %u rects (esperados 4)", l->index, frame_rects);
    CHECK(s->hud->nops + nmark == l->nops, "hud %u + radar %u != total %u", s->hud->nops, nmark, l->nops);
    for (uint32_t i = 0; i < s->hud->nops; i++) CHECK(!(valid_op_for_checks(&s->hud->ops[i]) && vrradar_seq_is_radar(s->hud->ops[i].seq)), "hud contiene op del radar");
}

static void process(const GpuDrawList *l, uint32_t f) {
    n_lists++;
    VrRadarSplit s;
    int rc = vrradar_split(l, &s);
    CHECK(rc == 0, "split rc=%d", rc);
    int want_dump = 0;
    VrRadarScene rs0, rs1;
    memset(&rs0, 0, sizeof rs0); memset(&rs1, 0, sizeof rs1);
    if (s.present) {
        n_present++;
        if (s.n_ops < ops_min) ops_min = s.n_ops;
        if (s.n_ops > ops_max) ops_max = s.n_ops;
        if (memcmp(last_rect, s.rect, sizeof last_rect)) { n_frames_rect_changed++; memcpy(last_rect, s.rect, sizeof last_rect); }
        check_split(l, &s);
        /* escena SIN ampliar y con la percepción activa */
        VrRadarPerc keep = vrradar_get_perception(), off = {0, 1.0f, 1.0f, 0.5f};
        vrradar_set_perception(&off);
        static VrVtx *copy_v; static VrBatch *copy_b; static VrState *copy_s;
        rc = vrradar_build(&s, &rs0);
        CHECK(rc == 0, "build0 rc=%d", rc);
        free(copy_v); free(copy_b); free(copy_s);
        copy_v = malloc((rs0.nv + 1) * sizeof *copy_v); copy_b = malloc((rs0.nb + 1) * sizeof *copy_b); copy_s = malloc((rs0.nstates + 1) * sizeof *copy_s);
        memcpy(copy_v, rs0.v, rs0.nv * sizeof *copy_v); memcpy(copy_b, rs0.b, rs0.nb * sizeof *copy_b); memcpy(copy_s, rs0.states, rs0.nstates * sizeof *copy_s);
        rs0.v = copy_v; rs0.b = copy_b; rs0.states = copy_s;
        vrradar_set_perception(&keep);
        rc = vrradar_build(&s, &rs1);
        CHECK(rc == 0, "build1 rc=%d", rc);
        CHECK(rs0.n_ext_tris == 0, "con percepción apagada hay triángulos extra");
        CHECK(rs1.nv == rs0.nv + rs1.n_ext_tris * 3, "nv ext inconsistente");
        if (rs1.n_cones) n_cone_lists++;
        if (rs1.n_cones_ext) n_ext_lists++;
        if (rs1.mode) n_state_lists++;
        if (rs1.mode == 1) CHECK(rs1.n_cones == 0, "estado de alerta/evasión con conos de soldado");
        /* batches: orden y rangos válidos, ev_end en índices de la lista HUD */
        uint32_t prev = 0;
        for (uint32_t i = 0; i < rs1.nb; i++) {
            CHECK(rs1.b[i].ev_end >= prev && rs1.b[i].ev_end <= s.hud->nops, "ev_end %u fuera de rango (hud %u)", rs1.b[i].ev_end, s.hud->nops);
            CHECK((uint64_t)rs1.b[i].first + rs1.b[i].count <= rs1.nv, "batch fuera de rango");
            prev = rs1.b[i].ev_end;
        }
        if (n_dumps < (uint32_t)max_dumps) {
            if (rs1.mode == 1 && d_state < 2) { want_dump = 1; d_state++; }
            else if (rs1.n_cones && d_cone < 4 && f % 3 == 0) { want_dump = 1; d_cone++; }
            else if (!rs1.n_cones && !rs1.mode && d_nocone < 2) { want_dump = 1; d_nocone++; }
        }
        if (want_dump) {
            n_dumps++;
            float bg[3] = {0, 0, 0};
            const float S = 4.0f;
            Img a = img_new((int)(s.rect[2] * S), (int)(s.rect[3] * S), S, bg);
            raster_scene(&a, rs0.v, rs0.b, rs0.nb, rs0.states, rs0.nstates, s.rect[0], s.rect[1]);
            char nm[128]; snprintf(nm, sizeof nm, "radar_%05u_tex.ppm", f); img_write(&a, nm);
            Img b2 = img_new((int)(s.rect[2] * S), (int)(s.rect[3] * S), S, bg);
            raster_scene(&b2, rs1.v, rs1.b, rs1.nb, rs1.states, rs1.nstates, s.rect[0], s.rect[1]);
            snprintf(nm, sizeof nm, "radar_%05u_ext.ppm", f); img_write(&b2, nm);
            fprintf(stderr, "[radar] f=%u rect=(%d,%d,%d,%d) ops=%u conos=%u ampliados=%u tris_ext=%u modo=%u\n", f, s.rect[0], s.rect[1], s.rect[2], s.rect[3],
                    s.n_ops, rs1.n_cones, rs1.n_cones_ext, rs1.n_ext_tris, rs1.mode);
            free(a.px); free(b2.px);
        }
    }
    /* referencias de pantalla completa: se aplican TODAS las listas, en orden, a ambos modelos */
    ref_apply(r_full, l, want_dump);
    ref_apply(r_hud, s.hud, want_dump);
    if (want_dump) {
        char nm[128];
        snprintf(nm, sizeof nm, "hud_%05u_full.ppm", f); save_ref(r_full, l, nm);
        snprintf(nm, sizeof nm, "hud_%05u_norad.ppm", f); save_ref(r_hud, s.hud, nm);
    }
}

static void frame_hook(Cpu *c, uint32_t f) {
    (void)c;
    if (force_mode >= 0 && f >= force_from && f < force_to) {   /* prueba: fuerza el modo de alerta global (0 normal, 1 interferencia, 2 evasión, 3 alerta) */
        uint32_t m = (uint32_t)force_mode, lv = 120;
        memcpy(emu_ram + (BA_ALERT_MODE & 0x1FFFFF), &m, 4);
        memcpy(emu_ram + (BA_ALERT_LEVEL & 0x1FFFFF), &lv, 4);
    }
    bridge_read(emu_ram, &bs);
    if (!cap_on) {
        gpu_capture_hiprec(1);
        gpu_capture_enable(1);
        cap_on = 1;
        return;
    }
    gpu_capture_vblank();
    const GpuDrawList *l;
    while ((l = gpu_capture_frame())) process(l, f);
}

static void on_exe_loaded(uint8_t *ram, uint32_t base, uint32_t size) {
    (void)ram; (void)base; (void)size;
    hle_vrradar_reset();
    vrradar_reset();
    hle_bind_declare_fixed("vrradar_draw");
}
static void on_bound(void) {
    if (hle_bind_fixed("vrradar_draw", 0x8003bd9cu) < 0) { fprintf(stderr, "no se pudo enlazar vrradar_draw\n"); fails++; }
}
static void on_start(void) {
    gpu_capture_set_mark_hook(vrradar_on_mark);
    cap_on = 0;
}

typedef struct { uint32_t start, dur, every; uint16_t mask; } PadEv;
static const PadEv PAD[] = {{120, 6, 0, 1u << 3}, {400, 6, 0, 1u << 3}, {0, 6, 97, 1u << 13}, {0, 6, 211, 1u << 7}};
static uint16_t pad_at(uint32_t f) {
    uint16_t b = 0;
    for (unsigned i = 0; i < sizeof PAD / sizeof PAD[0]; i++) {
        const PadEv *p = &PAD[i];
        if (p->every) { if (f >= p->every && f % p->every < p->dur) b |= p->mask; }
        else if (f >= p->start && f < p->start + p->dur) b |= p->mask;
    }
    return b;
}

int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "uso: %s <exe> <disco> <frames> <salida> [--preset original|vr|exigente] [--max N]\n", argv[0]); return 1; }
    uint32_t nframes = (uint32_t)strtoul(argv[3], 0, 0);
    outdir = argv[4];
    for (int i = 5; i < argc; i++) {
        if (!strcmp(argv[i], "--preset") && i + 1 < argc) { i++; preset = !strcmp(argv[i], "original") ? VRP_ORIGINAL : !strcmp(argv[i], "exigente") ? VRP_DEMANDING : VRP_VR; }
        else if (!strcmp(argv[i], "--max") && i + 1 < argc) max_dumps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--force") && i + 1 < argc) { int m; unsigned a, b; if (sscanf(argv[++i], "%d:%u:%u", &m, &a, &b) == 3) { force_mode = m; force_from = a; force_to = b; } }
    }
    mkdir(outdir, 0755);
    VrpConfig vc;
    vrp_config_preset(&vc, preset);
    vrradar_set_vrp(&vc);
    RefOpts ro = {2, 1, REF_HQ, 0};
    r_full = ref_create(&ro);
    r_hud = ref_create(&ro);
    const char *sigs = getenv("MV_SIGS");
    if (sigs) mv_host_set_sigs(sigs);
    char card[600];
    snprintf(card, sizeof card, "%s/card", outdir);
    mkdir(card, 0755);
    MvHostHooks h;
    memset(&h, 0, sizeof h);
    h.exe_loaded = on_exe_loaded; h.bound = on_bound; h.start = on_start; h.frame = frame_hook;
    mv_host_set_hooks(&h);
    mv_host_set_turbo(1);
    int r = mv_host_start(argv[1], argv[2], card);
    if (r) { fprintf(stderr, "mv_host_start = %d: %s\n", r, mv_host_error()); return 1; }
    uint32_t last = 0xFFFFFFFFu;
    for (;;) {
        uint32_t f = mv_host_frame_count();
        if (mv_host_state() == MV_HOST_ERROR) { fprintf(stderr, "error del motor en %u: %s\n", f, mv_host_error()); fails++; break; }
        if (f >= nframes) break;
        if (f != last) { last = f; mv_host_pad(pad_at(f), 128, 128, 128, 128); }
        usleep(200);
    }
    mv_host_pause();
    usleep(200000);
    unsigned calls, marked, fail;
    vrradar_hle_stats(&calls, &marked, &fail);
    fprintf(stderr, "[radar] FIN f=%u listas=%u con radar=%u (ops radar min %u max %u) con conos=%u ampliados=%u en estado=%u cambios de ventana=%u volcados=%u\n",
            mv_host_frame_count(), n_lists, n_present, ops_min, ops_max, n_cone_lists, n_ext_lists, n_state_lists, n_frames_rect_changed, n_dumps);
    fprintf(stderr, "[radar] HLE: llamadas=%u con marcas=%u cadena inesperada=%u\n", calls, marked, fail);
    CHECK(calls > 100, "la HLE casi no se llamó (%u): ¿dirección equivocada?", calls);
    CHECK(fail == 0, "cadenas de OT inesperadas: %u", fail);
    CHECK(n_present > 50, "pocas listas con radar (%u)", n_present);
    fprintf(stderr, fails ? "radar_run: %d FALLOS\n" : "radar_run: OK\n", fails);
    return fails ? 1 : 0;
}
