/* box_view: ver box_view.h. */
#include "box_view.h"
#include "box_data.h"
#include "../weapons/wcore.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SHELL_MAXV 2304
#define SHELL_MAXI 7168
#define SOFT_MAXV  (BOX_MAX_BAND * 4)
#define SOFT_MAXI  (BOX_MAX_BAND * 6)
#define UV_PER_M   5.0f              /* repeticiones de la baldosa por metro */
#define SOFT_LIFT  0.002f            /* la banda se adelanta 2 mm hacia el ojo respecto al plano del panel */
#define SOFT_ALPHA_CARD 0.88f

typedef struct { WVertex *v; uint16_t *idx; uint32_t nv, ni, cv, ci; } Buf;

static struct {
    int inited;
    BoxViewConfig cfg;
    BoxSight sight;
    uint8_t *tex; int tw, th;
    int want, wide;
    BoxState state;
    float drop, open;
    WQuat lag_q; int lag_ok;
    WPose head, shell;
    float floor_alpha;
    Buf shell_b, floor_b, soft_b;
    WMesh m_shell, m_floor, m_soft;
    uint32_t rev;
    float built_open; int built_wide; BoxStyle built_style; int built_valid;
} G;

/* ---------------- utilidades ---------------- */
static float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static float smoothstepf(float a, float b, float x) {
    float t = clampf((x - a) / (b - a), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}
static float ease_out(float t) { t = clampf(t, 0.f, 1.f); return t * t * (3.f - 2.f * t); }   /* suave en ambos extremos (bajada y subida) */

static void buf_free(Buf *b) { free(b->v); free(b->idx); memset(b, 0, sizeof *b); }
static int buf_alloc(Buf *b, uint32_t cv, uint32_t ci) {
    b->v = (WVertex *)calloc(cv, sizeof(WVertex));
    b->idx = (uint16_t *)calloc(ci, sizeof(uint16_t));
    b->cv = cv; b->ci = ci; b->nv = b->ni = 0;
    return (b->v && b->idx) ? 0 : -1;
}

static int style_black(void) { return G.cfg.style == BOX_STYLE_BLACK; }
static int use_tex(void) { return !style_black() && G.tex != NULL; }

/* color base del interior: blanco si hay textura (la multiplica); marrón liso si no */
static void shade(uint8_t out[4], float b, float alpha) {
    float r = 1.f, g = 1.f, bl = 1.f;
    if (style_black()) { r = g = bl = 0.f; }
    else if (!G.tex) { r = 0.43f; g = 0.37f; bl = 0.26f; }
    out[0] = (uint8_t)(clampf(b * r, 0.f, 1.f) * 255.f + 0.5f);
    out[1] = (uint8_t)(clampf(b * g, 0.f, 1.f) * 255.f + 0.5f);
    out[2] = (uint8_t)(clampf(b * bl, 0.f, 1.f) * 255.f + 0.5f);
    out[3] = (uint8_t)(clampf(alpha, 0.f, 1.f) * 255.f + 0.5f);
}

static int vtx(Buf *b, WVec3 p, WVec3 n, float u, float v, float bright, float alpha) {
    if (b->nv >= b->cv) return -1;
    WVertex *w = &b->v[b->nv];
    w->pos[0] = p.x; w->pos[1] = p.y; w->pos[2] = p.z;
    w->nrm[0] = n.x; w->nrm[1] = n.y; w->nrm[2] = n.z;
    w->uv[0] = u; w->uv[1] = v;
    shade(w->rgba, bright, alpha);
    return (int)b->nv++;
}

/* triángulo antihorario visto desde el lado al que apunta 'want' */
static void tri(Buf *b, int a, int c, int d, WVec3 want) {
    if (a < 0 || c < 0 || d < 0 || b->ni + 3 > b->ci) return;
    const WVertex *A = &b->v[a], *C = &b->v[c], *D = &b->v[d];
    WVec3 e1 = wv(C->pos[0] - A->pos[0], C->pos[1] - A->pos[1], C->pos[2] - A->pos[2]);
    WVec3 e2 = wv(D->pos[0] - A->pos[0], D->pos[1] - A->pos[1], D->pos[2] - A->pos[2]);
    if (wv_dot(wv_cross(e1, e2), want) < 0.f) { int t = c; c = d; d = t; }
    b->idx[b->ni++] = (uint16_t)a; b->idx[b->ni++] = (uint16_t)c; b->idx[b->ni++] = (uint16_t)d;
}

static float cur_scale_x(void) { return G.wide ? G.cfg.wide_x : 1.f; }
static float cur_scale_y(void) { return G.wide ? G.cfg.wide_y : 1.f; }

/* píxel del lienzo -> panel frontal (m); open = 0 cerrado (inicial del original), 1 abierto */
static void px_to_plane(const BoxVtx *v, float open, float *X, float *Y) {
    float k = 1.f - open;
    float px = v->x + v->sx * k, py = v->y + v->sy * k;
    px = G.sight.cx + (px - G.sight.cx) * cur_scale_x();
    py = G.sight.cy + (py - G.sight.cy) * cur_scale_y();
    float f = G.cfg.distance / G.cfg.focal_px;
    *X = (px - 0.5f * (float)G.sight.canvas_w) * f;
    *Y = -(py - G.cfg.view_cy) * f;
}

WVec3 box_view_px_to_local(float px, float py) {
    float f = G.cfg.distance / G.cfg.focal_px;
    return wv((px - 0.5f * (float)G.sight.canvas_w) * f, -(py - G.cfg.view_cy) * f, -G.cfg.distance);
}

/* ---------------- construcción de mallas ---------------- */
static void quad4(Buf *b, WVec3 p0, WVec3 p1, WVec3 p2, WVec3 p3, WVec3 n,
                  const float uv[4][2], const float br[4]) {
    int i0 = vtx(b, p0, n, uv[0][0], uv[0][1], br[0], 1.f);
    int i1 = vtx(b, p1, n, uv[1][0], uv[1][1], br[1], 1.f);
    int i2 = vtx(b, p2, n, uv[2][0], uv[2][1], br[2], 1.f);
    int i3 = vtx(b, p3, n, uv[3][0], uv[3][1], br[3], 1.f);
    tri(b, i0, i1, i2, n);
    tri(b, i0, i2, i3, n);
}

static float wrap_pi(float a) {
    while (a > (float)M_PI) a -= 2.f * (float)M_PI;
    while (a <= -(float)M_PI) a += 2.f * (float)M_PI;
    return a;
}

static void build_walls(Buf *b) {
    const BoxViewConfig *c = &G.cfg;
    float W = c->half_w, T = c->top, B = c->bottom, Z0 = -c->distance, Z1 = c->back;
    float s = UV_PER_M;
    float bf = 0.30f, bb = 0.12f;       /* luz que entra por la abertura: más cerca del panel, más clara */
    /* izquierda (x = -W, normal +X) */
    {
        float uv[4][2] = {{Z0 * s, -T * s}, {Z1 * s, -T * s}, {Z1 * s, B * s}, {Z0 * s, B * s}};
        float br[4] = {bf, bb, bb * 0.8f, bf * 0.8f};
        quad4(b, wv(-W, T, Z0), wv(-W, T, Z1), wv(-W, -B, Z1), wv(-W, -B, Z0), wv(1, 0, 0), uv, br);
    }
    { /* derecha */
        float uv[4][2] = {{Z0 * s, -T * s}, {Z1 * s, -T * s}, {Z1 * s, B * s}, {Z0 * s, B * s}};
        float br[4] = {bf, bb, bb * 0.8f, bf * 0.8f};
        quad4(b, wv(W, T, Z0), wv(W, T, Z1), wv(W, -B, Z1), wv(W, -B, Z0), wv(-1, 0, 0), uv, br);
    }
    { /* techo (y = +T, normal -Y) */
        float uv[4][2] = {{-W * s, Z0 * s}, {W * s, Z0 * s}, {W * s, Z1 * s}, {-W * s, Z1 * s}};
        float br[4] = {bf, bf, bb, bb};
        quad4(b, wv(-W, T, Z0), wv(W, T, Z0), wv(W, T, Z1), wv(-W, T, Z1), wv(0, -1, 0), uv, br);
    }
    { /* fondo (z = +back, normal -Z) */
        float uv[4][2] = {{-W * s, -T * s}, {W * s, -T * s}, {W * s, B * s}, {-W * s, B * s}};
        float br[4] = {bb, bb, bb * 0.7f, bb * 0.7f};
        quad4(b, wv(-W, T, Z1), wv(W, T, Z1), wv(W, -B, Z1), wv(-W, -B, Z1), wv(0, 0, -1), uv, br);
    }
}

static void build_floor(Buf *b) {
    const BoxViewConfig *c = &G.cfg;
    float W = c->half_w, B = c->bottom, Z0 = -c->distance, Z1 = c->back, s = UV_PER_M;
    float uv[4][2] = {{-W * s, Z0 * s}, {W * s, Z0 * s}, {W * s, Z1 * s}, {-W * s, Z1 * s}};
    float br[4] = {0.14f, 0.14f, 0.10f, 0.10f};
    quad4(b, wv(-W, -B, Z0), wv(W, -B, Z0), wv(W, -B, Z1), wv(-W, -B, Z1), wv(0, 1, 0), uv, br);
}

/* panel frontal: anillo entre el contorno de la abertura y el rectángulo interior + canto con grosor */
static void build_front(Buf *b, float open) {
    const BoxViewConfig *c = &G.cfg;
    int n = G.sight.nout;
    if (n < 3) return;
    float W = c->half_w, T = c->top, B = c->bottom, Z = -c->distance, margin = 0.012f;
    float X[BOX_MAX_OUT], Y[BOX_MAX_OUT], OX[BOX_MAX_OUT], OY[BOX_MAX_OUT], ang[BOX_MAX_OUT];
    float cxp = 0.f, cyp = 0.f;
    for (int i = 0; i < n; i++) {
        px_to_plane(&G.sight.out[i], open, &X[i], &Y[i]);
        X[i] = clampf(X[i], -W + margin, W - margin);
        Y[i] = clampf(Y[i], -B + margin, T - margin);
        cxp += X[i]; cyp += Y[i];
    }
    cxp /= (float)n; cyp /= (float)n;
    for (int i = 0; i < n; i++) {
        float dx = X[i] - cxp, dy = Y[i] - cyp;
        ang[i] = atan2f(dy, dx);
        float tx = dx > 1e-9f ? (W - cxp) / dx : (dx < -1e-9f ? (-W - cxp) / dx : 1e9f);
        float ty = dy > 1e-9f ? (T - cyp) / dy : (dy < -1e-9f ? (-B - cyp) / dy : 1e9f);
        float t = tx < ty ? tx : ty;
        if (t < 1.f) t = 1.f;
        OX[i] = cxp + dx * t; OY[i] = cyp + dy * t;
    }
    WVec3 nz = wv(0, 0, 1);
    float s = UV_PER_M;
    const float cornerX[4] = {W, -W, -W, W}, cornerY[4] = {T, T, -B, -B};
    float cang[4];
    for (int k = 0; k < 4; k++) cang[k] = atan2f(cornerY[k] - cyp, cornerX[k] - cxp);

    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        int a = vtx(b, wv(X[i], Y[i], Z), nz, X[i] * s, -Y[i] * s, 0.86f, 1.f);
        int bb = vtx(b, wv(X[j], Y[j], Z), nz, X[j] * s, -Y[j] * s, 0.86f, 1.f);
        int A = vtx(b, wv(OX[i], OY[i], Z), nz, OX[i] * s, -OY[i] * s, 0.20f, 1.f);
        int B2 = vtx(b, wv(OX[j], OY[j], Z), nz, OX[j] * s, -OY[j] * s, 0.20f, 1.f);
        /* esquinas del rectángulo que caen entre los rayos i y j */
        int ks[4], nk = 0;
        float dij = wrap_pi(ang[j] - ang[i]);
        float kd[4];
        for (int k = 0; k < 4; k++) {
            float dik = wrap_pi(cang[k] - ang[i]);
            if (dij != 0.f && dik * dij > 0.f && fabsf(dik) < fabsf(dij)) { ks[nk] = k; kd[nk] = fabsf(dik); nk++; }
        }
        for (int p = 0; p < nk; p++)           /* ordena por cercanía angular al rayo i */
            for (int q = p + 1; q < nk; q++)
                if (kd[q] < kd[p]) { float t = kd[p]; kd[p] = kd[q]; kd[q] = t; int u = ks[p]; ks[p] = ks[q]; ks[q] = u; }
        tri(b, a, bb, B2, nz);
        int prev = B2;
        for (int p = nk - 1; p >= 0; p--) {     /* de j hacia i */
            int k = ks[p];
            int kv = vtx(b, wv(cornerX[k], cornerY[k], Z), nz, cornerX[k] * s, -cornerY[k] * s, 0.18f, 1.f);
            tri(b, a, prev, kv, nz);
            prev = kv;
        }
        tri(b, a, prev, A, nz);
    }

    /* canto: pared de la abertura, de z = -D (borde hacia el ojo) a z = -D - rim, mirando al centro del hueco */
    float perim = 0.f;
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        float ex = X[j] - X[i], ey = Y[j] - Y[i];
        float len = hypotf(ex, ey);
        if (len < 1e-7f) continue;
        WVec3 nr = wv(ey / len, -ex / len, 0.f);          /* normal en el plano XY */
        float mx = 0.5f * (X[i] + X[j]) - cxp, my = 0.5f * (Y[i] + Y[j]) - cyp;
        if (nr.x * mx + nr.y * my > 0.f) { nr.x = -nr.x; nr.y = -nr.y; }   /* hacia dentro del hueco */
        float u0 = perim * s, u1 = (perim + len) * s, v1 = c->rim * s;
        int p0 = vtx(b, wv(X[i], Y[i], Z), nr, u0, 0.f, 0.80f, 1.f);
        int p1 = vtx(b, wv(X[j], Y[j], Z), nr, u1, 0.f, 0.80f, 1.f);
        int p2 = vtx(b, wv(X[j], Y[j], Z - c->rim), nr, u1, v1, 0.55f, 1.f);
        int p3 = vtx(b, wv(X[i], Y[i], Z - c->rim), nr, u0, v1, 0.55f, 1.f);
        tri(b, p0, p1, p2, nr);
        tri(b, p0, p2, p3, nr);
        perim += len;
    }
}

/* degradado interior (resta del original -> negro con alfa) */
static void build_soft(Buf *b, float open) {
    const BoxViewConfig *c = &G.cfg;
    float amax = style_black() ? 1.f : SOFT_ALPHA_CARD;
    float Z = -c->distance + SOFT_LIFT;
    WVec3 nz = wv(0, 0, 1);
    for (int i = 0; i < G.sight.nband; i++) {
        const BoxBandQuad *q = &G.sight.band[i];
        int id[4];
        for (int k = 0; k < 4; k++) {
            float X, Y;
            px_to_plane(&q->v[k], open, &X, &Y);
            id[k] = vtx(b, wv(X, Y, Z), nz, 0.f, 0.f, 0.f, q->a[k] * amax);
            if (id[k] >= 0) { b->v[id[k]].rgba[0] = b->v[id[k]].rgba[1] = b->v[id[k]].rgba[2] = 0; }
        }
        tri(b, id[0], id[1], id[2], nz);
        tri(b, id[1], id[3], id[2], nz);
    }
}

static void finish_mesh(WMesh *m, Buf *b, const char *name, int textured) {
    m->name = name; m->v = b->v; m->nv = b->nv; m->idx = b->idx; m->ni = b->ni;
    m->texture = (uint16_t)(textured ? 1 : 0);
}

static void rebuild(void) {
    G.shell_b.nv = G.shell_b.ni = 0;
    G.floor_b.nv = G.floor_b.ni = 0;
    G.soft_b.nv = G.soft_b.ni = 0;
    build_walls(&G.shell_b);
    build_front(&G.shell_b, G.open);
    build_floor(&G.floor_b);
    build_soft(&G.soft_b, G.open);
    int t = use_tex();
    finish_mesh(&G.m_shell, &G.shell_b, "shell", t);
    finish_mesh(&G.m_floor, &G.floor_b, "floor", t);
    finish_mesh(&G.m_soft, &G.soft_b, "soft", 0);
    G.built_open = G.open; G.built_wide = G.wide; G.built_style = G.cfg.style; G.built_valid = 1;
    G.rev++;
}

/* ---------------- API ---------------- */
void box_view_default_config(BoxViewConfig *c) {
    memset(c, 0, sizeof *c);
    c->stage = NULL; c->sight_id = 0; c->tex_id = 0; c->tex_y0 = 13; c->tex_y1 = 24;
    c->focal_px = 320.f; c->view_cy = 120.f;
    c->distance = 0.26f; c->rim = 0.012f;
    c->half_w = 0.21f; c->top = 0.17f; c->bottom = 0.40f; c->back = 0.20f;
    c->ipd = 0.064f;
    c->style = BOX_STYLE_TEX;
    c->lag_rate = 16.f; c->lag_max_deg = 2.5f;
    c->drop_time = 0.55f; c->open_time = 0.32f; c->lift = 0.85f;
    c->wide_x = 1.5f; c->wide_y = 2.0f;
}

void box_view_shutdown(void) {
    buf_free(&G.shell_b); buf_free(&G.floor_b); buf_free(&G.soft_b);
    free(G.tex);
    memset(&G, 0, sizeof G);
}

int box_view_init(const BoxViewConfig *c, const uint8_t *dir, size_t dir_len) {
    box_view_shutdown();
    if (c) G.cfg = *c; else box_view_default_config(&G.cfg);
    BoxViewConfig *k = &G.cfg;
    if (!(k->focal_px > 1.f)) k->focal_px = 320.f;
    if (!(k->distance > 0.05f)) k->distance = 0.26f;
    if (!(k->drop_time > 0.01f)) k->drop_time = 0.01f;
    if (!(k->open_time > 0.01f)) k->open_time = 0.01f;

    int rc = 1;
    if (dir && dir_len && k->stage) {
        const uint8_t *p; size_t n;
        if (box_stage_find(dir, dir_len, k->stage, k->sight_id, 's', &p, &n) == 0 &&
            box_sight_parse(p, n, &G.sight) == 0) rc = 0;
        if (rc == 0 && box_stage_find(dir, dir_len, k->stage, k->tex_id, 'p', &p, &n) == 0) {
            int w, h; uint8_t *rgba;
            if (box_tex_decode(p, n, &w, &h, &rgba) == 0) {
                int y0 = k->tex_y0, y1 = k->tex_y1;
                if (y0 < 0 || y1 > h || y1 <= y0) { y0 = 0; y1 = h; }
                G.tw = w; G.th = y1 - y0;
                G.tex = (uint8_t *)malloc((size_t)G.tw * G.th * 4);
                if (G.tex) memcpy(G.tex, rgba + (size_t)y0 * w * 4, (size_t)G.tw * G.th * 4);
                free(rgba);
            }
        }
    }
    if (rc != 0) {      /* respaldo genérico: rectángulo redondeado con proporción parecida, sin textura */
        box_sight_fallback(&G.sight, 320, 256, 160.f, 105.f, 285.f, 110.f, 40.f, 17.f);
        free(G.tex); G.tex = NULL;
    }
    if (buf_alloc(&G.shell_b, SHELL_MAXV, SHELL_MAXI) || buf_alloc(&G.floor_b, 16, 16) ||
        buf_alloc(&G.soft_b, SOFT_MAXV, SOFT_MAXI)) { box_view_shutdown(); return -1; }
    G.state = BOX_OFF; G.drop = 0.f; G.open = 0.f; G.want = 0; G.wide = 0;
    G.head.q = wquat_identity(); G.shell = G.head; G.lag_q = G.head.q; G.lag_ok = 0;
    G.inited = 1;
    rebuild();
    return rc;
}

void box_view_set_active(int on) { G.want = on ? 1 : 0; }
int  box_view_is_active(void) { return G.want; }
int  box_view_visible(void) { return G.inited && G.state != BOX_OFF; }
BoxState box_view_state(void) { return G.state; }
float box_view_drop(void) { return G.drop; }
float box_view_open(void) { return G.open; }
uint32_t box_view_revision(void) { return G.rev; }
WPose box_view_shell_pose(void) { return G.shell; }
int  box_view_is_wide(void) { return G.wide; }

void box_view_set_wide(int on) {
    on = on ? 1 : 0;
    if (!G.inited || on == G.wide) { G.wide = on; return; }
    G.wide = on; rebuild();
}
void box_view_set_style(BoxStyle s) {
    if (!G.inited || s == G.cfg.style) return;
    G.cfg.style = s; rebuild();
}
void box_view_set_lag(float rate, float max_deg) {
    G.cfg.lag_rate = rate > 0.f ? rate : 0.f;
    G.cfg.lag_max_deg = max_deg > 0.f ? max_deg : 0.f;
}

const uint8_t *box_view_texture(int *w, int *h) {
    if (!G.inited || style_black() || !G.tex) return NULL;
    if (w) *w = G.tw;
    if (h) *h = G.th;
    return G.tex;
}

static float lift_now(void) { return G.cfg.lift * (1.f - ease_out(G.drop)); }

void box_view_update(WPose head, float dt) {
    if (!G.inited) return;
    if (!(dt > 0.f)) dt = 0.f;
    if (dt > 0.1f) dt = 0.1f;
    G.head = head;
    const BoxViewConfig *c = &G.cfg;
    const float open_start = 0.6f;     /* la abertura empieza a abrirse al 60 % de la bajada */

    if (G.want) {
        if (G.state == BOX_OFF || G.state == BOX_LEAVING) {
            if (G.state == BOX_OFF) { G.lag_ok = 0; }
            G.state = BOX_ENTERING;
        }
        G.drop = clampf(G.drop + dt / c->drop_time, 0.f, 1.f);
        if (G.drop >= open_start) G.open = clampf(G.open + dt / c->open_time, 0.f, 1.f);
        if (G.drop >= 1.f && G.open >= 1.f) G.state = BOX_ACTIVE;
    } else if (G.state != BOX_OFF) {
        G.state = BOX_LEAVING;
        G.open = clampf(G.open - dt / (0.6f * c->open_time), 0.f, 1.f);   /* se cierra rápido, como al retirarse */
        if (G.open <= 0.5f) G.drop = clampf(G.drop - dt / c->drop_time, 0.f, 1.f);
        if (G.drop <= 0.f && G.open <= 0.f) G.state = BOX_OFF;
    }

    /* inercia: solo giro; el desfase queda acotado para que la abertura nunca salga de delante de los ojos */
    if (!G.lag_ok || c->lag_rate <= 0.f || dt <= 0.f) {
        if (!G.lag_ok || c->lag_rate <= 0.f) G.lag_q = head.q;
        G.lag_ok = 1;
    } else {
        float a = 1.f - expf(-c->lag_rate * dt);
        G.lag_q = wquat_slerp(G.lag_q, head.q, a);
        float d = fabsf(G.lag_q.x * head.q.x + G.lag_q.y * head.q.y + G.lag_q.z * head.q.z + G.lag_q.w * head.q.w);
        float ang = 2.f * acosf(clampf(d, 0.f, 1.f));
        float mx = c->lag_max_deg * (float)M_PI / 180.f;
        if (ang > mx && ang > 1e-6f) G.lag_q = wquat_slerp(head.q, G.lag_q, mx / ang);
    }
    G.lag_q = wquat_normalize(G.lag_q);

    float lift = lift_now();
    float tilt = -0.35f * (1.f - ease_out(G.drop));          /* llega algo ladeada hacia atrás y se endereza */
    WQuat tq = wquat_axis_angle(wv(1, 0, 0), tilt);
    G.shell.q = wquat_normalize(wquat_mul(G.lag_q, tq));
    G.shell.p = wv(head.p.x, head.p.y + lift, head.p.z);
    G.floor_alpha = smoothstepf(0.65f, 0.95f, (c->bottom - lift) / c->bottom);

    if (!G.built_valid || fabsf(G.open - G.built_open) > 1e-4f) rebuild();
}

int box_view_draws(BoxDraw *out, int max) {
    if (!box_view_visible() || !out || max <= 0) return 0;
    int n = 0;
    BoxDraw d;
    memset(&d, 0, sizeof d);
    d.pose = G.shell; d.alpha = 1.f;
    d.mesh = &G.m_shell; d.blend = BOX_BLEND_OPAQUE; d.texture = use_tex() ? 1 : 0; d.dynamic = 1;
    if (n < max) out[n++] = d;
    if (G.floor_alpha > 0.002f && n < max) {
        d.mesh = &G.m_floor; d.alpha = G.floor_alpha; d.dynamic = 0;
        d.blend = G.floor_alpha < 0.999f ? BOX_BLEND_ALPHA : BOX_BLEND_OPAQUE;
        d.texture = use_tex() ? 1 : 0;
        out[n++] = d;
    }
    if (G.m_soft.ni && n < max) {
        d.mesh = &G.m_soft; d.alpha = 1.f; d.blend = BOX_BLEND_ALPHA; d.texture = 0; d.dynamic = 1;
        out[n++] = d;
    }
    return n;
}

int box_view_aperture(BoxAperture *o) {
    if (!G.inited || !o || G.sight.nout < 3) return -1;
    float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
    for (int i = 0; i < G.sight.nout; i++) {
        float X, Y;
        px_to_plane(&G.sight.out[i], 1.f, &X, &Y);
        if (X < x0) x0 = X;
        if (X > x1) x1 = X;
        if (Y < y0) y0 = Y;
        if (Y > y1) y1 = Y;
    }
    float D = G.cfg.distance;
    o->left = atanf(-x0 / D); o->right = atanf(x1 / D);
    o->up = atanf(y1 / D); o->down = atanf(-y0 / D);
    o->center_pitch = atanf(0.5f * (y0 + y1) / D);
    return 0;
}

int box_view_outline(WVec3 *out, int max) {
    if (!G.inited || !out) return 0;
    int n = G.sight.nout < max ? G.sight.nout : max;
    for (int i = 0; i < n; i++) {
        float X, Y;
        px_to_plane(&G.sight.out[i], 1.f, &X, &Y);
        out[i] = wv(X, Y, -G.cfg.distance);
    }
    return n;
}
