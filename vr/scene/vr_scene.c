/* vr_scene: ver vr_scene.h para el contrato. Aquí: transformación vista del juego -> mundo VR, clasificación 3D/2D,
 * descartes, partición por épocas de VRAM.
 *
 * ================================ Escala (metros por unidad del juego) ================================
 * Hipótesis de partida: 1 unidad del motor = 1 mm (defecto 0,001). Evidencia de datos (partida real, zona s00a):
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

static void fill_vtx(VrVtx *o, const GpuOp *op, const GpuVtx *v, int gour, uint16_t flags, uint16_t st) {
    const GpuVtx *c = gour ? v : &op->v[0];
    o->rgba = pack_rgba(c->r, c->g, c->b);
    o->uv[0] = (float)v->u; o->uv[1] = (float)v->v;
    o->clut = (uint16_t)((op->clx >> 4) | (op->cly << 6));
    o->tpage = (uint16_t)(((op->tx >> 6) & 15) | (((op->ty >> 8) & 1) << 4) | ((op->abr & 3) << 5) | ((op->tp & 3) << 7));
    o->flags = flags;
    o->pad = st;
}

static int has_player_ok(const BridgeState *bs) {
    return bs && bs->has_player && (bs->valid & BRIDGE_OK_ALL_CORE) == BRIDGE_OK_ALL_CORE;
}

typedef struct { float m[3][3]; float t[3]; } Aff; /* v_vr = m*v + t */

static void xf(const Aff *a, const GpuVtx *v, float *out) {
    for (int r = 0; r < 3; r++) out[r] = a->m[r][0] * v->vx + a->m[r][1] * v->vy + a->m[r][2] * v->vz + a->t[r];
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

int vr_scene_build(const GpuDrawList *dl, const BridgeState *bs, const VrSceneOpts *op_in, VrScene *out) {
    if (!dl || !out) return -1;
    VrSceneOpts o = op_in ? *op_in : vr_scene_opts_default();
    if (!(o.scale > 0)) o.scale = 0.001f;
    memset(out, 0, sizeof *out);
    out->src = dl;
    out->scale = o.scale;

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

        if (op->type == GPU_OP_POLY) {
            out->stats.n_poly++;
            int nv = op->nv == 4 ? 4 : 3, nval = 0;
            for (int k = 0; k < nv; k++) nval += op->v[k].fvalid;
            static const int idx[2][3] = {{0, 1, 2}, {1, 2, 3}};
            int ntri = nv == 4 ? 2 : 1;
            if (nval == nv) {
                out->stats.n_poly3d++;
                for (int q = 0; q < ntri; q++) {
                    const GpuVtx *g[3] = {&op->v[idx[q][0]], &op->v[idx[q][1]], &op->v[idx[q][2]]};
                    float p[3][3];
                    int nanv = 0, near = 0;
                    for (int k = 0; k < 3; k++) {
                        if (!isfinite(g[k]->vx) || !isfinite(g[k]->vy) || !isfinite(g[k]->vz)) nanv = 1;
                        else if (g[k]->vz < o.near_units) near = 1;
                    }
                    if (nanv) { out->stats.drop_nan++; continue; }
                    if (near) { out->stats.drop_near++; continue; }
                    for (int k = 0; k < 3; k++) xf(&A, g[k], p[k]);
                    float e1[3], e2[3], e3[3];
                    for (int c = 0; c < 3; c++) { e1[c] = p[1][c] - p[0][c]; e2[c] = p[2][c] - p[0][c]; e3[c] = p[2][c] - p[1][c]; }
                    float l1 = e1[0]*e1[0] + e1[1]*e1[1] + e1[2]*e1[2], l2 = e2[0]*e2[0] + e2[1]*e2[1] + e2[2]*e2[2],
                          l3 = e3[0]*e3[0] + e3[1]*e3[1] + e3[2]*e3[2];
                    float me = o.max_edge_m * o.max_edge_m;
                    if (!(l1 <= me && l2 <= me && l3 <= me)) { out->stats.drop_huge++; continue; }
                    float nrm[3] = {e1[1]*e2[2] - e1[2]*e2[1], e1[2]*e2[0] - e1[0]*e2[2], e1[0]*e2[1] - e1[1]*e2[0]};
                    float area = 0.5f * sqrtf(nrm[0]*nrm[0] + nrm[1]*nrm[1] + nrm[2]*nrm[2]);
                    if (!(area >= o.min_area_m2)) { out->stats.drop_degenerate++; continue; }
                    /* frente = cara que vio la cámara del juego */
                    float tc[3] = {out->eye_vr[0] - p[0][0], out->eye_vr[1] - p[0][1], out->eye_vr[2] - p[0][2]};
                    int flip = (nrm[0]*tc[0] + nrm[1]*tc[1] + nrm[2]*tc[2]) < 0;
                    Tri *t = &g_tri[nt++];
                    int ord[3] = {0, flip ? 2 : 1, flip ? 1 : 2};
                    uint16_t ff = (uint16_t)(f | (flip ? VR_F_FLIPPED : 0));
                    for (int k = 0; k < 3; k++) {
                        VrVtx *vv = &t->v[k];
                        fill_vtx(vv, op, g[ord[k]], gour, ff, sid);
                        memcpy(vv->pos, p[ord[k]], sizeof vv->pos);
                    }
                    t->ev_end = g_next[i]; t->src_op = i;
                    t->kind = semi ? 1 : 0; t->blend = semi ? (op->abr & 3) : 0xFF;
                    out->stats.n_tris3d++;
                    if (flip) out->stats.n_flipped++;
                }
                continue;
            }
            if (nval && !o.partial_to_2d) { out->stats.drop_partial++; continue; }
            out->stats.n_poly2d++;
            if (o.drop_2d_all) continue;
            for (int q = 0; q < ntri; q++) {
                Tri *t = &g_tri[nt++];
                for (int k = 0; k < 3; k++) {
                    const GpuVtx *g = &op->v[idx[q][k]];
                    fill_vtx(&t->v[k], op, g, gour, (uint16_t)(f | VR_F_2D), sid);
                    t->v[k].pos[0] = g->x; t->v[k].pos[1] = g->y; t->v[k].pos[2] = 0;
                }
                t->ev_end = g_next[i]; t->src_op = i; t->kind = 2; t->blend = semi ? (op->abr & 3) : 0xFF;
                out->stats.n_tris2d++;
            }
        } else if (op->type == GPU_OP_RECT) {
            out->stats.n_rect++;
            if (o.drop_2d_all) continue;
            const GpuVtx *g = &op->v[0];
            float x0 = g->x, y0 = g->y, x1 = x0 + op->rw, y1 = y0 + op->rh;
            float u0 = g->u, v0 = g->v;
            float u1 = (f & GPU_F_FLIPX) ? u0 - op->rw : u0 + op->rw, v1 = (f & GPU_F_FLIPY) ? v0 - op->rh : v0 + op->rh;
            float cx[4] = {x0, x1, x0, x1}, cy[4] = {y0, y0, y1, y1}, cu[4] = {u0, u1, u0, u1}, cv[4] = {v0, v0, v1, v1};
            static const int idx[2][3] = {{0, 1, 2}, {1, 2, 3}};
            for (int q = 0; q < 2; q++) {
                Tri *t = &g_tri[nt++];
                for (int k = 0; k < 3; k++) {
                    int j = idx[q][k];
                    fill_vtx(&t->v[k], op, g, 0, (uint16_t)(f | VR_F_2D | VR_F_RECT), sid);
                    t->v[k].pos[0] = cx[j]; t->v[k].pos[1] = cy[j]; t->v[k].pos[2] = 0;
                    t->v[k].uv[0] = cu[j]; t->v[k].uv[1] = cv[j];
                }
                t->ev_end = g_next[i]; t->src_op = i; t->kind = 2; t->blend = semi ? (op->abr & 3) : 0xFF;
                out->stats.n_tris2d++;
            }
        } else { /* LINE: cuadrilátero de 1 px */
            out->stats.n_line++;
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

    /* ---- ensamblado: épocas (mismo ev_end), dentro clase 0,1,2; batch nuevo al cambiar blend ---- */
    GROW(g_vtx, g_vtx_cap, nt * 3 + 3);
    GROW(g_bat, g_bat_cap, nt + 4);
    size_t nv = 0, nb = 0;
    for (size_t s = 0; s < nt;) {
        size_t e = s;
        while (e < nt && g_tri[e].ev_end == g_tri[s].ev_end) e++;
        for (int kind = 0; kind < 3; kind++) {
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
