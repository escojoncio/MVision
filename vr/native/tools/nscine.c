/* nscine: reproduce OFFLINE la pantalla 3D de las cinemáticas (la cadena de la app) con la escena nativa de un volcado y la compara con la cámara directa del motor y con el
 * fotograma real. Cadena: ns_cinema_to_vr (mundo del motor -> mundo VR) -> vista de ojo (XRStereo.eye) -> proyección (ns_cinema_window + ns_cinema_projection) -> rasterizador.
 * Uso: nscine DIR_VOLCADO [DIR_SALIDA] [--gv k] [--frame f.png-ya-convertido-a-ppm]. NO ejecuta el juego. Herramienta de escritorio. */
#include "../ns_adapt.h"
#include "../ns_cinema.h"
#include "../ns_cinema_ref.h"
#include "../ns_cpuref.h"
#include "../ns_dump.h"
#include "../ns_extract.h"
#include "../ns_fx.h"
#include "../ns_fx_export.h"
#include "../ns_gpu_types.h"
#include "../ns_screen.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int16_t rs16(const uint8_t *r, uint32_t a) { int16_t v; memcpy(&v, r + (a & 0x1FFFFF), 2); return v; }

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "uso: nscine DIR [SALIDA] [--gv k] [--frame f.ppm]\n"); return 2; }
    const char *out = argc > 2 && strncmp(argv[2], "--", 2) ? argv[2] : ".";
    const char *framef = NULL;
    int gvk = -1;
    for (int i = 2; i < argc; i++) { if (!strcmp(argv[i], "--gv") && i + 1 < argc) gvk = atoi(argv[++i]); else if (!strcmp(argv[i], "--frame") && i + 1 < argc) framef = argv[++i]; }
    NsDump *d = ns_dump_open(argv[1]);
    if (!d) { fprintf(stderr, "no se pudo abrir %s\n", argv[1]); return 1; }
    NsScene sc; NsMeshStore *st = ns_store_create();
    if (ns_scene_alloc(&sc, NS_MIN_INST, 8192, NS_MIN_LIGHT, 0) || !st) return 1;
    NsExtractOpts xo; memset(&xo, 0, sizeof xo); NsExtractStats es;
    if (ns_extract_ex(&sc, st, d->ram, NS_RAM_SIZE, d->scratch, d->scratch ? 1024 : 0, &xo, &es)) { fprintf(stderr, "sin escena\n"); return 1; }
    NsAdaptOpts ao; memset(&ao, 0, sizeof ao); ao.vram = d->vram; ao.exposure = 1.0f;
    float eyep[3] = { sc.cam.eye[0], sc.cam.eye[1], sc.cam.eye[2] };
    NsAdaptStats as;
    /* referencia del motor: con descarte de reversos desde la cámara del juego */
    NsAdaptOpts aref = ao; aref.cull_eye = eyep;
    NsRTri *tref = NULL; size_t nref = ns_adapt_scene(&sc, st, &aref, &tref, &as);
    /* lo que recibe la app: SIN descarte en el adaptador (la geometría entera) y, si no se pide lo contrario, el descarte del sombreador (ns_gpu_tri_back) */
    NsAdaptSrc *src = NULL; ao.src_out = &src;
    NsRTri *tris = NULL; size_t nt = ns_adapt_scene(&sc, st, &ao, &tris, &as);
    size_t nall = nt;
    if (!getenv("NSCINE_NOCULL")) nt = ns_cine_cull_back(tris, src, nt, eyep);
    printf("triangulos: motor(con descarte)=%zu app(sin descarte)=%zu app(tras descarte del sombreador)=%zu\n", nref, nall, nt);
    NsRScene rs = { tref, nref, d->vram };
    printf("escena: tris=%zu H=%.0f ojo=(%.0f,%.0f,%.0f)\n", nt, sc.cam.h, eyep[0], eyep[1], eyep[2]);
    {   /* eye_inv de RAM (+0x10) frente a la cámara de la escena (+0x30) */
        float A[9]; for (int i = 0; i < 9; i++) A[i] = (float)rs16(d->ram, 0x800B2B7Cu + 0x10u + 2u * (uint32_t)i) / 4096.0f;
        float mx = 0; for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) { float dd = fabsf(A[r * 3 + c] - sc.cam.rot[c * 3 + r]); if (dd > mx) mx = dd; }
        printf("max|A(+0x10) - rot^T(+0x30)| = %.4f\n", mx);
    }
    int w = 640, h = 480;
    const char *aguadir = getenv("NSCINE_AGUA");   /* directorio de salida: PNG antes/despues del agua (ver mas abajo) */
    uint8_t *rgb = malloc((size_t)w * h * 3);
    char p[1024];
    NsRBuf b; ns_buf_alloc(&b, w, h);
    {   /* referencia directa con la cámara del motor */
        float A[9], t[3]; ns_cine_engine_view(&sc.cam, A, t);
        NsRCam c; ns_cam_game(&c, A, t, sc.cam.h, 160, 120, 320, 240, w, h); c.znear = 1;
        ns_buf_clear(&b, 0, 0, 0); ns_render_view(&rs, &c, 0, &b, NULL); ns_buf_to_rgb8(&b, rgb, 0, 0);
        snprintf(p, sizeof p, "%s/directa.png", out); ns_write_png(p, w, h, rgb);
    }
    uint8_t *ref = malloc((size_t)w * h * 3); memcpy(ref, rgb, (size_t)w * h * 3);
    if (framef) {
        int fw, fh; uint8_t *f = ns_read_ppm(framef, &fw, &fh);
        if (f) {   /* el fotograma real (320x256 con la imagen de 240 líneas centrada) a 640x480 */
            uint8_t *fr = malloc((size_t)w * h * 3);
            for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
                int sy = y * 240 / h + (fh - 240) / 2, sx = x * fw / w;
                memcpy(&fr[((size_t)y * w + x) * 3], &f[((size_t)sy * fw + sx) * 3], 3);
            }
            NsCineScore s; ns_cine_score(fr, ref, w, h, &s);
            printf("directa vs fotograma real: corr_bloques=%.3f com_real=(%.3f,%.3f) com_directa=(%.3f,%.3f) d_com=%.3f\n", s.block_corr, s.com_a[0], s.com_a[1], s.com_b[0], s.com_b[1], s.com_dist);
            free(fr); free(f);
        }
    }
    if (aguadir) {   /* agua: vista de ojo ancha (H=130, ~100 grados) con lo que hace el visor bajo el agua. antes = superficie de una cara semitransparente sin descarte + viñeta circular de barras; despues = descarte
                      * NSG_CULL_SEMI y sin barras. El tinte (multiplicativo, ns_screen_water_ref) es el mismo en ambos; la onda no se emula. */
        NsFxScene *fxs = ns_fx_scene_create(0); NsFxOpts fo = ns_fx_opts_default(); NsScreenFx sfx; memset(&sfx, 0, sizeof sfx);
        if (ns_fx_extract(d->ram, &fo, fxs) == 0) ns_fx_export_screen(fxs, &sfx);
        printf("agua: flags=0x%x tinte=(%.2f,%.2f,%.2f) caja_y=(%.0f..%.0f) ojo_y=%.0f\n", sfx.flags, sfx.water_tint[0], sfx.water_tint[1], sfx.water_tint[2], sfx.water_min[1], sfx.water_max[1], eyep[1]);
        for (int pass = 0; pass < 2; pass++) {
            NsRTri *tt = malloc(nall * sizeof *tt); NsAdaptSrc *ss = malloc(nall * sizeof *ss); memcpy(tt, tris, nall * sizeof *tt); memcpy(ss, src, nall * sizeof *ss);
            size_t nn = nall;
            if (pass == 1 && (sfx.flags & NS_SFX_WATER)) nn = ns_cine_cull_back_mode(tt, ss, nall, eyep, NSG_CULL_SEMI);
            float A[9], t3[3]; ns_cine_engine_view(&sc.cam, A, t3);
            NsRCam c; ns_cam_game(&c, A, t3, sc.cam.h, 160, 120, 320, 240, w, h); c.fx *= 130.0f / sc.cam.h; c.fy *= 130.0f / sc.cam.h; c.znear = 1;
            NsRScene s3 = { tt, nn, d->vram };
            ns_buf_clear(&b, 0, 0, 0); ns_render_view(&s3, &c, 0, &b, NULL); ns_buf_to_rgb8(&b, rgb, 0, 0);
            NsGpuOverlay bars; memset(&bars, 0, sizeof bars);
            bars.kind = NSG_OV_BARS; bars.abr = 2; bars.amount = 0.85f; bars.rgb[0] = bars.rgb[1] = bars.rgb[2] = 1; bars.p[0] = (float)6; bars.p[1] = 4.0f; bars.p[3] = sc.cam.h;
            for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
                uint8_t *px = &rgb[((size_t)y * w + x) * 3]; float col[3], o2[3];
                for (int k = 0; k < 3; k++) col[k] = px[k] / 255.0f * ((sfx.flags & NS_SFX_WATER) ? sfx.water_tint[k] : 1.0f);
                if (pass == 0 && (sfx.flags & NS_SFX_WATER)) { ns_screen_overlay_ref(&bars, ((float)x + 0.5f - c.cx) / c.fx, -((float)y + 0.5f - c.cy) / c.fy, 0.0f, col, o2); memcpy(col, o2, sizeof col); }
                for (int k = 0; k < 3; k++) px[k] = (uint8_t)(col[k] * 255.0f + 0.5f);
            }
            snprintf(p, sizeof p, "%s/agua_%s.png", aguadir, pass ? "despues" : "antes"); ns_write_png(p, w, h, rgb);
            printf("agua %s: triangulos=%zu\n", pass ? "despues" : "antes", nn);
            free(tt); free(ss);
        }
        ns_fx_scene_destroy(fxs);
    }
    for (int k = 0; k < NS_CINE_NGV; k++) {
        if (gvk >= 0 && k != gvk) continue;
        float gv[16]; ns_cine_test_gv(&sc.cam, k, gv);
        for (int e = 0; e < 2; e++) {
            NsRTri *tt = malloc(nt * sizeof *tt); memcpy(tt, tris, nt * sizeof *tt);
            NsRCam c;
            if (!ns_cine_app_chain(&sc.cam, gv, e, 0.063f, 3.0f, 0, 0, tt, nt, w, h, &c)) { printf("gv%d ojo%d: cadena invalida\n", k, e); free(tt); continue; }
            NsRScene s2 = { tt, nt, d->vram };
            ns_buf_clear(&b, 0, 0, 0); ns_render_view(&s2, &c, 0, &b, NULL); ns_buf_to_rgb8(&b, rgb, 0, 0);
            snprintf(p, sizeof p, "%s/app_gv%d_ojo%d.png", out, k, e); ns_write_png(p, w, h, rgb);
            NsCineScore sco; ns_cine_score(ref, rgb, w, h, &sco);
            printf("gv%d ojo%d: corr_bloques=%.3f com_directa=(%.3f,%.3f) com_app=(%.3f,%.3f) d_com=%.3f\n", k, e, sco.block_corr, sco.com_a[0], sco.com_a[1], sco.com_b[0], sco.com_b[1], sco.com_dist);
            free(tt);
        }
    }
    return 0;
}
