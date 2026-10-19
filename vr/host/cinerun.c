/* Prueba de las cinemáticas del motor en 3D con el host completo y datos locales (sin visor): arranca el juego, deja correr la secuencia (el guion de mando
 * base solo pulsa confirmar/inicio, no salta la introducción), y por cada escena ensamblada registra la clasificación (vr_view), los planos/cortes (vr_cine)
 * y, cada --every fotogramas, emula por CPU lo que la pantalla 3D mostraría a cada ojo (cámaras de vr_cine.c, rasterizador de hud_emu.c) más el fotograma
 * del oráculo (lo que vería la pantalla de cine). Escribe PPM en --out:  s_FFFFF_L.ppm, _R.ppm (ojos con el HUD por encima), _hud.ppm, _fb.ppm.
 *   cinerun <exe> <disco> <frames> --out DIR [--every N] [--from F] [--head fija|barrido] [--log FICHERO]
 * Código de salida 0 = todo bien. No lleva datos del juego. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "bridge.h"
#include "gpu.h"
#include "host.h"
#include "hud_emu.h"
#include "vr_cine.h"
#include "vr_view.h"
#include "vrhost.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "FALLO %d: ", __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); fails++; } } while (0)

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

static void head_pose(float yaw_deg, float x, float y, float out[16]) {
    float a = yaw_deg * 3.14159265f / 180.0f, c = cosf(a), s = sinf(a);
    float m[16] = {c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, x, y, 0, 1};
    memcpy(out, m, sizeof m);
}

static void write_ppm(const char *path, int w, int h, const uint8_t *rgb) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    fwrite(rgb, 1, (size_t)w * h * 3, f);
    fclose(f);
}

static uint32_t fnv(const void *p, size_t n, uint32_t h) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}

#define EW 640
#define EH 480

/* Efecto de pantalla de la escena (tinte y recubrimientos, en espacio gamma) sobre la imagen de un ojo, como hará el fragmento de la pantalla 3D (xr_screen3d). */
static float mixb(float c, float f, int m) {
    float r = m == 0 ? 0.5f * c + 0.5f * f : m == 1 ? c + f : m == 2 ? c - f : c + 0.25f * f;
    return r < 0 ? 0 : r > 1 ? 1 : r;
}
static void apply_fx(uint8_t *rgb, const VrFilter *f) {
    if (!f->active) return;
    int nc = f->n_covers > VR_FILTER_COVERS ? VR_FILTER_COVERS : f->n_covers;
    for (int i = 0; i < EW * EH; i++)
        for (int k = 0; k < 3; k++) {
            float c = (float)rgb[i * 3 + k] / 255.0f * f->tint[k];
            c = c > 1 ? 1 : c;
            for (int q = 0; q < nc; q++) if (f->covers_abr[q] >= 0) c = mixb(c, f->covers_rgb[q][k], f->covers_abr[q] & 3);
            if (nc == 0 && f->cover_abr >= 0) c = mixb(c, f->cover_rgb[k], f->cover_abr & 3);
            rgb[i * 3 + k] = (uint8_t)(c * 255.0f + 0.5f);
        }
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "uso: %s <exe> <disco> <frames> --out DIR [--every N] [--from F] [--head fija|barrido] [--log F]\n", argv[0]); return 1; }
    const char *exe = argv[1], *disc = argv[2], *out = NULL, *logp = NULL;
    uint32_t nframes = (uint32_t)strtoul(argv[3], 0, 0), every = 100, from = 0;
    int sweep = 0;
    for (int i = 4; i < argc; i++) {
        if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--every") && i + 1 < argc) every = (uint32_t)strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--from") && i + 1 < argc) from = (uint32_t)strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--head") && i + 1 < argc) sweep = !strcmp(argv[++i], "barrido");
        else if (!strcmp(argv[i], "--log") && i + 1 < argc) logp = argv[++i];
    }
    if (out) mkdir(out, 0755);
    FILE *lg = logp ? fopen(logp, "w") : stdout;
    if (!lg) lg = stdout;
    const char *sigs = getenv("MV_SIGS");
    if (sigs) mv_host_set_sigs(sigs);
    mkdir("build-host", 0755);
    mkdir("build-host/card", 0755);
    mv_vrhost_install();
    mv_host_set_turbo(1);
    int r = mv_host_start(exe, disc, "build-host/card");
    if (r) { fprintf(stderr, "mv_host_start = %d: %s\n", r, mv_host_error()); return 1; }

    static uint16_t evram[1024 * 512], tmp[1024 * 512];
    static uint8_t fbuf[MV_HOST_FB_BYTES];
    uint32_t last_f = 0xFFFFFFFFu, next_sample = from, n_scenes = 0, n_eng = 0, n_cuts = 0, n_video = 0, n_samples = 0, n_eng_samples = 0;
    uint32_t last_shot = 0;
    int last_kind = -1;
    uint32_t gvh_all = 2166136261u;
    uint32_t first_eng = 0, last_eng = 0, n_bad_cam = 0, n_big = 0;
    float max_jump = 0, max_turn = 0;
    float ipd = 0.063f, conv = 3.0f, tx = 0.6f, ty = 0.45f;
    HudImg im = hud_img_new(EW, EH), hudim = {0, 0, 0};
    float *zb = malloc(sizeof(float) * EW * EH);
    uint8_t *rgbL = malloc((size_t)EW * EH * 3), *rgbR = malloc((size_t)EW * EH * 3);
    for (;;) {
        uint32_t f = mv_host_frame_count();
        if (mv_host_state() == MV_HOST_ERROR) { fprintf(stderr, "error del motor en el frame %u: %s\n", f, mv_host_error()); fails++; break; }
        if (f >= nframes) break;
        if (f != last_f) {
            last_f = f;
            mv_host_pad(pad_at(f), 128, 128, 128, 128);
            float m[16];
            if (sweep) head_pose(70.0f * sinf((float)f * 0.05f), 0.3f * sinf((float)f * 0.031f), 1.65f + 0.2f * sinf((float)f * 0.043f), m);
            else head_pose(0, 0, 1.65f, m);
            mv_vr_set_head(m, 0.0);
        }
        int got = 0;
        for (int k = 0; k < 4; k++) {
            VrScene sc;
            if (!mv_host_scene(&sc)) break;
            got++;
            n_scenes++;
            if (!sc.src) continue;
            if (sc.src->vram_init) memcpy(evram, sc.src->vram_init, sizeof evram);
            MvVrGameState gs;
            mv_vrhost_game_state(&gs);
            uint32_t hf = sc.src->host_frame;
            int eng = sc.cine_active;
            if (eng) { n_eng++; if (!first_eng) first_eng = hf; last_eng = hf; }
            if (gs.view_kind == VRV_K_VIDEO) n_video++;
            if (sc.cine_cut && eng) n_cuts++;
            {   /* control independiente de los cortes: salto de posición y giro de la cámara entre escenas consecutivas (en el mundo VR de origen fijo) */
                static float pe[3], pf[3]; static int have_p; static float pcut;
                float fw[3] = {sc.game_view[8], sc.game_view[9], sc.game_view[10]};
                float nl = sqrtf(fw[0] * fw[0] + fw[1] * fw[1] + fw[2] * fw[2]);
                for (int q = 0; q < 3; q++) fw[q] /= nl > 0 ? nl : 1;
                if (eng && have_p) {
                    float d[3] = {sc.eye_vr[0] - pe[0], sc.eye_vr[1] - pe[1], sc.eye_vr[2] - pe[2]};
                    float jump = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) * 1000.0f;   /* unidades del motor */
                    float dt = fw[0] * pf[0] + fw[1] * pf[1] + fw[2] * pf[2];
                    float turn = acosf(dt > 1 ? 1 : dt < -1 ? -1 : dt) * 57.2957795f;
                    if (jump > max_jump) max_jump = jump;
                    if (turn > max_turn) max_turn = turn;
                    if (jump > 300 || turn > 8) { n_big++; fprintf(lg, "[salto] f=%u salto=%.0f giro=%.1f plano=%u corte=%d\n", hf, jump, turn, sc.cine_shot, sc.cine_cut); }
                    (void)pcut;
                }
                if (eng) { memcpy(pe, sc.eye_vr, sizeof pe); memcpy(pf, fw, sizeof pf); have_p = 1; } else have_p = 0;
            }
            int kind_changed = gs.view_kind != last_kind, shot_changed = sc.cine_shot != last_shot && eng;
            if (kind_changed || shot_changed || (sc.cine_cut && eng)) {
                fprintf(lg, "[cine] f=%u clase=%s motivo=%s plano=%u corte=%d cubiertas=%d 3D=%u tris | ojo=(%.2f %.2f %.2f) m\n", hf, vrview_kind_name(gs.view_kind), vrview_reason_name(gs.view_reason),
                        sc.cine_shot, sc.cine_cut, sc.cine_cover, sc.stats.n_tris3d, sc.eye_vr[0], sc.eye_vr[1], sc.eye_vr[2]);
                last_kind = gs.view_kind; last_shot = eng ? sc.cine_shot : last_shot;
            }
            int want = hf >= next_sample && out && eng && sc.has_3d;
            if (eng && sc.has_3d) {   /* la cámara del juego debe ser finita y la vista estéreo invertible */
                VrCineEye e;
                if (!vrcine_eye(sc.game_view, 0, ipd, conv, tx, ty, &e)) n_bad_cam++;
                gvh_all = fnv(sc.game_view, sizeof sc.game_view, gvh_all);
            }
            if (!want) { hud_emu_run(&sc, evram, NULL, 0, 0); continue; }
            next_sample = hf + every;
            n_samples++; n_eng_samples++;
            memcpy(tmp, evram, sizeof tmp);
            for (int eye = 0; eye < 2; eye++) {
                VrCineEye ce;
                if (!vrcine_eye(sc.game_view, eye, ipd, conv, tx, ty, &ce)) continue;
                memcpy(evram, tmp, sizeof evram);
                memset(im.rgba, 0, sizeof(float) * EW * EH * 4);
                hud_emu_run3d(&sc, evram, &im, zb, 1, ce.view, ce.tan_);
                uint8_t *dst = eye ? rgbR : rgbL;
                hud_emu_to_rgb(&im, dst);
                apply_fx(dst, &sc.filter);
                /* HUD 2D del panel (subtítulos, créditos, bandas) por encima, igual que el panel de la pantalla 3D */
                if (!eye) {
                    int dw = sc.src->disp.w > 0 ? sc.src->disp.w : 320, dh = sc.src->disp.h > 0 ? sc.src->disp.h : 240;
                    memcpy(evram, tmp, sizeof evram);
                    hudim = hud_img_new(dw, dh);
                    hud_emu_run(&sc, evram, &hudim, sc.src->disp.x, sc.src->disp.y);
                }
                if (hudim.rgba) {
                    for (int y = 0; y < EH; y++) for (int x = 0; x < EW; x++) {
                        int hx = x * hudim.w / EW, hy = y * hudim.h / EH;
                        const float *c = &hudim.rgba[((size_t)hy * hudim.w + hx) * 4];
                        float a = c[3] > 1 ? 1 : c[3];
                        for (int k2 = 0; k2 < 3; k2++) {
                            float v = c[k2] + (1.0f - a) * (float)dst[((size_t)y * EW + x) * 3 + k2] / 255.0f;
                            dst[((size_t)y * EW + x) * 3 + k2] = (uint8_t)(v < 0 ? 0 : v > 1 ? 255 : v * 255.0f + 0.5f);
                        }
                    }
                }
            }
            if (hudim.rgba) hud_img_free(&hudim);
            char p[512];
            snprintf(p, sizeof p, "%s/s_%05u_L.ppm", out, hf); write_ppm(p, EW, EH, rgbL);
            snprintf(p, sizeof p, "%s/s_%05u_R.ppm", out, hf); write_ppm(p, EW, EH, rgbR);
            int fw = 0, fh = 0;
            double luma = -1;
            if (mv_host_framebuffer(fbuf, MV_HOST_FB_BYTES, &fw, &fh, 0)) {
                uint8_t *rgb = malloc((size_t)fw * fh * 3);
                double sum = 0;
                for (int q = 0; q < fw * fh; q++) { for (int c = 0; c < 3; c++) rgb[q * 3 + c] = fbuf[q * 4 + c]; sum += 0.3 * fbuf[q * 4] + 0.59 * fbuf[q * 4 + 1] + 0.11 * fbuf[q * 4 + 2]; }
                luma = sum / ((double)fw * fh);
                snprintf(p, sizeof p, "%s/s_%05u_fb.ppm", out, hf); write_ppm(p, fw, fh, rgb);
                free(rgb);
            }
            double lsum = 0;
            for (int q = 0; q < EW * EH; q++) lsum += 0.3 * rgbL[q * 3] + 0.59 * rgbL[q * 3 + 1] + 0.11 * rgbL[q * 3 + 2];
            fprintf(lg, "[muestra] f=%u plano=%u tris3d=%u luma_oraculo=%.1f luma_3d=%.1f cubiertas=%d tinte=(%.2f %.2f %.2f) tiras=%d copias=%d\n", hf, sc.cine_shot, sc.stats.n_tris3d, luma, lsum / (EW * EH), sc.cine_cover,
                    sc.filter.tint[0], sc.filter.tint[1], sc.filter.tint[2], sc.filter.n_strips, sc.filter.n_polys);
            fflush(lg);
        }
        if (!got) usleep(300);
    }
    mv_host_pause();
    usleep(200000);
    MvVrGameState gs;
    mv_vrhost_game_state(&gs);
    fprintf(lg, "[cam] salto máx entre escenas=%.0f unidades, giro máx=%.1f grados, escenas con salto>300 o giro>8: %u\n", max_jump, max_turn, n_big);
    fprintf(lg, "[fin] f=%u escenas=%u en cinemática del motor=%u (primera f=%u, última f=%u), vídeo=%u, planos=%u (cortes con escena=%u, cortes totales=%u), muestras=%u, hash de cámaras=%08x, cámaras no invertibles=%u\n",
            mv_host_frame_count(), n_scenes, n_eng, first_eng, last_eng, n_video, gs.cine_shot, n_cuts, gs.cine_cuts, n_samples, gvh_all, n_bad_cam);
    CHECK(n_bad_cam == 0, "cámaras del juego no invertibles");
    fprintf(lg, fails ? "cinerun: %d FALLOS\n" : "cinerun: OK\n", fails);
    return fails ? 1 : 0;
}
