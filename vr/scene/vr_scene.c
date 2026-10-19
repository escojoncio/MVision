/* vr_scene: ver vr_scene.h para el contrato. Aquí: transformación vista del juego -> mundo VR, clasificación 3D/2D,
 * descartes, partición por épocas de VRAM.
 *
 * ================================ Escala (metros por unidad del juego) ================================
 * Hipótesis de partida: 1 unidad del motor = 1 mm (defecto 0,001). Evidencia de datos (partida real, zona inicial):
 *  - el jugador de pie tiene player_pos.y ~ 1100 y los enemigos de pie ~ 1080; el "centro" del cuerpo de un adulto
 *    está a ~1,0-1,1 m del suelo => 1 unidad ~ 1 mm. Agachado, player_pos.y ~ 500 (0,5 m).
 *  - los objetos del suelo (items) están en y = 500 y los blancos de suelo en y = 0; la cámara cenital del juego está a
 *    y ~ 8200 (8,2 m sobre el jugador), altura plausible para una cámara de techo de sala.
 *  - scene_dump imprime la altura del escenario (suelo a techo y de la figura del jugador) medida en la propia
 *    geometría capturada para confirmarlo en cada frame (ver bitácora).
 * La escala es configurable (opts.scale).
 *  - Medido con scene_dump (partida real, 1 unidad = 1 mm): vértices a <0,45 m del jugador cubren de -0,02 a +1,56..1,68 m
 *    sobre el suelo (cuerpo del personaje con la cabeza posiblemente recortada por el filtro), el suelo queda plano a
 *    y ~ 0 y la cámara cenital a 8,2 m. Réplica desde la cámara del juego == imagen del oráculo (ver bitácora).
 *  - feet_below_pos: unidades entre player_pos.y y los pies. player_pos.y es la altura sobre el suelo (de pie 1104,
 *    andando 1029-1104 por la animación, agachado ~496): -1 (auto) usa ctl_height, que el motor mantiene casi igual.
 *    Los pies resultantes caen a +-50 mm del suelo real; el host puede afinarlo con el suelo que vea (pendiente).
 * ========================================================================================================== */
#include "vr_scene.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

VrSceneOpts vr_scene_opts_default(void) {
    VrSceneOpts o;
    memset(&o, 0, sizeof o);
    o.scale = 0.001f;
    o.near_units = 24.0f;
    o.max_edge_m = 120.0f;
    o.min_area_m2 = 1e-9f;
    o.feet_below_pos = -1.0f;
    o.eye_normalize = 1;
    o.eye_from_view_t = 0;
    o.mirror_z = -1;
    o.screen_filter = 0;   /* opt-in: lo activa el anfitrión (vr/host) porque su renderer implementa la clase 3 (post-proceso por ojo) */
    o.filter_min_cover = 0.9f;
    o.drop_ghost = 1;
    o.drop_clear = 1;
    o.engine_near_clip = 0;   /* §49: todo lo que el juego emite llega al renderer; el plano cercano es el de la cabeza */
    o.recon_partial = 1; o.recon_ambig = 1;
    return o;
}

/* ---------------- memoria interna ---------------- */
typedef struct {
    VrVtx v[3];
    uint32_t ev_end, src_op;
    uint8_t kind, blend;
} Tri;

static Tri *g_tri; static size_t g_tri_cap;
static VrVtx *g_vtx; static size_t g_vtx_cap;
static VrBatch *g_bat; static size_t g_bat_cap;
static VrState *g_st; static size_t g_st_cap, g_st_n;
static uint32_t *g_hash; static size_t g_hash_cap; /* tabla abierta: índice+1 en g_st */
static uint32_t *g_next; static size_t g_next_cap;
static uint8_t *g_fate; static uint32_t g_fate_cap;
void vr_scene_set_fate(uint8_t *buf, uint32_t cap) { g_fate = buf; g_fate_cap = buf ? cap : 0; }
/* El destino de un polígono con varios triángulos es el mejor de ellos (emitido > descartado); `f` solo sustituye a VR_FATE_NONE o a un descarte. */
static void fate_set(uint32_t i, uint8_t f) {
    if (i >= g_fate_cap) return;
    uint8_t c = g_fate[i];
    if (c == VR_FATE_3D || c == VR_FATE_RECON) return;
    g_fate[i] = f;
}

void vr_scene_release(void) {
    free(g_tri); free(g_vtx); free(g_bat); free(g_st); free(g_hash); free(g_next);
    g_tri = NULL; g_vtx = NULL; g_bat = NULL; g_st = NULL; g_hash = NULL; g_next = NULL;
    g_tri_cap = g_vtx_cap = g_bat_cap = g_st_cap = g_st_n = g_hash_cap = g_next_cap = 0;
}

#define GROW(p, cap, need) do { if ((need) > (cap)) { size_t nc_ = (cap) ? (cap) : 256; while (nc_ < (need)) nc_ *= 2; \
    void *q_ = realloc((p), nc_ * sizeof *(p)); if (!q_) return -2; (p) = q_; (cap) = nc_; } } while (0)

const VrState *vr_scene_state(const VrScene *s, uint16_t idx) { return s && idx < s->nstates ? &s->states[idx] : NULL; }

static uint32_t hash_state(const VrState *s) {
    const uint8_t *p = (const uint8_t *)s;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < sizeof *s; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}
/* Devuelve el índice del estado (se crea si es nuevo); 0xFFFF si no cabe. -2 en error de memoria. */
static int state_id(const GpuOp *op) {
    VrState s;
    memset(&s, 0, sizeof s);
    s.twmx = op->twmx; s.twmy = op->twmy; s.twox = op->twox; s.twoy = op->twoy;
    s.cx1 = op->cx1; s.cy1 = op->cy1; s.cx2 = op->cx2; s.cy2 = op->cy2;
    if (!g_hash_cap) {
        g_hash_cap = 1024;
        g_hash = calloc(g_hash_cap, sizeof *g_hash);
        if (!g_hash) { g_hash_cap = 0; return -2; }
    }
    uint32_t h = hash_state(&s) & (uint32_t)(g_hash_cap - 1);
    for (size_t k = 0; k < g_hash_cap; k++, h = (h + 1) & (uint32_t)(g_hash_cap - 1)) {
        if (!g_hash[h]) break;
        if (!memcmp(&g_st[g_hash[h] - 1], &s, sizeof s)) return (int)(g_hash[h] - 1);
    }
    if (g_st_n >= 0xFFFEu || g_st_n * 2 >= g_hash_cap) return 0xFFFF; /* tabla llena: sin estado */
    GROW(g_st, g_st_cap, g_st_n + 1);
    g_st[g_st_n] = s;
    g_hash[h] = (uint32_t)(g_st_n + 1);
    return (int)g_st_n++;
}

/* ---------------- geometría ---------------- */
static uint32_t pack_rgba(uint8_t r, uint8_t g, uint8_t b) { return (uint32_t)r | (uint32_t)g << 8 | (uint32_t)b << 16 | 0xFF000000u; }

/* Vértice de trabajo 3D: posición de vista + atributos en float (permite interpolar al recortar). */
typedef struct { float p[3], u, v, r, g, b; } CV;

/* Centro del mundo de la lista (ver vr_scene.h, "Desplazamiento de proyección"): moda de (proj_ox+dox, proj_oy+doy) cuantizada a 0,5 px. */
typedef struct { int ok; float rx, ry, h; uint32_t n; } WorldRef;
static int poly_all_valid(const GpuOp *op) {
    int nv = op->nv == 4 ? 4 : 3;
    for (int k = 0; k < nv; k++) if (!op->v[k].vvalid) return 0;
    return 1;
}
static void list_ref(const GpuDrawList *dl, WorldRef *w) {
    memset(w, 0, sizeof *w);
    for (int pass = 0; pass < 3 && !w->ok; pass++) {   /* 0: 3D opacos; 1: cualquier 3D; 2: cualquier polígono con proj_h */
        struct { int x, y; uint32_t n; float h; } hb[16]; int nh = 0;
        for (uint32_t i = 0; i < dl->nops; i++) {
            const GpuOp *o = &dl->ops[i];
            if (o->type != GPU_OP_POLY || !(o->proj_h > 0) || !isfinite(o->proj_h)) continue;
            if (pass < 2 && !poly_all_valid(o)) continue;
            if (pass == 0 && (o->flags & GPU_F_SEMI)) continue;
            float fx = (o->proj_ox + o->dox) * 2.0f, fy = (o->proj_oy + o->doy) * 2.0f;
            if (!isfinite(fx) || !isfinite(fy) || fabsf(fx) > 1e6f || fabsf(fy) > 1e6f) continue;
            int x = (int)lroundf(fx), y = (int)lroundf(fy), k;
            for (k = 0; k < nh; k++) if (hb[k].x == x && hb[k].y == y) { hb[k].n++; break; }
            if (k == nh && nh < 16) { hb[nh].x = x; hb[nh].y = y; hb[nh].n = 1; hb[nh].h = o->proj_h; nh++; }
        }
        int best = -1;
        for (int k = 0; k < nh; k++) if (best < 0 || hb[k].n > hb[best].n) best = k;
        if (best >= 0) { w->ok = 1; w->rx = hb[best].x * 0.5f; w->ry = hb[best].y * 0.5f; w->h = hb[best].h; w->n = hb[best].n; }
    }
}

/* Vértice de vista del polígono 3D: (vx,vy,vz) corregido por el desplazamiento de proyección de la primitiva respecto del centro del mundo.
 * *fixed se pone a 1 si hubo corrección. */
static void cv_from(CV *c, const GpuOp *op, const GpuVtx *v, int gour, const WorldRef *w, int *fixed) {
    const GpuVtx *k = gour ? v : &op->v[0];
    c->p[0] = v->vx; c->p[1] = v->vy; c->p[2] = v->vz;
    if (w && w->ok && op->proj_h > 0 && isfinite(op->proj_h)) {
        float ex = op->proj_ox + (float)op->dox - w->rx, ey = op->proj_oy + (float)op->doy - w->ry;
        if (ex != 0.0f || ey != 0.0f) {
            c->p[0] += ex * v->vz / op->proj_h;
            c->p[1] += ey * v->vz / op->proj_h;
            if (fixed) *fixed = 1;
        }
    }
    c->u = (float)v->u; c->v = (float)v->v;
    c->r = k->r; c->g = k->g; c->b = k->b;
}
static uint8_t q8(float x) { return (uint8_t)(x <= 0 ? 0 : x >= 255 ? 255 : (int)(x + 0.5f)); }
static void fill_cv(VrVtx *o, const GpuOp *op, const CV *c, uint16_t flags, uint16_t st) {
    o->rgba = pack_rgba(q8(c->r), q8(c->g), q8(c->b));
    o->uv[0] = c->u; o->uv[1] = c->v;
    o->clut = (uint16_t)((op->clx >> 4) | (op->cly << 6));
    o->tpage = (uint16_t)(((op->tx >> 6) & 15) | (((op->ty >> 8) & 1) << 4) | ((op->abr & 3) << 5) | ((op->tp & 3) << 7));
    o->flags = flags;
    o->pad = st;
}
static void fill_vtx(VrVtx *o, const GpuOp *op, const GpuVtx *v, int gour, uint16_t flags, uint16_t st) {
    const GpuVtx *c = gour ? v : &op->v[0];
    o->rgba = pack_rgba(c->r, c->g, c->b);
    o->uv[0] = (float)v->u; o->uv[1] = (float)v->v;
    o->clut = (uint16_t)((op->clx >> 4) | (op->cly << 6));
    o->tpage = (uint16_t)(((op->tx >> 6) & 15) | (((op->ty >> 8) & 1) << 4) | ((op->abr & 3) << 5) | ((op->tp & 3) << 7));
    o->flags = flags;
    o->pad = st;
}

/* Cuadrilátero con posiciones de vista coherentes: los 4 vértices coplanares (distancia del 4.º al plano de los otros tres < 2 % de la mayor arista)
 * y triángulos no degenerados. Es la señal de que un AMBIG junto a SAT/BEHIND no es una coincidencia espuria. */
static int quad_coherent(const GpuOp *op) {
    const GpuVtx *v = op->v;
    float e1[3] = {v[1].vx - v[0].vx, v[1].vy - v[0].vy, v[1].vz - v[0].vz}, e2[3] = {v[2].vx - v[0].vx, v[2].vy - v[0].vy, v[2].vz - v[0].vz};
    float e3[3] = {v[3].vx - v[0].vx, v[3].vy - v[0].vy, v[3].vz - v[0].vz};
    float n[3] = {e1[1]*e2[2] - e1[2]*e2[1], e1[2]*e2[0] - e1[0]*e2[2], e1[0]*e2[1] - e1[1]*e2[0]};
    float nn = sqrtf(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
    float l1 = sqrtf(e1[0]*e1[0] + e1[1]*e1[1] + e1[2]*e1[2]), l2 = sqrtf(e2[0]*e2[0] + e2[1]*e2[1] + e2[2]*e2[2]), l3 = sqrtf(e3[0]*e3[0] + e3[1]*e3[1] + e3[2]*e3[2]);
    float lm = fmaxf(l1, fmaxf(l2, l3));
    if (!(nn > 1e-3f * lm * lm) || !isfinite(nn)) return 0;
    float d = fabsf(n[0]*e3[0] + n[1]*e3[1] + n[2]*e3[2]) / nn;
    return d < 0.02f * lm;
}

/* Sutherland-Hodgman contra el plano z = zn (se conserva z >= zn) de un polígono de n<=3 vértices -> <=4. Devuelve m. */
static int clip_near(const CV *in, int n, float zn, CV *out) {
    int m = 0;
    for (int i = 0; i < n; i++) {
        const CV *a = &in[i], *b = &in[(i + 1) % n];
        int ia = a->p[2] >= zn, ib = b->p[2] >= zn;
        if (ia) out[m++] = *a;
        if (ia != ib) {
            float s = (zn - a->p[2]) / (b->p[2] - a->p[2]);
            const float *pa = a->p, *pb = b->p;
            CV *o = &out[m++];
            for (int k = 0; k < 3; k++) o->p[k] = pa[k] + (pb[k] - pa[k]) * s;
            o->p[2] = zn;
            o->u = a->u + (b->u - a->u) * s; o->v = a->v + (b->v - a->v) * s;
            o->r = a->r + (b->r - a->r) * s; o->g = a->g + (b->g - a->g) * s; o->b = a->b + (b->b - a->b) * s;
        }
    }
    return m;
}

/* Realimentación (GpuOp.fbk, ver gpu.h): color de vértice modulado por el texel único que la captura resolvió, como lo hace la GPU
 * ((texel5 * c) >> 4 por canal; crudo: el texel). Devuelve el RGBA empaquetado (a=255). */
static uint32_t fb_color(const GpuOp *op, const GpuVtx *c) {
    uint16_t t = op->fbt;
    uint8_t out[3];
    for (int k = 0; k < 3; k++) {
        int t5 = (t >> (5 * k)) & 31, e = (t5 << 3) | (t5 >> 2);
        int cc = k == 0 ? c->r : k == 1 ? c->g : c->b;
        out[k] = (op->flags & GPU_F_RAW) ? (uint8_t)e : q8(e * (float)cc / 128.0f);
    }
    return pack_rgba(out[0], out[1], out[2]);
}

static int has_player_ok(const BridgeState *bs) {
    return bs && bs->has_player && (bs->valid & BRIDGE_OK_ALL_CORE) == BRIDGE_OK_ALL_CORE;
}

typedef struct { float m[3][3]; float t[3]; } Aff; /* v_vr = m*v + t */

static void xf(const Aff *a, const CV *v, float *out) {
    for (int r = 0; r < 3; r++) out[r] = a->m[r][0] * v->p[0] + a->m[r][1] * v->p[1] + a->m[r][2] * v->p[2] + a->t[r];
}

void vr_scene_player_origin(const BridgeState *bs, const VrSceneOpts *o, float org[3]) {
    org[0] = org[1] = org[2] = 0;
    if (!bs) return;
    if ((bs->valid & BRIDGE_OK_PLAYER_POS) && bs->has_player) {
        /* feet_below_pos < 0 (auto): ctl_height (el motor lo mantiene = player_pos.y sobre el suelo: de pie ~1104,
         * agachado ~550; ver "Escala"); si no es fiable, 1104. */
        float fb = o && o->feet_below_pos >= 0 ? o->feet_below_pos
                   : ((bs->valid & BRIDGE_OK_PLAYER_CTL) && bs->ctl_height > 0 ? (float)bs->ctl_height : 1104.0f);
        org[0] = bs->player_pos.x; org[1] = (float)bs->player_pos.y - fb; org[2] = bs->player_pos.z;
    } else if (bs->valid & BRIDGE_OK_CAMERA) {
        org[0] = bs->cam_target.x; org[1] = bs->cam_target.y; org[2] = bs->cam_target.z;
    }
}

/* Área (px) de la intersección de [x0,x1)x[y0,y1) con [a0,b0)x[a1,b1) */
static double isect_area(int x0, int y0, int x1, int y1, int ax0, int ay0, int ax1, int ay1) {
    int ix0 = x0 > ax0 ? x0 : ax0, iy0 = y0 > ay0 ? y0 : ay0, ix1 = x1 < ax1 ? x1 : ax1, iy1 = y1 < ay1 ? y1 : ay1;
    return (ix1 > ix0 && iy1 > iy0) ? (double)(ix1 - ix0) * (iy1 - iy0) : 0.0;
}

/* Clasificación de filtro de pantalla (ver vr_scene.h, "Clase 3"). Devuelve 0 no, 1 copia de pantalla, 2 recubrimiento semitransparente.
 * bbox (x0,y0,x1,y1; x1,y1 exclusivos) de destino de la primitiva 2D; (u0,v0,u1,v1) caja de texels inclusiva (polígonos). Todo se mide contra el área
 * MOSTRADA (dl->disp = búfer recién dibujado), NO contra el recorte: un panel pequeño con su propio recorte (minimapa, retrato) que se cubre a sí mismo
 * con un rect semitransparente o textura 15 bits no es un filtro de pantalla. */
static int g_frame_on;   /* esta lista trae máscara de visión (frame_scan) */
/* Polígono 2D sin textura que puede formar parte de la máscara de visión: plano "resta blanco" (semitransparente, modo 2, color >= 0xE0), plano opaco negro,
 * o degradado semitransparente de resta. */
static int frame_cand(const GpuOp *op) {
    if (op->type != GPU_OP_POLY || (op->flags & GPU_F_TEXTURED)) return 0;
    int semi = (op->flags & GPU_F_SEMI) != 0, gour = (op->flags & GPU_F_GOURAUD) != 0;
    const GpuVtx *g = &op->v[0];
    if (gour) return semi && (op->abr & 3) == 2;
    if (semi) return (op->abr & 3) == 2 && g->r >= 0xE0 && g->g >= 0xE0 && g->b >= 0xE0;
    return g->r <= 0x10 && g->g <= 0x10 && g->b <= 0x10;
}
/* Firma de la máscara: >= 8 polígonos planos candidatos y >= 8 degradados de resta, todos 2D (sin posición de vista). */
static int frame_scan(const GpuDrawList *dl) {
    int nflat = 0, ngour = 0;
    for (uint32_t i = 0; i < dl->nops; i++) {
        const GpuOp *op = &dl->ops[i];
        if (!frame_cand(op)) continue;
        int nv = op->nv == 4 ? 4 : 3, any = 0;
        for (int k = 0; k < nv; k++) any |= op->v[k].vvalid;
        if (any) continue;
        if (op->flags & GPU_F_GOURAUD) ngour++; else nflat++;
    }
    return nflat >= 8 && ngour >= 8;
}

/* Búfer de dibujo alterno (doble búfer). Un sprite que lee del búfer que NO se muestra ahora (el fotograma anterior: estela, fundido encadenado) lee de la VRAM
 * del renderer, que solo recibe eventos y no lo dibujado por primitivas: contendría restos de lo último subido (pantalla de título, etc.). El alterno es el último
 * área mostrada distinta de la actual, más los vecinos del área mostrada del tamaño de la pantalla (disposición lado a lado o apilada). */
static struct { int x, y, w, h; } g_disp_other, g_disp_cur;
static void disp_track(const GpuDrawList *dl) {
    if (dl->disp.w <= 0 || dl->disp.h <= 0) return;
    if (g_disp_cur.w > 0 && (g_disp_cur.x != dl->disp.x || g_disp_cur.y != dl->disp.y || g_disp_cur.w != dl->disp.w || g_disp_cur.h != dl->disp.h)) g_disp_other = g_disp_cur;
    g_disp_cur.x = dl->disp.x; g_disp_cur.y = dl->disp.y; g_disp_cur.w = dl->disp.w; g_disp_cur.h = dl->disp.h;
}
/* >= 50 % del origen (sx0,sy0)-(sx1,sy1) cae en el búfer alterno */
static int src_in_other_buffer(const GpuDrawList *dl, int sx0, int sy0, int sx1, int sy1) {
    double area = (double)(sx1 - sx0) * (sy1 - sy0);
    if (!(area > 0)) return 0;
    int w = dl->disp.w, h = dl->disp.h, x = dl->disp.x, y = dl->disp.y;
    int cand[5][4] = {{x + w, y, w, h}, {x - w, y, w, h}, {x, y + h, w, h}, {x, y - h, w, h}, {g_disp_other.x, g_disp_other.y, g_disp_other.w, g_disp_other.h}};
    for (int i = 0; i < 5; i++) {
        int cx = cand[i][0], cy = cand[i][1], cw = cand[i][2], ch = cand[i][3];
        if (cw <= 0 || ch <= 0 || cx < 0 || cy < 0 || cx + cw > 1024 || cy + ch > 512) continue;
        if (cx == x && cy == y) continue;
        if (isect_area(sx0, sy0, sx1, sy1, cx, cy, cx + cw, cy + ch) >= 0.5 * area) return 1;
    }
    return 0;
}

static int filter_class(const VrSceneOpts *o, const GpuDrawList *dl, const GpuOp *op, int is_rect, int x0, int y0, int x1, int y1, int u0, int v0, int u1, int v1) {
    if (x1 <= x0 || y1 <= y0) return 0;
    if (o->drop_ghost && (op->flags & GPU_F_TEXTURED) && op->tp >= 2 && dl->disp.w > 0 && dl->disp.h > 0) {   /* mezcla con el fotograma anterior (con o sin screen_filter) */
        int sx0, sy0, sx1, sy1;
        if (is_rect) {
            sx0 = op->tx + ((op->flags & GPU_F_FLIPX) ? u0 - (x1 - x0) + 1 : u0); sx1 = sx0 + (x1 - x0);
            sy0 = op->ty + ((op->flags & GPU_F_FLIPY) ? v0 - (y1 - y0) + 1 : v0); sy1 = sy0 + (y1 - y0);
        } else { sx0 = op->tx + u0; sy0 = op->ty + v0; sx1 = op->tx + u1 + 1; sy1 = op->ty + v1 + 1; }
        double darea = (double)(x1 - x0) * (y1 - y0), dga = (double)dl->disp.w * dl->disp.h;
        if (darea >= 0.2 * dga && isect_area(x0, y0, x1, y1, dl->disp.x, dl->disp.y, dl->disp.x + dl->disp.w, dl->disp.y + dl->disp.h) >= 0.8 * darea &&
            src_in_other_buffer(dl, sx0, sy0, sx1, sy1)) return 5;
    }
    if (!o->screen_filter) return 0;
    if (g_frame_on && !is_rect && frame_cand(op)) return 4;
    int dx0 = dl->disp.x, dy0 = dl->disp.y, dx1 = dl->disp.x + dl->disp.w, dy1 = dl->disp.y + dl->disp.h;
    double da = (double)dl->disp.w * dl->disp.h;
    if (!(da > 0)) return 0;
    if ((op->flags & GPU_F_TEXTURED) && op->tp >= 2 && !(op->flags & GPU_F_SEMI)) {   /* copia desplazada opaca; una mezcla semitransparente (brillo, desenfoque) no se modela con dv/du */
        int sx0, sy0, sx1, sy1;
        if (is_rect) { /* origen en (u0,v0); con flip crece hacia atrás; tamaño = el del destino */
            sx0 = op->tx + ((op->flags & GPU_F_FLIPX) ? u0 - (x1 - x0) + 1 : u0); sx1 = sx0 + (x1 - x0);
            sy0 = op->ty + ((op->flags & GPU_F_FLIPY) ? v0 - (y1 - y0) + 1 : v0); sy1 = sy0 + (y1 - y0);
        } else { sx0 = op->tx + u0; sy0 = op->ty + v0; sx1 = op->tx + u1 + 1; sy1 = op->ty + v1 + 1; }
        double area = (double)(sx1 - sx0) * (sy1 - sy0), darea = (double)(x1 - x0) * (y1 - y0);
        /* el destino tiene que ser de ancho de pantalla: banda fina (tira, h <= 16) de >= 45 % del ancho, o >= 50 % del área. Un parche local que copia
         * la pantalla (p. ej. un brillo alrededor de un retrato de 140x89) NO es un filtro de pantalla. */
        int wide = (x1 - x0) * 100 >= 45 * dl->disp.w && (y1 - y0) <= 16;
        if (area > 0 && (wide || darea >= 0.5 * da) && isect_area(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1) >= 0.5 * area &&
            isect_area(x0, y0, x1, y1, dx0, dy0, dx1, dy1) >= 0.5 * darea) return 1;
    }
    if ((op->flags & GPU_F_SEMI) && !(op->flags & GPU_F_TEXTURED)) { /* solo color plano/degradado: fundido, destello, tinte (un polígono texturizado a pantalla completa es contenido) */
        int cx0 = x0 > op->cx1 ? x0 : op->cx1, cy0 = y0 > op->cy1 ? y0 : op->cy1;
        int cx1 = x1 < op->cx2 + 1 ? x1 : op->cx2 + 1, cy1 = y1 < op->cy2 + 1 ? y1 : op->cy2 + 1;
        if (isect_area(cx0, cy0, cx1, cy1, dx0, dy0, dx1, dy1) >= o->filter_min_cover * da) return 2;
        /* línea de exploración: tira fina de ancho de pantalla (alto <= 2, >= 90 % del ancho mostrado) */
        if (is_rect && (y1 - y0) <= 2 && (x1 - x0) * 10 >= 9 * dl->disp.w &&
            isect_area(x0, y0, x1, y1, dx0, dy0, dx1, dy1) >= 0.9 * (double)(x1 - x0) * (y1 - y0)) return 3;
    }
    return 0;
}

/* máscara de visión: acumulación (resta por celda, 0..n) y estado de las líneas de exploración */
static float g_frame_acc[VR_FRAME_W * VR_FRAME_H];
static uint8_t g_frame_out[VR_FRAME_W * VR_FRAME_H];
static float g_scan_prev_y, g_scan_y0;
static int g_scan_cover_seq;   /* recubrimientos vistos (todos, no solo los conservados) antes de la primera línea */

/* Suma a g_frame_acc la resta que produce un triángulo (coordenadas de la imagen mostrada, px original). val[k] = resta/255 en cada vértice. */
static void frame_tri(const VrFilter *f, const float px[3], const float py[3], const float val[3]) {
    float aw = (float)f->area[2], ah = (float)f->area[3];
    if (!(aw > 0 && ah > 0)) return;
    float cw = aw / VR_FRAME_W, ch = ah / VR_FRAME_H;
    float minx = fminf(px[0], fminf(px[1], px[2])), maxx = fmaxf(px[0], fmaxf(px[1], px[2]));
    float miny = fminf(py[0], fminf(py[1], py[2])), maxy = fmaxf(py[0], fmaxf(py[1], py[2]));
    float area = (px[1] - px[0]) * (py[2] - py[0]) - (px[2] - px[0]) * (py[1] - py[0]);
    if (fabsf(area) < 1e-6f) return;
    int i0 = (int)floorf(minx / cw - 0.5f), i1 = (int)ceilf(maxx / cw - 0.5f), j0 = (int)floorf(miny / ch - 0.5f), j1 = (int)ceilf(maxy / ch - 0.5f);
    if (i0 < 0) i0 = 0;
    if (j0 < 0) j0 = 0;
    if (i1 > VR_FRAME_W - 1) i1 = VR_FRAME_W - 1;
    if (j1 > VR_FRAME_H - 1) j1 = VR_FRAME_H - 1;
    for (int j = j0; j <= j1; j++) {
        float y = ((float)j + 0.5f) * ch;
        for (int i = i0; i <= i1; i++) {
            float x = ((float)i + 0.5f) * cw;
            float w0 = ((px[1] - x) * (py[2] - y) - (px[2] - x) * (py[1] - y)) / area;
            float w1 = ((px[2] - x) * (py[0] - y) - (px[0] - x) * (py[2] - y)) / area;
            float w2 = 1.0f - w0 - w1;
            if (w0 < -1e-4f || w1 < -1e-4f || w2 < -1e-4f) continue;
            g_frame_acc[j * VR_FRAME_W + i] += w0 * val[0] + w1 * val[1] + w2 * val[2];
        }
    }
}

/* acumula el resumen VrFilter */
static void filter_note(VrScene *out, const GpuOp *op, int cls, int is_rect, float *tw) {
    VrFilter *f = &out->filter;
    if (cls == 5) { out->stats.n_filter_ghost++; return; }   /* mezcla con el fotograma anterior: se descarta del panel, no hay nada que aplicar */
    f->active = 1;
    if (cls == 4) {   /* marco de la máscara de visión: resta del polígono (opaco negro = resta total) */
        int nv = op->nv == 4 ? 4 : 3;
        int semi = (op->flags & GPU_F_SEMI) != 0, gour = (op->flags & GPU_F_GOURAUD) != 0;
        float px[4], py[4], vl[4];
        for (int k = 0; k < nv; k++) {
            const GpuVtx *g = gour ? &op->v[k] : &op->v[0];
            px[k] = (float)(op->v[k].x - f->area[0]); py[k] = (float)(op->v[k].y - f->area[1]);
            vl[k] = semi ? (float)(g->r + g->g + g->b) / (3.0f * 255.0f) : 1.0f;
        }
        static const int idx[2][3] = {{0, 1, 2}, {1, 2, 3}};
        for (int q = 0; q < (nv == 4 ? 2 : 1); q++) {
            float tx[3] = {px[idx[q][0]], px[idx[q][1]], px[idx[q][2]]}, ty[3] = {py[idx[q][0]], py[idx[q][1]], py[idx[q][2]]}, tv[3] = {vl[idx[q][0]], vl[idx[q][1]], vl[idx[q][2]]};
            frame_tri(f, tx, ty, tv);
        }
        out->stats.n_filter_frame++;
        return;
    }
    if (cls == 3) {   /* línea de exploración */
        const GpuVtx *c = &op->v[0];
        float y = (float)(c->y - f->area[1]);
        if (f->n_scan == 0) { g_scan_cover_seq = f->n_cover; g_scan_y0 = y; f->scan_h = (float)op->rh; f->scan_pitch = 0; f->scan_rgb[0] = c->r / 255.0f; f->scan_rgb[1] = c->g / 255.0f; f->scan_rgb[2] = c->b / 255.0f; f->scan_abr = op->abr & 3; }
        else {
            float d = fabsf(y - g_scan_prev_y);
            if (d > 0.5f && (f->scan_pitch == 0 || d < f->scan_pitch)) f->scan_pitch = d;
        }
        g_scan_prev_y = y;
        f->n_scan++; out->stats.n_filter_scan++;
        return;
    }
    if (cls == 2) {
        f->n_cover++; out->stats.n_filter_cover++;
        const GpuVtx *c = &op->v[0];
        f->cover_rgb[0] = c->r / 255.0f; f->cover_rgb[1] = c->g / 255.0f; f->cover_rgb[2] = c->b / 255.0f; f->cover_abr = op->abr & 3;
        if (f->n_covers == VR_FILTER_COVERS) {   /* se conservan los últimos */
            memmove(f->covers_rgb[0], f->covers_rgb[1], sizeof f->covers_rgb[0] * (VR_FILTER_COVERS - 1));
            memmove(&f->covers_abr[0], &f->covers_abr[1], sizeof f->covers_abr[0] * (VR_FILTER_COVERS - 1));
            f->n_covers--;
        }
        for (int k = 0; k < 3; k++) f->covers_rgb[f->n_covers][k] = f->cover_rgb[k];
        f->covers_abr[f->n_covers++] = f->cover_abr;
        return;
    }
    if (!is_rect) { f->n_polys++; out->stats.n_filter_polys++; }
    else {
        f->n_strips++; out->stats.n_filter_rects++;
        if (!(op->flags & GPU_F_RAW)) { /* promedio del color de modulación por área */
            float w = (float)op->rw * op->rh;
            for (int k = 0; k < 3; k++) f->tint[k] = (f->tint[k] * *tw + (k == 0 ? op->v[0].r : k == 1 ? op->v[0].g : op->v[0].b) / 128.0f * w) / (*tw + w);
            *tw += w;
        }
        int y0 = op->v[0].y - out->filter.area[1];
        int dv = (op->ty + op->v[0].v) - op->v[0].y, du = (op->tx + op->v[0].u) - op->v[0].x;
        for (int r = 0; r < op->rh; r++) {
            int i = y0 + r;
            if (i < 0 || i >= VR_FILTER_ROWS) continue;
            if (!f->rowcov[i]) { f->rowcov[i] = 1; f->dv[i] = (float)dv; f->du[i] = (float)du; if (i + 1 > f->nrows) f->nrows = i + 1; }
        }
    }
}

int vr_scene_build(const GpuDrawList *dl, const BridgeState *bs, const VrSceneOpts *op_in, VrScene *out) {
    if (!dl || !out) return -1;
    VrSceneOpts o = op_in ? *op_in : vr_scene_opts_default();
    if (!(o.scale > 0)) o.scale = 0.001f;
    memset(out, 0, sizeof *out);
    out->src = dl;
    out->scale = o.scale;
    out->filter.tint[0] = out->filter.tint[1] = out->filter.tint[2] = 1.0f; out->filter.cover_abr = -1;
    out->filter.area[0] = dl->disp.x; out->filter.area[1] = dl->disp.y; out->filter.area[2] = dl->disp.w; out->filter.area[3] = dl->disp.h;
    float tint_w = 0.0f;
    disp_track(dl);
    g_frame_on = o.screen_filter && frame_scan(dl);
    if (g_frame_on) memset(g_frame_acc, 0, sizeof g_frame_acc);
    g_scan_prev_y = g_scan_y0 = 0; g_scan_cover_seq = 0;
    /* centro del mundo y proyección del motor (una vez por lista); VrFilter.proj_h/cx_col/cy_row se rellenan siempre */
    WorldRef wref;
    list_ref(dl, &wref);
    out->stats.n_ref_group = wref.n;
    out->filter.proj_h = wref.ok ? wref.h : 160.0f;
    out->filter.cx_col = wref.ok ? wref.rx - (float)dl->disp.x : (dl->disp.w > 0 ? 0.5f * (float)dl->disp.w : 160.0f);
    out->filter.cy_row = wref.ok ? wref.ry - (float)dl->disp.y : (dl->disp.h > 0 ? 0.5f * (float)dl->disp.h : 128.0f);

    /* ---- transformación vista -> mundo VR ---- */
    Aff A;
    memset(&A, 0, sizeof A);
    int eye_ok = bs && (bs->valid & BRIDGE_OK_EYE);
    float org[3] = {0, 0, 0};
    if (o.origin_mode == 1) { for (int i = 0; i < 3; i++) org[i] = o.origin_game[i]; }
    else vr_scene_player_origin(bs, &o, org);
    float zs = 1.0f;
    if (eye_ok) {
        float R[3][3];
        for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) R[r][c] = (float)bs->eye_axes[r][c] * (1.0f / 4096.0f);
        if (o.eye_normalize) {
            /* La GTE usa N = eye_axes^T (mundo->vista) con la misma escala ~1,002 por eje (|columna| 4100..4110), y su
             * traslación es -N*eye_pos (medido). La inversa exacta de esa vista es R = N^-1 (vista->mundo); normalizar
             * las columnas dejaría el mundo escalado 0,1-0,4 % respecto al ojo (cm a 10 m, y deriva al mover la cámara). */
            float N[3][3], a[3][3], det;
            for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) N[r][c] = R[c][r];
            a[0][0] = N[1][1]*N[2][2] - N[1][2]*N[2][1]; a[0][1] = N[0][2]*N[2][1] - N[0][1]*N[2][2]; a[0][2] = N[0][1]*N[1][2] - N[0][2]*N[1][1];
            a[1][0] = N[1][2]*N[2][0] - N[1][0]*N[2][2]; a[1][1] = N[0][0]*N[2][2] - N[0][2]*N[2][0]; a[1][2] = N[0][2]*N[1][0] - N[0][0]*N[1][2];
            a[2][0] = N[1][0]*N[2][1] - N[1][1]*N[2][0]; a[2][1] = N[0][1]*N[2][0] - N[0][0]*N[2][1]; a[2][2] = N[0][0]*N[1][1] - N[0][1]*N[1][0];
            det = N[0][0]*a[0][0] + N[0][1]*a[1][0] + N[0][2]*a[2][0];
            if (fabsf(det) > 0.5f && fabsf(det) < 2.0f) {
                for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) R[r][c] = a[r][c] / det;
            } else { /* matriz degenerada: al menos normalizar columnas */
                for (int c = 0; c < 3; c++) {
                    float l = sqrtf(R[0][c] * R[0][c] + R[1][c] * R[1][c] + R[2][c] * R[2][c]);
                    if (l > 0.5f) for (int r = 0; r < 3; r++) R[r][c] /= l;
                }
            }
        }
        float det = R[0][0] * (R[1][1] * R[2][2] - R[1][2] * R[2][1]) - R[0][1] * (R[1][0] * R[2][2] - R[1][2] * R[2][0]) +
                    R[0][2] * (R[1][0] * R[2][1] - R[1][1] * R[2][0]);
        int mir = o.mirror_z < 0 ? det < 0 : o.mirror_z;
        zs = mir ? -1.0f : 1.0f;
        /* posición del ojo: la del motor (eye_pos) o la que impone la traslación que usa la GTE (e = -R * view_t) */
        float ep[3] = {(float)bs->eye_pos[0], (float)bs->eye_pos[1], (float)bs->eye_pos[2]};
        if (o.eye_from_view_t)
            for (int r = 0; r < 3; r++) ep[r] = -(R[r][0] * (float)bs->view_t[0] + R[r][1] * (float)bs->view_t[1] + R[r][2] * (float)bs->view_t[2]);
        for (int r = 0; r < 3; r++) {
            float s = r == 2 ? zs : 1.0f;
            for (int c = 0; c < 3; c++) A.m[r][c] = o.scale * s * R[r][c];
            A.t[r] = o.scale * s * (ep[r] - org[r]);
        }
        /* world_valid: criterio conservador del puente (jugador en escena y grupos básicos válidos): en título, cargas y
         * cambios de zona los ejes conservan valores que "parecen" válidos pero no son de la cámara real. */
        out->world_valid = has_player_ok(bs);
        if (o.user_map_on) {   /* mundo VR -> espacio del usuario (ver VrSceneOpts.user_map_on): A' = Mu * A, t' = Mu * t + c */
            float Mu[3][3], cc[3];
            for (int r = 0; r < 3; r++) for (int cI = 0; cI < 3; cI++) Mu[r][cI] = o.user_R[3 * r + cI] * o.user_k * (cI == 2 ? zs : 1.0f);
            for (int r = 0; r < 3; r++) cc[r] = o.user_p_ref[r] - o.user_R[3 * r + 1] * o.user_eye_h_m;
            float m2[3][3], t2[3];
            for (int r = 0; r < 3; r++) {
                for (int cI = 0; cI < 3; cI++) m2[r][cI] = Mu[r][0] * A.m[0][cI] + Mu[r][1] * A.m[1][cI] + Mu[r][2] * A.m[2][cI];
                t2[r] = Mu[r][0] * A.t[0] + Mu[r][1] * A.t[1] + Mu[r][2] * A.t[2] + cc[r];
            }
            memcpy(A.m, m2, sizeof A.m); memcpy(A.t, t2, sizeof A.t);
            out->user_space = 1;
            memcpy(out->user_R, o.user_R, sizeof out->user_R); out->user_k = o.user_k; out->user_eye_h = o.user_eye_h_m;
            memcpy(out->user_p_ref, o.user_p_ref, sizeof out->user_p_ref);
        }
    } else {
        /* sin cámara del motor: espacio de vista (x der, y abajo, z adelante) -> VR (x der, y arriba, -z adelante) */
        A.m[0][0] = o.scale; A.m[1][1] = -o.scale; A.m[2][2] = -o.scale;
    }
    /* game_view columna-mayor */
    for (int c = 0; c < 3; c++) for (int r = 0; r < 3; r++) out->game_view[c * 4 + r] = A.m[r][c];
    for (int r = 0; r < 3; r++) { out->game_view[12 + r] = A.t[r]; out->eye_vr[r] = A.t[r]; out->origin_game[r] = org[r]; }
    out->game_view[15] = 1.0f;

    /* ---- siguiente evento de VRAM tras cada op ---- */
    uint32_t n = dl->nops;
    GROW(g_next, g_next_cap, (size_t)n + 1);
    {
        uint32_t nx = n;
        for (uint32_t i = n; i-- > 0;) {
            g_next[i] = nx; /* primer evento con índice > i */
            int t = dl->ops[i].type;
            if (t == GPU_OP_FILL || t == GPU_OP_VCOPY || t == GPU_OP_UPLOAD) { nx = i; out->stats.n_events++; }
        }
    }
    g_st_n = 0;
    if (g_hash) memset(g_hash, 0, g_hash_cap * sizeof *g_hash);
    GROW(g_tri, g_tri_cap, (size_t)n * 2 + 2);

    size_t nt = 0;
    out->stats.n_ops = n;
    if (g_fate) memset(g_fate, 0, n < g_fate_cap ? n : g_fate_cap);
    for (uint32_t i = 0; i < n; i++) {
        const GpuOp *op = &dl->ops[i];
        if (op->type != GPU_OP_POLY && op->type != GPU_OP_RECT && op->type != GPU_OP_LINE) continue;
        int st = state_id(op);
        if (st < 0) return st;
        uint16_t sid = (uint16_t)st;
        uint16_t f = (uint16_t)(op->flags & 0x3FF);
        int gour = (op->flags & GPU_F_GOURAUD) != 0;
        int semi = (op->flags & GPU_F_SEMI) != 0;
        GROW(g_tri, g_tri_cap, nt + 4);

        GpuOp rc;   /* copia de trabajo si se reconstruye un vértice */
        if (op->type == GPU_OP_POLY && o.recon_partial && op->nv == 4 && op->proj_h > 0 && isfinite(op->proj_h)) {
            int bad = -1, nbad = 0, clean = 1;
            for (int k = 0; k < 4; k++) {
                if (!op->v[k].vvalid) { bad = k; nbad++; }
                else if (op->v[k].vflags != 0 || !isfinite(op->v[k].vx + op->v[k].vy + op->v[k].vz)) clean = 0;
            }
            if (nbad == 1 && clean) {
                const GpuVtx *a = &op->v[(bad + 1) & 3], *b = &op->v[(bad + 2) & 3], *c = &op->v[(bad + 3) & 3];
                float dxs = (float)op->v[bad].x - (float)op->dox - op->proj_ox, dys = (float)op->v[bad].y - (float)op->doy - op->proj_oy;
                if (fabsf(dxs) < 1000.0f && fabsf(dys) < 1000.0f) {
                    /* plano de los tres vértices fiables (espacio de vista) ∩ rayo del cuarto (px de pantalla / H) */
                    float e1[3] = {b->vx - a->vx, b->vy - a->vy, b->vz - a->vz}, e2[3] = {c->vx - a->vx, c->vy - a->vy, c->vz - a->vz};
                    float n[3] = {e1[1]*e2[2] - e1[2]*e2[1], e1[2]*e2[0] - e1[0]*e2[2], e1[0]*e2[1] - e1[1]*e2[0]};
                    float d[3] = {dxs / op->proj_h, dys / op->proj_h, 1.0f};
                    float den = n[0]*d[0] + n[1]*d[1] + n[2]*d[2];
                    float nn = sqrtf(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]), dn = sqrtf(d[0]*d[0] + d[1]*d[1] + 1.0f);
                    if (nn > 0 && fabsf(den) > 0.05f * nn * dn) {
                        float t = (n[0]*a->vx + n[1]*a->vy + n[2]*a->vz) / den;
                        float zmin = fminf(a->vz, fminf(b->vz, c->vz)), zmax = fmaxf(a->vz, fmaxf(b->vz, c->vz));
                        if (isfinite(t) && t > 0 && zmin > 0 && t > 0.2f * zmin && t < 5.0f * zmax) {
                            rc = *op;
                            rc.v[bad].vvalid = 1; rc.v[bad].vflags = 0;
                            rc.v[bad].vx = d[0] * t; rc.v[bad].vy = d[1] * t; rc.v[bad].vz = t;
                            op = &rc;
                            out->stats.n_recon++;
                            fate_set(i, VR_FATE_RECON);
                        }
                    }
                }
            }
        }
        if (op->type == GPU_OP_POLY && o.recon_partial && op->nv == 4 && op->proj_h > 0 && isfinite(op->proj_h)) {
            /* esquinas diagonales (resplandores/destellos: cartel de cara a la cámara a z constante): las otras dos esquinas salen de las dos dadas.
             * Orden del cuadrilátero de la GPU: v0 arriba-izq, v1 arriba-der, v2 abajo-izq, v3 abajo-der. */
            int m = (op->v[0].vvalid ? 1 : 0) | (op->v[1].vvalid ? 2 : 0) | (op->v[2].vvalid ? 4 : 0) | (op->v[3].vvalid ? 8 : 0);
            if (m == 9 || m == 6) {
                int p0 = m == 9 ? 0 : 1, p1 = m == 9 ? 3 : 2;
                const GpuVtx *a = &op->v[p0], *b = &op->v[p1];
                float zz = a->vz;
                if (isfinite(a->vx + a->vy + a->vz + b->vx + b->vy + b->vz) && zz > 0 && fabsf(b->vz - zz) <= 0.02f * zz + 1.0f &&
                    a->vx != b->vx && a->vy != b->vy) {
                    rc = *op;
                    int k1 = m == 9 ? 1 : 0, k2 = m == 9 ? 2 : 3;   /* vértices a rellenar */
                    GpuVtx *g1 = &rc.v[k1], *g2 = &rc.v[k2];
                    g1->vvalid = g2->vvalid = 1; g1->vflags = g2->vflags = 0;
                    g1->vz = g2->vz = zz;
                    if (m == 9) { g1->vx = b->vx; g1->vy = a->vy; g2->vx = a->vx; g2->vy = b->vy; }   /* v1 = (x3,y0), v2 = (x0,y3) */
                    else        { g1->vx = a->vx; g1->vy = b->vy; g2->vx = b->vx; g2->vy = a->vy; }   /* v0 = (x2,y1), v3 = (x1,y2) */
                    op = &rc;
                    out->stats.n_recon++;
                    fate_set(i, VR_FATE_RECON);
                }
            }
        }
        if (op->type == GPU_OP_POLY) {
            out->stats.n_poly++;
            int nv = op->nv == 4 ? 4 : 3, nval = 0;
            unsigned uf = 0;   /* unión de vflags */
            for (int k = 0; k < nv; k++) { nval += op->v[k].vvalid; if (op->v[k].vvalid) uf |= op->v[k].vflags; }
            static const int idx[2][3] = {{0, 1, 2}, {1, 2, 3}};
            int ntri = nv == 4 ? 2 : 1;
            if (nval == nv) {
                /* contrato §28: polígono 3D = todos vvalid. AMBIG junto a SAT/BEHIND: posiciones poco fiables -> descartar. */
                if ((uf & GPU_VF_AMBIG) && (uf & (GPU_VF_SAT | GPU_VF_BEHIND)) && !(o.recon_ambig && nv == 4 && quad_coherent(op))) { out->stats.drop_ambig++; fate_set(i, VR_FATE_AMBIG); continue; }
                if ((uf & GPU_VF_DERIVED) && o.drop_derived) { out->stats.drop_derived++; fate_set(i, VR_FATE_DERIVED); continue; }
                out->stats.n_poly3d++;
                int clipmode = (uf & GPU_VF_BEHIND) != 0 || o.clip_all_near;
                if (uf & GPU_VF_BEHIND) out->stats.n_poly_behind++;
                CV cv[4];
                int ofs_fixed = 0;
                for (int k = 0; k < nv; k++) cv_from(&cv[k], op, &op->v[k], gour, &wref, &ofs_fixed);
                if (ofs_fixed) out->stats.n_ofs_fix++;
                for (int q = 0; q < ntri; q++) {
                    const CV *g3[3] = {&cv[idx[q][0]], &cv[idx[q][1]], &cv[idx[q][2]]};
                    int nanv = 0, near = 0, behind = 0;
                    for (int k = 0; k < 3; k++) {
                        if (!isfinite(g3[k]->p[0]) || !isfinite(g3[k]->p[1]) || !isfinite(g3[k]->p[2])) nanv = 1;
                        else if (g3[k]->p[2] < o.near_units) { if (o.engine_near_clip) near = 1; else behind = 1; }
                    }
                    if (nanv) { out->stats.drop_nan++; fate_set(i, VR_FATE_NAN); continue; }
                    CV poly[4]; int pn = 3;
                    if (behind) out->stats.n_unclipped++;
                    CV tin[3] = {*g3[0], *g3[1], *g3[2]};
                    if (near) {
                        if (!clipmode) { out->stats.drop_near++; fate_set(i, VR_FATE_NEAR); continue; }
                        pn = clip_near(tin, 3, o.near_units, poly);
                        if (pn < 3) { out->stats.drop_behind++; fate_set(i, VR_FATE_BEHIND); continue; }
                        out->stats.n_clipped++;
                    } else memcpy(poly, tin, sizeof tin);
                    for (int fan = 0; fan + 2 < pn; fan++) {
                        const CV *g[3] = {&poly[0], &poly[fan + 1], &poly[fan + 2]};
                        float p[3][3];
                        for (int k = 0; k < 3; k++) xf(&A, g[k], p[k]);
                        float e1[3], e2[3], e3[3];
                        for (int c = 0; c < 3; c++) { e1[c] = p[1][c] - p[0][c]; e2[c] = p[2][c] - p[0][c]; e3[c] = p[2][c] - p[1][c]; }
                        float l1 = e1[0]*e1[0] + e1[1]*e1[1] + e1[2]*e1[2], l2 = e2[0]*e2[0] + e2[1]*e2[1] + e2[2]*e2[2],
                              l3 = e3[0]*e3[0] + e3[1]*e3[1] + e3[2]*e3[2];
                        float me = o.max_edge_m * o.max_edge_m;
                        if (!(l1 <= me && l2 <= me && l3 <= me)) { out->stats.drop_huge++; fate_set(i, VR_FATE_HUGE); continue; }
                        float nrm[3] = {e1[1]*e2[2] - e1[2]*e2[1], e1[2]*e2[0] - e1[0]*e2[2], e1[0]*e2[1] - e1[1]*e2[0]};
                        float area = 0.5f * sqrtf(nrm[0]*nrm[0] + nrm[1]*nrm[1] + nrm[2]*nrm[2]);
                        if (!(area >= o.min_area_m2)) { out->stats.drop_degenerate++; fate_set(i, VR_FATE_DEGEN); continue; }
                        /* frente = cara que vio la cámara del juego */
                        float tc[3] = {out->eye_vr[0] - p[0][0], out->eye_vr[1] - p[0][1], out->eye_vr[2] - p[0][2]};
                        int flip = (nrm[0]*tc[0] + nrm[1]*tc[1] + nrm[2]*tc[2]) < 0;
                        Tri *t = &g_tri[nt++];
                        int ord[3] = {0, flip ? 2 : 1, flip ? 1 : 2};
                        uint16_t ff = (uint16_t)(f | (flip ? VR_F_FLIPPED : 0));
                        for (int k = 0; k < 3; k++) {
                            VrVtx *vv = &t->v[k];
                            fill_cv(vv, op, g[ord[k]], ff, sid);
                            memcpy(vv->pos, p[ord[k]], sizeof vv->pos);
                        }
                        t->ev_end = g_next[i]; t->src_op = i;
                        t->kind = semi ? 1 : 0; t->blend = semi ? (op->abr & 3) : 0xFF;
                        out->stats.n_tris3d++;
                        if (i < g_fate_cap) g_fate[i] = VR_FATE_3D;
                        if (flip) out->stats.n_flipped++;
                        GROW(g_tri, g_tri_cap, nt + 4);
                    }
                }
                continue;
            }
            if (nval && !o.partial_to_2d) { out->stats.drop_partial++; fate_set(i, VR_FATE_PARTIAL); continue; }
            out->stats.n_poly2d++;
            fate_set(i, VR_FATE_2D);
            if (o.drop_2d_all) continue;
            int fb = op->fbk != 0;
            if (fb && !op->fbt) { out->stats.n_fb_clear++; continue; }   /* texel 0 = transparente: no pinta nada */
            int fcls = 0;
            if (!fb) {
                int bx0 = 1 << 20, by0 = 1 << 20, bx1 = -(1 << 20), by1 = -(1 << 20), bu0 = 1 << 20, bv0 = 1 << 20, bu1 = -(1 << 20), bv1 = -(1 << 20);
                for (int k = 0; k < nv; k++) {
                    const GpuVtx *g = &op->v[k];
                    bx0 = g->x < bx0 ? g->x : bx0; bx1 = g->x > bx1 ? g->x : bx1; by0 = g->y < by0 ? g->y : by0; by1 = g->y > by1 ? g->y : by1;
                    bu0 = g->u < bu0 ? g->u : bu0; bu1 = g->u > bu1 ? g->u : bu1; bv0 = g->v < bv0 ? g->v : bv0; bv1 = g->v > bv1 ? g->v : bv1;
                }
                fcls = filter_class(&o, dl, op, 0, bx0, by0, bx1, by1, bu0, bv0, bu1, bv1);
            }
            if (fcls) filter_note(out, op, fcls, 0, &tint_w);
            for (int q = 0; q < ntri; q++) {
                Tri *t = &g_tri[nt++];
                for (int k = 0; k < 3; k++) {
                    const GpuVtx *g = &op->v[idx[q][k]];
                    fill_vtx(&t->v[k], op, g, gour, (uint16_t)(f | VR_F_2D | (fcls ? VR_F_FILTER : 0)), sid);
                    t->v[k].pos[0] = g->x; t->v[k].pos[1] = g->y; t->v[k].pos[2] = 0;
                    if (fb) {   /* sin textura: color = vértice * texel; flags sin TEXTURED/RAW */
                        t->v[k].rgba = fb_color(op, gour ? g : &op->v[0]);
                        t->v[k].uv[0] = t->v[k].uv[1] = 0; t->v[k].flags &= (uint16_t)~(GPU_F_TEXTURED | GPU_F_RAW);
                    }
                }
                int semi_e = fb ? semi && (op->fbt & 0x8000) : semi;
                t->ev_end = g_next[i]; t->src_op = i; t->kind = fcls ? 3 : 2; t->blend = semi_e ? (op->abr & 3) : 0xFF;
                if (fb) out->stats.n_fb_baked++;
                out->stats.n_tris2d += fcls ? 0 : 1; out->stats.n_filter_tris += fcls ? 1 : 0;
            }
        } else if (op->type == GPU_OP_RECT) {
            out->stats.n_rect++;
            fate_set(i, VR_FATE_2D);
            if (o.drop_2d_all) continue;
            const GpuVtx *g = &op->v[0];
            int fb = op->fbk != 0;
            if (fb && !op->fbt) { out->stats.n_fb_clear++; continue; }
            int fcls = fb ? 0 : filter_class(&o, dl, op, 1, g->x, g->y, g->x + op->rw, g->y + op->rh, g->u, g->v, g->u, g->v);
            if (fcls) filter_note(out, op, fcls, 1, &tint_w);
            else if (!semi && !(op->flags & GPU_F_TEXTURED) && (double)op->rw * op->rh >= 0.9 * (double)dl->disp.w * dl->disp.h) {
                /* borrado/máscara a pantalla completa: en el panel HUD (alfa premultiplicado, cleared a transparente) lo dejaría OPACO e
                 * ocultaría el mundo. Se cuenta y, con opts.drop_clear (def. 1), no se emite. */
                out->stats.n_clear++;
                if (o.drop_clear) continue;
            }
            float x0 = g->x, y0 = g->y, x1 = x0 + op->rw, y1 = y0 + op->rh;
            float u0 = g->u, v0 = g->v;
            float u1 = (f & GPU_F_FLIPX) ? u0 - op->rw : u0 + op->rw, v1 = (f & GPU_F_FLIPY) ? v0 - op->rh : v0 + op->rh;
            float cx[4] = {x0, x1, x0, x1}, cy[4] = {y0, y0, y1, y1}, cu[4] = {u0, u1, u0, u1}, cv[4] = {v0, v0, v1, v1};
            static const int idx[2][3] = {{0, 1, 2}, {1, 2, 3}};
            for (int q = 0; q < 2; q++) {
                Tri *t = &g_tri[nt++];
                for (int k = 0; k < 3; k++) {
                    int j = idx[q][k];
                    fill_vtx(&t->v[k], op, g, 0, (uint16_t)(f | VR_F_2D | VR_F_RECT | (fcls ? VR_F_FILTER : 0)), sid);
                    t->v[k].pos[0] = cx[j]; t->v[k].pos[1] = cy[j]; t->v[k].pos[2] = 0;
                    t->v[k].uv[0] = cu[j]; t->v[k].uv[1] = cv[j];
                    if (fb) {
                        t->v[k].rgba = fb_color(op, g);
                        t->v[k].uv[0] = t->v[k].uv[1] = 0; t->v[k].flags &= (uint16_t)~(GPU_F_TEXTURED | GPU_F_RAW | GPU_F_FLIPX | GPU_F_FLIPY);
                    }
                }
                int semi_e = fb ? semi && (op->fbt & 0x8000) : semi;
                t->ev_end = g_next[i]; t->src_op = i; t->kind = fcls ? 3 : 2; t->blend = semi_e ? (op->abr & 3) : 0xFF;
                if (fb) out->stats.n_fb_baked++;
                out->stats.n_tris2d += fcls ? 0 : 1; out->stats.n_filter_tris += fcls ? 1 : 0;
            }
        } else { /* LINE: cuadrilátero de 1 px */
            out->stats.n_line++;
            fate_set(i, VR_FATE_2D);
            if (o.drop_2d_all) continue;
            const GpuVtx *a = &op->v[0], *b = &op->v[1];
            float dx = (float)(b->x - a->x), dy = (float)(b->y - a->y), l = sqrtf(dx*dx + dy*dy);
            float cx[4], cy[4];
            /* El píxel (x,y) ocupa [x,x+1)x[y,y+1): la línea (extremos inclusivos) va por los centros (+0,5) y se alarga 0,5 en
             * cada extremo, para cubrir exactamente los píxeles del rasterizador de líneas. */
            if (l < 1e-6f) { /* punto: cuadrado 1x1 */
                cx[0] = cx[2] = a->x; cx[1] = cx[3] = a->x + 1.0f; cy[0] = cy[1] = a->y; cy[2] = cy[3] = a->y + 1.0f;
            } else {
                float ux = dx / l * 0.5f, uy = dy / l * 0.5f, nx = -uy, ny = ux;
                float ax = a->x + 0.5f - ux, ay = a->y + 0.5f - uy, bx = b->x + 0.5f + ux, by = b->y + 0.5f + uy;
                cx[0] = ax - nx; cy[0] = ay - ny; cx[1] = ax + nx; cy[1] = ay + ny;
                cx[2] = bx - nx; cy[2] = by - ny; cx[3] = bx + nx; cy[3] = by + ny;
            }
            const GpuVtx *cs[4] = {a, a, b, b};
            static const int idx[2][3] = {{0, 1, 2}, {1, 2, 3}};
            for (int q = 0; q < 2; q++) {
                Tri *t = &g_tri[nt++];
                for (int k = 0; k < 3; k++) {
                    int j = idx[q][k];
                    fill_vtx(&t->v[k], op, cs[j], gour, (uint16_t)((f & ~GPU_F_TEXTURED) | VR_F_2D | VR_F_LINE), sid);
                    t->v[k].pos[0] = cx[j]; t->v[k].pos[1] = cy[j]; t->v[k].pos[2] = 0;
                }
                t->ev_end = g_next[i]; t->src_op = i; t->kind = 2; t->blend = semi ? (op->abr & 3) : 0xFF;
                out->stats.n_tris2d++;
            }
        }
    }

    /* ---- cierre del resumen de clase 3b: líneas de exploración y máscara de visión ---- */
    {
        VrFilter *f = &out->filter;
        if (f->n_scan > 0) {
            if (!(f->scan_pitch >= 1.0f)) f->scan_pitch = f->n_scan > 1 ? 2.0f : 0.0f;   /* sin pareja: no se puede saber el periodo */
            if (f->scan_pitch >= 1.0f) { float ph = fmodf(g_scan_y0, f->scan_pitch); f->scan_phase = ph < 0 ? ph + f->scan_pitch : ph; }
            if (f->n_scan < 8 || !(f->scan_pitch >= 1.0f)) f->n_scan = 0;   /* un par de tiras sueltas no son un efecto de pantalla */
            { int dropped = f->n_cover - f->n_covers, sa = g_scan_cover_seq - dropped; f->scan_after = sa < 0 ? 0 : sa > f->n_covers ? f->n_covers : sa; }
        }
        if (g_frame_on && out->stats.n_filter_frame > 0) {
            for (int i = 0; i < VR_FRAME_W * VR_FRAME_H; i++) { float v = g_frame_acc[i]; g_frame_out[i] = (uint8_t)(v <= 0 ? 0 : v >= 1 ? 255 : (int)(v * 255.0f + 0.5f)); }
            f->frame_on = 1; f->frame = g_frame_out; f->frame_w = VR_FRAME_W; f->frame_h = VR_FRAME_H;
            if (bs && (bs->valid & BRIDGE_OK_EQUIP) && bs->item >= 2 && bs->item <= 4) { f->frame_rgb[0] = 0.12f; f->frame_rgb[1] = 0.085f; f->frame_rgb[2] = 0.055f; }   /* caja de cartón: el interior no es negro puro */
        }
    }

    /* ---- ensamblado: épocas (mismo ev_end), dentro clase 0,1,2; batch nuevo al cambiar blend ---- */
    GROW(g_vtx, g_vtx_cap, nt * 3 + 3);
    GROW(g_bat, g_bat_cap, nt + 4);
    size_t nv = 0, nb = 0;
    for (size_t s = 0; s < nt;) {
        size_t e = s;
        while (e < nt && g_tri[e].ev_end == g_tri[s].ev_end) e++;
        for (int kind = 0; kind < 4; kind++) {
            VrBatch *cur = NULL;
            for (size_t k = s; k < e; k++) {
                const Tri *t = &g_tri[k];
                if (t->kind != kind) continue;
                if (!cur || cur->blend != t->blend) {
                    cur = &g_bat[nb++];
                    cur->first = (uint32_t)nv; cur->count = 0; cur->kind = (uint8_t)kind; cur->blend = t->blend; cur->pad = 0;
                    cur->ev_end = t->ev_end; cur->src_op = t->src_op;
                }
                memcpy(&g_vtx[nv], t->v, sizeof t->v);
                nv += 3; cur->count += 3;
            }
        }
        s = e;
    }
    out->v = g_vtx; out->nv = (uint32_t)nv;
    out->b = g_bat; out->nb = (uint32_t)nb;
    out->states = g_st; out->nstates = (uint32_t)g_st_n;
    out->has_3d = out->stats.n_tris3d > 0;
    out->stats.n_batches = (uint32_t)nb;
    return 0;
}
