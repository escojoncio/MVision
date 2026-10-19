/* nsrender: herramienta de escritorio para el rasterizador de referencia (ns_cpuref). NO entra en la app.
 *   nsrender --demo --out DIR                         escena sintética (sin datos del juego): vista, panorama, cubemap
 *   nsrender --vram vram.bin --tris tris.bin --out DIR [opciones]
 * vram.bin: 1024*512 palabras de 16 bits little-endian (1 MiB). tris.bin: matriz de NsRTri (84 bytes, ver ns_cpuref.h).
 * Cámara libre:  --pos x,y,z  --yaw g  --pitch g  --fov g  --size WxH  --znear n      (mundo con Y hacia abajo)
 * Cámara del motor:  --game a00,a01,a02,a10,a11,a12,a20,a21,a22,tx,ty,tz,H,ofx,ofy  [--game-ref 320x240] [--game-size WxH]
 *   (e = A*p + t con A fila-mayor en unidades de mundo; H, ofx, ofy en píxeles de la pantalla de referencia)
 * Comparación:  --ref fotograma.ppm  (P6; se compara con la vista del motor; escribe game_diff.png y las métricas) [--thr n]
 * Salidas en DIR:  view.png (cámara libre), game.png, pano.png (equirectangular, huecos en magenta; --nomark los desactiva),
 *   pano_raw.png, cube.png (cruz 4x3), cube_<0..5>.png, y con --ppm también .ppm. Selección: --only view,game,pano,cube
 * Otras: --pano WxH (def. 2048x1024)  --float (sin cuantizar a 5 bits)  --bg r,g,b (0..255)  --dump-demo DIR */
#include "../ns_cpuref.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int parse_floats(const char *s, float *o, int n) {
    int k = 0;
    while (k < n && *s) { char *e; o[k] = strtof(s, &e); if (e == s) return -1; k++; s = e; if (*s == ',') s++; }
    return k == n ? 0 : -1;
}
static int parse_wh(const char *s, int *w, int *h) { return sscanf(s, "%dx%d", w, h) == 2 && *w > 0 && *h > 0 ? 0 : -1; }
static void *readall(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long l = ftell(f); fseek(f, 0, SEEK_SET);
    void *p = malloc((size_t)l + 1);
    if (p && fread(p, 1, (size_t)l, f) != (size_t)l) { free(p); p = NULL; }
    fclose(f);
    if (p) *n = (size_t)l;
    return p;
}

/* ---- escena de demostración ---- */
static uint16_t rgb15(int r, int g, int b, int s) { return (uint16_t)(r | g << 5 | b << 10 | (s ? 0x8000 : 0)); }
static NsRVert V(float x, float y, float z, float u, float v, int r, int g, int b) {
    NsRVert o; memset(&o, 0, sizeof o); o.p[0] = x; o.p[1] = y; o.p[2] = z; o.u = u; o.v = v; o.r = (uint8_t)r; o.g = (uint8_t)g; o.b = (uint8_t)b; return o;
}
static size_t demo_quad(NsRTri *t, NsRVert a, NsRVert b, NsRVert c, NsRVert d, uint8_t flags, uint16_t tpage, uint16_t clut, int abr) {
    memset(t, 0, 2 * sizeof *t);
    t[0].v[0] = a; t[0].v[1] = b; t[0].v[2] = c; t[1].v[0] = a; t[1].v[1] = c; t[1].v[2] = d;
    for (int i = 0; i < 2; i++) { t[i].flags = flags; t[i].tpage = tpage; t[i].clut = clut; t[i].abr = (uint8_t)abr; }
    return 2;
}
static NsRTri *make_demo(uint16_t *vram, size_t *n) {
    /* página 15 bpp en (320,0): degradado con cuadrícula; página 4 bpp en (64,0) con CLUT en (0,480): ladrillo */
    for (int y = 0; y < 256; y++) for (int x = 0; x < 64; x++) {
        int g = ((x & 8) ^ (y & 8)) ? 8 : 20;
        vram[y * 1024 + 320 + x] = rgb15(g, (y >> 3) & 31, (x * 2) & 31, 0);
    }
    for (int x = 0; x < 64; x++) vram[0 * 1024 + 320 + x] = 0;   /* fila 0 transparente (texel 0) */
    for (int i = 0; i < 16; i++) vram[480 * 1024 + i] = rgb15(6 + i, 3 + i / 2, 2, 0);
    vram[480 * 1024 + 0] = 0;
    for (int y = 0; y < 256; y++) for (int xw = 0; xw < 64; xw++) {
        uint16_t w = 0;
        for (int k = 0; k < 4; k++) { int u = xw * 4 + k; int idx = ((y & 15) < 2 || (((u + ((y >> 4) & 1) * 16) & 31) < 2)) ? 1 : 6 + ((u * 7 + y * 3) & 7); w |= (uint16_t)(idx << (4 * k)); }
        vram[y * 1024 + 64 + xw] = w;
    }
    NsRTri *t = calloc(64, sizeof *t); size_t m = 0;
    uint16_t tp15 = (uint16_t)(5 | (2 << 7)), tp4 = 1, cl = (uint16_t)(480 << 6);
    float R = 300, H = 150;
    m += demo_quad(t + m, V(-R, H, -R, 0, 0, 128, 128, 128), V(R, H, -R, 255, 0, 128, 128, 128), V(R, H, R, 255, 255, 128, 128, 128), V(-R, H, R, 0, 255, 128, 128, 128), NS_TF_TEXTURED, tp4, cl, 0);   /* suelo (+Y abajo) */
    m += demo_quad(t + m, V(-R, -H, -R, 0, 0, 255, 128, 64), V(R, -H, -R, 0, 0, 64, 255, 128), V(R, -H, R, 0, 0, 128, 64, 255), V(-R, -H, R, 0, 0, 255, 255, 64), 0, 0, 0, 0);                       /* techo gouraud */
    for (int k = 0; k < 4; k++) {   /* paredes */
        float a = (float)k * 1.5707963f, c = cosf(a), s = sinf(a);
        float x0 = -R, x1 = R;
        NsRVert v0 = V(c * x0 + s * R, -H, -s * x0 + c * R, 0, 0, 128, 128, 128), v1 = V(c * x1 + s * R, -H, -s * x1 + c * R, 63, 0, 128, 128, 128);
        NsRVert v2 = V(c * x1 + s * R, H, -s * x1 + c * R, 63, 255, 128, 128, 128), v3 = V(c * x0 + s * R, H, -s * x0 + c * R, 0, 255, 128, 128, 128);
        if (k == 3) continue;        /* pared abierta: se ve el hueco en el panorama */
        m += demo_quad(t + m, v0, v1, v2, v3, NS_TF_TEXTURED, tp15, 0, 0);
    }
    /* cristal semitransparente (mezcla 50%) y neblina aditiva delante */
    m += demo_quad(t + m, V(-100, -80, 150, 0, 0, 90, 200, 255), V(100, -80, 150, 0, 0, 90, 200, 255), V(100, 80, 150, 0, 0, 90, 200, 255), V(-100, 80, 150, 0, 0, 90, 200, 255), NS_TF_SEMI, 0, 0, 0);
    m += demo_quad(t + m, V(-60, -120, 100, 0, 0, 255, 160, 40), V(60, -120, 100, 0, 0, 255, 160, 40), V(60, 20, 100, 0, 0, 255, 160, 40), V(-60, 20, 100, 0, 0, 255, 160, 40), NS_TF_SEMI, 0, 0, 1);
    *n = m;
    return t;
}

static void out_png(const char *dir, const char *name, int w, int h, const uint8_t *rgb, int ppm) {
    char p[1024];
    snprintf(p, sizeof p, "%s/%s.png", dir, name);
    if (ns_write_png(p, w, h, rgb)) fprintf(stderr, "no se pudo escribir %s\n", p);
    if (ppm) { snprintf(p, sizeof p, "%s/%s.ppm", dir, name); ns_write_ppm(p, w, h, rgb); }
}
static int want(const char *only, const char *k) { return !only || strstr(only, k); }

int main(int argc, char **argv) {
    const char *vramf = NULL, *trisf = NULL, *dir = ".", *only = NULL, *reff = NULL, *dumpdir = NULL;
    int demo = 0, ppm = 0, mark = 1, thr = 8;
    uint32_t opts = 0;
    float pos[3] = {0, 0, 0}, yaw = 0, pitch = 0, fov = 90, znear = 1, bg[3] = {0, 0, 0};
    int vw = 640, vh = 480, pw = 2048, ph = 1024, rw = 320, rh = 240, gw = 0, gh = 0;
    float game[15]; int have_game = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        #define NEXT() (i + 1 < argc ? argv[++i] : (fprintf(stderr, "falta valor para %s\n", a), exit(2), (char *)0))
        if (!strcmp(a, "--demo")) demo = 1;
        else if (!strcmp(a, "--vram")) vramf = NEXT();
        else if (!strcmp(a, "--tris")) trisf = NEXT();
        else if (!strcmp(a, "--out")) dir = NEXT();
        else if (!strcmp(a, "--only")) only = NEXT();
        else if (!strcmp(a, "--ref")) reff = NEXT();
        else if (!strcmp(a, "--dump-demo")) dumpdir = NEXT();
        else if (!strcmp(a, "--ppm")) ppm = 1;
        else if (!strcmp(a, "--nomark")) mark = 0;
        else if (!strcmp(a, "--float")) opts |= NS_O_FLOAT;
        else if (!strcmp(a, "--thr")) thr = atoi(NEXT());
        else if (!strcmp(a, "--yaw")) yaw = (float)atof(NEXT());
        else if (!strcmp(a, "--pitch")) pitch = (float)atof(NEXT());
        else if (!strcmp(a, "--fov")) fov = (float)atof(NEXT());
        else if (!strcmp(a, "--znear")) znear = (float)atof(NEXT());
        else if (!strcmp(a, "--pos")) { if (parse_floats(NEXT(), pos, 3)) { fprintf(stderr, "--pos x,y,z\n"); return 2; } }
        else if (!strcmp(a, "--bg")) { if (parse_floats(NEXT(), bg, 3)) { fprintf(stderr, "--bg r,g,b\n"); return 2; } }
        else if (!strcmp(a, "--size")) { if (parse_wh(NEXT(), &vw, &vh)) return 2; }
        else if (!strcmp(a, "--pano")) { if (parse_wh(NEXT(), &pw, &ph)) return 2; }
        else if (!strcmp(a, "--game-ref")) { if (parse_wh(NEXT(), &rw, &rh)) return 2; }
        else if (!strcmp(a, "--game-size")) { if (parse_wh(NEXT(), &gw, &gh)) return 2; }
        else if (!strcmp(a, "--game")) { if (parse_floats(NEXT(), game, 15)) { fprintf(stderr, "--game necesita 15 valores\n"); return 2; } have_game = 1; }
        else { fprintf(stderr, "opción desconocida: %s (véase la cabecera de nsrender.c)\n", a); return 2; }
    }
    uint16_t *vram = calloc(1024 * 512, 2);
    NsRTri *tris = NULL; size_t ntris = 0;
    if (demo) { tris = make_demo(vram, &ntris); pos[1] = 0; if (!have_game) { yaw = yaw ? yaw : 20; } }
    else {
        size_t nv = 0, nt = 0;
        uint16_t *v = vramf ? readall(vramf, &nv) : NULL;
        tris = trisf ? readall(trisf, &nt) : NULL;
        if (!v || nv != 1024 * 512 * 2 || !tris || nt % sizeof(NsRTri)) { fprintf(stderr, "uso: nsrender (--demo | --vram vram.bin --tris tris.bin) --out DIR (ver cabecera)\n"); return 2; }
        memcpy(vram, v, nv); free(v); ntris = nt / sizeof(NsRTri);
    }
    if (dumpdir) {
        char p[1024]; FILE *f;
        snprintf(p, sizeof p, "%s/vram.bin", dumpdir); if ((f = fopen(p, "wb"))) { fwrite(vram, 2, 1024 * 512, f); fclose(f); }
        snprintf(p, sizeof p, "%s/tris.bin", dumpdir); if ((f = fopen(p, "wb"))) { fwrite(tris, sizeof(NsRTri), ntris, f); fclose(f); }
    }
    NsRScene sc = {tris, ntris, vram};
    NsRStats st;
    printf("escena: %zu triángulos\n", ntris);

    if (want(only, "view")) {
        NsRCam c; ns_cam_yaw_pitch(&c, pos, yaw, pitch, fov, vw, vh); c.znear = znear;
        NsRBuf b; ns_buf_alloc(&b, vw, vh); ns_buf_clear(&b, bg[0] / 8, bg[1] / 8, bg[2] / 8);
        ns_render_view(&sc, &c, opts, &b, &st);
        uint8_t *rgb = malloc((size_t)vw * vh * 3); ns_buf_to_rgb8(&b, rgb, 0, 0);
        out_png(dir, "view", vw, vh, rgb, ppm);
        printf("view %dx%d: %.0f ms, dibujados %zu, huecos %.2f%%\n", vw, vh, st.ms, st.tris_drawn, st.hole_frac * 100);
        free(rgb); ns_buf_free(&b);
    }
    if (have_game && want(only, "game")) {
        if (!gw) { gw = rw; gh = rh; }
        NsRCam c; ns_cam_game(&c, game, game + 9, game[12], game[13], game[14], rw, rh, gw, gh); c.znear = znear;
        NsRBuf b; ns_buf_alloc(&b, gw, gh); ns_buf_clear(&b, bg[0] / 8, bg[1] / 8, bg[2] / 8);
        ns_render_view(&sc, &c, opts, &b, &st);
        uint8_t *rgb = malloc((size_t)gw * gh * 3); ns_buf_to_rgb8(&b, rgb, 0, 0);
        out_png(dir, "game", gw, gh, rgb, ppm);
        printf("game %dx%d: %.0f ms, dibujados %zu, huecos %.2f%%\n", gw, gh, st.ms, st.tris_drawn, st.hole_frac * 100);
        if (reff) {
            int w2, h2; uint8_t *ref = ns_read_ppm(reff, &w2, &h2);
            if (!ref) fprintf(stderr, "no se pudo leer %s (P6 8 bits)\n", reff);
            else if (w2 != gw || h2 != gh) fprintf(stderr, "tamaño distinto: ref %dx%d, render %dx%d (use --game-size %dx%d)\n", w2, h2, gw, gh, w2, h2);
            else {
                uint8_t *df = malloc((size_t)gw * gh * 3); NsRDiff d;
                ns_diff_rgb8(rgb, ref, gw, gh, thr, df, &d);
                out_png(dir, "game_diff", gw, gh, df, ppm);
                printf("diff vs ref: error medio %.2f/255, píxeles con |dif|>%d: %.2f%%\n", d.mae, d.thr, d.frac_over * 100);
                free(df);
            }
            free(ref);
        }
        free(rgb); ns_buf_free(&b);
    }
    if (want(only, "pano") || want(only, "cube")) {
        NsRCam pose; ns_cam_yaw_pitch(&pose, pos, yaw, pitch, 90, 64, 64);
        if (have_game) {   /* panorama desde el ojo del motor: posición del ojo = -A^T t, orientación = A */
            memcpy(pose.A, game, 9 * sizeof(float)); memcpy(pose.t, game + 9, 3 * sizeof(float));
        }
        int n = pw / 4; if (n < 16) n = 16;
        NsRBuf faces[6];
        for (int f = 0; f < 6; f++) { ns_buf_alloc(&faces[f], n, n); }
        ns_render_cube(&sc, &pose, n, znear, opts, faces, &st);
        printf("cubemap 6x%d: %.0f ms, dibujados(suma) %zu, huecos medios %.2f%%\n", n, st.ms, st.tris_drawn, st.hole_frac * 100);
        if (want(only, "cube")) {
            uint8_t *cr = malloc((size_t)4 * n * 3 * n * 3);
            ns_cube_cross_rgb8(faces, 0xFF00FF, mark, cr);
            out_png(dir, "cube", 4 * n, 3 * n, cr, ppm); free(cr);
            uint8_t *fr = malloc((size_t)n * n * 3);
            for (int f = 0; f < 6; f++) { char nm[32]; snprintf(nm, sizeof nm, "cube_%d", f); ns_buf_to_rgb8(&faces[f], fr, mark, 0xFF00FF); out_png(dir, nm, n, n, fr, ppm); }
            free(fr);
        }
        if (want(only, "pano")) {
            NsRBuf pano; ns_buf_alloc(&pano, pw, ph);
            ns_cube_to_equirect(faces, &pose, &pano);
            uint8_t *rgb = malloc((size_t)pw * ph * 3);
            ns_buf_to_rgb8(&pano, rgb, mark, 0xFF00FF); out_png(dir, "pano", pw, ph, rgb, ppm);
            if (mark) { ns_buf_to_rgb8(&pano, rgb, 0, 0); out_png(dir, "pano_raw", pw, ph, rgb, ppm); }
            size_t holes = 0; for (size_t i = 0; i < (size_t)pw * ph; i++) holes += pano.iz[i] == 0;
            printf("panorama %dx%d: huecos %.2f%%\n", pw, ph, 100.0 * (double)holes / ((double)pw * ph));
            free(rgb); ns_buf_free(&pano);
        }
        for (int f = 0; f < 6; f++) ns_buf_free(&faces[f]);
    }
    free(tris); free(vram);
    return 0;
}
