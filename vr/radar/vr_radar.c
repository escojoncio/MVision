/* vr_radar: ver vr_radar.h. */
#include "vr_radar.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================================================ marcas */
#define NRANGE 32
static struct { uint32_t lo, hi; } rng[NRANGE];
static unsigned rng_n;           /* total insertados (anillo) */
static int open_;
static uint32_t open_seq, last_seq;
static int have_last;

void vrradar_reset(void) { rng_n = 0; open_ = 0; open_seq = 0; have_last = 0; last_seq = 0; memset(rng, 0, sizeof rng); }

void vrradar_on_mark(uint32_t id, uint32_t seq) {
    /* La numeración de la captura vuelve a 0 cada vez que se apaga y se enciende (gpu_capture_enable): las marcas llegan siempre en
     * orden de seq, así que una marca "del pasado" delata un reinicio y los rangos viejos ya no valen (darían falsos positivos con
     * los seq nuevos). El host además llama a vrradar_reset() al encender la captura. */
    if (have_last && (int32_t)(seq - last_seq) < 0) { rng_n = 0; open_ = 0; memset(rng, 0, sizeof rng); }
    last_seq = seq; have_last = 1;
    if (id == VRRADAR_MARK_BEGIN) { open_ = 1; open_seq = seq; return; }
    if (id == VRRADAR_MARK_END) {
        if (open_ && (int32_t)(seq - open_seq) > 0) {
            rng[rng_n % NRANGE].lo = open_seq;
            rng[rng_n % NRANGE].hi = seq;
            rng_n++;
        }
        open_ = 0;
    }
}

int vrradar_seq_is_radar(uint32_t seq) {
    unsigned n = rng_n < NRANGE ? rng_n : NRANGE;
    for (unsigned i = 0; i < n; i++)
        if ((int32_t)(seq - rng[i].lo) >= 0 && (int32_t)(seq - rng[i].hi) < 0) return 1;
    return 0;
}

uint32_t vrradar_ranges(uint32_t *lo, uint32_t *hi, uint32_t max) {
    unsigned n = rng_n < NRANGE ? rng_n : NRANGE;
    if (n > max) n = max;
    for (unsigned i = 0; i < n; i++) { lo[i] = rng[i].lo; hi[i] = rng[i].hi; }
    return n;
}

/* ================================================================================================ percepción */
static VrRadarPerc perc = {0, 1.0f, 1.0f, 0.5f};
void vrradar_set_perception(const VrRadarPerc *p) { if (p) perc = *p; }
VrRadarPerc vrradar_get_perception(void) { return perc; }

/* ================================================================================================ separación */
static GpuOp *hud_ops, *rad_ops;
static size_t hud_cap, rad_cap;
static uint32_t *rad_pre, *hud_evpos;      /* rad_pre[i] = nº de eventos en rad_ops[0,i); hud_evpos[k] = índice en hud de su k-ésimo evento (k == total -> nops) */
static size_t pre_cap, evpos_cap;
static GpuDrawList hud_list, rad_list;
static const GpuDrawList *last_radar;       /* la lista que preparó el último split */

static int is_event(int t) { return t == GPU_OP_UPLOAD || t == GPU_OP_VCOPY || t == GPU_OP_FILL; }
static int is_prim(int t) { return t == GPU_OP_POLY || t == GPU_OP_RECT || t == GPU_OP_LINE; }

static int grow_ops(GpuOp **p, size_t *cap, size_t need) {
    if (need <= *cap) return 1;
    size_t n = *cap ? *cap : 256;
    while (n < need) n *= 2;
    GpuOp *q = (GpuOp *)realloc(*p, n * sizeof **p);
    if (!q) return 0;
    *p = q; *cap = n;
    return 1;
}
static int grow_u32(uint32_t **p, size_t *cap, size_t need) {
    if (need <= *cap) return 1;
    size_t n = *cap ? *cap : 256;
    while (n < need) n *= 2;
    uint32_t *q = (uint32_t *)realloc(*p, n * sizeof **p);
    if (!q) return 0;
    *p = q; *cap = n;
    return 1;
}

int vrradar_split(const GpuDrawList *dl, VrRadarSplit *out) {
    if (!dl || !out) return -1;
    memset(out, 0, sizeof *out);
    out->hud = dl;
    last_radar = NULL;
    size_t n = dl->nops;
    size_t nr = 0;
    if (n && dl->ops)
        for (size_t i = 0; i < n; i++)
            if (is_prim(dl->ops[i].type) && vrradar_seq_is_radar(dl->ops[i].seq)) nr++;
    if (!nr) return 0;

    if (!grow_ops(&hud_ops, &hud_cap, n - nr + 1) || !grow_ops(&rad_ops, &rad_cap, n + 1) ||
        !grow_u32(&rad_pre, &pre_cap, n + 2) || !grow_u32(&hud_evpos, &evpos_cap, n + 2))
        return -2;
    size_t nh = 0, nrad = 0, nev_h = 0, nev_r = 0;
    /* centro del entorno de dibujo del radar = moda de (dox,doy) entre sus primitivas (el marco negro usa el entorno base) */
    struct { int x, y; unsigned c; } mode[8];
    int nm = 0;
    for (size_t i = 0; i < n; i++) {
        const GpuOp *o = &dl->ops[i];
        int ev = is_event(o->type);
        int rad = !ev && is_prim(o->type) && vrradar_seq_is_radar(o->seq);
        if (ev) {
            hud_evpos[nev_h++] = (uint32_t)nh;
            rad_pre[nrad] = (uint32_t)nev_r++;
            hud_ops[nh++] = *o;
            rad_ops[nrad++] = *o;
        } else if (rad) {
            rad_pre[nrad] = (uint32_t)nev_r;
            GpuOp *r = &rad_ops[nrad++];
            *r = *o;
            for (int k = 0; k < 4; k++) { r->v[k].vvalid = 0; r->v[k].fvalid = 0; r->v[k].vflags = 0; }
            r->proj_h = r->proj_ox = r->proj_oy = 0.0f;
            int j = 0;
            for (; j < nm; j++) if (mode[j].x == o->dox && mode[j].y == o->doy) { mode[j].c++; break; }
            if (j == nm && nm < 8) { mode[nm].x = o->dox; mode[nm].y = o->doy; mode[nm].c = 1; nm++; }
        } else {
            hud_ops[nh++] = *o;
        }
    }
    rad_pre[nrad] = (uint32_t)nev_r;
    hud_evpos[nev_h] = (uint32_t)nh;
    int best = 0;
    for (int j = 1; j < nm; j++) if (mode[j].c > mode[best].c) best = j;
    int cx = nm ? mode[best].x : 0, cy = nm ? mode[best].y : 0;

    hud_list = *dl;  hud_list.ops = hud_ops; hud_list.nops = (uint32_t)nh; hud_list.cap_ops = (uint32_t)hud_cap;
    rad_list = *dl;  rad_list.ops = rad_ops; rad_list.nops = (uint32_t)nrad; rad_list.cap_ops = (uint32_t)rad_cap;
    rad_list.vram_init = NULL;      /* la VRAM inicial ya viaja con la lista del HUD */
    rad_list.vram_after = NULL;
    out->present = 1;
    out->n_ops = (uint32_t)nr;
    out->center[0] = cx; out->center[1] = cy;
    out->rect[0] = cx - 35; out->rect[1] = cy - 27; out->rect[2] = 71; out->rect[3] = 54;
    out->hud = &hud_list;
    out->radar = &rad_list;
    last_radar = &rad_list;
    return 0;
}

/* ================================================================================================ escena del radar */
static VrVtx *ov; static size_t ov_cap;
static VrBatch *ob; static size_t ob_cap;
static VrState *os_; static size_t os_cap;

static int grow_bytes(void **p, size_t *cap, size_t need, size_t esz) {
    if (need <= *cap) return 1;
    size_t n = *cap ? *cap : 1024;
    while (n < need) n *= 2;
    void *q = realloc(*p, n * esz);
    if (!q) return 0;
    *p = q; *cap = n;
    return 1;
}

/* Colores de los conos del juego (r,g,b de los vértices): [0] origen, [1] resto. */
typedef struct { uint8_t m[3], f[3]; int soldier_alert; } ConeCol;
static const ConeCol CONES[] = {
    {{0x00, 0x80, 0x80}, {0x00, 0x00, 0x10}, 0},   /* soldado, normal */
    {{0xA0, 0x00, 0x00}, {0x10, 0x00, 0x00}, 1},   /* soldado, sospecha / alerta */
};

static int cone_match(const GpuOp *o) {
    if (o->type != GPU_OP_POLY || o->nv != 4) return 0;
    if ((o->flags & (GPU_F_GOURAUD | GPU_F_SEMI | GPU_F_TEXTURED)) != (GPU_F_GOURAUD | GPU_F_SEMI)) return 0;
    for (unsigned c = 0; c < sizeof CONES / sizeof CONES[0]; c++) {
        const GpuVtx *v = o->v;
        if (v[1].r != CONES[c].m[0] || v[1].g != CONES[c].m[1] || v[1].b != CONES[c].m[2]) continue;
        int ok = 1;
        for (int k = 0; k < 4; k += (k == 0 ? 2 : 1))   /* vértices 0, 2, 3 */
            if (v[k].r != CONES[c].f[0] || v[k].g != CONES[c].f[1] || v[k].b != CONES[c].f[2]) ok = 0;
        if (ok) return 1 + (int)c;
    }
    return 0;
}

typedef struct { VrVtx *v; size_t n, cap; } VBuf;
static int vb_push(VBuf *b, float x, float y, uint8_t r, uint8_t g, uint8_t bl) {
    if (b->n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 4096;
        VrVtx *q = (VrVtx *)realloc(b->v, nc * sizeof *q);
        if (!q) return 0;
        b->v = q; b->cap = nc;
    }
    VrVtx *v = &b->v[b->n++];
    memset(v, 0, sizeof *v);
    v->pos[0] = x; v->pos[1] = y; v->pos[2] = 0.0f;
    v->rgba = (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)bl << 16) | (255u << 24);
    v->tpage = (uint16_t)(1u << 5);                         /* abr 1: B + F */
    v->flags = (uint16_t)(GPU_F_GOURAUD | GPU_F_SEMI | VR_F_2D);
    v->pad = 0xFFFF;
    return 1;
}
static VBuf ext;

static uint8_t sc8(uint8_t c, float k) { float f = (float)c * k; return (uint8_t)(f > 255.0f ? 255.0f : f + 0.5f); }

static int quad(const float p[4][2], const uint8_t col[3], float k) {
    static const int idx[6] = {0, 1, 2, 0, 2, 3};
    for (int i = 0; i < 6; i++)
        if (!vb_push(&ext, p[idx[i]][0], p[idx[i]][1], sc8(col[0], k), sc8(col[1], k), sc8(col[2], k))) return 0;
    return 1;
}
static int dot(float x, float y, const uint8_t col[3], float k) {
    float fx = floorf(x + 0.5f), fy = floorf(y + 0.5f);
    float p[4][2] = {{fx, fy}, {fx + 1, fy}, {fx + 1, fy + 1}, {fx, fy + 1}};
    return quad(p, col, k);
}
/* Punteado a lo largo de p->q cada `step` píxeles. */
static int dotted(const float p[2], const float q[2], float step, const uint8_t col[3], float k) {
    float dx = q[0] - p[0], dy = q[1] - p[1];
    float len = sqrtf(dx * dx + dy * dy);
    int n = (int)(len / step);
    if (n > 400) n = 400;
    for (int i = 0; i <= n; i++) {
        float t = len > 0 ? (float)i * step / len : 0;
        if (!dot(p[0] + dx * t, p[1] + dy * t, col, k)) return 0;
    }
    return 1;
}

static void lerp2(float o[2], const float a[2], const float b[2], float t) { o[0] = a[0] + (b[0] - a[0]) * t; o[1] = a[1] + (b[1] - a[1]) * t; }
static void scl2(float o[2], const float O[2], const float p[2], float m) { o[0] = O[0] + (p[0] - O[0]) * m; o[1] = O[1] + (p[1] - O[1]) * m; }

/* Corona de un cono: polígono del juego v0=A, v1=O (origen), v2=F (frente), v3=B. Región original = triángulos (A,O,F) y (O,F,B), cuyos
 * bordes exteriores son A-F y F-B; la ampliada son los trapecios entre esos bordes y su escalado respecto a O. */
static int ext_cone(const GpuOp *o, int kind, const VrRadarPerc *p) {
    const uint8_t *col = CONES[kind - 1].m;
    float A[2] = {o->v[0].x, o->v[0].y}, O[2] = {o->v[1].x, o->v[1].y}, F[2] = {o->v[2].x, o->v[2].y}, B[2] = {o->v[3].x, o->v[3].y};
    float m = p->range_mult;
    if (fabsf(A[0]) > 4000 || fabsf(A[1]) > 4000 || fabsf(F[0]) > 4000 || fabsf(F[1]) > 4000 || fabsf(B[0]) > 4000 || fabsf(B[1]) > 4000) return 1;
    float pf = p->periph_frac;
    int has_p = pf < 1.0f;
    if (pf < 0.05f) pf = 0.05f;
    if (!has_p) pf = 1.0f;
    const float K_CORE = 0.28f, K_PER = 0.12f, K_EDGE = 0.70f;
    /* La regla de periferia solo rige para d > periph_near * L: entre L y n*L (n = periph_near si 1 < n < range_mult) todo el cono es
     * núcleo; n >= range_mult: la corona entera es núcleo (sin periferia). Con n <= 1 (presets) la periferia arranca en el borde original. */
    float n = 1.0f;
    if (has_p && p->periph_near > 1.0f) {
        if (p->periph_near >= m) { has_p = 0; pf = 1.0f; }
        else n = p->periph_near;
    }
    /* lado 1: F -> A ; lado 2: F -> B */
    const float *S[2] = {A, B};
    for (int s = 0; s < 2; s++) {
        float P[2], mF[2], mP[2], mS[2], nF[2], nP[2], nS[2];
        lerp2(P, F, S[s], pf);
        scl2(mF, O, F, m); scl2(mP, O, P, m); scl2(mS, O, S[s], m);
        scl2(nF, O, F, n); scl2(nP, O, P, n); scl2(nS, O, S[s], n);
        if (n > 1.0f) {        /* franja n*L: núcleo en todo el ancho */
            float band[4][2] = {{F[0], F[1]}, {S[s][0], S[s][1]}, {nS[0], nS[1]}, {nF[0], nF[1]}};
            if (!quad(band, col, K_CORE)) return 0;
        }
        float core[4][2] = {{nF[0], nF[1]}, {nP[0], nP[1]}, {mP[0], mP[1]}, {mF[0], mF[1]}};
        if (!quad(core, col, K_CORE)) return 0;
        if (has_p) {
            float per[4][2] = {{nP[0], nP[1]}, {nS[0], nS[1]}, {mS[0], mS[1]}, {mP[0], mP[1]}};
            if (!quad(per, col, K_PER)) return 0;
        }
        /* bordes punteados: arco exterior (denso en el núcleo, ralo en la periferia), lado radial y frontera núcleo/periferia */
        if (!dotted(mF, mP, 2.0f, col, K_EDGE)) return 0;
        if (has_p) {
            if (!dotted(mP, mS, 4.0f, col, K_EDGE * 0.8f)) return 0;
            if (!dotted(nP, mP, 4.0f, col, K_EDGE * 0.6f)) return 0;
        }
        if (!dotted(S[s], mS, 3.0f, col, K_EDGE)) return 0;
    }
    return 1;
}

int vrradar_build(const VrRadarSplit *s, VrRadarScene *out) {
    if (!out) return -1;
    memset(out, 0, sizeof *out);
    if (!s || !s->present || !s->radar || s->radar != last_radar) return 0;
    const GpuDrawList *rl = s->radar;

    VrSceneOpts o = vr_scene_opts_default();
    o.screen_filter = 0;
    VrScene sc;
    int rc = vr_scene_build(rl, NULL, &o, &sc);
    if (rc < 0) return rc;

    /* conos de soldado y posición de inserción */
    ext.n = 0;
    int first_cone = -1;
    uint32_t ncone = 0, next = 0;
    int textured = 0;
    int enable = perc.enabled && perc.range_mult > 1.0f;
    for (uint32_t i = 0; i < rl->nops; i++) {
        const GpuOp *op = &rl->ops[i];
        if ((op->type == GPU_OP_RECT || op->type == GPU_OP_POLY) && (op->flags & GPU_F_TEXTURED)) textured = 1;
        int k = cone_match(op);
        if (!k) continue;
        ncone++;
        if (first_cone < 0) first_cone = (int)i;
        if (enable) {
            size_t before = ext.n;
            if (!ext_cone(op, k, &perc)) { ext.n = before; return -2; }
            if (ext.n > before) next++;
        }
    }

    size_t nv = sc.v ? sc.nv : 0, nb = sc.b ? sc.nb : 0, ns = sc.states ? sc.nstates : 0;
    size_t add = ext.n;
    if (!grow_bytes((void **)&ov, &ov_cap, nv + add + 1, sizeof *ov) || !grow_bytes((void **)&ob, &ob_cap, nb + 2, sizeof *ob) ||
        !grow_bytes((void **)&os_, &os_cap, ns + 1, sizeof *os_))
        return -2;

    /* lote de inserción: el último que empieza en o antes del primer cono (los conos van tras paredes y fondo) */
    size_t ins = 0;
    if (first_cone >= 0)
        for (size_t i = 0; i < nb; i++) if ((int)sc.b[i].src_op <= first_cone) ins = i;
    size_t vins = nb ? sc.b[ins].first : 0;
    if (!add) {
        if (nv) memcpy(ov, sc.v, nv * sizeof *ov);
        if (nb) memcpy(ob, sc.b, nb * sizeof *ob);
    } else {
        if (vins) memcpy(ov, sc.v, vins * sizeof *ov);
        memcpy(ov + vins, ext.v, add * sizeof *ov);
        if (nv > vins) memcpy(ov + vins + add, sc.v + vins, (nv - vins) * sizeof *ov);
        for (size_t i = 0; i < ins; i++) ob[i] = sc.b[i];
        ob[ins] = (VrBatch){.first = (uint32_t)vins, .count = (uint32_t)add, .kind = 2, .blend = 1, .ev_end = sc.b ? sc.b[ins].ev_end : 0, .src_op = sc.b ? sc.b[ins].src_op : 0};
        for (size_t i = ins; i < nb; i++) { ob[i + 1] = sc.b[i]; ob[i + 1].first += (uint32_t)add; }
        nb++;
    }
    if (ns) memcpy(os_, sc.states, ns * sizeof *os_);

    /* índices de eventos / ops: de la lista del radar a la del HUD (misma secuencia de eventos) */
    for (size_t i = 0; i < nb; i++) {
        uint32_t e = ob[i].ev_end;
        if (e > rl->nops) e = rl->nops;
        uint32_t k = rad_pre[e];
        ob[i].ev_end = hud_evpos[k];
        uint32_t so = ob[i].src_op;
        if (so > rl->nops) so = rl->nops;
        ob[i].src_op = hud_evpos[rad_pre[so]];
    }

    out->present = 1;
    memcpy(out->rect, s->rect, sizeof out->rect);
    out->v = ov; out->nv = (uint32_t)(nv + add);
    out->b = ob; out->nb = (uint32_t)nb;
    out->states = os_; out->nstates = (uint32_t)ns;
    out->n_cones = ncone; out->n_cones_ext = next; out->n_ext_tris = (uint32_t)(add / 3);
    out->mode = textured ? 1u : 0u;
    return 0;
}

/* ================================================================================================ anclaje */
static float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void cross3(float o[3], const float a[3], const float b[3]) {
    o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0];
}
static int norm3(float v[3]) {
    float l = sqrtf(dot3(v, v));
    if (!(l > 1e-6f) || !isfinite(l)) return 0;
    v[0] /= l; v[1] /= l; v[2] /= l;
    return 1;
}
static int finite3(const float v[3]) { return isfinite(v[0]) && isfinite(v[1]) && isfinite(v[2]); }

VrRadarWristCfg vrradar_wrist_cfg_default(void) {
    VrRadarWristCfg c;
    c.width_m = 0.07f; c.height_m = 0.0f; c.along_m = 0.13f; c.lift_m = 0.065f; c.skin_m = 0.008f; c.bezel_m = 0.004f; c.bezel_back_m = 0.002f;
    c.rot90 = 0; c.gaze_boost = 0.35f; c.gaze_cos_in = 0.93f; c.gaze_cos_out = 0.75f;
    return c;
}

int vrradar_wrist_quad(const VrRadarArm *arm, const VrRadarWristCfg *cfg_, float aspect, VrRadarQuad *q) {
    return vrradar_wrist_quad_eye(arm, cfg_, aspect, NULL, q);
}

int vrradar_wrist_quad_eye(const VrRadarArm *arm, const VrRadarWristCfg *cfg_, float aspect, const float eye[3], VrRadarQuad *q) {
    return vrradar_wrist_quad_mesh(arm, cfg_, aspect, eye, NULL, 0, q);
}

/* Radio de la malla en la dirección n (unitaria, perpendicular al eje): máx. de dot(v - wrist, n) sobre los vértices; 0 si no hay o alguno no es finito. */
static float mesh_radius(const float wrist[3], const float n[3], const float *verts, uint32_t nverts) {
    if (!verts || !nverts) return 0.0f;
    float r = -INFINITY;
    for (uint32_t i = 0; i < nverts; i++) {
        const float *v = verts + 3 * (size_t)i;
        if (!finite3(v)) return 0.0f;
        float d = (v[0] - wrist[0]) * n[0] + (v[1] - wrist[1]) * n[1] + (v[2] - wrist[2]) * n[2];
        if (d > r) r = d;
    }
    return r;
}

int vrradar_wrist_quad_mesh(const VrRadarArm *arm, const VrRadarWristCfg *cfg_, float aspect, const float eye[3], const float *verts, uint32_t nverts, VrRadarQuad *q) {
    if (!q) return 0;
    memset(q, 0, sizeof *q);
    if (!arm) return 0;
    VrRadarWristCfg cfg = cfg_ ? *cfg_ : vrradar_wrist_cfg_default();
    if (!finite3(arm->elbow) || !finite3(arm->wrist) || !finite3(arm->dorsal)) return 0;
    float a[3] = {arm->elbow[0] - arm->wrist[0], arm->elbow[1] - arm->wrist[1], arm->elbow[2] - arm->wrist[2]};
    if (!norm3(a)) return 0;
    float d = dot3(arm->dorsal, a);
    float n[3] = {arm->dorsal[0] - a[0] * d, arm->dorsal[1] - a[1] * d, arm->dorsal[2] - a[2] * d};
    int n_ok = norm3(n);
    if (eye && finite3(eye)) {      /* cara hacia los ojos: giro alrededor del eje del antebrazo (componente de ojo - punto del eje perpendicular a él) */
        float p[3] = {arm->wrist[0] + a[0] * cfg.along_m, arm->wrist[1] + a[1] * cfg.along_m, arm->wrist[2] + a[2] * cfg.along_m};
        float e[3] = {eye[0] - p[0], eye[1] - p[1], eye[2] - p[2]};
        float ed = dot3(e, a);
        float en[3] = {e[0] - a[0] * ed, e[1] - a[1] * ed, e[2] - a[2] * ed};
        if (dot3(en, en) > 0.02f * 0.02f && norm3(en)) { memcpy(n, en, sizeof n); n_ok = 1; }   /* ojo casi sobre el eje: se queda el dorso */
    }
    if (!n_ok) return 0;
    float r[3] = {-a[0], -a[1], -a[2]}, u[3];
    cross3(u, n, r);
    for (int k = 0; k < (cfg.rot90 & 3); k++) {          /* (r,u) -> (u,-r) */
        float t[3] = {r[0], r[1], r[2]};
        for (int i = 0; i < 3; i++) { r[i] = u[i]; u[i] = -t[i]; }
    }
    if (!(cfg.width_m > 0.0f) || !isfinite(cfg.width_m)) return 0;
    float h = cfg.height_m > 0.0f ? cfg.height_m : cfg.width_m / (aspect > 0.1f ? aspect : 1.0f);
    q->half_w = cfg.width_m * 0.5f; q->half_h = h * 0.5f;
    q->bezel = cfg.bezel_m > 0 ? cfg.bezel_m : 0; q->bezel_back = cfg.bezel_back_m > 0 ? cfg.bezel_back_m : 0;
    float lift = cfg.lift_m, rad = mesh_radius(arm->wrist, n, verts, nverts);
    if (rad > 0.01f && isfinite(rad) && isfinite(cfg.skin_m)) lift = rad + (cfg.skin_m > 0.0f ? cfg.skin_m : 0.0f);   /* radio real de la malla en esa dirección + holgura */
    for (int i = 0; i < 3; i++) {
        q->center[i] = arm->wrist[i] + a[i] * cfg.along_m + n[i] * lift;
        q->right[i] = r[i]; q->up[i] = u[i]; q->normal[i] = n[i];
        q->corners[0][i] = q->center[i] - r[i] * q->half_w + u[i] * q->half_h;
        q->corners[1][i] = q->center[i] + r[i] * q->half_w + u[i] * q->half_h;
        q->corners[2][i] = q->center[i] + r[i] * q->half_w - u[i] * q->half_h;
        q->corners[3][i] = q->center[i] - r[i] * q->half_w - u[i] * q->half_h;
        q->model[0 + i] = r[i] * 2.0f * q->half_w;
        q->model[4 + i] = u[i] * 2.0f * q->half_h;
        q->model[8 + i] = n[i];
        q->model[12 + i] = q->center[i];
    }
    q->model[3] = q->model[7] = q->model[11] = 0.0f; q->model[15] = 1.0f;
    q->valid = 1;
    return 1;
}

int vrradar_arm_from_controller(const float g[16], VrRadarArm *out) {
    if (!g || !out) return 0;
    for (int i = 0; i < 16; i++) if (!isfinite(g[i])) return 0;
    for (int i = 0; i < 3; i++) {
        out->wrist[i] = g[12 + i];
        out->elbow[i] = g[12 + i] + g[8 + i] * 0.27f;
        out->dorsal[i] = g[4 + i];
    }
    return 1;
}

int vrradar_pick_arm(const float grip[16], const VrRadarArm *ik, VrRadarArm *out) {
    if (!out) return 0;
    if (ik && finite3(ik->elbow) && finite3(ik->wrist) && finite3(ik->dorsal)) {
        float a[3] = {ik->elbow[0] - ik->wrist[0], ik->elbow[1] - ik->wrist[1], ik->elbow[2] - ik->wrist[2]};
        if (norm3(a)) { *out = *ik; return 1; }
    }
    return grip && vrradar_arm_from_controller(grip, out) ? 2 : 0;
}

static float sstep(float a, float b, float x) {
    if (b <= a) return x >= b ? 1.0f : 0.0f;
    float t = (x - a) / (b - a);
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    return t * t * (3.0f - 2.0f * t);
}

float vrradar_visibility(const float normal[3], const float center[3], const float eye[3], float prev, float dt) {
    prev = prev >= 1.0f ? 1.0f : (prev > 0.0f ? prev : 0.0f);   /* NaN -> 0 */
    float target = 0.0f;
    if (normal && center && eye && finite3(normal) && finite3(center) && finite3(eye)) {
        float n[3] = {normal[0], normal[1], normal[2]}, e[3] = {eye[0] - center[0], eye[1] - center[1], eye[2] - center[2]};
        if (norm3(n) && norm3(e)) {
            const float rad = 3.14159265358979f / 180.0f;
            float c = dot3(n, e);
            target = c > cosf(VRRADAR_VIS_ON_DEG * rad) ? 1.0f : (c < cosf(VRRADAR_VIS_OFF_DEG * rad) ? 0.0f : (prev > 0.0f ? 1.0f : 0.0f));
        }
    }
    float step = dt > 0.0f && isfinite(dt) ? dt / VRRADAR_VIS_FADE_S : 0.0f;
    float d = target - prev;
    return prev + (d > step ? step : (d < -step ? -step : d));
}

float vrradar_gaze(const VrRadarQuad *q, const float eye[3], const float fwd[3], const VrRadarWristCfg *cfg_) {
    if (!q || !q->valid || !eye || !fwd) return 0.0f;
    VrRadarWristCfg cfg = cfg_ ? *cfg_ : vrradar_wrist_cfg_default();
    float to[3] = {q->center[0] - eye[0], q->center[1] - eye[1], q->center[2] - eye[2]};
    if (!norm3(to)) return 0.0f;
    float look = dot3(fwd, to);
    float face = -dot3(q->normal, to);
    float f = sstep(cfg.gaze_cos_out, cfg.gaze_cos_in, look) * sstep(0.1f, 0.5f, face);
    return f * (cfg.gaze_boost > 0 ? cfg.gaze_boost : 0.0f);
}
