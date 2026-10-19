/* vr_hands: ver vr_hands.h. */
#include "vr_hands.h"
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef char vrhands_check_wvertex[(sizeof(VrHandsVertex) == sizeof(WVertex) && offsetof(VrHandsVertex, nrm) == offsetof(WVertex, nrm) &&
                                    offsetof(VrHandsVertex, uv) == offsetof(WVertex, uv) && offsetof(VrHandsVertex, rgba) == offsetof(WVertex, rgba)) ? 1 : -1];

typedef struct {
    const WeaponState *s;
    uint32_t uid;                 /* identidad del enlace: ids de malla = uid * 64 + índice; id de textura = uid */
    const uint8_t *atlas; uint32_t aw, ah;
} Bound;

typedef struct {
    VrHandsMesh m[VRH_MAX_MESHES];
    VrHandsTex t[VRH_MAX_BOUND];
    VrHandsFrame f;
} Ring;

struct VrHandsRig {
    VrHandsConfig cfg;
    Bound b[VRH_MAX_BOUND];
    Ring ring[3];
    uint32_t seq;
};

static uint32_t g_uid;            /* contador global: un id jamás se reutiliza para otra geometría */

void vrhands_default_config(VrHandsConfig *c) {
    memset(c, 0, sizeof *c);
    c->scale = 1.0f;
    c->grip_mode = VRH_GRIP_AUTO;
    c->grip_manual = wpose_identity();
    c->grip_pitch_deg = 0.0f;
    c->cull_dist = 4.0f;
    /* luz fija de arriba, algo hacia delante y a la izquierda (sombra suave hacia abajo-derecha): estilo de juego de PS1 */
    c->light_dir[0] = -0.30f; c->light_dir[1] = 0.85f; c->light_dir[2] = -0.43f;
    c->ambient = 0.78f; c->diffuse = 0.62f;
}

VrHandsRig *vrhands_create(const VrHandsConfig *c) {
    VrHandsRig *r = (VrHandsRig *)calloc(1, sizeof *r);
    if (!r) return NULL;
    if (c) r->cfg = *c; else vrhands_default_config(&r->cfg);
    if (!(r->cfg.scale > 0.0f) || !isfinite(r->cfg.scale)) r->cfg.scale = 1.0f;
    if (!(r->cfg.cull_dist > 0.0f)) r->cfg.cull_dist = 4.0f;
    return r;
}
void vrhands_destroy(VrHandsRig *r) { free(r); }

/* ---------------- utilidades ---------------- */
static int finite3(const float *p) { return isfinite(p[0]) && isfinite(p[1]) && isfinite(p[2]); }
static int pose_ok(const WPose *p) {
    return finite3(&p->p.x) && isfinite(p->q.x) && isfinite(p->q.y) && isfinite(p->q.z) && isfinite(p->q.w);
}
WPose vrhands_pose_to_w(const VrHandsPose *p) {
    WPose w = wpose_identity();
    if (!p) return w;
    w.p = wv(p->pos[0], p->pos[1], p->pos[2]);
    w.q = (WQuat){p->rot[0], p->rot[1], p->rot[2], p->rot[3]};
    return w;
}
static int is_identity_pose(const WPose *g) {
    return fabsf(g->p.x) < 1e-6f && fabsf(g->p.y) < 1e-6f && fabsf(g->p.z) < 1e-6f &&
           fabsf(g->q.x) < 1e-6f && fabsf(g->q.y) < 1e-6f && fabsf(g->q.z) < 1e-6f && fabsf(g->q.w - 1.0f) < 1e-6f;
}

/* Matriz columna-mayor local->mundo de una pose, con escala uniforme sobre la rotación. */
static void pose_matrix(const WPose *p, float s, float *m) {
    WQuat q = wquat_normalize(p->q);
    float x = q.x, y = q.y, z = q.z, w = q.w;
    float xx = x * x, yy = y * y, zz = z * z, xy = x * y, xz = x * z, yz = y * z, wx = w * x, wy = w * y, wz = w * z;
    m[0] = (1 - 2 * (yy + zz)) * s; m[1] = 2 * (xy + wz) * s;     m[2] = 2 * (xz - wy) * s;     m[3] = 0;
    m[4] = 2 * (xy - wz) * s;     m[5] = (1 - 2 * (xx + zz)) * s; m[6] = 2 * (yz + wx) * s;     m[7] = 0;
    m[8] = 2 * (xz + wy) * s;     m[9] = 2 * (yz - wx) * s;     m[10] = (1 - 2 * (xx + yy)) * s; m[11] = 0;
    m[12] = p->p.x; m[13] = p->p.y; m[14] = p->p.z; m[15] = 1;
}

/* ---------------- enlace ---------------- */
static int meshes_valid(const WeaponState *s) {
    int n; const WMesh *ms = weapon_meshes(s, &n);
    if (!ms || n < 0 || n > 64) return 0;      /* mesh_id = uid * 64 + índice: con más de 64 mallas los ids colisionarían */
    for (int i = 0; i < n; i++) {
        const WMesh *m = &ms[i];
        if (m->nv == 0 || m->ni == 0) continue;
        if (!m->v || !m->idx || m->ni % 3) return 0;
        for (uint32_t k = 0; k < m->ni; k++) if (m->idx[k] >= m->nv) return 0;   /* la GPU leería fuera */
    }
    return 1;
}

int vrhands_bind(VrHandsRig *r, const WeaponState *s, const uint8_t *atlas, uint32_t w, uint32_t h) {
    if (!r || !s) return -1;
    if (!meshes_valid(s)) return -2;
    if (atlas && (!w || !h || w > 16384 || h > 16384)) return -1;
    Bound *slot = NULL;
    for (int i = 0; i < VRH_MAX_BOUND; i++) if (r->b[i].s == s) { slot = &r->b[i]; break; }
    for (int i = 0; i < VRH_MAX_BOUND && !slot; i++) if (!r->b[i].s) slot = &r->b[i];
    if (!slot) return -3;
    slot->s = s; slot->uid = ++g_uid; slot->atlas = atlas; slot->aw = atlas ? w : 0; slot->ah = atlas ? h : 0;
    return 0;
}
void vrhands_unbind(VrHandsRig *r, const WeaponState *s) {
    if (!r) return;
    for (int i = 0; i < VRH_MAX_BOUND; i++) if (r->b[i].s == s) memset(&r->b[i], 0, sizeof r->b[i]);
}
static const Bound *find_bound(const VrHandsRig *r, const WeaponState *s) {
    for (int i = 0; i < VRH_MAX_BOUND; i++) if (r->b[i].s == s && s) return &r->b[i];
    return NULL;
}

/* ---------------- agarre ---------------- */
#define MAXP 64
/* Pose de reposo de las piezas (canales a cero) en el espacio del arma. */
static int rest_poses(const WeaponState *s, WPose *po) {
    WeaponState tmp = *s;
    memset(tmp.channel, 0, sizeof tmp.channel);
    return wcore_part_poses(&tmp, wpose_identity(), po, MAXP);
}
static int part_by_name(const WeaponState *s, const char *name) { return wcore_part_find(s, name); }

/* sc = escala vértices -> metros (la misma que usa vrhands_build): las poses de pieza van en metros, los vértices no. */
static int derive_grip(const WeaponState *s, float sc, WPose *out) {
    if (!s || !out) return -1;
    int nm; const WMesh *ms = weapon_meshes(s, &nm);
    WPose po[MAXP]; int np = rest_poses(s, po);
    int ip_slide = part_by_name(s, "slide"), ip_trig = part_by_name(s, "trigger"), ip_frame = part_by_name(s, "frame");
    if (ip_frame < 0) { int n; const WPart *pp = weapon_parts(s, &n); for (int i = 0; i < n; i++) if (pp[i].parent < 0) { ip_frame = i; break; } }
    if (ip_slide < 0 || ip_trig < 0 || ip_frame < 0 || ip_slide >= np || ip_trig >= np || ip_frame >= np) return -1;
    int ms_ = weapon_part_mesh(s, ip_slide), mt = weapon_part_mesh(s, ip_trig), mf = weapon_part_mesh(s, ip_frame);
    if (ms_ < 0 || mt < 0 || mf < 0 || ms_ >= nm || mt >= nm || mf >= nm) return -1;
    const WMesh *Ms = &ms[ms_], *Mt = &ms[mt], *Mf = &ms[mf];
    if (!Ms->nv || !Mt->nv || !Mf->nv) return -1;
    float slide_bottom = 1e30f, trig_rear = -1e30f;
    for (uint32_t i = 0; i < Ms->nv; i++) {
        WVec3 p = wpose_apply(po[ip_slide], wv(Ms->v[i].pos[0] * sc, Ms->v[i].pos[1] * sc, Ms->v[i].pos[2] * sc));
        if (p.y < slide_bottom) slide_bottom = p.y;
    }
    for (uint32_t i = 0; i < Mt->nv; i++) {
        WVec3 p = wpose_apply(po[ip_trig], wv(Mt->v[i].pos[0] * sc, Mt->v[i].pos[1] * sc, Mt->v[i].pos[2] * sc));
        if (p.z > trig_rear) trig_rear = p.z;
    }
    WVec3 lo = wv(1e30f, 1e30f, 1e30f), hi = wv(-1e30f, -1e30f, -1e30f);
    int cnt = 0;
    for (uint32_t i = 0; i < Mf->nv; i++) {
        WVec3 p = wpose_apply(po[ip_frame], wv(Mf->v[i].pos[0] * sc, Mf->v[i].pos[1] * sc, Mf->v[i].pos[2] * sc));
        if (p.y > slide_bottom + 1e-4f || p.z <= trig_rear) continue;
        lo = wv(fminf(lo.x, p.x), fminf(lo.y, p.y), fminf(lo.z, p.z));
        hi = wv(fmaxf(hi.x, p.x), fmaxf(hi.y, p.y), fmaxf(hi.z, p.z));
        cnt++;
    }
    if (cnt < 4 || !finite3(&lo.x) || !finite3(&hi.x)) return -1;
    out->p = wv((lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f);
    out->q = wquat_identity();
    return 0;
}
int vrhands_derive_grip(const WeaponState *s, WPose *out) { return derive_grip(s, 1.0f, out); }

WPose vrhands_grip(const VrHandsRig *r, const WeaponState *s) {
    WPose def = wpose_identity();
    if (s && s->def) def = s->def->points[WP_GRIP];
    if (!r || !s) return def;
    WPose d;
    switch (r->cfg.grip_mode) {
    case VRH_GRIP_DEF: return def;
    case VRH_GRIP_MANUAL: return r->cfg.grip_manual;
    case VRH_GRIP_DERIVED: return derive_grip(s, r->cfg.scale, &d) == 0 ? d : def;
    default: break;
    }
    if (!is_identity_pose(&def)) return def;
    return derive_grip(s, r->cfg.scale, &d) == 0 ? d : def;
}

WPose vrhands_weapon_world(const VrHandsRig *r, const WeaponState *s, const VrHandsPose *hand) {
    WPose h = vrhands_pose_to_w(hand);
    h.q = wquat_normalize(h.q);
    float pitch = r ? r->cfg.grip_pitch_deg * 0.017453292519943295f : 0.0f;
    if (pitch != 0.0f) h.q = wquat_mul(h.q, wquat_axis_angle(wv(1, 0, 0), pitch));
    WPose rec = s ? s->recoil_offset : wpose_identity();
    if (!pose_ok(&rec) || (rec.q.x == 0 && rec.q.y == 0 && rec.q.z == 0 && rec.q.w == 0)) rec = wpose_identity();   /* estado a cero */
    WPose gr = vrhands_grip(r, s);
    gr.q = wquat_normalize(gr.q);          /* wpose_inv solo conjuga: un cuaternión no unitario (def a cero) deformaría el arma */
    return wpose_mul(wpose_mul(h, rec), wpose_inv(gr));
}

/* ---------------- frame ---------------- */
static void fill_common(const VrHandsRig *r, VrHandsFrame *f) {
    memset(f, 0, sizeof *f);
    f->light_dir[0] = r->cfg.light_dir[0]; f->light_dir[1] = r->cfg.light_dir[1]; f->light_dir[2] = r->cfg.light_dir[2];
    f->ambient = r->cfg.ambient; f->diffuse = r->cfg.diffuse;
}

static int add_mesh(Ring *g, const Bound *b, int mi, const WMesh *m, const WPose *world, float scale, uint32_t flags) {
    if (g->f.nmeshes >= VRH_MAX_MESHES) return 0;
    VrHandsMesh *o = &g->m[g->f.nmeshes++];
    memset(o, 0, sizeof *o);
    o->mesh_id = b->uid * 64u + (uint32_t)(mi & 63);
    o->tex_id = (m->texture && b->atlas) ? b->uid : 0;
    pose_matrix(world, scale, o->model);
    o->verts = (const VrHandsVertex *)m->v; o->nverts = m->nv;
    o->idx = m->idx; o->nidx = m->ni;
    o->flags = flags;
    return 1;
}

int vrhands_build(VrHandsRig *r, const WeaponState *s, const VrHandsPose *hand, VrHandsFrame *out) {
    if (!out) return 0;
    if (!r) { memset(out, 0, sizeof *out); return 0; }
    Ring *g = &r->ring[r->seq % 3];
    fill_common(r, &g->f);
    g->f.seq = ++r->seq;
    g->f.meshes = g->m; g->f.tex = g->t;
    const Bound *b = find_bound(r, s);
    int ok = b && hand && hand->valid && finite3(hand->pos) &&
             isfinite(hand->rot[0]) && isfinite(hand->rot[1]) && isfinite(hand->rot[2]) && isfinite(hand->rot[3]) &&
             (hand->rot[0] * hand->rot[0] + hand->rot[1] * hand->rot[1] + hand->rot[2] * hand->rot[2] + hand->rot[3] * hand->rot[3]) > 1e-12f;
    if (ok) {
        WPose W = vrhands_weapon_world(r, s, hand);
        if (!pose_ok(&W)) ok = 0;
        else {
            int nm; const WMesh *ms = weapon_meshes(s, &nm);
            int np; weapon_parts(s, &np);
            WPose po[MAXP];
            int n = wcore_part_poses(s, W, po, MAXP);
            (void)np;
            float cull2 = r->cfg.cull_dist * r->cfg.cull_dist;
            for (int i = 0; i < n; i++) {
                int mi = weapon_part_mesh(s, i);
                if (mi < 0 || mi >= nm || !ms[mi].nv || !ms[mi].ni) continue;
                if (!pose_ok(&po[i])) continue;
                WVec3 d = wv_sub(po[i].p, W.p);
                if (!(wv_dot(d, d) <= cull2)) continue;      /* apartada (canal "oculto") o NaN */
                add_mesh(g, b, mi, &ms[mi], &po[i], r->cfg.scale, 0);
            }
            for (int k = 0; k < W_MAX_HELD; k++) {
                const WHeld *h = &s->held[k];
                if (!h->active || h->mesh < 0 || h->mesh >= nm || !ms[h->mesh].nv || !ms[h->mesh].ni) continue;
                if (!pose_ok(&h->world)) continue;
                WVec3 d = wv_sub(h->world.p, W.p);
                if (!(wv_dot(d, d) <= cull2)) continue;
                add_mesh(g, b, h->mesh, &ms[h->mesh], &h->world, r->cfg.scale, VRH_MF_HELD);
            }
        }
    }
    if (ok && g->f.nmeshes > 0 && b->atlas) {
        g->t[0].id = b->uid; g->t[0].w = b->aw; g->t[0].h = b->ah; g->t[0].rgba = b->atlas;
        g->f.ntex = 1;
    }
    *out = g->f;
    return g->f.nmeshes;
}

/* ---------------- buzón sim -> render ---------------- */
static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;
static Ring g_box[3];
static int g_latest = -1, g_reading = -1;

void vrhands_post(const VrHandsFrame *f) {
    if (!f) return;
    pthread_mutex_lock(&g_mx);
    int i = 0;
    while (i == g_latest || i == g_reading) i++;           /* siempre hay uno libre: solo 2 excluidos de 3 */
    Ring *g = &g_box[i];
    int nm = f->nmeshes < 0 ? 0 : (f->nmeshes > VRH_MAX_MESHES ? VRH_MAX_MESHES : f->nmeshes);
    int nt = f->ntex < 0 ? 0 : (f->ntex > VRH_MAX_BOUND ? VRH_MAX_BOUND : f->ntex);
    if (!f->meshes) nm = 0;
    if (!f->tex) nt = 0;
    if (nm) memcpy(g->m, f->meshes, (size_t)nm * sizeof(VrHandsMesh));
    if (nt) memcpy(g->t, f->tex, (size_t)nt * sizeof(VrHandsTex));
    g->f = *f; g->f.nmeshes = nm; g->f.ntex = nt; g->f.meshes = g->m; g->f.tex = g->t;
    g_latest = i;
    pthread_mutex_unlock(&g_mx);
}
void vrhands_post_clear(void) {
    pthread_mutex_lock(&g_mx);
    g_latest = -1;
    pthread_mutex_unlock(&g_mx);
}

/* El renderer avisa de que ya no usa el frame entregado por mv_host_hands (ni sus punteros a vértices/atlas). */
__attribute__((weak)) void mv_host_hands_done(void) {
    pthread_mutex_lock(&g_mx);
    g_reading = -1;
    pthread_mutex_unlock(&g_mx);
}
int vrhands_render_busy(void) {
    pthread_mutex_lock(&g_mx);
    int b = g_reading >= 0;
    pthread_mutex_unlock(&g_mx);
    return b;
}

int vrhands_fetch(VrHandsFrame *out) {
    if (!out) return 0;
    int rc = 0;
    pthread_mutex_lock(&g_mx);
    if (g_latest >= 0 && g_box[g_latest].f.nmeshes > 0) {
        g_reading = g_latest;
        *out = g_box[g_latest].f;
        rc = 1;
    } else {
        memset(out, 0, sizeof *out);
    }
    pthread_mutex_unlock(&g_mx);
    return rc;
}

/* Implementación débil: el integrador puede definir la suya (p. ej. compensar la latencia de la mano con vrhands_fetch). */
__attribute__((weak)) int mv_host_hands(VrHandsFrame *out) { return vrhands_fetch(out); }
