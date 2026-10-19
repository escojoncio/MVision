/* camtest: validación dinámica de la cámara VR con el runtime headless y una cabeza sintética.
 *   camtest <exe_parcheado> <frames> <dir_salida>          (disco: MV_DISC; mismo guion de mando que el headless)
 * Opciones por entorno: CAM_HIDE=0 (no ocultar cabeza), CAM_HFOV=100 (grados de la vista previa), CAM_ENGINE_H=0,
 *   CAM_NOVR=1 (no activar la cámara VR: línea base), CAM_PHASE=ini fija el tiempo inicial de las fases (depuración).
 * Fases (t = frames desde que la cámara VR está activa): quieto, emparejado (guiñada alterna +-20 grados por frame),
 * giro 360 grados de guiñada y vaivén de cabeceo +-60 grados. De cada lista de dibujo capturada (geometría de alta
 * precisión) se hace una vista previa de campo ancho re-proyectando con vrcam_delta y se escribe PPM.
 *
 * MODO SERVIDOR DE INSTANTÁNEAS (CAM_SERVER=lista): evita repetir los ~6500 frames hasta la parte jugable en cada prueba.
 *   CAM_SERVER=lista.txt camtest <exe> <frames> <dir_base>
 *   El proceso arranca una vez y ejecuta el guion de arranque hasta el punto de partida: t=0 de la cámara VR (el puente dijo
 *   has_player && in_control durante 8 frames + 20 de margen), o el frame CAM_SNAP_FRAME=N si se define. Desde ahí, por cada línea
 *   de la lista hace fork(): el hijo hereda por copia en escritura el proceso ENTERO (RAM/VRAM/GPU/GTE/SPU emulados, planificador,
 *   pilas de las fibras del juego, estado del puente y de la cámara), carga su guion CAM_SCRIPT, lo ejecuta, escribe en
 *   <dir_base>/<nombre>/ (PPM, ops_*.txt, big2d.log y log.txt = su stderr) y termina; el padre espera (alarm desarmada) y sigue con
 *   el siguiente, y al final sale. Lista: una escenario por línea, "nombre fichero_guion" o solo "fichero_guion" (nombre = base sin
 *   extensión; los repetidos llevan sufijo _2...); '#' comenta. CAM_JOBS=N (def. 1) ejecuta N hijos a la vez.
 *   Arranque: CAM_PAD="inicio:mascara_hex:duracion[:cada],..." sustituye el guion de mando de arranque; CAM_BOOT_SHOT=N vuelca el original cada N frames
 *   antes de la instantánea (boot_fNNNNN.ppm en <dir_base>).
 *   CAM_SERVER_WATCH=1: tras la ronda el padre se queda en la instantánea y espera a que cambie el fichero de lista; al cambiar lo vuelve a
 *   leer y ejecuta la ronda nueva SIN repetir el arranque (iterar guiones en segundos); una línea "quit" lo termina.
 *   Cada hijo imprime al final "[camtest] hash ... RAM/VRAM": dos hijos con el mismo guion deben dar el mismo hash y PPM idénticos.
 *   El hijo comparte el fichero de tarjeta MV_CARD con los demás (si varios escriben, usar CAM_JOBS=1 o tarjetas distintas). */
#include <math.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "bridge.h"
#include "cdrom.h"
#include "cpu.h"
#include "gpu.h"
#include "hle.h"
#include "sys.h"
#include "ref_raster.h"
#include "vr_camera.h"
#include "vr_usermap.h"
#include "vr_scene.h"
#include "hud_emu.h"

extern jmp_buf rt_fatal_jmp;
extern int rt_fatal_armed;

/* ---- guion de mando ---- */
typedef struct { uint32_t start, dur, every; uint16_t mask; } PadEv;
static PadEv pad_ev[16];
static int npad_ev;
static int f_act = -1, cur_t = -1, boot_shot, g_keep_ambig, g_fill_partial, g_filled;

/* ---- guion de inmersión (CAM_SCRIPT=fichero): tras activarse la cámara VR, mando y cabeza por tiempo relativo t ----
 *   pad  t0 t1 mascara_hex          botones pulsados en t0 <= t < t1
 *   head t0 t1 yaw0 yaw1 elev0 elev1   interpolación lineal (grados)
 *   dump t0 t1 paso                 panel PPM p_tNNNN cada `paso` frames
 *   shot t0 t1 paso                 original del juego (o_tNNNN = oráculo gpu.c) y, con la pose propia del motor, la vista previa VR a la misma
 *                                   resolución sin filtro (e_) y con filtro de pantalla (ef_), para comparar el efecto lado a lado
 *                                   Con filtro de pantalla activo (clase 3/3b) escribe además g_tNNNN = escena 3D (e_) + post-proceso del visor (hud_emu_post2) con el ojo = pose del motor, comparable
 *                                   con o_ (el log trae "[goggles] ... MAD"), y, con `dump`, pg_tNNNN = lo mismo en el campo ancho de p_ (la máscara de visión cubre todo el campo).
 *                                   Equipar un objeto sin menús: poke t 800B5F46 <objeto,0> (objeto actual, s16) y poke t <0x800B5F28+(37+objeto)*2> 0100 (cantidad >= 0); objetos: 0 tabaco,
 *                                   1 mira, 2-4 cajas, 5 visión nocturna, 6 térmica, 7 máscara de gas; la mira/marco de 1ª persona necesita además `pad t0 t1 1000` (triángulo) unos 40 fotogramas
 *   poke t addr hex                 escribe bytes (cadena hexadecimal) en la RAM del juego en el frame t (dirección del juego, hex)
 *   pokes t addr 'texto'            escribe una cadena ASCII terminada en cero
 *   gcall t func a0 a1 a2 a3 [a4 [sp]] llama a una función del juego (EXE) en el frame t con esos argumentos (hex; a4 va en la pila, [sp+16]); se
 *                                   guarda y restaura el estado de la CPU; el retorno v0 sale en el log ("[gcall]"). Sirve para probar la tarjeta con
 *                                   las funciones propias del juego sin llegar a su menú
 *   card t                          reabre la tarjeta (MV_CARD) desde el fichero: lo que guardó OTRO proceso/hijo se lee del disco, no de la copia en memoria
 *   peek t addr n                   vuelca n bytes de la RAM en el log; cmp t a b n: compara dos zonas de RAM
 *   keep_ambig 0|1                  (prueba) dibuja también los polígonos AMBIG junto a SAT/BEHIND que vr/scene descarta
 *   fill_partial 0|1                (prueba) quads con 3 vértices de vista limpios (sin SAT/BEHIND/AMBIG/DERIVED) y el 4.º sin posición: lo reconstruye como paralelogramo (v_k = v_a + v_b - v_opuesto, también uv y color)
 *   escena t0 t1 paso [fovh fovv lat]  medición de pérdidas y huecos de la ruta propia (vr_scene tal como la monta el anfitrión): por lista, destino de cada primitiva
 *                                   (emitida / parcial / AMBIG / cerca / detrás / enorme / degenerada), y cobertura de un visor de fovh x fovv grados con la pose de cabeza
 *                                   del instante t+lat (lat en VBlanks: retardo de presentación) para: oráculo (todo lo que el juego emitió con vista completa), escena sin
 *                                   espacio de usuario (como la IPA antigua) y escena en el espacio del usuario. Líneas "[esc]"; resumen "[esc-total]"
 *   turn t grados                  vrcam_turn(grados) en el frame t (giro con el stick; + = izquierda), como hace el anfitrión
 *   end  t                          fin
 * Con guion no hay fases fijas; se registra cada 15 frames posición/estado/oxígeno y las primitivas 2D a pantalla completa. */
typedef struct { int kind, t0, t1; unsigned mask; float a[4]; uint32_t u[7]; char s[160]; } ScrEv;
static ScrEv scr[256]; static int nscr, scr_end = 1 << 30, scr_on;
static void scr_load(const char *path) {
    nscr = 0; scr_end = 1 << 30; scr_on = 0;
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); exit(1); }
    char l[256];
    while (fgets(l, sizeof l, f) && nscr < 256) {
        ScrEv e; memset(&e, 0, sizeof e);
        char k[16];
        if (sscanf(l, "%15s", k) != 1 || k[0] == '#') continue;
        if (!strcmp(k, "pad")) { e.kind = 0; sscanf(l, "%*s %d %d %x", &e.t0, &e.t1, &e.mask); }
        else if (!strcmp(k, "head")) { e.kind = 1; sscanf(l, "%*s %d %d %f %f %f %f", &e.t0, &e.t1, &e.a[0], &e.a[1], &e.a[2], &e.a[3]); }
        else if (!strcmp(k, "dump")) { e.kind = 2; sscanf(l, "%*s %d %d %f", &e.t0, &e.t1, &e.a[0]); }
        else if (!strcmp(k, "fill_partial")) { sscanf(l, "%*s %d", &g_fill_partial); continue; }
        else if (!strcmp(k, "keep_ambig")) { sscanf(l, "%*s %d", &g_keep_ambig); continue; }   /* prueba: dibujar también los AMBIG junto a SAT/BEHIND */
        else if (!strcmp(k, "shot")) { e.kind = 3; sscanf(l, "%*s %d %d %f", &e.t0, &e.t1, &e.a[0]); }
        else if (!strcmp(k, "pokes")) { e.kind = 5; char *q = strstr(l, "pokes"); sscanf(q + 5, "%d %x", &e.t0, &e.u[0]); q = strchr(q + 5, '\''); if (q) { snprintf(e.s, sizeof e.s, "%s", q + 1); char *z = strchr(e.s, '\''); if (z) *z = 0; } }
        else if (!strcmp(k, "poke")) { e.kind = 4; char hx[150] = ""; sscanf(l, "%*s %d %x %149s", &e.t0, &e.u[0], hx); snprintf(e.s, sizeof e.s, "%s", hx); }
        else if (!strcmp(k, "gcall")) { e.kind = 6; sscanf(l, "%*s %d %x %x %x %x %x %x %x", &e.t0, &e.u[0], &e.u[1], &e.u[2], &e.u[3], &e.u[4], &e.u[5], &e.u[6]); }
        else if (!strcmp(k, "peek")) { e.kind = 7; sscanf(l, "%*s %d %x %x", &e.t0, &e.u[0], &e.u[1]); }
        else if (!strcmp(k, "cov")) { e.kind = 10; sscanf(l, "%*s %d %d", &e.t0, &e.t1); }   /* cobertura: vuelca los centroides mundo de los polígonos 3D de cada lista a <salida>/cov.bin */
        else if (!strcmp(k, "card")) { e.kind = 9; sscanf(l, "%*s %d", &e.t0); }
        else if (!strcmp(k, "cmp")) { e.kind = 8; sscanf(l, "%*s %d %x %x %x", &e.t0, &e.u[0], &e.u[1], &e.u[2]); }
        else if (!strcmp(k, "escena")) { e.kind = 10; e.a[1] = 110; e.a[2] = 100; e.a[3] = 3; sscanf(l, "%*s %d %d %f %f %f %f", &e.t0, &e.t1, &e.a[0], &e.a[1], &e.a[2], &e.a[3]); }
        else if (!strcmp(k, "turn")) { e.kind = 11; sscanf(l, "%*s %d %f", &e.t0, &e.a[0]); }   /* giro del mando (vrcam_turn, como el anfitrión) en el frame t */
        else if (!strcmp(k, "end")) { sscanf(l, "%*s %d", &scr_end); continue; }
        else continue;
        scr[nscr++] = e;
    }
    if (nscr >= 256 && fgets(l, sizeof l, f)) fprintf(stderr, "[guion] %s: máximo 256 órdenes, el resto se ignora\n", path);
    fclose(f); scr_on = 1;
}
static void script_pad(uint32_t f) {
    static uint16_t last = 0xFFFF;
    uint16_t b = 0;
    if (f_act >= 0 && scr_on) {
        for (int i = 0; i < nscr; i++) if (scr[i].kind == 0 && cur_t >= scr[i].t0 && cur_t < scr[i].t1) b |= (uint16_t)scr[i].mask;
    } else
    if (f_act >= 0) {   /* con la cámara VR activa: solo la prueba de marcha (adelante) en dos ventanas */
        int t = cur_t;
        if ((t >= 10 && t < 50) || (t >= 1340 && t < 1380)) b |= 1u << 4;
    } else
    for (int i = 0; i < npad_ev; i++) {
        const PadEv *p = &pad_ev[i];
        if (p->every) { if (f >= p->every && f % p->every < p->dur) b |= p->mask; }
        else if (f >= p->start && f < p->start + p->dur) b |= p->mask;
    }
    if (b != last) { pad_set_state(0, b, 0x80, 0x80, 0x80, 0x80); last = b; }
}

/* ---- cabeza sintética ---- */
static float g_yaw, g_elev;   /* grados */
static int head_fn(float m[16], double *t) {
    float y = g_yaw * 3.14159265f / 180, e = g_elev * 3.14159265f / 180, cy = cosf(y), sy = sinf(y), ce = cosf(e), se = sinf(e);
    float X[3] = {cy, 0, -sy}, Y[3] = {sy * se, ce, cy * se}, Z[3] = {sy * ce, -se, cy * ce};
    for (int i = 0; i < 3; i++) { m[i] = X[i]; m[4 + i] = Y[i]; m[8 + i] = Z[i]; }
    m[3] = m[7] = m[11] = 0; m[12] = 0; m[13] = 1.6f; m[14] = 0; m[15] = 1;
    *t = 0;
    return 1;
}

/* ---- imagen ---- */
#define PW 800
#define PH 640
static uint8_t img[PW * PH * 3];
static void write_ppm(const char *dir, const char *name, const uint8_t *rgb, int w, int h) {
    char p[512]; snprintf(p, sizeof p, "%s/%s.ppm", dir, name);
    FILE *f = fopen(p, "wb");
    if (!f) { perror(p); return; }
    fprintf(f, "P6\n%d %d\n255\n", w, h); fwrite(rgb, 1, (size_t)w * h * 3, f); fclose(f);
}

/* ---- muestreo de textura original desde la VRAM ---- */
static const uint16_t *vram;
static int tex_sample(const GpuOp *o, int u, int v, uint16_t *out) {
    u &= 255; v &= 255;
    if (o->twmx || o->twmy || o->twox || o->twoy) {
        u = (u & ~(o->twmx * 8)) | ((o->twox & o->twmx) * 8);
        v = (v & ~(o->twmy * 8)) | ((o->twoy & o->twmy) * 8);
    }
    int ty = (o->ty + v) & 511;
    uint16_t t;
    if (o->tp == 0) {
        uint16_t w = vram[ty * 1024 + ((o->tx + u / 4) & 1023)];
        int idx = (w >> ((u & 3) * 4)) & 15;
        t = vram[(o->cly & 511) * 1024 + ((o->clx + idx) & 1023)];
    } else if (o->tp == 1) {
        uint16_t w = vram[ty * 1024 + ((o->tx + u / 2) & 1023)];
        int idx = (w >> ((u & 1) * 8)) & 255;
        t = vram[(o->cly & 511) * 1024 + ((o->clx + idx) & 1023)];
    } else t = vram[ty * 1024 + ((o->tx + u) & 1023)];
    *out = t;
    return t != 0;
}

static int g_dbg_semi;
typedef struct { float x, y, z, u, v, r, g, b; } PV;
static float g_f, g_cx, g_cy;
static int g_w, g_h;
static uint8_t *g_img, *g_painted;
static uint64_t g_pix_drawn;

static void raster_tri(const GpuOp *o, const PV *a, const PV *b, const PV *c) {
    float sx[3], sy[3], iz[3];
    const PV *p[3] = {a, b, c};
    for (int i = 0; i < 3; i++) { iz[i] = 1.0f / p[i]->z; sx[i] = g_cx + g_f * p[i]->x * iz[i]; sy[i] = g_cy + g_f * p[i]->y * iz[i]; }
    float minx = fminf(sx[0], fminf(sx[1], sx[2])), maxx = fmaxf(sx[0], fmaxf(sx[1], sx[2]));
    float miny = fminf(sy[0], fminf(sy[1], sy[2])), maxy = fmaxf(sy[0], fmaxf(sy[1], sy[2]));
    int x0 = (int)fmaxf(0, floorf(minx)), x1 = (int)fminf(g_w - 1, ceilf(maxx)), y0 = (int)fmaxf(0, floorf(miny)), y1 = (int)fminf(g_h - 1, ceilf(maxy));
    float den = (sy[1] - sy[2]) * (sx[0] - sx[2]) + (sx[2] - sx[1]) * (sy[0] - sy[2]);
    if (fabsf(den) < 1e-6f) return;
    for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) {
        float px = x + 0.5f, py = y + 0.5f;
        float w0 = ((sy[1] - sy[2]) * (px - sx[2]) + (sx[2] - sx[1]) * (py - sy[2])) / den;
        float w1 = ((sy[2] - sy[0]) * (px - sx[2]) + (sx[0] - sx[2]) * (py - sy[2])) / den;
        float w2 = 1 - w0 - w1;
        const float e = -0.002f;
        if (w0 < e || w1 < e || w2 < e) continue;
        float iw = w0 * iz[0] + w1 * iz[1] + w2 * iz[2];
        if (iw <= 0) continue;
        float k0 = w0 * iz[0] / iw, k1 = w1 * iz[1] / iw, k2 = w2 * iz[2] / iw;
        float u = k0 * a->u + k1 * b->u + k2 * c->u, v = k0 * a->v + k1 * b->v + k2 * c->v;
        float cr = k0 * a->r + k1 * b->r + k2 * c->r, cg = k0 * a->g + k1 * b->g + k2 * c->g, cb = k0 * a->b + k1 * b->b + k2 * c->b;
        float R, G, B;
        int blend = (o->flags & GPU_F_SEMI) != 0;
        if (o->flags & GPU_F_TEXTURED) {
            uint16_t t;
            if (!tex_sample(o, (int)floorf(u + 0.001f), (int)floorf(v + 0.001f), &t)) continue;
            if (!(t & 0x8000)) blend = 0;   /* texturizado semitransparente: solo se mezclan los texels con bit 15; el resto es opaco */
            float tr = (t & 31) * 8.0f, tg = ((t >> 5) & 31) * 8.0f, tb = ((t >> 10) & 31) * 8.0f;
            if (o->flags & GPU_F_RAW) { R = tr; G = tg; B = tb; }
            else { R = tr * cr / 128; G = tg * cg / 128; B = tb * cb / 128; }
        } else { R = cr; G = cg; B = cb; }
        uint8_t *q = g_img + ((size_t)y * g_w + x) * 3;
        if (g_dbg_semi && x == 160 && y == 128) fprintf(stderr, "  px(160,128) seq=%u flags=0x%x abr=%d tp=%d tx=%d ty=%d clut=(%d,%d) blend=%d RGB=(%.0f,%.0f,%.0f) dest=(%d,%d,%d) uv=(%.0f,%.0f) col=(%.0f,%.0f,%.0f)\n", o->seq, o->flags, o->abr, o->tp, o->tx, o->ty, o->clx, o->cly, blend, R, G, B, q[0], q[1], q[2], u, v, cr, cg, cb);
        if (blend) {   /* mezcla semitransparente de la GPU emulada sobre lo ya dibujado (B = fondo, F = fuente) */
            float F[3] = {R, G, B};
            for (int k = 0; k < 3; k++) {
                float Bk = q[k], r;
                switch (o->abr) { case 0: r = 0.5f * Bk + 0.5f * F[k]; break; case 1: r = Bk + F[k]; break; case 2: r = Bk - F[k]; break; default: r = Bk + 0.25f * F[k]; }
                F[k] = r < 0 ? 0 : r;
            }
            R = F[0]; G = F[1]; B = F[2];
        }
        q[0] = (uint8_t)(R > 255 ? 255 : R); q[1] = (uint8_t)(G > 255 ? 255 : G); q[2] = (uint8_t)(B > 255 ? 255 : B);
        g_painted[y * g_w + x] = 1; g_pix_drawn++;
    }
}

/* recorta contra z >= near y dibuja */
static void draw_clipped(const GpuOp *o, const PV *t0, const PV *t1, const PV *t2) {
    const float near = 24.0f;
    PV in[3] = {*t0, *t1, *t2}, out[5];
    int n = 0;
    for (int i = 0; i < 3; i++) {
        const PV *a = &in[i], *b = &in[(i + 1) % 3];
        int ia = a->z >= near, ib = b->z >= near;
        if (ia) out[n++] = *a;
        if (ia != ib) {
            float s = (near - a->z) / (b->z - a->z);
            PV m;
            float *pa = (float *)a, *pb = (float *)b, *pm = (float *)&m;
            for (int k = 0; k < 8; k++) pm[k] = pa[k] + (pb[k] - pa[k]) * s;
            m.z = near;
            out[n++] = m;
        }
    }
    for (int i = 1; i + 1 < n; i++) raster_tri(o, &out[0], &out[i], &out[i + 1]);
}


/* Desplazamiento de la GTE por primitiva: los sprites de "resplandor"/halo ponen OFX/OFY en su centro de pantalla y proyectan solo el
 * desplazamiento local (vx,vy = +-1000 a profundidad vz), así que (vx,vy,vz) NO es una posición de vista del mundo. La posición de vista real es
 * X = vx + (OFX_op - OFX_cam)*vz/H (y análogo en Y), con OFX_cam = centro del mundo (la moda de proj_ox+dox sobre los polígonos 3D de la lista). */
static float g_refx, g_refy; static int g_ref_ok;
static void list_ref(const GpuDrawList *l) {
    struct { int x, y, n; } h[16]; int nh = 0;
    for (uint32_t i = 0; i < l->nops; i++) {
        const GpuOp *o = &l->ops[i];
        if (o->type != GPU_OP_POLY || !(o->proj_h > 0) || !o->v[0].vvalid || (o->flags & GPU_F_SEMI)) continue;
        int x = (int)lroundf((o->proj_ox + o->dox) * 2), y = (int)lroundf((o->proj_oy + o->doy) * 2), k;
        for (k = 0; k < nh; k++) if (h[k].x == x && h[k].y == y) { h[k].n++; break; }
        if (k == nh && nh < 16) { h[nh].x = x; h[nh].y = y; h[nh].n = 1; nh++; }
    }
    int best = -1;
    for (int k = 0; k < nh; k++) if (best < 0 || h[k].n > h[best].n) best = k;
    g_ref_ok = best >= 0;
    if (best >= 0) { g_refx = h[best].x * 0.5f; g_refy = h[best].y * 0.5f; }
}
static void vtx_view(const GpuOp *o, const GpuVtx *v, float *x, float *y, float *z) {
    *x = v->vx; *y = v->vy; *z = v->vz;
    if (g_ref_ok && o->proj_h > 0 && getenv("CAM_NO_OFXFIX") == NULL) {
        *x += (o->proj_ox + o->dox - g_refx) * v->vz / o->proj_h;
        *y += (o->proj_oy + o->doy - g_refy) * v->vz / o->proj_h;
    }
}
typedef struct { uint32_t polys, drawn, partial, behind, ambig, clipped, nonvr2d, sat, derived, legacy; } RStat;
static RStat g_tot;   /* acumulado de descartes por causa (se imprime al final) */

/* Re-proyecta una lista con la transformación `d` (NULL = identidad) y el modelo estenopeico (f, cx, cy, w, h). */
static int g_dbg;
static int g_dbg_partial;   /* CAM_PARTIAL=1 */
/* VRAM por época (tramo entre eventos de VRAM de la lista): las texturas se muestrean como estaban al dibujar cada primitiva, no como
 * quedan al final del frame (si no, un sprite cuya textura se sube después del dibujo sale con el contenido del frame siguiente). */
#define MAXSNAP 48
static uint16_t *snap[MAXSNAP]; static uint32_t snap_op[MAXSNAP]; static int nsnap, snap_max_ev;
static RStat render_list(const GpuDrawList *l, const VrCamDelta *d, float f, float cx, float cy, int w, int h, uint8_t *im, uint8_t *pt) {
    g_f = f; g_cx = cx; g_cy = cy; g_w = w; g_h = h; g_img = im; g_painted = pt; g_pix_drawn = 0;
    g_dbg_partial = getenv("CAM_PARTIAL") != NULL;
    memset(im, 0, (size_t)w * h * 3); memset(pt, 0, (size_t)w * h);
    RStat s = {0};
    int ep = 0;
    for (uint32_t i = 0; i < l->nops; i++) {
        const GpuOp *o = &l->ops[i];
        if (nsnap) { while (ep + 1 < nsnap && snap_op[ep + 1] <= i) ep++; vram = snap[ep]; }
        if (o->type != GPU_OP_POLY) continue;
        s.polys++;
        if (g_dbg_semi && (o->flags & GPU_F_SEMI) && o->abr == 1 && o->tx == 640 && o->tp == 1 && o->v[0].vvalid) { fprintf(stderr, "  glow op%u seq=%u nv=%d dox=%d doy=%d pox=%.1f poy=%.1f ph=%.1f ref=(%.1f,%.1f):", i, o->seq, o->nv, o->dox, o->doy, o->proj_ox, o->proj_oy, o->proj_h, g_refx, g_refy); for (int k = 0; k < o->nv; k++) fprintf(stderr, " (%d,%d vf%d vv%d fv%d vx%.0f vy%.0f vz%.0f f(%.1f,%.1f))", o->v[k].x - o->dox, o->v[k].y - o->doy, o->v[k].vflags, o->v[k].vvalid, o->v[k].fvalid, o->v[k].vx, o->v[k].vy, o->v[k].vz, o->v[k].fx - o->dox, o->v[k].fy - o->doy); fprintf(stderr, "\n"); }
        if (g_dbg_semi && (o->flags & GPU_F_SEMI) && o->v[0].vvalid) fprintf(stderr, "  semi3D op%u flags=0x%x abr=%d tp=%d tx=%d ty=%d clut=(%d,%d) nv=%d v0=(%d,%d,vz%.0f) col=%02x%02x%02x\n", i, o->flags, o->abr, o->tp, o->tx, o->ty, o->clx, o->cly, o->nv, o->v[0].x, o->v[0].y, o->v[0].vz, o->v[0].r, o->v[0].g, o->v[0].b);
        int nvalid = 0;
        for (int k = 0; k < o->nv; k++) nvalid += o->v[k].vvalid;
        GpuOp fillop;
        if (g_fill_partial && o->nv == 4 && nvalid == 3) {   /* paralelogramo: el vértice sin posición = suma de sus vecinos - el opuesto */
            int bad = -1, clean = 1;
            for (int k = 0; k < 4; k++) { if (!o->v[k].vvalid) bad = k; else if (o->v[k].vflags) clean = 0; }
            if (bad >= 0 && clean) {
                int c = 3 - bad, a = bad == 0 || bad == 3 ? 1 : 0, b = bad == 0 || bad == 3 ? 2 : 3;
                fillop = *o;
                GpuVtx *fv = &fillop.v[bad];
                const GpuVtx *va = &o->v[a], *vb = &o->v[b], *vc = &o->v[c];
                fv->vx = va->vx + vb->vx - vc->vx; fv->vy = va->vy + vb->vy - vc->vy; fv->vz = va->vz + vb->vz - vc->vz;
                int uu = va->u + vb->u - vc->u, vv = va->v + vb->v - vc->v, rr = va->r + vb->r - vc->r, gg = va->g + vb->g - vc->g, bb = va->b + vb->b - vc->b;
                fv->u = (uint8_t)(uu < 0 ? 0 : uu > 255 ? 255 : uu); fv->v = (uint8_t)(vv < 0 ? 0 : vv > 255 ? 255 : vv);
                fv->r = (uint8_t)(rr < 0 ? 0 : rr > 255 ? 255 : rr); fv->g = (uint8_t)(gg < 0 ? 0 : gg > 255 ? 255 : gg); fv->b = (uint8_t)(bb < 0 ? 0 : bb > 255 ? 255 : bb);
                fv->vvalid = 1; fv->vflags = 0;
                o = &fillop; nvalid = 4; g_filled++;
            }
        }
        if (!nvalid) s.nonvr2d++;
        { int nf = 0; for (int k = 0; k < o->nv; k++) nf += o->v[k].fvalid; if (nf == o->nv) s.legacy++; }   /* regla anterior: 3D = todos fvalid */
        if (nvalid != o->nv) {
            if (nvalid && g_dbg_partial) {   /* CAM_PARTIAL=1: todos los polígonos parcialmente 3D (parte de los vértices con posición de vista) */
                fprintf(stderr, "  [parcial] op%u seq=%u nv=%d flags=0x%x abr=%d tp=%d tex=(%d,%d) col=%02x%02x%02x ph=%.1f pox=%.1f poy=%.1f dox=%d doy=%d:", i, o->seq, o->nv, o->flags, o->abr, o->tp, o->tx, o->ty, o->v[0].r, o->v[0].g, o->v[0].b, o->proj_h, o->proj_ox, o->proj_oy, o->dox, o->doy);
                for (int k = 0; k < o->nv; k++) fprintf(stderr, " [xy=(%d,%d) vv%d fv%d vf%d v=(%.0f,%.0f,%.0f)]", o->v[k].x, o->v[k].y, o->v[k].vvalid, o->v[k].fvalid, o->v[k].vflags, o->v[k].vx, o->v[k].vy, o->v[k].vz);
                fprintf(stderr, "\n");
            }
            if (nvalid) { s.partial++;
                if (getenv("CAM_DBG") && g_dbg && (s.partial % 40) == 1) { fprintf(stderr, "  parcial op%u nv=%d dox=%d doy=%d:", i, o->nv, o->dox, o->doy); for (int k = 0; k < o->nv; k++) fprintf(stderr, " (%d,%d vv%d f%d fl%d z%.0f)", o->v[k].x - o->dox, o->v[k].y - o->doy, o->v[k].vvalid, o->v[k].fvalid, o->v[k].vflags, o->v[k].vz); fprintf(stderr, "\n"); } }
            continue; }
        if (g_dbg_partial) {   /* CAM_PARTIAL=1: polígonos 3D grandes en pantalla (> 5000 px2) con su color y su textura */
            int x0 = 1 << 20, x1 = -(1 << 20), y0 = 1 << 20, y1 = -(1 << 20);
            for (int k = 0; k < o->nv; k++) { if (o->v[k].x < x0) x0 = o->v[k].x; if (o->v[k].x > x1) x1 = o->v[k].x; if (o->v[k].y < y0) y0 = o->v[k].y; if (o->v[k].y > y1) y1 = o->v[k].y; }
            if ((long)(x1 - x0) * (y1 - y0) > 5000) {
                fprintf(stderr, "  [grande3D] op%u seq=%u nv=%d flags=0x%x abr=%d tp=%d tex=(%d,%d) col=%02x%02x%02x bbox=(%d,%d)-(%d,%d) clip=(%d,%d,%d,%d):", i, o->seq, o->nv, o->flags, o->abr, o->tp, o->tx, o->ty, o->v[0].r, o->v[0].g, o->v[0].b, x0, y0, x1, y1, o->cx1, o->cy1, o->cx2, o->cy2);
                for (int k = 0; k < o->nv; k++) fprintf(stderr, " [vf%d v=(%.0f,%.0f,%.0f)]", o->v[k].vflags, o->v[k].vx, o->v[k].vy, o->v[k].vz);
                fprintf(stderr, "\n");
            }
        }
        /* mismas reglas que vr/scene (§28): AMBIG junto a SAT/BEHIND -> descartar; SAT/DERIVED se aceptan; BEHIND se recorta */
        unsigned uf = 0;
        for (int k = 0; k < o->nv; k++) uf |= o->v[k].vflags;
        if ((uf & GPU_VF_AMBIG) && (uf & (GPU_VF_SAT | GPU_VF_BEHIND))) { s.ambig++; if (!g_keep_ambig) continue; }
        if (uf & GPU_VF_SAT) s.sat++;
        if (uf & GPU_VF_DERIVED) s.derived++;
        PV pv[4];
        for (int k = 0; k < o->nv; k++) {
            const GpuVtx *v = &o->v[k];
            float x, y, z; vtx_view(o, v, &x, &y, &z);
            if (d) {
                float X = d->A[0] * x + d->A[1] * y + d->A[2] * z + d->b[0], Y = d->A[3] * x + d->A[4] * y + d->A[5] * z + d->b[1],
                      Z = d->A[6] * x + d->A[7] * y + d->A[8] * z + d->b[2];
                x = X; y = Y; z = Z;
            }
            pv[k] = (PV){x, y, z, v->u, v->v, (o->flags & GPU_F_GOURAUD) ? v->r : o->v[0].r, (o->flags & GPU_F_GOURAUD) ? v->g : o->v[0].g,
                         (o->flags & GPU_F_GOURAUD) ? v->b : o->v[0].b};
            if (!(o->flags & GPU_F_GOURAUD)) { pv[k].r = o->v[0].r; pv[k].g = o->v[0].g; pv[k].b = o->v[0].b; }
        }
        int allb = 1;
        for (int k = 0; k < o->nv; k++) if (pv[k].z >= 24) allb = 0;
        if (allb) { s.behind++; continue; }
        s.drawn++;
        for (int k = 0; k < o->nv; k++) if (pv[k].z < 24) { s.clipped++; break; }
        draw_clipped(o, &pv[0], &pv[1], &pv[2]);
        if (o->nv == 4) draw_clipped(o, &pv[1], &pv[2], &pv[3]);
    }
    g_tot.polys += s.polys; g_tot.drawn += s.drawn; g_tot.partial += s.partial; g_tot.behind += s.behind; g_tot.ambig += s.ambig;
    g_tot.clipped += s.clipped; g_tot.nonvr2d += s.nonvr2d; g_tot.sat += s.sat; g_tot.derived += s.derived; g_tot.legacy += s.legacy;
    return s;
}

/* ---- estado de la prueba ---- */
static RefR *r1;
static const char *outdir;
static uint32_t max_frames;
static int hist_n;
static struct { int f; int x, z; float view; int po; } hist[20000];
static uint32_t lists_seen, pair_ok, none_rec;
static double sum_cov; static int n_cov;
static float hfov_prev = 100.0f;
static GpuDisplay last_disp;

static void pose_at(int t, float *gy, float *ge) {
    *gy = *ge = 0;
    if (scr_on) {
        for (int i = 0; i < nscr; i++) if (scr[i].kind == 1 && t >= scr[i].t0 && t < scr[i].t1) {
            float k = scr[i].t1 > scr[i].t0 ? (float)(t - scr[i].t0) / (scr[i].t1 - scr[i].t0) : 0;
            *gy = scr[i].a[0] + (scr[i].a[1] - scr[i].a[0]) * k; *ge = scr[i].a[2] + (scr[i].a[3] - scr[i].a[2]) * k;
        }
        return;
    }
    if (t < 120) return;
    if (t < 240) { if (t >= 228) return;   /* 12 frames quietos (el registro de cámara de la lista dibujada en t va ~5 frames por detrás) antes del barrido: el panel t=240 se dibuja con la pose de cabeza que se muestra */
        int s = (t >> 1) & 1; *gy = s ? 20.0f : -20.0f; *ge = s ? 30.0f : -30.0f; return; }
    if (t < 960) { *gy = (float)(t - 240) * 0.5f; return; }
    if (t < 1320) { *ge = 60.0f * sinf((float)(t - 960) * 6.2831853f / 360.0f); return; }
    *gy = 90.0f;
}
static void phase_pose(int t, int f) { (void)f; pose_at(t, &g_yaw, &g_elev); }

static double mad_img(const uint8_t *a, const uint8_t *b, int n, const uint8_t *mask) {
    double s = 0; int cnt = 0;
    for (int i = 0; i < n; i++) {
        if (mask && !mask[i]) continue;
        for (int k = 0; k < 3; k++) s += abs((int)a[3 * i + k] - (int)b[3 * i + k]);
        cnt++;
    }
    return cnt ? s / (3.0 * cnt) : -1;
}

static VrCamRecord prev_copy, cur_copy;
static float pair_yaw0;
static int shot_this(int t) {
    if (!scr_on) return 0;
    for (int i = 0; i < nscr; i++) if (scr[i].kind == 3 && t >= scr[i].t0 && t < scr[i].t1 && scr[i].a[0] >= 1 && (t - scr[i].t0) % (int)scr[i].a[0] == 0) return 1;
    return 0;
}
static int dump_this(int t) {
    if (scr_on) {
        for (int i = 0; i < nscr; i++) if (scr[i].kind == 2 && t >= scr[i].t0 && t < scr[i].t1 && scr[i].a[0] >= 1 && (t - scr[i].t0) % (int)scr[i].a[0] == 0) return 1;
        return 0;
    }
    if (t == 60 || t == 61 || t == 130 || t == 131) return 1;
    if (t >= 240 && t < 960 && (t - 240) % 45 == 0) return 1;
    if (t >= 960 && t < 1320 && (t - 960) % 45 == 0) return 1;
    return 0;
}

/* ---- consistencia del mundo: el mismo vértice estático, visto desde dos cámaras distintas, debe dar la misma posición mundo ---- */
typedef struct { uint64_t key; float w[4][3]; int nv; } WE;
static WE *wprev, *wcur; static size_t wnp, wnc, wcap;
static double *werr; static size_t nwerr, wecap;
static int wc_inv3(const float a[9], float o[9]) {
    float c00 = a[4] * a[8] - a[5] * a[7], c01 = a[5] * a[6] - a[3] * a[8], c02 = a[3] * a[7] - a[4] * a[6];
    float det = a[0] * c00 + a[1] * c01 + a[2] * c02;
    if (fabsf(det) < 1e-6f) return 0;
    float id = 1.0f / det;
    o[0] = c00 * id; o[1] = (a[2] * a[7] - a[1] * a[8]) * id; o[2] = (a[1] * a[5] - a[2] * a[4]) * id;
    o[3] = c01 * id; o[4] = (a[0] * a[8] - a[2] * a[6]) * id; o[5] = (a[2] * a[3] - a[0] * a[5]) * id;
    o[6] = c02 * id; o[7] = (a[1] * a[6] - a[0] * a[7]) * id; o[8] = (a[0] * a[4] - a[1] * a[3]) * id;
    return 1;
}
static int wecmp(const void *a, const void *b) { uint64_t x = ((const WE *)a)->key, y = ((const WE *)b)->key; return x < y ? -1 : x > y; }
static void world_collect(const GpuDrawList *l, const VrCamRecord *r) {
    if (!wcap) { wcap = 1 << 14; wprev = malloc(wcap * sizeof(WE)); wcur = malloc(wcap * sizeof(WE)); werr = malloc((wecap = 1 << 22) * sizeof(double)); }
    float Mt[9] = {r->M[0], r->M[3], r->M[6], r->M[1], r->M[4], r->M[7], r->M[2], r->M[5], r->M[8]}, Q[9];
    if (!wc_inv3(Mt, Q)) return;
    WE *tmp = wprev; wprev = wcur; wcur = tmp; wnp = wnc; wnc = 0;
    static uint32_t occ[1 << 16]; memset(occ, 0, sizeof occ);
    for (uint32_t i = 0; i < l->nops && wnc < wcap; i++) {
        const GpuOp *o = &l->ops[i];
        if (o->type != GPU_OP_POLY) continue;
        int ok = 1; for (int k = 0; k < o->nv; k++) if (!o->v[k].vvalid || o->v[k].vz < 200) ok = 0;
        if (!ok) continue;
        uint64_t h = 1469598103934665603ull;
        #define WMIX(x) h = (h ^ (uint64_t)(x)) * 1099511628211ull
        WMIX(o->nv); WMIX(o->tp); WMIX(o->tx); WMIX(o->ty); WMIX(o->clx); WMIX(o->cly);
        for (int k = 0; k < o->nv; k++) { WMIX(o->v[k].u); WMIX(o->v[k].v); }
        uint32_t *c = &occ[(h >> 7) & 0xFFFF]; h = h * 31 + (*c)++;
        WE *e = &wcur[wnc++]; e->key = h; e->nv = o->nv;
        for (int k = 0; k < o->nv; k++) for (int a = 0; a < 3; a++) {
            float v3[3] = {o->v[k].vx, o->v[k].vy, o->v[k].vz};
            e->w[k][a] = (float)r->eye[a] + Q[3 * a] * v3[0] + Q[3 * a + 1] * v3[1] + Q[3 * a + 2] * v3[2];
        }
    }
    qsort(wcur, wnc, sizeof(WE), wecmp);
    size_t a = 0, b = 0;
    while (a < wnp && b < wnc) {
        if (wprev[a].key < wcur[b].key) a++; else if (wprev[a].key > wcur[b].key) b++;
        else {
            for (int k = 0; k < wcur[b].nv && nwerr < wecap; k++) {
                double d = 0; for (int q = 0; q < 3; q++) { double x = wprev[a].w[k][q] - wcur[b].w[k][q]; d += x * x; }
                werr[nwerr++] = sqrt(d);
            }
            a++; b++;
        }
    }
}
static int dcmp(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
static void world_report(const char *tag) {
    if (!nwerr) { fprintf(stderr, "[camtest] consistencia mundo %s: sin pares\n", tag); return; }
    qsort(werr, nwerr, sizeof(double), dcmp);
    size_t lt20 = 0; for (size_t i = 0; i < nwerr; i++) if (werr[i] < 20) lt20++;
    fprintf(stderr, "[camtest] consistencia del mundo %s: %zu vértices emparejados entre frames consecutivos; error de posición mundo (unidades ~mm): mediana %.1f, p75 %.1f, p90 %.1f; %.1f%% < 20\n", tag, nwerr,
            werr[nwerr / 2], werr[nwerr * 3 / 4], werr[nwerr * 9 / 10], 100.0 * lt20 / nwerr);
}
static BridgeState bs;
/* vr_scene (mismas reglas que el renderer): estadísticas acumuladas por listas fuera/bajo el agua */
typedef struct { uint32_t lists, polys, poly3d, poly2d, tris3d, drop_partial, drop_near, drop_ambig, drop_behind, poly_behind, clipped, n_filter_rects, n_filter_polys, n_filter_cover, filter_tris, n_clear, lists_filter; } SceneAcc;
static SceneAcc sacc[2];
static VrFilter last_filter; static int last_filter_ok; static uint32_t last_ofs_fix;
static uint32_t ref_chk, ref_bad, ofs_chk, ofs_bad;   /* vr_scene (centro del mundo, polígonos corregidos) vs la implementación propia de este programa */
static void scene_acc(const GpuDrawList *l) {
    VrSceneOpts so = vr_scene_opts_default();
    so.screen_filter = 1;   // la vista previa aplica el filtro de pantalla (clase 3)
    VrScene sc;
    if (vr_scene_build(l, &bs, &so, &sc) != 0) return;
    SceneAcc *a = &sacc[bs.in_water ? 1 : 0];
    a->lists++; a->polys += sc.stats.n_poly; a->poly3d += sc.stats.n_poly3d; a->poly2d += sc.stats.n_poly2d; a->tris3d += sc.stats.n_tris3d;
    a->drop_partial += sc.stats.drop_partial; a->drop_near += sc.stats.drop_near; a->drop_ambig += sc.stats.drop_ambig; a->drop_behind += sc.stats.drop_behind;
    a->poly_behind += sc.stats.n_poly_behind; a->clipped += sc.stats.n_clipped; a->n_filter_rects += sc.stats.n_filter_rects; a->n_filter_polys += sc.stats.n_filter_polys;
    a->n_filter_cover += sc.stats.n_filter_cover; a->filter_tris += sc.stats.n_filter_tris; a->n_clear += sc.stats.n_clear; a->lists_filter += sc.filter.active;
    last_filter = sc.filter; last_filter_ok = 1; last_ofs_fix = sc.stats.n_ofs_fix;
}
/* Aplica el filtro de pantalla (clase 3) a una imagen w x h con proyección estenopeica: eh = distancia focal del motor en px nativos, fp = la de
 * la imagen, cy_row = fila nativa del centro óptico, cy_img = fila de la imagen donde cae ese centro (h/2 en la vista ancha; cy_row si la imagen
 * tiene la proyección del motor). Por fila: tomar la fila equivalente del motor r, su desplazamiento d(r) y muestrear la fila de imagen del ángulo
 * r + d(r); luego tinte. seq=1 emula el original, que copia el búfer sobre sí mismo de arriba abajo: si d < 0 la fila fuente ya está procesada
 * (tinte aplicado dos veces); seq=0 lee siempre la imagen sin filtrar (una sola pasada). */
static void apply_filter_img2(uint8_t *im, int w, int h, const VrFilter *f, float eh, float cy_row, float fp, float cy_img, int seq) {
    uint8_t *tmp = malloc((size_t)w * h * 3);
    memcpy(tmp, im, (size_t)w * h * 3);
    const uint8_t *src = seq ? im : tmp;
    for (int y = 0; y < h; y++) {
        float r = (y - cy_img) * eh / fp + cy_row;
        int ri = (int)floorf(r);
        float d = (ri >= 0 && ri < VR_FILTER_ROWS && f->rowcov[ri]) ? f->dv[ri] : 0;
        int ys = (int)lroundf(cy_img + (r + d - cy_row) * fp / eh);
        if (ys < 0) ys = 0; if (ys >= h) ys = h - 1;
        for (int x = 0; x < w; x++) for (int k = 0; k < 3; k++) {
            float v = src[((size_t)ys * w + x) * 3 + k] * f->tint[k];
            im[((size_t)y * w + x) * 3 + k] = (uint8_t)(v > 255 ? 255 : v);
        }
    }
    free(tmp);
}
static void apply_filter_img(uint8_t *im, int w, int h, const VrFilter *f, float eh, float cy_row, float fp) { apply_filter_img2(im, w, h, f, eh, cy_row, fp, h / 2.0f, 0); }
static void apply_filter_preview(uint8_t *im, const VrFilter *f, float eh, float cy_row, float fp) { apply_filter_img(im, PW, PH, f, eh, cy_row, fp); }
/* Primitivas 2D (sin posición de vista) que cubren >= 40 % del área mostrada: candidatas a "filtro de pantalla" (kind 3 de vr/scene). */
static FILE *big_log; static uint32_t big_lists, big_water_lists, big_air_lists, big_water_ops, big_air_ops;
static void big2d_scan(const GpuDrawList *l, int t, int f) {
    if (!big_log) { char p[512]; snprintf(p, sizeof p, "%s/big2d.log", outdir); big_log = fopen(p, "w"); }
    if (!big_log || l->disp.w <= 0 || l->disp.h <= 0) return;
    float area_d = (float)l->disp.w * l->disp.h;
    int nb = 0, wtr = bs.in_water;
    for (uint32_t i = 0; i < l->nops; i++) {
        const GpuOp *o = &l->ops[i];
        int x0 = 1 << 20, y0 = 1 << 20, x1 = -(1 << 20), y1 = -(1 << 20);
        if (o->type == GPU_OP_RECT) { x0 = o->v[0].x; y0 = o->v[0].y; x1 = x0 + o->rw; y1 = y0 + o->rh; }
        else if (o->type == GPU_OP_POLY) {
            int nv = 0; for (int k = 0; k < o->nv; k++) nv += o->v[k].vvalid;
            if (nv) continue;
            for (int k = 0; k < o->nv; k++) { if (o->v[k].x < x0) x0 = o->v[k].x; if (o->v[k].x > x1) x1 = o->v[k].x; if (o->v[k].y < y0) y0 = o->v[k].y; if (o->v[k].y > y1) y1 = o->v[k].y; }
        } else continue;
        int cx0 = x0 < o->cx1 ? o->cx1 : x0, cx1 = x1 > o->cx2 + 1 ? o->cx2 + 1 : x1, cy0 = y0 < o->cy1 ? o->cy1 : y0, cy1 = y1 > o->cy2 + 1 ? o->cy2 + 1 : y1;
        if (cx1 <= cx0 || cy1 <= cy0) continue;
        float a = (float)(cx1 - cx0) * (cy1 - cy0);
        if (a < 0.40f * area_d) continue;
        nb++;
        fprintf(big_log, "t=%d f=%d water=%d o2=%d list=%u op=%u/%u type=%d nv=%d flags=0x%x abr=%d tp=%d box=(%d,%d)-(%d,%d) clip=(%d,%d)-(%d,%d) cov=%.0f%% col0=%02x%02x%02x col1=%02x%02x%02x uv0=(%d,%d) disp=(%d,%d,%d,%d) seq=%u\n",
                t, f, wtr, bs.o2, l->index, i, l->nops, o->type, o->nv, o->flags, o->abr, o->tp, x0, y0, x1, y1, o->cx1, o->cy1, o->cx2, o->cy2, 100.0 * a / area_d,
                o->v[0].r, o->v[0].g, o->v[0].b, o->v[1].r, o->v[1].g, o->v[1].b, o->v[0].u, o->v[0].v, l->disp.x, l->disp.y, l->disp.w, l->disp.h, o->seq);
    }
    big_lists++;
    if (wtr) { big_water_lists++; big_water_ops += nb; } else { big_air_lists++; big_air_ops += nb; }
}

static int is_vram_event(int t) { return t == GPU_OP_FILL || t == GPU_OP_VCOPY || t == GPU_OP_UPLOAD; }
static void snap_take(uint32_t op, const uint16_t *src) {
    if (nsnap >= MAXSNAP) return;
    if (!snap[nsnap]) snap[nsnap] = malloc(1024 * 512 * 2);
    memcpy(snap[nsnap], src, 1024 * 512 * 2); snap_op[nsnap++] = op;
}
static void apply_part(const GpuDrawList *l, uint32_t a, uint32_t b) {
    GpuDrawList s = *l; s.ops = l->ops + a; s.nops = b - a; s.vram_init = a == 0 ? l->vram_init : NULL;
    if (b > a || s.vram_init) ref_apply(r1, &s, 1);
}
/* Aplica la lista al oráculo por tramos, guardando la VRAM de cada época; si kstrip > 0 captura la imagen mostrada justo antes de esa op. */
static void apply_epochs(const GpuDrawList *l, int kstrip, uint8_t *pre, int *pw, int *ph) {
    nsnap = 0;
    if (l->vram_init) snap_take(0, l->vram_init); else snap_take(0, ref_vram(r1));
    uint32_t i = 0, nev = 0;
    while (i < l->nops) {
        uint32_t e = i; while (e < l->nops && !is_vram_event(l->ops[e].type)) e++;
        uint32_t a = i;
        if (kstrip > 0 && (uint32_t)kstrip >= a && (uint32_t)kstrip < e) {
            apply_part(l, a, (uint32_t)kstrip); a = (uint32_t)kstrip;
            int w, h; const uint8_t *q = ref_present(r1, &l->disp, &w, &h);
            if (w <= 640 && h <= 512) { memcpy(pre, q, (size_t)w * h * 3); *pw = w; *ph = h; }
        }
        apply_part(l, a, e);
        if (e < l->nops) { apply_part(l, e, e + 1); nev++; snap_take(e + 1, ref_vram(r1)); }
        i = e + 1;
    }
    if (nev > (uint32_t)snap_max_ev) snap_max_ev = (int)nev;
}
/* ===================================== §49: medición de la ruta propia (pérdidas y huecos) ============================== * Una lista dibujada llega al visor por vr_scene (montada como hace vr/host: con el registro de cámara de la lista). Aquí se cuenta,
 * por lista, qué ha emitido el juego (primitivas con posición de vista) y qué ha llegado a la escena, y se mide la cobertura de un visor
 * de fovh x fovv grados con la cabeza donde esté en el instante de presentación (t + lat) para tres casos:
 *   E  oráculo      todo lo que el juego emitió con posición de vista completa, re-proyectado con vrcam_delta exacto a la pose de presentación;
 *   Sa escena "vieja"  VrScene en ejes del juego (sin espacio de usuario), tal como la dibuja la IPA anterior (mundo sin girar);
 *   Sn escena nueva    VrScene en el espacio del usuario (opts.user_map_on).
 * Huecos = píxeles que E pinta y la escena no (pérdida de nuestra ruta) o, comparando varias ejecuciones, lo que el motor no emitió. */
#define ESC_MAXW 440
static float esc_f; static int esc_w, esc_h;
static uint8_t esc_mask[3][ESC_MAXW * ESC_MAXW];
static float esc_hm[16];   /* cabeza de presentación */
static void esc_poly(uint8_t *m, const float *p /* n x {x,y,d} en la vista de la cabeza, m */, int n) {
    float in[8][3], o[10][3]; int k = 0;
    for (int i = 0; i < n; i++) memcpy(in[i], p + 3 * i, sizeof in[i]);
    const float zn = 0.05f;
    for (int i = 0; i < n; i++) {
        const float *a = in[i], *b = in[(i + 1) % n];
        int ia = a[2] >= zn, ib = b[2] >= zn;
        if (ia) memcpy(o[k++], a, 12);
        if (ia != ib) { float t = (zn - a[2]) / (b[2] - a[2]); for (int c = 0; c < 3; c++) o[k][c] = a[c] + (b[c] - a[c]) * t; o[k][2] = zn; k++; }
    }
    if (k < 3) return;
    float sx[10], sy[10];
    for (int i = 0; i < k; i++) { sx[i] = esc_w * 0.5f + esc_f * o[i][0] / o[i][2]; sy[i] = esc_h * 0.5f - esc_f * o[i][1] / o[i][2]; }
    for (int i = 1; i + 1 < k; i++) {
        float x0 = sx[0], y0 = sy[0], x1 = sx[i], y1 = sy[i], x2 = sx[i + 1], y2 = sy[i + 1];
        float den = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2);
        if (fabsf(den) < 1e-6f) continue;
        int bx0 = (int)fmaxf(0, floorf(fminf(x0, fminf(x1, x2)))), bx1 = (int)fminf(esc_w - 1, ceilf(fmaxf(x0, fmaxf(x1, x2))));
        int by0 = (int)fmaxf(0, floorf(fminf(y0, fminf(y1, y2)))), by1 = (int)fminf(esc_h - 1, ceilf(fmaxf(y0, fmaxf(y1, y2))));
        for (int y = by0; y <= by1; y++) for (int x = bx0; x <= bx1; x++) {
            float px = x + 0.5f, py = y + 0.5f;
            float w0 = ((y1 - y2) * (px - x2) + (x2 - x1) * (py - y2)) / den, w1 = ((y2 - y0) * (px - x2) + (x0 - x2) * (py - y2)) / den, w2 = 1 - w0 - w1;
            if (w0 >= -0.02f && w1 >= -0.02f && w2 >= -0.02f) m[y * esc_w + x] = 1;
        }
    }
}
/* punto del espacio del usuario (m) -> vista de la cabeza de presentación (x derecha, y arriba, d delante) */
static void esc_head_view(const float U[3], float *o) {
    float d[3] = {U[0] - esc_hm[12], U[1] - esc_hm[13], U[2] - esc_hm[14]};
    o[0] = esc_hm[0] * d[0] + esc_hm[1] * d[1] + esc_hm[2] * d[2];
    o[1] = esc_hm[4] * d[0] + esc_hm[5] * d[1] + esc_hm[6] * d[2];
    o[2] = -(esc_hm[8] * d[0] + esc_hm[9] * d[1] + esc_hm[10] * d[2]);
}
static void esc_mask_scene(uint8_t *m, const VrScene *sc) {
    memset(m, 0, (size_t)esc_w * esc_h);
    for (uint32_t b = 0; b < sc->nb; b++) {
        if (sc->b[b].kind > 1) continue;
        for (uint32_t i = sc->b[b].first; i + 2 < sc->b[b].first + sc->b[b].count; i += 3) {
            float p[9];
            for (int k = 0; k < 3; k++) esc_head_view(sc->v[i + k].pos, p + 3 * k);
            esc_poly(m, p, 3);
        }
    }
}
static uint8_t esc_fate[1 << 17];
static void esc_set_head(int t, float *yawD, float *elD) {
    float gy, ge; pose_at(t, &gy, &ge);
    float y = gy * 3.14159265f / 180, e = ge * 3.14159265f / 180, cy = cosf(y), sy = sinf(y), ce = cosf(e), se = sinf(e);
    float X[3] = {cy, 0, -sy}, Y[3] = {sy * se, ce, cy * se}, Z[3] = {sy * ce, -se, cy * ce};
    float *m = esc_hm;
    for (int i = 0; i < 3; i++) { m[i] = X[i]; m[4 + i] = Y[i]; m[8 + i] = Z[i]; }
    m[3] = m[7] = m[11] = 0; m[12] = 0; m[13] = 1.6f; m[14] = 0; m[15] = 1;
    vrcam_head_angles(m, yawD, elD);
}
/* escena como la monta vr/host (build_one): ojo y ejes del REGISTRO de la lista; origen bajo el ojo VR */
static const VrScene *esc_build(const GpuDrawList *l, const VrCamRecord *r, int user_on) {
    static VrScene sc;
    BridgeState b2 = bs;
    VrSceneOpts o = vr_scene_opts_default();
    o.screen_filter = 1;
    for (int i = 0; i < 3; i++) { b2.eye_pos[i] = r->eye[i]; for (int j = 0; j < 3; j++) b2.eye_axes[i][j] = (int16_t)lrintf(r->M[3 * i + j] * 4096.0f); }
    if (r->active) {
        float org[3]; vr_scene_player_origin(&b2, &o, org);
        o.origin_mode = 1; o.origin_game[0] = (float)r->eye[0]; o.origin_game[1] = org[1]; o.origin_game[2] = (float)r->eye[2];
    }
    if (getenv("CAM_ESC_LEGACY")) { o.engine_near_clip = 1; o.recon_partial = 0; o.recon_ambig = 0; }   /* opciones previas a §49 */
    if (user_on && r->active) vrcam_scene_user_map(&o, r, o.origin_game[1]);
    vr_scene_set_fate(esc_fate, sizeof esc_fate);
    int rc = vr_scene_build(l, &b2, &o, &sc);
    vr_scene_set_fate(NULL, 0);
    return rc == 0 ? &sc : NULL;
}
typedef struct {
    uint32_t recon, unclip;
    uint32_t lists, game_ops;                 /* primitivas con algún vértice de vista */
    uint32_t fate[VR_FATE_COUNT];
    uint32_t part_h[5][5][16];                /* parciales: [nv][nvalid][vflags] */
    uint32_t amb_h[16];
    double cov_e, cov_a, cov_n, hole_a, hole_n; uint32_t n_cov;
    double off_h_abs, off_h_max, off_v_abs; uint32_t n_off;
} EscAcc;
static EscAcc eacc;
static int esc_this(int t, float a[4]) {
    if (!scr_on) return 0;
    for (int i = 0; i < nscr; i++) if (scr[i].kind == 10 && t >= scr[i].t0 && t < scr[i].t1 && scr[i].a[0] >= 1 && (t - scr[i].t0) % (int)scr[i].a[0] == 0) { memcpy(a, scr[i].a, 4 * sizeof(float)); return 1; }
    return 0;
}
static void esc_list(const GpuDrawList *l, const VrCamRecord *r, int t, const float a[4]) {
    float fovh = a[1], fovv = a[2]; int lat = (int)a[3];
    esc_f = (ESC_MAXW * 0.5f) / tanf(fovh * 3.14159265f / 360.0f);
    esc_w = ESC_MAXW; esc_h = (int)lroundf(2 * esc_f * tanf(fovv * 3.14159265f / 360.0f)); if (esc_h > ESC_MAXW) esc_h = ESC_MAXW; if (esc_h < 8) esc_h = 8;
    float yawD, elD; esc_set_head(t + lat, &yawD, &elD);
    /* destino de cada primitiva (escena nueva) */
    const VrScene *sn = esc_build(l, r, 1);
    if (!sn) return;
    uint32_t fc[VR_FATE_COUNT] = {0}, go = 0;
    for (uint32_t i = 0; i < l->nops && i < sizeof esc_fate; i++) {
        const GpuOp *o = &l->ops[i];
        if (o->type != GPU_OP_POLY) continue;
        int nvv = 0; unsigned uf = 0; for (int k = 0; k < o->nv; k++) { nvv += o->v[k].vvalid; if (o->v[k].vvalid) uf |= o->v[k].vflags; }
        if (!nvv) continue;
        go++; fc[esc_fate[i]]++;
        if (esc_fate[i] == VR_FATE_PARTIAL) eacc.part_h[o->nv & 3 ? o->nv : 4][nvv & 3 ? nvv : 4][uf & 15]++;
        if (esc_fate[i] == VR_FATE_AMBIG) eacc.amb_h[uf & 15]++;
        static int dumped; if (getenv("CAM_ESC_DUMP") && dumped < 120 && esc_fate[i] >= VR_FATE_PARTIAL && esc_fate[i] <= VR_FATE_BEHIND) {
            dumped++;
            fprintf(stderr, "  [perdida] op%u fate=%d nv=%d flags=0x%x abr=%d tp=%d tex=(%d,%d) ph=%.0f ox=%.0f oy=%.0f dox=%d doy=%d:", i, esc_fate[i], o->nv, o->flags, o->abr, o->tp, o->tx, o->ty, o->proj_h, o->proj_ox, o->proj_oy, o->dox, o->doy);
            for (int k = 0; k < o->nv; k++) fprintf(stderr, " [xy=(%d,%d) vv%d vf%d v=(%.0f,%.0f,%.0f)]", o->v[k].x, o->v[k].y, o->v[k].vvalid, o->v[k].vflags, o->v[k].vx, o->v[k].vy, o->v[k].vz);
            fprintf(stderr, "\n");
        }
    }
    /* cobertura: escena nueva */
    esc_mask_scene(esc_mask[2], sn);
    const VrScene *sa = esc_build(l, r, 0);
    if (!sa) return;
    esc_mask_scene(esc_mask[1], sa);
    /* oráculo: todo polígono con todos los vértices de vista, re-proyectado con la pose de presentación */
    VrCamDelta d;
    memset(esc_mask[0], 0, (size_t)esc_w * esc_h);
    if (vrcam_delta(r, esc_hm, &d)) {
        for (uint32_t i = 0; i < l->nops; i++) {
            const GpuOp *o = &l->ops[i];
            if (o->type != GPU_OP_POLY) continue;
            int ok = 1; for (int k = 0; k < o->nv; k++) ok &= o->v[k].vvalid;
            if (!ok) continue;
            float p[4][3];
            for (int k = 0; k < o->nv; k++) {
                float x, y, z; vtx_view(o, &o->v[k], &x, &y, &z);
                float X = d.A[0] * x + d.A[1] * y + d.A[2] * z + d.b[0], Y = d.A[3] * x + d.A[4] * y + d.A[5] * z + d.b[1], Z = d.A[6] * x + d.A[7] * y + d.A[8] * z + d.b[2];
                p[k][0] = X * 0.001f; p[k][1] = -Y * 0.001f; p[k][2] = Z * 0.001f;
            }
            esc_poly(esc_mask[0], &p[0][0], 3);
            if (o->nv == 4) { float q[9]; memcpy(q, p[1], 9 * sizeof(float)); esc_poly(esc_mask[0], q, 3); }
        }
    }
    uint32_t ne = 0, ina = 0, inn = 0, ha = 0, hn = 0, npx = (uint32_t)(esc_w * esc_h);
    for (uint32_t i = 0; i < npx; i++) { ne += esc_mask[0][i]; ina += esc_mask[1][i]; inn += esc_mask[2][i]; ha += esc_mask[0][i] && !esc_mask[1][i]; hn += esc_mask[0][i] && !esc_mask[2][i]; }
    /* desfase entre el eje del motor al construir (en el espacio del usuario) y la cabeza de presentación */
    float plan_yaw = r->yaw_deg - r->theta_play_deg, plan_el = -r->pitch_deg;
    float offh = fmodf(yawD - plan_yaw + 540.0f, 360.0f) - 180.0f, offv = elD - plan_el;
    eacc.recon += sn->stats.n_recon; eacc.unclip += sn->stats.n_unclipped;
    eacc.lists++; eacc.game_ops += go; for (int k = 0; k < VR_FATE_COUNT; k++) { eacc.fate[k] += fc[k]; }
    eacc.cov_e += 100.0 * ne / npx; eacc.cov_a += 100.0 * ina / npx; eacc.cov_n += 100.0 * inn / npx;
    eacc.hole_a += 100.0 * ha / npx; eacc.hole_n += 100.0 * hn / npx; eacc.n_cov++;
    eacc.off_h_abs += fabsf(offh); eacc.off_v_abs += fabsf(offv); if (fabsf(offh) > eacc.off_h_max) eacc.off_h_max = fabsf(offh); eacc.n_off++;
    uint32_t e3 = fc[VR_FATE_3D] + fc[VR_FATE_RECON];
    fprintf(stderr, "[esc] t=%d cabeza(yaw %.1f el %.1f) eje-motor(yaw %.1f el %.1f) desfase(h %.1f v %.1f) theta=%.1f | juego %u -> escena %u (%.1f%%) 2D-no-contada | perdidas: parcial %u ambig %u cerca %u detras %u enorme %u degen %u nan %u deriv %u | cobertura%%: oraculo %.1f vieja %.1f nueva %.1f | huecos%% (oraculo y no escena): vieja %.1f nueva %.1f\n",
            t, yawD, elD, plan_yaw, plan_el, offh, offv, r->theta_play_deg, go, e3, go ? 100.0 * e3 / go : 100.0, fc[VR_FATE_PARTIAL], fc[VR_FATE_AMBIG], fc[VR_FATE_NEAR], fc[VR_FATE_BEHIND],
            fc[VR_FATE_HUGE], fc[VR_FATE_DEGEN], fc[VR_FATE_NAN], fc[VR_FATE_DERIVED], 100.0 * ne / npx, 100.0 * ina / npx, 100.0 * inn / npx, 100.0 * ha / npx, 100.0 * hn / npx);
    if (getenv("CAM_ESC_PPM") && (eacc.lists % 8) == 1) {   /* [oráculo | vieja | nueva] */
        static uint8_t im[ESC_MAXW * ESC_MAXW * 3 * 3];
        int W3 = esc_w * 3;
        for (int y = 0; y < esc_h; y++) for (int x = 0; x < esc_w; x++) for (int c = 0; c < 3; c++) {
            uint8_t v = esc_mask[c][y * esc_w + x];
            uint8_t *q = im + ((size_t)y * W3 + c * esc_w + x) * 3;
            q[0] = v ? 200 : 20; q[1] = v ? 200 : 20; q[2] = v ? 200 : 20;
            if (c > 0 && esc_mask[0][y * esc_w + x] && !v) { q[0] = 255; q[1] = 40; q[2] = 40; }   /* hueco: lo ve el oráculo y falta en la escena */
        }
        char nm[64]; snprintf(nm, sizeof nm, "esc_t%04d", t); write_ppm(outdir, nm, im, W3, esc_h);
    }
}
static void esc_report(void) {
    if (!eacc.lists) return;
    EscAcc *e = &eacc;
    fprintf(stderr, "[esc-total] listas %u: primitivas del juego con posición de vista %u; destino: emitidas %u (%.2f%%), parcial %u, ambig %u, cerca %u, detrás %u, enorme %u, degenerada %u, nan %u, derivada %u, reconstruida %u\n",
            e->lists, e->game_ops, e->fate[VR_FATE_3D] + e->fate[VR_FATE_RECON], 100.0 * (e->fate[VR_FATE_3D] + e->fate[VR_FATE_RECON]) / (e->game_ops ? e->game_ops : 1), e->fate[VR_FATE_PARTIAL], e->fate[VR_FATE_AMBIG], e->fate[VR_FATE_NEAR],
            e->fate[VR_FATE_BEHIND], e->fate[VR_FATE_HUGE], e->fate[VR_FATE_DEGEN], e->fate[VR_FATE_NAN], e->fate[VR_FATE_DERIVED], e->fate[VR_FATE_RECON]);
    fprintf(stderr, "[esc-total] por lista: reconstruidas %.2f, sin recorte del motor (vértices detrás/cerca emitidos) %.2f\n", (double)e->recon / e->lists, (double)e->unclip / e->lists);
    fprintf(stderr, "[esc-total] por lista: juego %.1f, perdidas: parcial %.2f ambig %.2f cerca %.2f detrás %.2f enorme %.2f degenerada %.2f\n", (double)e->game_ops / e->lists, (double)e->fate[VR_FATE_PARTIAL] / e->lists,
            (double)e->fate[VR_FATE_AMBIG] / e->lists, (double)e->fate[VR_FATE_NEAR] / e->lists, (double)e->fate[VR_FATE_BEHIND] / e->lists, (double)e->fate[VR_FATE_HUGE] / e->lists, (double)e->fate[VR_FATE_DEGEN] / e->lists);
    for (int nv = 3; nv <= 4; nv++) for (int nval = 1; nval < nv; nval++) for (int f = 0; f < 16; f++) if (e->part_h[nv][nval][f])
        fprintf(stderr, "[esc-total]   parcial nv=%d con vista en %d: vflags=0x%x -> %u\n", nv, nval, f, e->part_h[nv][nval][f]);
    for (int f = 0; f < 16; f++) if (e->amb_h[f]) fprintf(stderr, "[esc-total]   ambig vflags=0x%x -> %u\n", f, e->amb_h[f]);
    if (e->n_cov) fprintf(stderr, "[esc-total] cobertura media del visor %%: oráculo %.1f, escena vieja %.1f, escena nueva %.1f; huecos medios (oráculo y no escena) %%: vieja %.2f, nueva %.2f; desfase eje motor-cabeza: medio %.1f max %.1f (h) %.1f (v) grados\n",
            e->cov_e / e->n_cov, e->cov_a / e->n_cov, e->cov_n / e->n_cov, e->hole_a / e->n_cov, e->hole_n / e->n_cov, e->off_h_abs / e->n_off, e->off_h_max, e->off_v_abs / e->n_off);
}

/* ---- cobertura (orden `cov t0 t1`): por lista, los polígonos 3D (todos los vértices con posición de vista) con su centroide en
 * coordenadas de MUNDO del motor (mundo = M v + ojo), para medir cuánta geometría llega al renderer según la dirección de la cabeza.
 * Registro binario en <salida>/cov.bin: por lista {int32 t, float yaw, float pitch, int32 ojo[3], uint32 n, uint32 total, parciales, sin3d} y n x {float w[3], uint8 nv, flags, vflags, pad}. */
static int cov_active(int t) {
    if (!scr_on) return 0;
    for (int i = 0; i < nscr; i++) if (scr[i].kind == 10 && t >= scr[i].t0 && t < scr[i].t1) return 1;
    return 0;
}
static void cov_record(const GpuDrawList *l, const VrCamRecord *r, int t) {
    static FILE *fp; static char opened;
    if (!opened) { char p[512]; snprintf(p, sizeof p, "%s/cov.bin", outdir); fp = fopen(p, "wb"); opened = 1; }
    if (!fp) return;
    uint32_t n = 0, nall = 0, npart = 0, n2d = 0;
    for (uint32_t i = 0; i < l->nops; i++) {
        const GpuOp *o = &l->ops[i]; int nvv = o->nv == 4 ? 4 : 3, ok = 1, any = 0;
        if (o->type != GPU_OP_POLY) continue;
        nall++;
        if (!(o->proj_h > 0)) { n2d++; continue; }
        for (int k = 0; k < nvv; k++) { ok &= o->v[k].vvalid; any |= o->v[k].vvalid; }
        if (ok) n++; else if (any) npart++; else n2d++;
    }
    int32_t th[3] = {t, 0, 0}; float yp[2] = {r->yaw_deg, r->pitch_deg};
    fwrite(&th[0], 4, 1, fp); fwrite(yp, 4, 2, fp); fwrite(r->eye, 4, 3, fp); fwrite(&n, 4, 1, fp);
    fwrite(&nall, 4, 1, fp); fwrite(&npart, 4, 1, fp); fwrite(&n2d, 4, 1, fp);
    for (uint32_t i = 0; i < l->nops; i++) {
        const GpuOp *o = &l->ops[i]; int nvv = o->nv == 4 ? 4 : 3, ok = 1;
        if (o->type != GPU_OP_POLY || !(o->proj_h > 0)) continue;
        for (int k = 0; k < nvv; k++) ok &= o->v[k].vvalid;
        if (!ok) continue;
        float c[3] = {0, 0, 0}; unsigned vf = 0;
        for (int k = 0; k < nvv; k++) {
            float x, y, z; vtx_view(o, &o->v[k], &x, &y, &z); vf |= o->v[k].vflags;
            c[0] += r->M[0] * x + r->M[1] * y + r->M[2] * z; c[1] += r->M[3] * x + r->M[4] * y + r->M[5] * z; c[2] += r->M[6] * x + r->M[7] * y + r->M[8] * z;
        }
        float w[3] = {c[0] / nvv + r->eye[0], c[1] / nvv + r->eye[1], c[2] / nvv + r->eye[2]};
        uint8_t m[4] = {(uint8_t)nvv, (uint8_t)((o->flags & GPU_F_SEMI) ? 1 : 0), (uint8_t)vf, 0};
        fwrite(w, 4, 3, fp); fwrite(m, 1, 4, fp);
    }
    fflush(fp);
}
/* ---- inventario del motor (orden `cov`): objetos/piezas/paquetes del canal principal en la RAM del juego, por modo de recorte ----
 * Canal principal 0x800B2B7C: +0x56 nº de objetos, +0x58 cola de punteros; objeto: +0x28 flags, +0x2C máscara de mapa, +0x2E nº de piezas,
 * +0x32 modo de recorte (0 oculto, 1 parcial, 2 dentro), piezas de 0x5C bytes desde +0x48 (+0x4C contador de visibilidad); cada pieza:
 * +0x50 desplazamiento z, +0x52 nº de paquetes, +0x54+4*buf puntero al búfer de paquetes (52 bytes; cabecera 16 bits baja != 0 = emitido),
 * +0x48 siguiente tramo. Salida: línea "[eng]" en el log. */
static uint32_t eng_rd32(uint32_t a) { uint32_t v; memcpy(&v, emu_ram + (a & 0x1FFFFF), 4); return v; }
static int eng_rd16(uint32_t a) { int16_t v; memcpy(&v, emu_ram + (a & 0x1FFFFF), 2); return v; }
static void eng_inventory(int t) {
    const uint32_t chn = 0x800B2B7Cu;
    int nobj = eng_rd16(chn + 0x56); uint32_t q = eng_rd32(chn + 0x58); uint32_t buf = eng_rd32(0x800BCAB0u) & 1;
    uint32_t cur = eng_rd32(0x800BCAF8u);
    int bym[4] = {0}, hid = 0, mapoff = 0; long packs = 0, emit = 0, packs_m0 = 0, emit_vis = 0;
    if (nobj < 0 || nobj > 2000 || q < 0x80000000u) return;
    for (int i = 0; i < nobj; i++) {
        uint32_t o = eng_rd32(q + 4u * i); if (o < 0x80000000u || o >= 0x80200000u) continue;
        int m = eng_rd16(o + 0x32); if (m < 0 || m > 3) m = 3;
        uint32_t fl = eng_rd32(o + 0x28), mp = (uint32_t)eng_rd16(o + 0x2C) & 0xFFFF;
        bym[m]++; if (fl & 0x80) hid++; if (mp && !(mp & cur)) mapoff++;
        int np = eng_rd16(o + 0x2E); if (np < 0 || np > 64) continue;
        for (int k = 0; k < np; k++) {
            uint32_t seg = o + 0x48u + 0x5Cu * k;
            for (int g = 0; g < 8 && seg >= 0x80000000u && seg < 0x80200000u; g++) {
                int n = eng_rd16(seg + 0x52); uint32_t pb = eng_rd32(seg + 0x54 + 4 * buf);
                if (n > 0 && n < 4000 && pb >= 0x80000000u && pb < 0x80200000u) {
                    int e = 0; for (int j = 0; j < n; j++) e += (eng_rd16(pb + 52u * j) & 0xFFFF) != 0;
                    packs += n; emit += e; if (m == 0) packs_m0 += n; else emit_vis += e;
                }
                seg = eng_rd32(seg + 0x48);
            }
        }
    }
    fprintf(stderr, "[eng] t=%d objs=%d modo0=%d modo1=%d modo2=%d ocultos(0x80)=%d fuera_de_mapa=%d paquetes=%ld emitidos=%ld (en objetos modo0: %ld; emitidos en modo>0: %ld)\n",
            t, nobj, bym[0], bym[1], bym[2], hid, mapoff, packs, emit, packs_m0, emit_vis);
}
static void process_list(const GpuDrawList *l, int t, int f) {
    lists_seen++;
    list_ref(l);
    if (scr_on) { big2d_scan(l, t, f); scene_acc(l); }   /* last_filter = resumen de la clase 3 de ESTA lista */
    last_disp = l->disp;
    static uint8_t *pt;
    if (!pt) pt = malloc(PW * PH);
    int shot = shot_this(t), kstrip = -1;
    int bshot = boot_shot > 0 && cur_t < 0 && (f % boot_shot) == 0;
    static uint8_t pre_img[640 * 512 * 3]; int pre_w = 0, pre_h = 0;
    if (shot) for (uint32_t i = 0; i < l->nops; i++) {   /* primera tira de copia de pantalla (rect texturizado 15 bits, ancho, bajo) */
        const GpuOp *o = &l->ops[i];
        if (o->type == GPU_OP_RECT && (o->flags & GPU_F_TEXTURED) && o->tp >= 2 && o->rw >= 64 && o->rh <= 8) { kstrip = (int)i; break; }
    }
    if (scr_on) apply_epochs(l, kstrip, pre_img, &pre_w, &pre_h);
    else { nsnap = 0; ref_apply(r1, l, 1); }
    vram = ref_vram(r1);   /* estado final del frame (render_list usa el de cada época si hay instantáneas) */
    if (shot) { int ow, oh; const uint8_t *orc = ref_present(r1, &l->disp, &ow, &oh); char nm[64]; snprintf(nm, sizeof nm, "o_t%04d", t); write_ppm(outdir, nm, orc, ow, oh); }
    if (bshot) { int ow, oh; const uint8_t *orc = ref_present(r1, &l->disp, &ow, &oh); char nm[64]; snprintf(nm, sizeof nm, "boot_f%05d", f); write_ppm(outdir, nm, orc, ow, oh); }
    uint32_t n = l->index + 1;
    uint32_t sq = 0; for (uint32_t i = 0; i < l->nops; i++) if (l->ops[i].type == GPU_OP_POLY && l->ops[i].v[0].vvalid) { sq = l->ops[i].seq; break; }
    const VrCamRecord *r = vrcam_seq_record(sq);
    if (!r && getenv("CAM_NOVR")) { static VrCamRecord dummy; r = &dummy; }
    if (!r) { none_rec++; if (getenv("CAM_DBG") && (lists_seen % 400) == 1) fprintf(stderr, "[dbg] lista %u idx=%u envios=%u\n", lists_seen, l->index, vrcam_draw_count()); return; }
    if (r->serial != cur_copy.serial) { prev_copy = cur_copy; cur_copy = *r; }
    int novr = getenv("CAM_NOVR") != NULL;
    if (!r->active && !novr) return;
    if (t < 0) return;
    pair_ok++;
    { float ea[4]; if (esc_this(t, ea)) esc_list(l, r, t, ea); }
    if (cov_active(t)) { cov_record(l, r, t); eng_inventory(t); }
    /* proyección del motor (primer polígono 3D con proyección) para el centro de la imagen del juego */
    float eh = 0, eox = 0, eoy = 0;
    for (uint32_t i = 0; i < l->nops; i++) if (l->ops[i].type == GPU_OP_POLY && l->ops[i].proj_h > 0) { eh = l->ops[i].proj_h; break; }
    if (g_ref_ok) { eox = g_refx; eoy = g_refy; }
    else for (uint32_t i = 0; i < l->nops; i++) if (l->ops[i].type == GPU_OP_POLY && l->ops[i].proj_h > 0) { eox = l->ops[i].proj_ox + l->ops[i].dox; eoy = l->ops[i].proj_oy + l->ops[i].doy; break; }
    if (scr_on && last_filter_ok && eh > 0) {   /* comprobación cruzada con vr_scene: proj_h/centro del filtro y nº de polígonos 3D corregidos */
        ref_chk++;
        if (fabsf(last_filter.proj_h - eh) > 0.01f || fabsf(last_filter.cx_col - (eox - l->disp.x)) > 0.01f || fabsf(last_filter.cy_row - (eoy - l->disp.y)) > 0.01f) {
            ref_bad++;
            if (ref_bad <= 5) fprintf(stderr, "[ref] DISCREPANCIA t=%d: vr_scene H=%.2f c=(%.2f,%.2f) vs propio H=%.2f c=(%.2f,%.2f)\n", t, last_filter.proj_h, last_filter.cx_col, last_filter.cy_row, eh, eox - l->disp.x, eoy - l->disp.y);
        }
        uint32_t nfix = 0;
        for (uint32_t i = 0; i < l->nops; i++) {
            const GpuOp *o = &l->ops[i];
            int nvv = o->nv == 4 ? 4 : 3, okv = 1;
            if (o->type != GPU_OP_POLY || !(o->proj_h > 0)) continue;
            for (int k = 0; k < nvv; k++) okv &= o->v[k].vvalid;
            if (!okv) continue;
            { unsigned uf = 0; for (int k = 0; k < nvv; k++) uf |= o->v[k].vflags; if ((uf & GPU_VF_AMBIG) && (uf & (GPU_VF_SAT | GPU_VF_BEHIND))) continue; }   /* vr_scene los descarta antes */
            if (o->proj_ox + o->dox - g_refx != 0.0f || o->proj_oy + o->doy - g_refy != 0.0f) nfix++;
        }
        ofs_chk++;
        if (nfix != last_ofs_fix) { ofs_bad++; if (ofs_bad <= 5) fprintf(stderr, "[ref] polígonos corregidos t=%d: vr_scene %u vs propio %u\n", t, last_ofs_fix, nfix); }
    }
    VrCamDelta d;
    float head[16]; double tt; head_fn(head, &tt);
    if (novr) { memset(&d, 0, sizeof d); d.A[0] = d.A[4] = d.A[8] = 1; }
    else if (!vrcam_delta(r, r->head, &d)) return;
    g_dbg_semi = shot && getenv("CAM_DBGSEMI") != NULL;
    if (shot) {   /* misma pose que el motor: vista previa a la resolución del original, sin y con filtro de pantalla */
        int ow, oh; const uint8_t *orc = ref_present(r1, &l->disp, &ow, &oh); (void)orc;
        static uint8_t se[640 * 512 * 3], sp[640 * 512];
        if (ow <= 640 && oh <= 512 && eh > 0) {
            render_list(l, &d, eh, eox - l->disp.x, eoy - l->disp.y, ow, oh, se, sp);
            char nm[64]; snprintf(nm, sizeof nm, "e_t%04d", t); write_ppm(outdir, nm, se, ow, oh);
            if (last_filter_ok && last_filter.active) {   /* vista VR emulada: la escena 3D (e_) + el post-proceso del visor (hud_emu_post2 == vr_fs_post) con el ojo = pose del motor */
                static float gs[640 * 512 * 3], gd[640 * 512 * 3]; static uint8_t go[640 * 512 * 3], gm[640 * 512];
                for (int i = 0; i < ow * oh * 3; i++) gs[i] = se[i] / 255.0f;
                hud_emu_post2(gs, gd, ow, oh, last_filter.cx_col, last_filter.cy_row, last_filter.proj_h, last_filter.proj_h, &last_filter);
                for (int i = 0; i < ow * oh * 3; i++) { float v = gd[i] * 255.0f + 0.5f; go[i] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }
                snprintf(nm, sizeof nm, "g_t%04d", t); write_ppm(outdir, nm, go, ow, oh);
                /* comparación con el original: sin las zonas de HUD que van al panel (radar arriba a la derecha, icono abajo a la izquierda, mira de las gafas = capa 2D) */
                for (int y = 0; y < oh; y++) for (int x = 0; x < ow; x++) gm[y * ow + x] = !((x > ow * 55 / 100 && y < oh * 30 / 100) || (x < ow * 30 / 100 && y > oh * 80 / 100));
                fprintf(stderr, "[goggles] t=%d vista VR emulada (g_) vs original (o_): MAD %.2f (sin HUD) | escena sin post (e_) vs original: %.2f | recub=%d lineas=%d (periodo %.1f fase %.1f) mascara=%d\n", t,
                        mad_img(go, orc, ow * oh, gm), mad_img(se, orc, ow * oh, gm), last_filter.n_covers, last_filter.n_scan, last_filter.scan_pitch, last_filter.scan_phase, last_filter.frame_on);
            }
            { /* diferencia con el original a la misma pose (e_ vs o_): global (lo no pintado cuenta como negro) y solo lo pintado */
                int npaint = 0; for (int i = 0; i < ow * oh; i++) npaint += sp[i] != 0;
                const uint8_t *cmpi = (kstrip >= 0 && pre_w == ow && pre_h == oh) ? pre_img : orc;   /* bajo el agua: el original justo ANTES del filtro */
                fprintf(stderr, "[shot] t=%d e_ vs o_: MAD global %.3f, pintado %.3f, cobertura %.1f%%, polígonos parciales rellenados (acum) %d\n", t, mad_img(se, cmpi, ow * oh, NULL), mad_img(se, cmpi, ow * oh, sp), 100.0 * npaint / (ow * oh), g_filled);
            }
            float cyr = eoy - l->disp.y;
            { const VrFilter *vf = &last_filter; float dmn = 0, dmx = 0; int nc = 0;
              for (int i = 0; i < VR_FILTER_ROWS; i++) if (vf->rowcov[i]) { nc++; if (vf->dv[i] < dmn) dmn = vf->dv[i]; if (vf->dv[i] > dmx) dmx = vf->dv[i]; }
              fprintf(stderr, "[shot] t=%d imagen %dx%d H=%.1f centro óptico fila=%.1f col=%.1f (disp %d,%d %dx%d) | filtro activo=%d tiras=%d polis=%d recub=%d tinte=(%.2f,%.2f,%.2f) filas=%d dv=[%.0f,%.0f] cover_abr=%d\n",
                      t, ow, oh, eh, cyr, eox - l->disp.x, l->disp.x, l->disp.y, l->disp.w, l->disp.h, last_filter_ok && vf->active, vf->n_strips, vf->n_polys, vf->n_cover, vf->tint[0], vf->tint[1], vf->tint[2], nc, dmn, dmx, vf->cover_abr); }
            if (last_filter_ok && last_filter.active) {
                static uint8_t sf[640 * 512 * 3]; memcpy(sf, se, (size_t)ow * oh * 3);
                apply_filter_img2(sf, ow, oh, &last_filter, eh, cyr, eh, cyr, 0);
                snprintf(nm, sizeof nm, "ef_t%04d", t); write_ppm(outdir, nm, sf, ow, oh);
                apply_filter_img2(se, ow, oh, &last_filter, eh, cyr, eh, cyr, 1);
                snprintf(nm, sizeof nm, "efs_t%04d", t); write_ppm(outdir, nm, se, ow, oh);   /* emulando la copia en sitio */
                if (pre_w == ow && pre_h == oh) {   /* validación del modelo del filtro: original sin filtrar -> mi filtro, contra el original filtrado */
                    static uint8_t qf[640 * 512 * 3];
                    memcpy(qf, pre_img, (size_t)ow * oh * 3); snprintf(nm, sizeof nm, "q_t%04d", t); write_ppm(outdir, nm, qf, ow, oh);
                    apply_filter_img2(qf, ow, oh, &last_filter, eh, cyr, eh, cyr, 0);
                    snprintf(nm, sizeof nm, "qf_t%04d", t); write_ppm(outdir, nm, qf, ow, oh);
                    { /* el mismo efecto con el emulador del post-proceso del visor (vr/scene/hud_emu.c == vr_fs_post), ojo = pose del motor: cy_img = fila del centro, f_eye = H */
                        static float fsrc[640 * 512 * 3], fdst[640 * 512 * 3]; static uint8_t emu[640 * 512 * 3];
                        for (int i = 0; i < ow * oh * 3; i++) fsrc[i] = pre_img[i] / 255.0f;
                        hud_emu_post(fsrc, fdst, ow, oh, last_filter.cy_row, last_filter.proj_h, &last_filter);
                        for (int i = 0; i < ow * oh * 3; i++) { float v = fdst[i] * 255.0f + 0.5f; emu[i] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }
                        snprintf(nm, sizeof nm, "qp_t%04d", t); write_ppm(outdir, nm, emu, ow, oh);
                        fprintf(stderr, "[shot] t=%d filtro: emulación del post-proceso del visor vs original filtrado MAD %.3f (modelo de camtest qf_ %.3f; sin filtro %.3f)\n", t,
                                mad_img(emu, orc, ow * oh, NULL), mad_img(qf, orc, ow * oh, NULL), mad_img(pre_img, orc, ow * oh, NULL));
                    }
                    memcpy(qf, pre_img, (size_t)ow * oh * 3);
                    apply_filter_img2(qf, ow, oh, &last_filter, eh, cyr, eh, cyr, 1);
                    snprintf(nm, sizeof nm, "qfs_t%04d", t); write_ppm(outdir, nm, qf, ow, oh);
                }
            }
        }
    }
    /* a) comprobación de identidad: re-proyectar con la pose del registro (y la proyección del motor) vs imagen del oráculo */
    if (!scr_on && (t == 60 || t == 61 || t == 130 || t == 131)) {
        int w = 320, h = 256;
        static uint8_t a[320 * 256 * 3], p2[320 * 256];
        g_dbg = (t == 60 || t == 600);
        RStat s = render_list(l, &d, eh, eox - l->disp.x, eoy - l->disp.y, w, h, a, p2);
        int ow, oh;
        const uint8_t *orc = ref_present(r1, &l->disp, &ow, &oh);
        double m = mad_img(a, orc, w * h, p2);
        char nm[64]; snprintf(nm, sizeof nm, "id_t%d_re", t); write_ppm(outdir, nm, a, w, h);
        snprintf(nm, sizeof nm, "id_t%d_orc", t); write_ppm(outdir, nm, orc, ow, oh);
        /* emparejamiento desplazado: registro n-1 */
        const VrCamRecord *r2 = prev_copy.serial ? &prev_copy : NULL;
        double m2 = -1;
        if (r2 && r2->active) {
            VrCamDelta d2;
            if (vrcam_delta(r2, r2->head, &d2)) {
                render_list(l, &d2, eh, eox - l->disp.x, eoy - l->disp.y, w, h, a, p2);
                m2 = mad_img(a, orc, w * h, p2);
            }
        }
        fprintf(stderr, "[camtest] t=%d (f=%d): identidad vs oráculo, error medio |d| = %.2f (emparejado con registro vecino: %.2f); polis %u dibujados %u parciales %u  H=%.0f rec: yaw=%.1f pitch=%.1f serial=%u (envío %u)\n",
                t, f, m, m2, s.polys, s.drawn, s.partial, eh, r->yaw_deg, r->pitch_deg, r->serial, n);
    }
    /* a2) emparejado: con la cabeza FIJA (guiñada 0) dos frames consecutivos cuyo registro difiere ~40 grados deben dar la misma
     * escena si el registro asociado es el correcto; con el registro vecino, no. */
    static int pair_n;
    if (!scr_on && t >= 130 && t < 240) world_collect(l, r);
    if (!scr_on && t >= 130 && t < 240 && pair_n < 2) {
        int pn = pair_n++;
        static uint8_t ia[2][PW * PH * 3], iw[2][PW * PH * 3], pa[PW * PH];
        float save_y = g_yaw, save_e = g_elev; g_yaw = g_elev = 0;
        float h0[16]; double t0; head_fn(h0, &t0);
        g_yaw = save_y; g_elev = save_e;
        VrCamDelta dd; float fp = (PW / 2.0f) / tanf(hfov_prev * 3.14159265f / 360.0f);
        if (vrcam_delta(r, h0, &dd)) render_list(l, &dd, fp, PW / 2.0f, PH / 2.0f, PW, PH, ia[pn], pa);
        const VrCamRecord *rw = prev_copy.serial ? &prev_copy : NULL;
        if (rw && vrcam_delta(rw, h0, &dd)) render_list(l, &dd, fp, PW / 2.0f, PH / 2.0f, PW, PH, iw[pn], pa);
        if (pn == 1) {
            double mc = mad_img(ia[0], ia[1], PW * PH, NULL), mw = mad_img(iw[0], iw[1], PW * PH, NULL);
            fprintf(stderr, "[camtest] emparejado (cabeza fija, frames con yaw de motor %.0f y %.0f): diferencia entre frames consecutivos con el registro correcto = %.2f, con el registro vecino = %.2f\n", pair_yaw0, r->yaw_deg, mc, mw);
            write_ppm(outdir, "pair_a0", ia[0], PW, PH); write_ppm(outdir, "pair_a1", ia[1], PW, PH); write_ppm(outdir, "pair_w1", iw[1], PW, PH);
        } else pair_yaw0 = r->yaw_deg;
    }
    /* b) vista previa de campo ancho con la pose actual de la cabeza */
    if (dump_this(t)) {
        VrCamDelta dc;
        if (novr) dc = d; else if (!vrcam_delta(r, head, &dc)) return;
        float fp = (PW / 2.0f) / tanf(hfov_prev * 3.14159265f / 360.0f);
        RStat s = render_list(l, &dc, fp, PW / 2.0f, PH / 2.0f, PW, PH, img, pt);
        uint32_t cov = 0;
        for (int i = 0; i < PW * PH; i++) cov += pt[i];
        double c = 100.0 * cov / (PW * PH);
        sum_cov += c; n_cov++;
        char nm[64]; snprintf(nm, sizeof nm, "p_t%04d", t); write_ppm(outdir, nm, img, PW, PH);
        if (scr_on && last_filter_ok && last_filter.active) {   /* campo ancho (el del visor, mayor que el del motor) + post-proceso del visor: la máscara cubre TODO el campo */
            static float ws[PW * PH * 3], wd[PW * PH * 3]; static uint8_t wo[PW * PH * 3];
            for (int i = 0; i < PW * PH * 3; i++) ws[i] = img[i] / 255.0f;
            hud_emu_post2(ws, wd, PW, PH, PW / 2.0f, PH / 2.0f, fp, fp, &last_filter);
            for (int i = 0; i < PW * PH * 3; i++) { float v = wd[i] * 255.0f + 0.5f; wo[i] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }
            char nw[64]; snprintf(nw, sizeof nw, "pg_t%04d", t); write_ppm(outdir, nw, wo, PW, PH);
        }
        if (scr_on && last_filter_ok && last_filter.active) {
            static uint8_t imf[PW * PH * 3];
            memcpy(imf, img, sizeof imf);
            apply_filter_preview(imf, &last_filter, eh, eoy - l->disp.y, fp);
            char nf[64]; snprintf(nf, sizeof nf, "f_t%04d", t); write_ppm(outdir, nf, imf, PW, PH);
        }
        if (scr_on) {   /* todas las primitivas que no son polígonos 3D de la lista (2D, rects, líneas, eventos de VRAM), en orden */
            char p2[512]; snprintf(p2, sizeof p2, "%s/ops_t%04d.txt", outdir, t);
            FILE *fo = fopen(p2, "w");
            if (fo) {
                fprintf(fo, "# lista %u nops=%u disp=(%d,%d,%d,%d) agua=%d o2=%d\n", l->index, l->nops, l->disp.x, l->disp.y, l->disp.w, l->disp.h, bs.in_water, bs.o2);
                uint32_t n3 = 0;
                for (uint32_t i = 0; i < l->nops; i++) {
                    const GpuOp *o = &l->ops[i];
                    if (o->type == GPU_OP_POLY) { int nv = 0; for (int k = 0; k < o->nv; k++) nv += o->v[k].vvalid; if (nv == o->nv) { n3++; continue; } }
                    fprintf(fo, "op%u type=%d nv=%d flags=0x%x abr=%d tp=%d tx=%d ty=%d clut=(%d,%d) v0=(%d,%d) v1=(%d,%d) v2=(%d,%d) rect=%dx%d dst=(%d,%d,%d,%d) col=%02x%02x%02x uv=(%d,%d) clip=(%d,%d,%d,%d)\n",
                            i, o->type, o->nv, o->flags, o->abr, o->tp, o->tx, o->ty, o->clx, o->cly, o->v[0].x, o->v[0].y, o->v[1].x, o->v[1].y, o->v[2].x, o->v[2].y, o->rw, o->rh,
                            o->rx, o->ry, o->rw, o->rh, o->v[0].r, o->v[0].g, o->v[0].b, o->v[0].u, o->v[0].v, o->cx1, o->cy1, o->cx2, o->cy2);
                }
                fprintf(fo, "# polígonos 3D (omitidos arriba): %u\n", n3);
                fclose(fo);
            }
        }
        if (!novr && getenv("CAM_DIAG")) {   /* mismo frame con la pose de cabeza que usó el motor al dibujarlo: separa "hueco de geometría" de "latencia de pose" */
            VrCamDelta dn; static uint8_t imo[PW * PH * 3], pto[PW * PH];
            if (vrcam_delta(r, r->head, &dn)) {
                RStat so = render_list(l, &dn, fp, PW / 2.0f, PH / 2.0f, PW, PH, imo, pto);
                uint32_t co = 0; for (int i = 0; i < PW * PH; i++) co += pto[i];
                char n2[64]; snprintf(n2, sizeof n2, "own_t%04d", t); write_ppm(outdir, n2, imo, PW, PH);
                { int ow, oh; const uint8_t *orc = ref_present(r1, &l->disp, &ow, &oh); snprintf(n2, sizeof n2, "orc_t%04d", t); write_ppm(outdir, n2, orc, ow, oh); }
                fprintf(stderr, "[camtest]   diag t=%d: con la pose propia del motor cubierta %.1f%% (polis %u dibujados %u parc %u detras %u ambig %u 2D %u)\n", t, 100.0 * co / (PW * PH), so.polys, so.drawn, so.partial, so.behind, so.ambig, so.nonvr2d);
            }
        }
        fprintf(stderr, "[camtest] t=%4d yaw=%6.1f elev=%6.1f -> vista %.0fx%.0f cubierta %.1f%%  polis %u dibujados %u parciales(perdidos) %u detras %u ambig %u recortados %u 2D %u sat %u deriv %u  eye=(%d,%d,%d) vista motor yaw=%.1f pitch=%.1f\n",
                t, g_yaw, g_elev, hfov_prev, hfov_prev * PH / PW, c, s.polys, s.drawn, s.partial, s.behind, s.ambig, s.clipped, s.nonvr2d, s.sat, s.derived, r->eye[0], r->eye[1], r->eye[2], r->yaw_deg, r->pitch_deg);
    }
}


/* ---- modo servidor de instantáneas (ver cabecera) ---- */
static const char *srv_list, *srv_base;
static int srv_frame = -1, srv_done;
typedef struct { char name[96], path[400]; } SrvJob;
static void state_reset_counters(void) {
    memset(sacc, 0, sizeof sacc); memset(&g_tot, 0, sizeof g_tot);
    lists_seen = pair_ok = none_rec = 0; sum_cov = 0; n_cov = 0; nwerr = 0; memset(&eacc, 0, sizeof eacc);
    big_lists = big_water_lists = big_air_lists = big_water_ops = big_air_ops = 0; big_log = NULL;
    last_filter_ok = 0; hist_n = 0;
}
static double now_s(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }
static double t_start;
/* Devuelve 1 en cada hijo (que continúa el juego con su guion); el padre no vuelve (sale tras esperar a todos). */
static int server_snapshot(uint32_t f) {
    srv_done = 1;
    int watch = getenv("CAM_SERVER_WATCH") != NULL;
    struct stat lst; time_t last_mt = 0; long last_ns = 0;
    double t_snap = now_s();
    SrvJob *jobs = calloc(256, sizeof *jobs);
    for (int round = 0;; round++) {
        int nj = 0, quit = 0;
        if (!stat(srv_list, &lst)) { last_mt = lst.st_mtime; last_ns = lst.st_mtim.tv_nsec; }   /* antes de leer: una edición posterior se detecta */
        FILE *fl = fopen(srv_list, "r");
        if (!fl) { perror(srv_list); exit(1); }
        char l[600];
        while (fgets(l, sizeof l, fl) && nj < 256) {
            char a[400], b[400]; int n = sscanf(l, "%399s %399s", a, b);
            if (n < 1 || a[0] == '#') continue;
            if (!strcmp(a, "quit")) { quit = 1; break; }
            SrvJob *j = &jobs[nj];
            const char *path = n >= 2 ? b : a;
            if (n >= 2) snprintf(j->name, sizeof j->name, "%s", a);
            else { const char *bn = strrchr(path, '/'); bn = bn ? bn + 1 : path; snprintf(j->name, sizeof j->name, "%s", bn); char *dot = strrchr(j->name, '.'); if (dot) *dot = 0; }
            snprintf(j->path, sizeof j->path, "%s", path);
            for (int k = 0; k < nj; k++) if (!strcmp(jobs[k].name, j->name)) { size_t L = strlen(j->name); snprintf(j->name + L, sizeof j->name - L, "_%d", nj + 1); break; }
            nj++;
        }
        fclose(fl);
        if (quit) { fprintf(stderr, "[server] quit\n"); exit(0); }
        int maxjobs = getenv("CAM_JOBS") ? atoi(getenv("CAM_JOBS")) : 1; if (maxjobs < 1) maxjobs = 1;
        double t_r0 = now_s();
        if (!round) fprintf(stderr, "[server] instantánea en f=%u (t=%d) tras %.1f s de arranque\n", f, cur_t, t_snap - t_start);
        fprintf(stderr, "[server] ronda %d: %d escenarios, %d a la vez\n", round, nj, maxjobs);
        alarm(0);
        fflush(NULL);
        int running = 0; pid_t pids[256]; double t0s[256];
        for (int i = 0; i < nj; i++) {
            while (running >= maxjobs) {
                int st; pid_t w = wait(&st);
                if (w < 0 && errno == EINTR) continue;
                if (w < 0) break;
                for (int k = 0; k < i; k++) if (pids[k] == w) { fprintf(stderr, "[server] %-20s fin: estado %d, %.1f s\n", jobs[k].name, WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st), now_s() - t0s[k]); pids[k] = 0; }
                running--;
            }
            pid_t pid = fork();
            if (pid < 0) { perror("fork"); exit(1); }
            if (pid == 0) {   /* hijo: continúa el juego desde el estado exacto con su guion */
                static char od[600];
                snprintf(od, sizeof od, "%s/%s", srv_base, jobs[i].name);
                mkdir(od, 0755);
                outdir = od;
                char lp[700]; snprintf(lp, sizeof lp, "%s/log.txt", od);
                if (!freopen(lp, "w", stderr)) _exit(3);
                setvbuf(stderr, NULL, _IONBF, 0);
                state_reset_counters();
                scr_load(jobs[i].path);
                srv_list = NULL;
                fprintf(stderr, "[server] hijo %s: guion %s desde f=%u t=%d\n", jobs[i].name, jobs[i].path, f, cur_t);
                free(jobs);
                alarm(60);
                return 1;
            }
            pids[i] = pid; t0s[i] = now_s(); running++;
        }
        int st; pid_t w;
        while ((w = wait(&st)) > 0)
            for (int k = 0; k < nj; k++) if (pids[k] == w) fprintf(stderr, "[server] %-20s fin: estado %d, %.1f s\n", jobs[k].name, WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st), now_s() - t0s[k]);
        fprintf(stderr, "[server] ronda %d: %.1f s (arranque %.1f s)\n", round, now_s() - t_r0, t_snap - t_start);
        if (!watch) exit(0);
        fprintf(stderr, "[server] esperando que cambie %s (una línea 'quit' lo termina)\n", srv_list);
        for (;;) {   /* el padre sigue en la instantánea: una lista nueva se ejecuta sin repetir el arranque */
            usleep(500000);
            if (!stat(srv_list, &lst) && (lst.st_mtime != last_mt || lst.st_mtim.tv_nsec != last_ns)) {
                usleep(400000);   /* esperar a que el editor termine de escribir (si no, se leería una lista a medias) */
                struct stat l2;
                if (!stat(srv_list, &l2) && l2.st_mtime == lst.st_mtime && l2.st_mtim.tv_nsec == lst.st_mtim.tv_nsec) break;
            }
        }
    }
}
static uint64_t fnv(const void *p, size_t n) { const uint8_t *q = p; uint64_t h = 1469598103934665603ull; for (size_t i = 0; i < n; i++) h = (h ^ q[i]) * 1099511628211ull; return h; }


/* Órdenes de guion que tocan la RAM/CPU del juego (poke/pokes/gcall/peek/cmp), en el gancho de VBlank. */
static int in_gcall;
static void script_mem(Cpu *c, int t) {
    for (int i = 0; i < nscr; i++) {
        const ScrEv *e = &scr[i];
        if (e->kind < 4 || e->t0 != t) continue;
        uint32_t ma = e->u[0] & 0x1FFFFF;
        uint8_t *m = emu_ram + ma;
        size_t room = EMU_RAM_SIZE - ma;   /* bytes hasta el final de la RAM: poke/pokes/peek/cmp nunca se salen del búfer */
        switch (e->kind) {
        case 4: { size_t n = strlen(e->s) / 2; if (n > room) n = room;
            for (size_t k = 0; k < n; k++) { unsigned v = 0; if (sscanf(e->s + 2 * k, "%2x", &v) != 1) { fprintf(stderr, "[poke] t=%d: hex inválido en el byte %zu, se detiene\n", t, k); break; } m[k] = (uint8_t)v; }
            break; }
        case 5: { size_t n = strlen(e->s) + 1; if (n > room) n = room; memcpy(m, e->s, n); break; }
        case 6: {
            Cpu save = *c;
            uint32_t sp = e->u[6] ? e->u[6] & ~7u : (save.r[29] - 0x200) & ~7u;   /* pila propia opcional (a5 = 7.º número): evita pisar los búferes de prueba */
            { uint32_t sa = sp & 0x1FFFFFFFu; if (sa < 0x10000 || sa > 0x1FFFF0u) { fprintf(stderr, "[gcall] t=%d: pila %08x fuera de la RAM, ignorada\n", t, sp); break; } }
            c->r[29] = sp; c->r[4] = e->u[1]; c->r[5] = e->u[2]; c->r[6] = e->u[3]; c->r[7] = e->u[4];
            mem_w32(c, sp + 16, e->u[5]);
            in_gcall = 1;
            rt_call(c, e->u[0]);
            in_gcall = 0;
            fprintf(stderr, "[gcall] t=%d f=%08x(%x,%x,%x,%x,%x) sp=%08x -> v0=0x%08x\n", t, e->u[0], e->u[1], e->u[2], e->u[3], e->u[4], e->u[5], sp, c->r[2]);
            *c = save;
            break; }
        case 7: { fprintf(stderr, "[peek] t=%d %08x:", t, e->u[0]); for (uint32_t k = 0; k < e->u[1] && k < 256 && k < room; k++) fprintf(stderr, " %02x", m[k]); fprintf(stderr, "\n"); break; }
        case 9: { const char *cp = getenv("MV_CARD"); if (cp) { card_close(0); int r = card_open(0, cp); fprintf(stderr, "[card] t=%d reabierta %s -> %d\n", t, cp, r); } break; }
        case 8: { uint32_t a2 = e->u[1] & 0x1FFFFF; size_t n = e->u[2], r2 = EMU_RAM_SIZE - a2; if (n > room) n = room; if (n > r2) n = r2;
            fprintf(stderr, "[cmp] t=%d %08x vs %08x (%zu bytes): %s\n", t, e->u[0], e->u[1], n, memcmp(m, emu_ram + a2, n) ? "DISTINTAS" : "IGUALES"); break; }
        }
    }
}

static void on_frame(Cpu *c, uint32_t f) {
    if (in_gcall) return;   /* VBlank durante una llamada de guion: no reentrar */
    (void)c;
    alarm(60);
    script_pad(f);
    static int novr;
    if (getenv("CAM_NOVR")) novr = 1;
    if (scr_on && cur_t >= 0) for (int i = 0; i < nscr; i++) if (scr[i].kind == 11 && scr[i].t0 == cur_t) vrcam_turn(scr[i].a[0]);   /* giro del stick: como vrhost frame_hook, antes de bridge_read/vrcam_frame */
    bridge_read(emu_ram, &bs);
    vrcam_frame(emu_ram, &bs);
    if (f_act < 0 && (vrcam_active() || (getenv("CAM_NOVR") && bs.in_control && bs.has_player && f > 6000))) f_act = (int)f + 20;
    cur_t = f_act < 0 ? -1 : (int)f - f_act;
    if (srv_list && !srv_done && (srv_frame >= 0 ? (int)f == srv_frame : cur_t == 0)) server_snapshot(f);
    if (scr_on && cur_t >= 0) script_mem(c, cur_t);
    if (cur_t >= 0) phase_pose(cur_t, (int)f); else g_yaw = g_elev = 0;
    if (hist_n < 20000) { hist[hist_n].f = (int)f; hist[hist_n].x = bs.player_pos.x; hist[hist_n].z = bs.player_pos.z; hist[hist_n].view = vrcam_view_yaw_deg(); hist[hist_n].po = vrcam_pad_origin(); hist_n++; }
    if (cur_t == 60 || cur_t == 62) {
        const int16_t *m = (const int16_t *)(emu_ram + (0x800B2B7Cu & 0x1FFFFF) + 0x10);   /* canal principal: matriz mundo->vista */
        double n[3]; for (int r = 0; r < 3; r++) n[r] = sqrt((double)m[3 * r] * m[3 * r] + (double)m[3 * r + 1] * m[3 * r + 1] + (double)m[3 * r + 2] * m[3 * r + 2]) / 4096.0;
        fprintf(stderr, "[camtest] t=%d norma de las filas de la matriz mundo->vista (x,y,z): %.4f %.4f %.4f (activo=%d)\n", cur_t, n[0], n[1], n[2], vrcam_active());
    }
    gpu_capture_vblank();
    const GpuDrawList *l;
    while ((l = gpu_capture_frame())) process_list(l, cur_t, (int)f);
    static int logevery; if (!logevery) logevery = getenv("CAM_LOGEVERY") ? atoi(getenv("CAM_LOGEVERY")) : 15;
    if (scr_on && cur_t >= 0 && cur_t % logevery == 0) {
        uint32_t st, o2r, ctl; memcpy(&st, emu_ram + (0x800ACBE0u & 0x1FFFFF), 4); memcpy(&o2r, emu_ram + (0x800ACBC4u & 0x1FFFFF), 4); memcpy(&ctl, emu_ram + (0x800ACB84u & 0x1FFFFF), 4);
        fprintf(stderr, "[dive] t=%d f=%u modo=%s ctl=%d agua=%d o2=%d vida=%d pos=(%d,%d,%d) vrcam=%d agachado=%d tendido=%d estado=0x%08x o2raw=0x%08x ctlraw=0x%08x\n", cur_t, f, bridge_mode_name(&bs), bs.in_control, bs.in_water, bs.o2,
                bs.life, bs.player_pos.x, bs.player_pos.y, bs.player_pos.z, vrcam_active(), bs.crouching, bs.prone, st, o2r, ctl);
    }
    if (f % 500 == 0) fprintf(stderr, "[camtest] frame %u modo=%s activo=%d t=%d\n", f, bridge_mode_name(&bs), vrcam_active(), cur_t);
    (void)novr;
    if (f >= max_frames || cur_t >= (scr_on ? scr_end : 1400)) {
        { GteHpStats g; gte_hp_stats(&g); fprintf(stderr, "[camtest] tabla GTE: almacenados=%u desalojos=%u omitidos=%u (sf0=%u sat=%u div=%u z=%u) consultas=%u aciertos=%u max/gen=%u\n", g.stored, g.evict, g.skipped, g.skip_sf, g.skip_sat, g.skip_ovf, g.skip_z, g.lookups, g.hits, g.max_gen_stored); }
        if (scr_on) fprintf(stderr, "[camtest] 2D a pantalla completa (>=40%% del área): listas bajo el agua %u con %u ops; listas fuera %u con %u ops\n", big_water_lists, big_water_ops, big_air_lists, big_air_ops);
        if (scr_on) for (int w = 0; w < 2; w++) {
            const SceneAcc *a = &sacc[w];
            fprintf(stderr, "[scene] %s: listas %u polis %u 3D %u (con BEHIND %u, tris recortados %u) 2D %u | tris3D %u | descartes: parcial %u cerca-sin-BEHIND %u ambig %u tris-detrás %u | filtro: tiras %u polis %u recubrimientos %u tris %u (listas con filtro %u) | borrados %u\n",
                    w ? "bajo el agua" : "fuera del agua", a->lists, a->polys, a->poly3d, a->poly_behind, a->clipped, a->poly2d, a->tris3d, a->drop_partial, a->drop_near, a->drop_ambig, a->drop_behind,
                    a->n_filter_rects, a->n_filter_polys, a->n_filter_cover, a->filter_tris, a->lists_filter, a->n_clear);
        }
        fprintf(stderr, "[camtest] descartes acumulados (frames volcados): polis %u dibujados %u parciales %u detrás-del-todo %u ambig %u recortados %u 2D %u (sat %u deriv %u); regla anterior (todos fvalid) habría dibujado %u\n", g_tot.polys, g_tot.drawn, g_tot.partial, g_tot.behind, g_tot.ambig, g_tot.clipped, g_tot.nonvr2d, g_tot.sat, g_tot.derived, g_tot.legacy);
        if (srv_list && !srv_done) { fprintf(stderr, "[server] error: no se llegó al punto de instantánea (f=%u, t=%d); no se ejecutó ningún escenario\n", f, cur_t); exit(2); }
        esc_report();
        world_report(getenv("CAM_KEEP_OVERSCAN") ? "(keep_overscan)" : "");
        if (scr_on) fprintf(stderr, "[camtest] vr_scene vs propio: centro/H del filtro %u comprobadas, %u discrepancias; polígonos con desplazamiento OFX corregido %u comprobadas, %u discrepancias\n", ref_chk, ref_bad, ofs_chk, ofs_bad);
        if (scr_on) fprintf(stderr, "[camtest] épocas de VRAM: máximo de eventos por lista %d (instantáneas %d)\n", snap_max_ev, MAXSNAP);
        { const uint16_t *vr = ref_vram(r1); fprintf(stderr, "[camtest] hash f=%u RAM=%016llx VRAM=%016llx\n", f, (unsigned long long)fnv(emu_ram, EMU_RAM_SIZE), (unsigned long long)fnv(vr, 1024 * 512 * 2)); }
        VrCamStats s; vrcam_stats(&s);
        fprintf(stderr, "[camtest] FIN f=%u: listas %u, con registro VR %u, sin registro %u; envíos %u; cámaras %u (VR %u) activaciones %u desactivaciones %u recentrados %u cabeza-perdida %u; cobertura media %.1f%%\n",
                f, lists_seen, pair_ok, none_rec, vrcam_draw_count(), s.builds, s.vr_builds, s.activations, s.deactivations, s.recenters, s.head_misses, n_cov ? sum_cov / n_cov : 0);
        { /* marcha hacia delante con la mirada fija: dirección de desplazamiento frente a la mirada */
            int w[2][2] = {{20, 60}, {1350, 1390}};
            for (int k = 0; k < 2 && f_act >= 0; k++) {
                int i0 = f_act + w[k][0], i1 = f_act + w[k][1], a = -1, b = -1;
                for (int i = 0; i < hist_n; i++) { if (hist[i].f == i0) a = i; if (hist[i].f == i1) b = i; }
                if (a < 0 || b < 0) continue;
                int dx = hist[b].x - hist[a].x, dz = hist[b].z - hist[a].z;
                double dir = atan2(dx, dz) * 180 / 3.14159265; if (dir < 0) dir += 360;
                fprintf(stderr, "[camtest] marcha 'adelante' t=%d..%d: desplazamiento (%d,%d), dirección %.0f; mirada(yaw motor)=%.0f origen_mando=%d => dirección-mirada=%.0f grados\n",
                        w[k][0], w[k][1], dx, dz, dir, hist[b].view, hist[b].po, fmod(dir - hist[b].view + 720, 360) > 180 ? fmod(dir - hist[b].view + 720, 360) - 360 : fmod(dir - hist[b].view + 720, 360));
            }
        }
        exit(0);
    }
}
static void on_alarm(int s) { (void)s; rt_fatal(NULL, "sin VBlank en 60 s"); }
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "uso: %s <exe> <frames> <dir_salida>\n", argv[0]); return 1; }
    const char *exe = argv[1], *disc = getenv("MV_DISC");
    max_frames = (uint32_t)strtoul(argv[2], 0, 0);
    outdir = argv[3];
    mkdir(outdir, 0755);
    t_start = now_s();
    if (getenv("CAM_SERVER")) { srv_list = getenv("CAM_SERVER"); srv_base = argv[3]; }
    if (getenv("CAM_SNAP_FRAME")) srv_frame = atoi(getenv("CAM_SNAP_FRAME"));
    if (getenv("CAM_SCRIPT")) scr_load(getenv("CAM_SCRIPT"));
    if (getenv("CAM_HFOV")) hfov_prev = (float)atof(getenv("CAM_HFOV"));
    if (getenv("CAM_PAD")) {   /* guion de mando de ARRANQUE propio: "inicio:mascara_hex:duracion[:cada],..." (sustituye al de defecto) */
        char *dup = strdup(getenv("CAM_PAD")), *sv = NULL;
        for (char *tok = strtok_r(dup, ",", &sv); tok && npad_ev < 16; tok = strtok_r(NULL, ",", &sv)) {
            unsigned st = 0, m = 0, du = 6, ev = 0;
            if (sscanf(tok, "%u:%x:%u:%u", &st, &m, &du, &ev) >= 2) pad_ev[npad_ev++] = (PadEv){st, du, ev, (uint16_t)m};
        }
        free(dup);
    } else {
    pad_ev[npad_ev++] = (PadEv){120, 6, 0, 1u << 3};
    pad_ev[npad_ev++] = (PadEv){400, 6, 0, 1u << 3};
    pad_ev[npad_ev++] = (PadEv){0, 6, 97, 1u << 13};
    pad_ev[npad_ev++] = (PadEv){0, 6, 211, 1u << 7};
    }
    if (getenv("CAM_BOOT_SHOT")) boot_shot = atoi(getenv("CAM_BOOT_SHOT"));
    FILE *f = fopen(exe, "rb");
    if (!f) { perror(exe); return 1; }
    static uint8_t buf[EMU_RAM_SIZE + 0x800];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    if (n < 0x800 || memcmp(buf, EXE_MAGIC, 8)) { fprintf(stderr, "ejecutable no válido\n"); return 1; }
    uint32_t pc = rd32(buf + 0x10), gp = rd32(buf + 0x14), t = rd32(buf + 0x18), sz = rd32(buf + 0x1C);
    memcpy(&emu_ram[t & 0x1FFFFF], buf + 0x800, sz);
    if (disc && !cd_open(disc)) { fprintf(stderr, "sin discos\n"); return 1; }
    RefOpts o1 = {1, 1, 0};
    r1 = ref_create(&o1);
    VrCamConfig cfg; vrcam_config_default(&cfg);
    cfg.head = getenv("CAM_NOVR") ? NULL : head_fn;
    if (getenv("CAM_HIDE")) cfg.hide_head = atoi(getenv("CAM_HIDE"));
    if (getenv("CAM_ENGINE_H")) cfg.engine_h = atoi(getenv("CAM_ENGINE_H"));
    if (getenv("CAM_KEEP_OVERSCAN")) cfg.keep_overscan = atoi(getenv("CAM_KEEP_OVERSCAN"));
    if (getenv("CAM_PREDICT")) cfg.predict_s = (float)atof(getenv("CAM_PREDICT"));   /* s de predicción de mirada (0 = sin) */
    if (getenv("CAM_LAG")) cfg.draw_lag = atoi(getenv("CAM_LAG"));
    if (getenv("CAM_UP")) cfg.eye_up = (float)atof(getenv("CAM_UP"));
    if (getenv("CAM_FWD")) cfg.eye_fwd = (float)atof(getenv("CAM_FWD"));
    vrcam_init(&cfg);
    Cpu c = {0};
    c.r[28] = gp;
    c.r[29] = c.r[30] = 0x801FFFF0u;
    gte_reset();
    gpu_reset();
    gpu_capture_hiprec(1);
    gpu_capture_enable(1);
    rt_host_frame = on_frame;
    signal(SIGALRM, on_alarm);
    rt_fatal_armed = 1;
    if (setjmp(rt_fatal_jmp)) return 2;
    alarm(60);
    rt_call(&c, pc);
    return 0;
}
