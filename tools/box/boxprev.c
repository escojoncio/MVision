/* boxprev: render de prueba (trazado de rayos por CPU) de la carcasa de visión interior sobre un panorama.
 * Sirve para comparar el aspecto con el original y revisar el estéreo. Salida PPM.
 *   boxprev --pano p.ppm --out o.ppm [--stages f --stage s --sight 0x.. --tex 0x..] [--w 640 --h 512]
 *           [--focal px] [--cx px --cy px] [--eye c|l|r] [--yaw g --pitch g --roll g] [--t seg] [--leave seg]
 *           [--style card|black] [--wide] [--aa 2] [--nolag]
 * --t: segundos desde la activación (transición); --leave: tras activar, segundos desde que se retira. */
#include "box_view.h"
#include "../weapons/wcore.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { int w, h; float *rgb; } Pano;
static Pano load_pano(const char *p) {
    Pano r = {0, 0, NULL};
    FILE *f = fopen(p, "rb"); if (!f) return r;
    int mx; if (fscanf(f, "P6 %d %d %d", &r.w, &r.h, &mx) != 3) { fclose(f); return r; }
    fgetc(f);
    size_t n = (size_t)r.w * r.h * 3; uint8_t *b = malloc(n);
    if (fread(b, 1, n, f) != n) { free(b); fclose(f); r.w = 0; return r; }
    fclose(f);
    r.rgb = malloc(n * sizeof(float));
    for (size_t i = 0; i < n; i++) r.rgb[i] = b[i];
    free(b); return r;
}
static void sample_pano(const Pano *p, WVec3 d, float *c) {
    d = wv_norm(d);
    float lon = atan2f(d.x, -d.z), lat = asinf(d.y);
    float u = (lon + (float)M_PI) / (2 * (float)M_PI) * p->w - 0.5f, v = ((float)M_PI / 2 - lat) / (float)M_PI * p->h - 0.5f;
    int x0 = (int)floorf(u), y0 = (int)floorf(v); float fx = u - x0, fy = v - y0;
    for (int k = 0; k < 3; k++) {
        float a = 0;
        for (int j = 0; j < 2; j++) for (int i = 0; i < 2; i++) {
            int xx = ((x0 + i) % p->w + p->w) % p->w, yy = y0 + j; if (yy < 0) yy = 0; if (yy >= p->h) yy = p->h - 1;
            a += p->rgb[((size_t)yy * p->w + xx) * 3 + k] * (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
        }
        c[k] = a;
    }
}

typedef struct { float t, c[3], a; } Hit;
static int cmp_hit(const void *a, const void *b) { float x = ((const Hit *)a)->t, y = ((const Hit *)b)->t; return x < y ? -1 : x > y; }

int main(int argc, char **argv) {
    const char *pano = NULL, *out = "out.ppm", *stages = NULL, *stage = "init", *eye = "c", *style = "card";
    int W = 640, H = 512, aa = 1, wide = 0, nolag = 0;
    float focal = -1, cx = -1, cy = -1, yaw = 0, pitch = 0, roll = 0, T = 3.f, leave = -1.f;
    uint16_t sid = 0, tid = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i]; const char *v = i + 1 < argc ? argv[i + 1] : "";
        if (!strcmp(a, "--pano")) pano = argv[++i]; else if (!strcmp(a, "--out")) out = argv[++i];
        else if (!strcmp(a, "--stages")) stages = argv[++i]; else if (!strcmp(a, "--stage")) stage = argv[++i];
        else if (!strcmp(a, "--sight")) sid = (uint16_t)strtol(v, 0, 0), i++; else if (!strcmp(a, "--tex")) tid = (uint16_t)strtol(v, 0, 0), i++;
        else if (!strcmp(a, "--w")) W = atoi(v), i++; else if (!strcmp(a, "--h")) H = atoi(v), i++;
        else if (!strcmp(a, "--focal")) focal = (float)atof(v), i++; else if (!strcmp(a, "--cx")) cx = (float)atof(v), i++;
        else if (!strcmp(a, "--cy")) cy = (float)atof(v), i++; else if (!strcmp(a, "--eye")) eye = argv[++i];
        else if (!strcmp(a, "--yaw")) yaw = (float)atof(v), i++; else if (!strcmp(a, "--pitch")) pitch = (float)atof(v), i++;
        else if (!strcmp(a, "--roll")) roll = (float)atof(v), i++; else if (!strcmp(a, "--t")) T = (float)atof(v), i++;
        else if (!strcmp(a, "--leave")) leave = (float)atof(v), i++; else if (!strcmp(a, "--style")) style = argv[++i];
        else if (!strcmp(a, "--wide")) wide = 1; else if (!strcmp(a, "--aa")) aa = atoi(v), i++; else if (!strcmp(a, "--nolag")) nolag = 1;
    }
    if (!pano) { fprintf(stderr, "falta --pano\n"); return 1; }
    Pano P = load_pano(pano); if (!P.w) { fprintf(stderr, "panorama no valido\n"); return 1; }
    uint8_t *dir = NULL; size_t dl = 0;
    if (stages) { FILE *f = fopen(stages, "rb"); if (f) { fseek(f, 0, SEEK_END); dl = ftell(f); fseek(f, 0, SEEK_SET); dir = malloc(dl); if (fread(dir, 1, dl, f) != dl) { free(dir); dir = NULL; } fclose(f); } }
    BoxViewConfig c; box_view_default_config(&c);
    c.stage = stage; c.sight_id = sid; c.tex_id = tid; c.style = strcmp(style, "black") ? BOX_STYLE_TEX : BOX_STYLE_BLACK;
    if (nolag) c.lag_rate = 0;
    int rc = box_view_init(&c, dir, dl);
    fprintf(stderr, "datos: %s\n", rc == 0 ? "disco" : "generico");
    box_view_set_wide(wide);
    if (focal < 0) focal = 0.5f * W / tanf(50.f * (float)M_PI / 180.f);
    if (cx < 0) cx = 0.5f * W; if (cy < 0) cy = 0.5f * H;
    const float D2R = (float)M_PI / 180.f;
    WQuat q = wquat_normalize(wquat_mul(wquat_axis_angle(wv(0, 1, 0), -yaw * D2R),
                              wquat_mul(wquat_axis_angle(wv(1, 0, 0), pitch * D2R), wquat_axis_angle(wv(0, 0, 1), roll * D2R))));
    WPose head = {wv(0, 1.6f, 0), q};
    box_view_set_active(1);
    float tt = 0;
    while (tt < T) { box_view_update(head, 1.f / 120.f); tt += 1.f / 120.f; }
    if (leave >= 0) { box_view_set_active(0); tt = 0; while (tt < leave) { box_view_update(head, 1.f / 120.f); tt += 1.f / 120.f; } }
    BoxDraw dr[BOX_MAX_DRAWS]; int nd = box_view_draws(dr, BOX_MAX_DRAWS);
    float ex = !strcmp(eye, "l") ? -0.5f * c.ipd : (!strcmp(eye, "r") ? 0.5f * c.ipd : 0.f);
    WVec3 o_w = wpose_apply(head, wv(ex, 0, 0));
    WPose sh = box_view_shell_pose(), ish = wpose_inv(sh);
    WVec3 o_l = wpose_apply(ish, o_w);
    int tw = 0, th = 0; const uint8_t *tex = box_view_texture(&tw, &th);
    uint8_t *img = malloc((size_t)W * H * 3);
    Hit hits[256];
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        float acc[3] = {0, 0, 0};
        for (int sy = 0; sy < aa; sy++) for (int sx = 0; sx < aa; sx++) {
            float px = x + (sx + 0.5f) / aa, py = y + (sy + 0.5f) / aa;
            WVec3 dc = wv((px - cx) / focal, -(py - cy) / focal, -1.f);
            WVec3 d_w = wquat_rotate(q, dc);
            WVec3 d_l = wquat_rotate(wquat_inv(sh.q), d_w);
            int nh = 0;
            for (int k = 0; k < nd; k++) {
                const WMesh *m = dr[k].mesh;
                for (uint32_t i = 0; i + 2 < m->ni && nh < 255; i += 3) {
                    const WVertex *A = &m->v[m->idx[i]], *B = &m->v[m->idx[i + 1]], *C = &m->v[m->idx[i + 2]];
                    WVec3 a = wv(A->pos[0], A->pos[1], A->pos[2]), e1 = wv_sub(wv(B->pos[0], B->pos[1], B->pos[2]), a), e2 = wv_sub(wv(C->pos[0], C->pos[1], C->pos[2]), a);
                    WVec3 pv = wv_cross(d_l, e2); float det = wv_dot(e1, pv);
                    if (fabsf(det) < 1e-12f) continue;
                    float inv = 1.f / det; WVec3 s = wv_sub(o_l, a);
                    float u = wv_dot(s, pv) * inv; if (u < 0 || u > 1) continue;
                    WVec3 qv = wv_cross(s, e1); float v = wv_dot(d_l, qv) * inv; if (v < 0 || u + v > 1) continue;
                    float t = wv_dot(e2, qv) * inv; if (t <= 1e-5f) continue;
                    /* solo caras que miran al ojo */
                    WVec3 n = wv_cross(e1, e2); if (wv_dot(n, d_l) > 0 && m->v == dr[0].mesh->v) { /* casco: descarta traseras */ }
                    float w0 = 1 - u - v;
                    float col[3], al = 1.f;
                    float vr = A->rgba[0] * w0 + B->rgba[0] * u + C->rgba[0] * v, vg = A->rgba[1] * w0 + B->rgba[1] * u + C->rgba[1] * v,
                          vb = A->rgba[2] * w0 + B->rgba[2] * u + C->rgba[2] * v;
                    float va = (A->rgba[3] * w0 + B->rgba[3] * u + C->rgba[3] * v) / 255.f;
                    col[0] = vr; col[1] = vg; col[2] = vb;
                    if (dr[k].texture && tex) {
                        float tu = A->uv[0] * w0 + B->uv[0] * u + C->uv[0] * v, tv = A->uv[1] * w0 + B->uv[1] * u + C->uv[1] * v;
                        tu -= floorf(tu); tv -= floorf(tv);
                        int ix = (int)(tu * tw) % tw, iy = (int)(tv * th) % th;
                        const uint8_t *tp = tex + ((size_t)iy * tw + ix) * 4;
                        col[0] = vr / 255.f * tp[0]; col[1] = vg / 255.f * tp[1]; col[2] = vb / 255.f * tp[2];
                    }
                    al = va * dr[k].alpha;
                    hits[nh].t = t; memcpy(hits[nh].c, col, sizeof col); hits[nh].a = al; nh++;
                }
            }
            qsort(hits, nh, sizeof(Hit), cmp_hit);
            float trans = 1.f, c3[3] = {0, 0, 0};
            for (int i = 0; i < nh && trans > 1e-3f; i++) {
                for (int k = 0; k < 3; k++) c3[k] += trans * hits[i].a * hits[i].c[k];
                trans *= 1.f - hits[i].a;
            }
            if (trans > 1e-3f) {
                float bg[3]; sample_pano(&P, d_w, bg);
                for (int k = 0; k < 3; k++) c3[k] += trans * bg[k];
            }
            for (int k = 0; k < 3; k++) acc[k] += c3[k];
        }
        for (int k = 0; k < 3; k++) { float v = acc[k] / (aa * aa); img[((size_t)y * W + x) * 3 + k] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v + 0.5f); }
    }
    FILE *f = fopen(out, "wb"); fprintf(f, "P6\n%d %d\n255\n", W, H); fwrite(img, 1, (size_t)W * H * 3, f); fclose(f);
    return 0;
}
