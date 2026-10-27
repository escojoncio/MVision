/* hands_preview: render de referencia por software de la lista de mallas de vr_hands (VrHandsFrame) desde un ojo sintético,
 * con el arma en la mano delante de la cámara. Replica la iluminación de render/metal/vr_weapon.metal. Lee los modelos
 * originales del archivo de stages del usuario (datos locales; nada de ello viaja con el repo).
 *   hands_preview <archivo_stages> <stage> <dir_salida>
 * Escribe, por modelo (36f6 con supresor, 21b6 sin él) y escenario, PNG:
 *   <id>_eye.png       vista desde el ojo (cabeza a 1,6 m) con el arma en la mano derecha
 *   <id>_side.png      vista cercana lateral
 *   <id>_free.png      con un cargador en la mano libre y una vaina en el aire
 *   <id>_eyeR.png      ojo derecho (separación interpupilar 63 mm): paralaje estéreo
 *
 *   hands_preview --poses <dir_salida>      (no necesita datos del juego)
 * Escribe un PNG por pose de vr_hand_pose (pose_NN_<nombre>.png; las poses con gatillo, además pose_NN_<nombre>_pulled.png con el gatillo a fondo): mano derecha (fila de arriba) e
 * izquierda (abajo) con las soldaduras aplicadas, vistas de la palma, del dorso y de perfil (lado del pulgar). */
/* Solo con -DVRH_PREVIEW_BUILD (lo pone el Makefile): fuera de él el fichero queda vacío, así app/stage.sh puede copiar los .c de vr/hands
 * al target de la app sin arrastrar un main(). */
#ifdef VRH_PREVIEW_BUILD
#include "vr_hands.h"
#include "vr_grip.h"
#include "vr_glove.h"
#include "vr_body.h"
#include "wprev.h"
#include "../weapons/wcut.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { float x, y, z; } V3;
static V3 v3(float x, float y, float z) { V3 r = {x, y, z}; return r; }
static V3 sub(V3 a, V3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static V3 cross(V3 a, V3 b) { return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static V3 norm(V3 a) { float l = sqrtf(dot(a, a)); return l > 1e-12f ? v3(a.x / l, a.y / l, a.z / l) : a; }

typedef struct { V3 e, r, u, f; float fov_deg; } Cam;   /* ejes de la cámara en mundo: r derecha, u arriba, f delante (-Z de la vista) */
static Cam look(V3 eye, V3 target, V3 up, float fov) {
    Cam c; c.e = eye; c.f = norm(sub(target, eye)); c.r = norm(cross(c.f, up)); c.u = cross(c.r, c.f); c.fov_deg = fov; return c;
}

/* vértice en el espacio de la cámara (x der, y arriba, z hacia ATRÁS: delante = -z), atributos linealizables */
typedef struct { float p[3], n[3], uv[2], c[3]; } PV;
static PV lerp_pv(const PV *a, const PV *b, float t) {
    PV o; const float *pa = a->p, *pb = b->p; float *po = o.p;
    for (int i = 0; i < 13; i++) po[i] = pa[i] + (pb[i] - pa[i]) * t;
    return o;
}

typedef struct { int w, h; float *z; float *rgb; } Buf;

static void shade_tri(Buf *B, const PV *a, const PV *b, const PV *c, const VrHandsTex *tex, const VrHandsFrame *fr, const Cam *cam, float fy) {
    /* proyecta */
    float sx[3], sy[3], iw[3]; const PV *v[3] = {a, b, c};
    for (int i = 0; i < 3; i++) {
        float d = -v[i]->p[2];
        iw[i] = 1.0f / d;
        sx[i] = B->w * 0.5f + fy * v[i]->p[0] * iw[i];
        sy[i] = B->h * 0.5f - fy * v[i]->p[1] * iw[i];
    }
    float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
    if (fabsf(area) < 1e-9f) return;
    int x0 = (int)floorf(fminf(sx[0], fminf(sx[1], sx[2]))), x1 = (int)ceilf(fmaxf(sx[0], fmaxf(sx[1], sx[2])));
    int y0 = (int)floorf(fminf(sy[0], fminf(sy[1], sy[2]))), y1 = (int)ceilf(fmaxf(sy[0], fmaxf(sy[1], sy[2])));
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > B->w - 1) x1 = B->w - 1; if (y1 > B->h - 1) y1 = B->h - 1;
    V3 L = norm(v3(fr->light_dir[0], fr->light_dir[1], fr->light_dir[2]));
    for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) {
        float px = x + 0.5f, py = y + 0.5f;
        float w0 = ((sx[1] - px) * (sy[2] - py) - (sx[2] - px) * (sy[1] - py)) / area;
        float w1 = ((sx[2] - px) * (sy[0] - py) - (sx[0] - px) * (sy[2] - py)) / area;
        float w2 = 1.0f - w0 - w1;
        if (w0 < 0 || w1 < 0 || w2 < 0) continue;
        float q = w0 * iw[0] + w1 * iw[1] + w2 * iw[2];        /* 1/d interpolado */
        if (q <= 0) continue;
        if (q <= B->z[y * B->w + x]) continue;                  /* mayor 1/d = más cerca (Z invertida) */
        float p0 = w0 * iw[0] / q, p1 = w1 * iw[1] / q, p2 = w2 * iw[2] / q;   /* pesos con corrección de perspectiva */
        float n[3], uv[2], col[3], pe[3];
        for (int k = 0; k < 3; k++) { n[k] = p0 * a->n[k] + p1 * b->n[k] + p2 * c->n[k]; col[k] = p0 * a->c[k] + p1 * b->c[k] + p2 * c->c[k]; pe[k] = p0 * a->p[k] + p1 * b->p[k] + p2 * c->p[k]; }
        uv[0] = p0 * a->uv[0] + p1 * b->uv[0] + p2 * c->uv[0]; uv[1] = p0 * a->uv[1] + p1 * b->uv[1] + p2 * c->uv[1];
        float rgb[3] = {1, 1, 1};
        if (tex && tex->rgba) {
            int tx = (int)floorf(uv[0] * tex->w), ty = (int)floorf(uv[1] * tex->h);
            if (tx < 0) tx = 0; if (ty < 0) ty = 0; if (tx >= (int)tex->w) tx = tex->w - 1; if (ty >= (int)tex->h) ty = tex->h - 1;
            const uint8_t *t = tex->rgba + ((size_t)ty * tex->w + tx) * 4;
            if (t[3] < 128) continue;                          /* alfa-test como el shader */
            rgb[0] = t[0] / 255.f; rgb[1] = t[1] / 255.f; rgb[2] = t[2] / 255.f;
        }
        /* normal en espacio de cámara -> mundo; cara trasera (vista desde dentro) invierte la normal */
        V3 nc = norm(v3(n[0], n[1], n[2]));
        V3 pe3 = v3(pe[0], pe[1], pe[2]);
        if (dot(nc, pe3) > 0) nc = v3(-nc.x, -nc.y, -nc.z);    /* la dirección al ojo es -pe: cara que mira hacia fuera del ojo = trasera */
        V3 nw = v3(cam->r.x * nc.x + cam->u.x * nc.y - cam->f.x * nc.z, cam->r.y * nc.x + cam->u.y * nc.y - cam->f.y * nc.z, cam->r.z * nc.x + cam->u.z * nc.y - cam->f.z * nc.z);
        float h = dot(nw, L) * 0.5f + 0.5f;
        float lit = fr->ambient + fr->diffuse * h * h;
        float out[3];
        for (int k = 0; k < 3; k++) { out[k] = rgb[k] * (col[k] * (255.0f / 128.0f)) * lit; if (out[k] > 1) out[k] = 1; }
        B->z[y * B->w + x] = q;
        for (int k = 0; k < 3; k++) B->rgb[(y * B->w + x) * 3 + k] = out[k];
    }
}

static void draw_tri(Buf *B, const VrHandsMesh *m, uint32_t t, const VrHandsFrame *fr, const Cam *cam, float fy, const VrHandsTex *tex) {
    PV in[3];
    for (int k = 0; k < 3; k++) {
        const VrHandsVertex *vx = &m->verts[m->idx[t * 3 + k]];
        const float *M = m->model;
        V3 p = v3(M[0] * vx->pos[0] + M[4] * vx->pos[1] + M[8] * vx->pos[2] + M[12], M[1] * vx->pos[0] + M[5] * vx->pos[1] + M[9] * vx->pos[2] + M[13],
                  M[2] * vx->pos[0] + M[6] * vx->pos[1] + M[10] * vx->pos[2] + M[14]);
        V3 n = v3(M[0] * vx->nrm[0] + M[4] * vx->nrm[1] + M[8] * vx->nrm[2], M[1] * vx->nrm[0] + M[5] * vx->nrm[1] + M[9] * vx->nrm[2],
                  M[2] * vx->nrm[0] + M[6] * vx->nrm[1] + M[10] * vx->nrm[2]);
        V3 d = sub(p, cam->e);
        in[k].p[0] = dot(d, cam->r); in[k].p[1] = dot(d, cam->u); in[k].p[2] = -dot(d, cam->f);
        in[k].n[0] = dot(n, cam->r); in[k].n[1] = dot(n, cam->u); in[k].n[2] = -dot(n, cam->f);
        in[k].uv[0] = vx->uv[0]; in[k].uv[1] = vx->uv[1];
        for (int q = 0; q < 3; q++) in[k].c[q] = vx->rgba[q] / 255.0f;
    }
    /* recorte contra el plano cercano (delante = z < -near) */
    const float near_ = 0.03f;
    PV poly[4]; int np = 0;
    for (int i = 0; i < 3; i++) {
        const PV *a = &in[i], *b = &in[(i + 1) % 3];
        int ia = a->p[2] < -near_, ib = b->p[2] < -near_;
        if (ia) poly[np++] = *a;
        if (ia != ib) { float tt = (-near_ - a->p[2]) / (b->p[2] - a->p[2]); poly[np++] = lerp_pv(a, b, tt); }
    }
    for (int i = 1; i + 1 < np; i++) shade_tri(B, &poly[0], &poly[i], &poly[i + 1], tex, fr, cam, fy);
}

static void render_view(const VrHandsFrame *fr, const Cam *cam, int W, int H, int ss, WImage *img) {
    Buf B = {W * ss, H * ss, NULL, NULL};
    B.z = calloc((size_t)B.w * B.h, sizeof(float)); B.rgb = calloc((size_t)B.w * B.h * 3, sizeof(float));
    for (int i = 0; i < B.w * B.h; i++) {   /* fondo: degradado sobrio */
        float t = (float)(i / B.w) / B.h; B.rgb[i * 3] = 0.16f + 0.10f * (1 - t); B.rgb[i * 3 + 1] = 0.18f + 0.10f * (1 - t); B.rgb[i * 3 + 2] = 0.24f + 0.12f * (1 - t);
    }
    float fy = 0.5f * B.h / tanf(cam->fov_deg * 0.5f * 3.14159265f / 180.0f);
    for (int i = 0; i < fr->nmeshes; i++) {
        const VrHandsMesh *m = &fr->meshes[i];
        const VrHandsTex *tex = NULL;
        for (int k = 0; k < fr->ntex; k++) if (fr->tex[k].id == m->tex_id && m->tex_id) tex = &fr->tex[k];
        for (uint32_t t = 0; t < m->nidx / 3; t++) draw_tri(&B, m, t, fr, cam, fy, tex);
    }
    *img = wimg_new(W, H);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) for (int k = 0; k < 3; k++) {
        float s = 0; for (int sy = 0; sy < ss; sy++) for (int sx = 0; sx < ss; sx++) s += B.rgb[((y * ss + sy) * B.w + x * ss + sx) * 3 + k];
        s /= ss * ss; img->rgb[(y * W + x) * 3 + k] = (uint8_t)(fminf(fmaxf(s, 0), 1) * 255.0f + 0.5f);
    }
    free(B.z); free(B.rgb);
}

/* ---- eje de la mano (solo para la vista previa): tres barritas con color de vértice ---- */
static VrHandsVertex gz_v[3][8]; static uint16_t gz_i[36];
static const uint16_t BOXI[36] = {0,1,3, 0,3,2, 4,6,7, 4,7,5, 0,2,6, 0,6,4, 1,5,7, 1,7,3, 2,3,7, 2,7,6, 0,4,5, 0,5,1};
static void gizmo(VrHandsMesh *out, const VrHandsPose *h) {
    float len[3][3] = {{0.06f, 0.002f, 0.002f}, {0.002f, 0.06f, 0.002f}, {0.002f, 0.002f, 0.06f}};
    uint8_t col[3][3] = {{255, 40, 40}, {40, 255, 40}, {40, 80, 255}};
    memcpy(gz_i, BOXI, sizeof gz_i);
    for (int a = 0; a < 3; a++) {
        for (int i = 0; i < 8; i++) {
            float sx = (i & 1) ? 1.f : -1.f, sy = (i & 2) ? 1.f : -1.f, sz = (i & 4) ? 1.f : -1.f;
            float c[3] = {a == 0 ? len[0][0] : 0, a == 1 ? len[1][1] : 0, a == 2 ? -len[2][2] : 0};   /* z azul = delante (-Z) */
            gz_v[a][i].pos[0] = c[0] + sx * len[a][0]; gz_v[a][i].pos[1] = c[1] + sy * len[a][1]; gz_v[a][i].pos[2] = c[2] + sz * len[a][2];
            gz_v[a][i].nrm[1] = 1; memcpy(gz_v[a][i].rgba, col[a], 3); gz_v[a][i].rgba[3] = 255;
        }
    }
    WPose p = vrhands_pose_to_w(h);
    for (int a = 0; a < 3; a++) {
        memset(&out[a], 0, sizeof out[a]);
        out[a].mesh_id = 0xFFFF0000u + a;
        out[a].verts = gz_v[a]; out[a].nverts = 8; out[a].idx = gz_i; out[a].nidx = 36;
        WQuat q = wquat_normalize(p.q); float x = q.x, y = q.y, z = q.z, w = q.w;
        float *m = out[a].model;
        m[0] = 1 - 2 * (y * y + z * z); m[1] = 2 * (x * y + w * z); m[2] = 2 * (x * z - w * y); m[3] = 0;
        m[4] = 2 * (x * y - w * z); m[5] = 1 - 2 * (x * x + z * z); m[6] = 2 * (y * z + w * x); m[7] = 0;
        m[8] = 2 * (x * z + w * y); m[9] = 2 * (y * z - w * x); m[10] = 1 - 2 * (x * x + y * y); m[11] = 0;
        m[12] = p.p.x; m[13] = p.p.y; m[14] = p.p.z; m[15] = 1;
    }
}

static uint8_t *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long l = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc((size_t)l + 1);
    if (!d || fread(d, 1, (size_t)l, f) != (size_t)l) { free(d); fclose(f); return NULL; }
    fclose(f); *n = (size_t)l; return d;
}


static VrHandsPose mkpose(float x, float y, float z, float yaw_deg, float pitch_deg) {
    WQuat qy = wquat_axis_angle(wv(0, 1, 0), yaw_deg * 0.017453292f), qx = wquat_axis_angle(wv(1, 0, 0), pitch_deg * 0.017453292f);
    WQuat q = wquat_mul(qy, qx);
    VrHandsPose p = {1, {x, y, z}, {q.x, q.y, q.z, q.w}};
    return p;
}
static int mesh_index(const WeaponDef *d, const char *name) {
    for (int i = 0; d->mesh_names && d->mesh_names[i]; i++) if (!strcmp(d->mesh_names[i], name)) return i;
    return -1;
}
static void save(const char *dir, const char *id, const char *tag, const VrHandsFrame *fr, const Cam *cam, int W, int H) {
    WImage im; render_view(fr, cam, W, H, 2, &im);
    char path[512]; snprintf(path, sizeof path, "%s/%s_%s.png", dir, id, tag);
    if (wimg_write_png(&im, path)) printf("no se pudo escribir %s\n", path); else printf("  %s\n", path);
    wimg_free(&im);
}


/* ---- modo --poses: la mano nueva (vr_hand_skin) en cada pose de vr_hand_pose, sosteniendo objetos y con el antebrazo de la IK ---- */
static const char *const POSE_NAME[HP_COUNT] = {"open", "relax", "fist", "point", "trigger", "grip_pistol", "grip_rifle", "grip_support", "grip_sphere", "grip_placed", "grip_box"};
static VrHandsVertex hp_v[2][3 * VRHA_NT]; static uint16_t hp_i[3 * VRHA_NT];
static float hp_pos[VRHA_NV][3], hp_nrm[VRHA_NV][3];

/* Malla de la mano (un vértice por esquina) con la transformación H (3x4, marco de la mano en mm -> mundo en m). Color por rol: guante del traje (azul marino grisáceo), palma, yemas
 * y puntas algo más oscuras. */
static int hand_mesh(int side, const float xf[VRHS_J][12], const float H[12], VrHandsVertex *out) {
    vrhs_skin(xf, side, hp_pos, hp_nrm);
    static const float col[4][3] = {{0.15f, 0.16f, 0.20f}, {0.21f, 0.23f, 0.29f}, {0.18f, 0.19f, 0.24f}, {0.13f, 0.14f, 0.17f}};
    int nv = 0;
    for (int p = 0; p < VRHA_NP; p++) {
        const VrHaPiece *pc = &vrha_piece[p];
        for (int t = pc->t0; t < pc->t0 + pc->nt; t++) for (int k = 0; k < 3; k++) {
            const int vi = pc->v0 + vrha_tri[t][k];
            VrHandsVertex *o = &out[nv++];
            for (int a = 0; a < 3; a++) {
                o->pos[a] = H[4 * a] * hp_pos[vi][0] + H[4 * a + 1] * hp_pos[vi][1] + H[4 * a + 2] * hp_pos[vi][2] + H[4 * a + 3];
                float n = H[4 * a] * hp_nrm[vi][0] + H[4 * a + 1] * hp_nrm[vi][1] + H[4 * a + 2] * hp_nrm[vi][2];
                o->nrm[a] = n;
            }
            float l = sqrtf(o->nrm[0] * o->nrm[0] + o->nrm[1] * o->nrm[1] + o->nrm[2] * o->nrm[2]);
            for (int a = 0; a < 3; a++) o->nrm[a] /= l > 1e-9f ? l : 1.0f;
            o->uv[0] = o->uv[1] = 0;
            const float *cc = col[vrha_role[t] & 3];
            for (int a = 0; a < 3; a++) o->rgba[a] = (uint8_t)(cc[a] * 128.0f + 0.5f);
            o->rgba[3] = 255;
        }
    }
    for (int i = 0; i < nv; i++) hp_i[i] = (uint16_t)i;
    return nv;
}
static void mesh_of(VrHandsMesh *m, uint32_t id, const VrHandsVertex *v, int nv, const uint16_t *idx, int ni) {
    memset(m, 0, sizeof *m);
    m->mesh_id = id; m->verts = v; m->nverts = (uint32_t)nv; m->idx = idx; m->nidx = (uint32_t)ni;
    m->model[0] = m->model[5] = m->model[10] = m->model[15] = 1.0f;
}
static void frame_of(VrHandsFrame *fr, const VrHandsMesh *m, int n, const Cam *cam, const VrHandsTex *tex, int ntex) {
    memset(fr, 0, sizeof *fr);
    V3 l = norm(v3(-cam->f.x * 0.8f - cam->r.x * 0.35f + cam->u.x * 0.45f, -cam->f.y * 0.8f - cam->r.y * 0.35f + cam->u.y * 0.45f, -cam->f.z * 0.8f - cam->r.z * 0.35f + cam->u.z * 0.45f));
    fr->nmeshes = n; fr->meshes = m; fr->light_dir[0] = l.x; fr->light_dir[1] = l.y; fr->light_dir[2] = l.z; fr->ambient = 0.55f; fr->diffuse = 0.9f;
    fr->tex = tex; fr->ntex = ntex;
}
static void sheet_save(WImage *sheet, const char *dir, const char *name) {
    char path[512]; snprintf(path, sizeof path, "%s/%s.png", dir, name);
    if (wimg_write_png(sheet, path)) printf("no se pudo escribir %s\n", path); else printf("  %s\n", path);
    wimg_free(sheet);
}

/* primitivas (m) con color de vértice */
static VrHandsVertex pr_v[8192]; static uint16_t pr_i[16384]; static int pr_nv, pr_ni;
static void prim_box(const float lo[3], const float hi[3], const float M[12], const uint8_t c[3]) {
    static const int F[6][4] = {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
    static const float NF[6][3] = {{0, 0, -1}, {0, 0, 1}, {0, -1, 0}, {0, 1, 0}, {-1, 0, 0}, {1, 0, 0}};
    for (int f = 0; f < 6; f++) {
        int b = pr_nv;
        for (int k = 0; k < 4; k++) {
            int i = F[f][k];
            float p[3] = {(i & 1) ? hi[0] : lo[0], (i & 2) ? hi[1] : lo[1], (i & 4) ? hi[2] : lo[2]};
            VrHandsVertex *o = &pr_v[pr_nv++];
            for (int a = 0; a < 3; a++) { o->pos[a] = M[4 * a] * p[0] + M[4 * a + 1] * p[1] + M[4 * a + 2] * p[2] + M[4 * a + 3]; o->nrm[a] = M[4 * a] * NF[f][0] + M[4 * a + 1] * NF[f][1] + M[4 * a + 2] * NF[f][2]; }
            o->rgba[0] = c[0]; o->rgba[1] = c[1]; o->rgba[2] = c[2]; o->rgba[3] = 255;
        }
        const int q[6] = {0, 1, 2, 0, 2, 3};
        for (int k = 0; k < 6; k++) pr_i[pr_ni++] = (uint16_t)(b + q[k]);
    }
}
static void prim_sphere(const float c[3], float r, const uint8_t col[3]) {
    const int NU = 24, NVV = 14;
    int b = pr_nv;
    for (int j = 0; j <= NVV; j++) for (int i = 0; i <= NU; i++) {
        float th = 3.14159265f * j / NVV, ph = 6.2831853f * i / NU;
        float n[3] = {sinf(th) * cosf(ph), cosf(th), sinf(th) * sinf(ph)};
        VrHandsVertex *o = &pr_v[pr_nv++];
        for (int a = 0; a < 3; a++) { o->pos[a] = c[a] + r * n[a]; o->nrm[a] = n[a]; }
        o->rgba[0] = col[0]; o->rgba[1] = col[1]; o->rgba[2] = col[2]; o->rgba[3] = 255;
    }
    for (int j = 0; j < NVV; j++) for (int i = 0; i < NU; i++) {
        int a0 = b + j * (NU + 1) + i, a1 = a0 + 1, b0 = a0 + NU + 1, b1 = b0 + 1;
        const int q[6] = {a0, b0, a1, a1, b0, b1};
        for (int k = 0; k < 6; k++) pr_i[pr_ni++] = (uint16_t)q[k];
    }
}
/* cilindro (manga) de p0 a p1 con radios r0, r1 */
static void prim_tube(const float p0[3], const float p1[3], float r0, float r1, const uint8_t col[3]) {
    float ax[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]}, l = sqrtf(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
    for (int a = 0; a < 3; a++) ax[a] /= l;
    float t[3] = {fabsf(ax[1]) < 0.9f ? 0.0f : 1.0f, fabsf(ax[1]) < 0.9f ? 1.0f : 0.0f, 0}, u[3], w[3];
    u[0] = ax[1] * t[2] - ax[2] * t[1]; u[1] = ax[2] * t[0] - ax[0] * t[2]; u[2] = ax[0] * t[1] - ax[1] * t[0];
    float ul = sqrtf(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]); for (int a = 0; a < 3; a++) u[a] /= ul;
    w[0] = ax[1] * u[2] - ax[2] * u[1]; w[1] = ax[2] * u[0] - ax[0] * u[2]; w[2] = ax[0] * u[1] - ax[1] * u[0];
    const int NU = 16; int b = pr_nv;
    for (int e = 0; e < 2; e++) for (int i = 0; i <= NU; i++) {
        float ph = 6.2831853f * i / NU, r = e ? r1 : r0; const float *p = e ? p1 : p0;
        VrHandsVertex *o = &pr_v[pr_nv++];
        for (int a = 0; a < 3; a++) { o->nrm[a] = cosf(ph) * u[a] + sinf(ph) * w[a]; o->pos[a] = p[a] + r * o->nrm[a]; }
        o->rgba[0] = col[0]; o->rgba[1] = col[1]; o->rgba[2] = col[2]; o->rgba[3] = 255;
    }
    for (int i = 0; i < NU; i++) { const int q[6] = {b + i, b + NU + 1 + i, b + i + 1, b + i + 1, b + NU + 1 + i, b + NU + 2 + i}; for (int k = 0; k < 6; k++) pr_i[pr_ni++] = (uint16_t)q[k]; }
}

static void render_cell(WImage *sheet, const VrHandsMesh *m, int n, const Cam *cam, int W, int H, int cx, int cy, const VrHandsTex *tex, int ntex) {
    VrHandsFrame fr; frame_of(&fr, m, n, cam, tex, ntex);
    WImage im; render_view(&fr, cam, W, H, 2, &im);
    wimg_blit(sheet, &im, cx * W, cy * H);
    wimg_free(&im);
}

static void poses_sheets(const char *dir) {
    const int W = 420, H = 520;
    const float Hm[12] = {0.001f, 0, 0, 0, 0, 0.001f, 0, 0, 0, 0, 0.001f, 0};
    for (int pose = 0; pose < HP_COUNT; pose++) for (int pulled = 0; pulled <= 1; pulled++) {
        if (pulled && !vrhandpose_has_trigger(pose)) continue;
        VrHandPoseIn in = {pose, pose == HP_RELAX ? 0.0f : 1.0f, pulled ? 1.0f : 0.0f, 0, 0};
        WImage sheet = wimg_new(3 * W, 2 * H);
        for (int side = 0; side < 2; side++) {
            const int sd = side == 0 ? 1 : -1;
            float xf[VRHS_J][12];
            vrhandpose_blend(sd, &in, xf);
            float Hc[12]; memcpy(Hc, Hm, sizeof Hc); Hc[7] = 0.095f;    /* centrado: la mano mide ~0,19 m hacia -Y */
            int nv = hand_mesh(sd, xf, Hc, hp_v[0]);
            VrHandsMesh m; mesh_of(&m, 1, hp_v[0], nv, hp_i, nv);
            const V3 eyes[3] = {v3(sd * 0.50f, 0, 0), v3(-sd * 0.50f, 0, 0), v3(0, 0, 0.50f)};   /* palma, dorso, perfil del pulgar */
            for (int vw = 0; vw < 3; vw++) { Cam cam = look(eyes[vw], v3(0, 0, 0), v3(0, -1, 0), 30); render_cell(&sheet, &m, 1, &cam, W, H, vw, side, NULL, 0); }
        }
        char name[96]; snprintf(name, sizeof name, "pose_%02d_%s%s", pose, POSE_NAME[pose], pulled ? "_pulled" : "");
        sheet_save(&sheet, dir, name);
    }
}

/* pistola primitiva de test_grip.c (mm, marco del modelo: -Z boca, +Y arriba): mango x +-16, y -100..0, z 0..35; armazón; corredera; gatillo con centro (0, -18, -45.5) */
static void pistol_proxy(const float M[12]) {
    static const uint8_t cg[3] = {52, 52, 56}, cs[3] = {66, 66, 72}, ct[3] = {120, 50, 40};
    const float g0[3] = {-0.016f, -0.100f, 0}, g1[3] = {0.016f, 0, 0.035f}, f0[3] = {-0.010f, -0.030f, -0.120f}, f1[3] = {0.010f, 0, 0};
    const float s0[3] = {-0.010f, 0, -0.190f}, s1[3] = {0.010f, 0.032f, 0.010f}, t0[3] = {-0.003f, -0.026f, -0.049f}, t1[3] = {0.003f, -0.010f, -0.042f};
    prim_box(g0, g1, M, cg); prim_box(f0, f1, M, cg); prim_box(s0, s1, M, cs); prim_box(t0, t1, M, ct);
}
static void wpose_m12(WPose p, float M[12]) {
    WQuat q = wquat_normalize(p.q); float x = q.x, y = q.y, z = q.z, w = q.w;
    float r[12] = {1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y), p.p.x, 2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x), p.p.y,
                   2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y), p.p.z};
    memcpy(M, r, sizeof r);
}
static void m12_scale(float *M, float s) { for (int a = 0; a < 3; a++) for (int j = 0; j < 3; j++) M[4 * a + j] *= s; }

/* Mano derecha empuñando la pistola primitiva por el zócalo de vr_grip (gatillo suelto y a fondo) y la granada (esfera) con el zócalo de la granada; en el marco del arma. */
static void hold_sheets(const char *dir) {
    const int W = 520, H = 440;
    for (int which = 0; which < 2; which++) {
        const VrGripEntry *e = vrgrip_find(which ? 2 : 0);
        if (!e) continue;
        WPose handle = {{0, -0.050f, 0.0175f}, {0, 0, 0, 1}};
        if (which) handle.p = wv(0, 0, 0);
        WImage sheet = wimg_new(3 * W, 2 * H);
        for (int pulled = 0; pulled < 2; pulled++) {
            if (which && pulled) continue;
            VrHandPoseIn in = {e->pose, 1.0f, pulled ? 1.0f : 0.0f, 0, 0};
            float xf[VRHS_J][12];
            vrhandpose_blend(1, &in, xf);
            float Hm[12]; wpose_m12(vrgrip_weapon_from_hand(handle, e->grip, 1), Hm); m12_scale(Hm, VRHS_UNIT_M);   /* mano (mm) -> arma (m) */
            int nv = hand_mesh(1, xf, Hm, hp_v[0]);
            pr_nv = pr_ni = 0;
            float I[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
            if (!which) pistol_proxy(I);
            else { const float c[3] = {0, 0, 0}; const uint8_t col[3] = {80, 100, 70}; prim_sphere(c, 0.032f, col); }
            VrHandsMesh m[2]; mesh_of(&m[0], 1, hp_v[0], nv, hp_i, nv); mesh_of(&m[1], 2, pr_v, pr_nv, pr_i, pr_ni);
            const V3 ctr = which ? v3(0, 0, 0) : v3(0, -0.045f, -0.020f);
            const V3 eyes[3] = {v3(0.45f, 0.06f, 0.06f), v3(-0.45f, 0.10f, 0.03f), v3(0.08f, 0.40f, 0.28f)};   /* dorso (+X), palma/dedos (-X), arriba-atrás */
            for (int vw = 0; vw < 3; vw++) {
                Cam cam = look(v3(ctr.x + eyes[vw].x, ctr.y + eyes[vw].y, ctr.z + eyes[vw].z), ctr, v3(0, 1, 0), 32);
                render_cell(&sheet, m, 2, &cam, W, H, vw, pulled, NULL, 0);
            }
        }
        sheet_save(&sheet, dir, which ? "hold_sphere" : "hold_pistol");
    }
}

/* ---- brazo completo: la IK de vr_body (muñeca = agarre - R_mano * centro de agarre, antebrazo con el giro de la mano, codo girado por los límites de la muñeca) con la mano nueva y el
 * brazo/antebrazo del juego (MV_DUMP = volcado con ram.bin/vram.bin: modelos y texturas reales, antebrazo adelgazado como ns_hand; sin volcado, mangas primitivas) ---- */
static uint8_t *pv_ram; static uint16_t *pv_vram;
static VrGlove *dump_glove(void) {
    const char *d = getenv("MV_DUMP");
    if (!d) return NULL;
    char p[512]; size_t n = 0;
    snprintf(p, sizeof p, "%s/ram.bin", d); pv_ram = slurp(p, &n); if (!pv_ram || n != 0x200000) return NULL;
    snprintf(p, sizeof p, "%s/vram.bin", d); pv_vram = (uint16_t *)slurp(p, &n); if (!pv_vram || n != 1024 * 512 * 2) return NULL;
    #define R32(a) ((uint32_t)pv_ram[(a) & 0x1FFFFF] | (uint32_t)pv_ram[((a) + 1) & 0x1FFFFF] << 8 | (uint32_t)pv_ram[((a) + 2) & 0x1FFFFF] << 16 | (uint32_t)pv_ram[((a) + 3) & 0x1FFFFF] << 24)
    uint32_t ps = R32(R32(0x800ACBB0u)); static const int PI_[2][3] = {{7, 8, 9}, {2, 3, 4}}; uint32_t md[2][3];
    for (int s = 0; s < 2; s++) for (int k = 0; k < 3; k++) md[s][k] = R32(ps + 0x48 + 0x5C * (uint32_t)PI_[s][k] + 0x40);
    #undef R32
    VrGlove *g = vrglove_create();
    if (vrglove_build(g, pv_ram, pv_vram, md, 0) != 1) { vrglove_destroy(g); return NULL; }
    /* antebrazo adelgazado (misma regla que ns_hand_fore_slim): vértices no soldados hacia el eje, radio de la muñeca ~1,15 x el del puño del guante */
    for (int s = 0; s < 2; s++) {
        VrGlovePiece *P = &g->p[s][1];
        float c[3] = {0, 0, 0}; int n0 = 0;
        for (int i = 0; i < P->nv; i++) if (P->weld[i] == 0xFF) { for (int a = 0; a < 3; a++) c[a] += P->v[4 * i + a]; n0++; }
        float l = sqrtf(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]); if (!n0 || l < 1) continue;
        for (int a = 0; a < 3; a++) c[a] /= l;
        float tmax = -1e9f, rs = 0, cr = 0; int nr = 0, nc = 0;
        for (int i = 0; i < P->nv; i++) if (P->weld[i] == 0xFF) { float t = c[0] * P->v[4 * i] + c[1] * P->v[4 * i + 1] + c[2] * P->v[4 * i + 2]; if (t > tmax) tmax = t; }
        for (int i = 0; i < P->nv; i++) { if (P->weld[i] != 0xFF) continue; float v[3] = {P->v[4 * i], P->v[4 * i + 1], P->v[4 * i + 2]}, t = c[0] * v[0] + c[1] * v[1] + c[2] * v[2]; if (t < 0.75f * tmax) continue;
            float r[3] = {v[0] - t * c[0], v[1] - t * c[1], v[2] - t * c[2]}; rs += sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]); nr++; }
        for (int i = 0; i < VRHA_NV; i++) if (vrha_pos[i][1] > 10.0f) { cr += sqrtf(vrha_pos[i][0] * vrha_pos[i][0] + vrha_pos[i][2] * vrha_pos[i][2]); nc++; }
        float sl = fminf(1.0f, fmaxf(0.6f, 1.15f * (cr / (float)nc) * (float)nr / rs));
        printf("  antebrazo %s del volcado: radio de la muñeca %.1f mm -> factor %.2f\n", s ? "derecho" : "izquierdo", rs / (float)nr, sl);
        for (int i = 0; i < P->nv; i++) if (P->weld[i] == 0xFF) {
            float v[3] = {P->v[4 * i], P->v[4 * i + 1], P->v[4 * i + 2]}, t = c[0] * v[0] + c[1] * v[1] + c[2] * v[2];
            for (int a = 0; a < 3; a++) P->v[4 * i + a] = (int16_t)lrintf(t * c[a] + sl * (v[a] - t * c[a]));
        }
    }
    return g;
}
static void m9_mul(float *o, const float *a, const float *b) { float r[9]; for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) r[3 * i + j] = a[3 * i] * b[j] + a[3 * i + 1] * b[3 + j] + a[3 * i + 2] * b[6 + j]; memcpy(o, r, sizeof r); }
static void rot9(float *o, char ax, float deg) { float c = cosf(deg * 0.0174533f), s = sinf(deg * 0.0174533f); float x[9] = {1, 0, 0, 0, c, -s, 0, s, c}, y[9] = {c, 0, s, 0, 1, 0, -s, 0, c}, z[9] = {c, -s, 0, s, c, 0, 0, 0, 1}; memcpy(o, ax == 'x' ? x : ax == 'y' ? y : z, sizeof x); }

static void arm_sheets(const char *dir) {
    VrGlove *g = dump_glove();
    printf("  brazo: %s\n", g ? "modelos del volcado (MV_DUMP)" : "mangas primitivas (sin MV_DUMP)");
    static VrGloveOut go;
    const int W = 560, H = 480;
    /* escenas (mano derecha): agarre del mando en el mundo (mm; personaje mirando a +Z, su izquierda +X, hombro derecho en (-190, 1400, 0)) */
    struct { const char *name; float p[3]; float yaw, pitch, roll; int pose; } SC[4] = {
        {"pistola_delante", {-120, 1250, 420}, 0, 0, 0, HP_GRIP_PISTOL},
        {"pistola_palma_abajo", {-120, 1250, 420}, 0, 0, -90, HP_GRIP_PISTOL},
        {"pistola_muneca_doblada", {-120, 1250, 420}, 0, -55, 0, HP_GRIP_PISTOL},
        {"granada_alta", {-180, 1450, 380}, 20, 30, -40, HP_GRIP_SPHERE},
    };
    VrArmRig rig = {-1.0f, {0, -260, -50}, {0, -249, 47}};
    VrArmLimits lim; vrbody_limits_default(&lim);
    for (int sc = 0; sc < 4; sc++) {
        /* orientación de la pieza mano: apretón de manos (palma a +X, dedos a +Z, pulgar arriba) girada */
        float R0[9] = {1, 0, 0, 0, 0, 1, 0, -1, 0}, A[9], B[9], C[9], R[9];
        rot9(A, 'y', SC[sc].yaw); rot9(B, 'x', -SC[sc].pitch); rot9(C, 'z', SC[sc].roll);
        m9_mul(R, A, B); m9_mul(R, R, C); m9_mul(R, R, R0);
        float gc[3], wr[3]; vrhs_grip_center(1, gc);
        for (int i = 0; i < 3; i++) wr[i] = SC[sc].p[i] - (R[3 * i] * gc[0] + R[3 * i + 1] * gc[1] + R[3 * i + 2] * gc[2]);
        VrArmIn in; memset(&in, 0, sizeof in);
        in.shoulder[0] = -190; in.shoulder[1] = 1400; in.shoulder[2] = 0;
        in.torso[0] = in.torso[4] = in.torso[8] = 1;
        memcpy(in.wrist, wr, sizeof wr); memcpy(in.hand, R, sizeof R);
        in.pole[0] = -0.45f; in.pole[1] = -1; in.pole[2] = -0.35f;
        VrArmOut o; memset(&o, 0, sizeof o);
        for (int it = 0; it < 40; it++) { vrbody_solve_arm(&rig, &lim, &in, &o); in.has_prev = 1; in.tau_prev = o.tau; in.tau_f_prev = o.tau_f; in.swivel_prev = o.swivel; }   /* el codo converge (5 grados por cálculo) */
        float Rf[9]; memcpy(Rf, o.R_fore, sizeof Rf); vrbody_stretch_bone(Rf, rig.wrist_off, o.stretch);
        float Ru[9]; memcpy(Ru, o.R_upper, sizeof Ru); vrbody_stretch_bone(Ru, rig.elbow_off, o.stretch);
        float gap[3]; for (int i = 0; i < 3; i++) gap[i] = o.elbow[i] + (Rf[3 * i] * rig.wrist_off[0] + Rf[3 * i + 1] * rig.wrist_off[1] + Rf[3 * i + 2] * rig.wrist_off[2]) - o.wrist[i];
        printf("  %s: codo girado %.0f grados, giro del antebrazo %.0f grados, antebrazo-muñeca %.2f mm, flags %u\n", SC[sc].name, o.swivel, o.tau_f * 57.3f, sqrtf(gap[0] * gap[0] + gap[1] * gap[1] + gap[2] * gap[2]), o.flags);
        /* mano */
        VrHandPoseIn hin = {SC[sc].pose, 1.0f, 0.0f, 0.0f, 0.0f};
        float xf[VRHS_J][12];
        vrhandpose_blend(1, &hin, xf);
        float Hm[12]; for (int i = 0; i < 3; i++) { for (int j = 0; j < 3; j++) Hm[4 * i + j] = o.R_hand[3 * i + j] * 0.001f; Hm[4 * i + 3] = o.wrist[i] * 0.001f; }
        int nv = hand_mesh(1, xf, Hm, hp_v[0]);
        VrHandsMesh m[6]; int nm = 0;
        mesh_of(&m[nm++], 1, hp_v[0], nv, hp_i, nv);
        pr_nv = pr_ni = 0;
        VrHandsTex tex; int ntex = 0;
        if (g) {
            VrGlovePose gp; memset(&gp, 0, sizeof gp);
            for (int i = 0; i < 3; i++) { gp.org[0][i] = in.shoulder[i] * 0.001f; gp.org[1][i] = o.elbow[i] * 0.001f; gp.org[2][i] = o.wrist[i] * 0.001f; }
            memcpy(gp.R[0], Ru, sizeof Ru); memcpy(gp.R[1], Rf, sizeof Rf); memcpy(gp.R[2], o.R_hand, sizeof Rf);
            gp.upm = 1000.0f;
            if (vrglove_pose(g, 1, &gp, &go) == 0) {
                static uint16_t gi[2][2 * VRGLOVE_MAXOUT];
                for (int k = 0; k < 2; k++) {
                    for (uint32_t i = 0; i < g->p[1][k].nidx_out; i++) gi[k][i] = g->p[1][k].idx[i];
                    mesh_of(&m[nm], 10 + (uint32_t)k, go.v[k], (int)go.nv[k], gi[k], (int)g->p[1][k].nidx_out); m[nm].tex_id = 7; nm++;
                }
                tex.id = 7; tex.w = g->aw; tex.h = g->ah; tex.rgba = g->atlas; ntex = 1;
            }
        } else {
            static const uint8_t sl[3] = {38, 42, 52};
            float s0[3], e0[3], w0[3];
            for (int i = 0; i < 3; i++) { s0[i] = in.shoulder[i] * 0.001f; e0[i] = o.elbow[i] * 0.001f; w0[i] = o.wrist[i] * 0.001f; }
            prim_tube(s0, e0, 0.045f, 0.038f, sl); prim_tube(e0, w0, 0.036f, 0.027f, sl);
        }
        if (SC[sc].pose == HP_GRIP_PISTOL || SC[sc].pose == HP_GRIP_SPHERE) {   /* objeto en la mano por el zócalo de vr_grip */
            const VrGripEntry *e = vrgrip_find(SC[sc].pose == HP_GRIP_PISTOL ? 0 : 2);
            WPose handle = {{0, -0.050f, 0.0175f}, {0, 0, 0, 1}};
            if (SC[sc].pose == HP_GRIP_SPHERE) handle.p = wv(0, 0, 0);
            float qh[4]; vrbody_mat_to_quat(o.R_hand, qh);
            WPose Hw = {{o.wrist[0] * 0.001f, o.wrist[1] * 0.001f, o.wrist[2] * 0.001f}, {qh[0], qh[1], qh[2], qh[3]}};
            WPose Mw = wpose_mul(Hw, wpose_inv(vrgrip_weapon_from_hand(handle, e->grip, 1)));   /* modelo -> mundo */
            float M[12]; wpose_m12(Mw, M);
            if (SC[sc].pose == HP_GRIP_PISTOL) pistol_proxy(M);
            else { const float c0[3] = {M[3], M[7], M[11]}; const uint8_t col[3] = {40, 52, 36}; prim_sphere(c0, 0.032f, col); }
        }
        if (pr_ni) mesh_of(&m[nm++], 2, pr_v, pr_nv, pr_i, pr_ni);
        WImage sheet = wimg_new(3 * W, H);
        const V3 hc = v3(o.wrist[0] * 0.001f, o.wrist[1] * 0.001f, o.wrist[2] * 0.001f), el = v3(o.elbow[0] * 0.001f, o.elbow[1] * 0.001f, o.elbow[2] * 0.001f);
        const V3 mid = v3(0.5f * (hc.x + el.x), 0.5f * (hc.y + el.y), 0.5f * (hc.z + el.z));
        Cam c1 = look(v3(-0.02f, 1.62f, 0.06f), v3(hc.x, hc.y, hc.z + 0.05f), v3(0, 1, 0), 60);                 /* primera persona */
        Cam c2 = look(v3(mid.x - 0.55f, mid.y + 0.10f, mid.z + 0.05f), mid, v3(0, 1, 0), 42);                    /* desde la derecha */
        Cam c3 = look(v3(mid.x + 0.05f, mid.y + 0.55f, mid.z - 0.10f), mid, v3(0, 0, 1), 42);                     /* desde arriba */
        render_cell(&sheet, m, nm, &c1, W, H, 0, 0, ntex ? &tex : NULL, ntex);
        render_cell(&sheet, m, nm, &c2, W, H, 1, 0, ntex ? &tex : NULL, ntex);
        render_cell(&sheet, m, nm, &c3, W, H, 2, 0, ntex ? &tex : NULL, ntex);
        char name[96]; snprintf(name, sizeof name, "arm_%d_%s", sc, SC[sc].name);
        sheet_save(&sheet, dir, name);
    }
    if (g) vrglove_destroy(g);
    free(pv_ram); free(pv_vram);
}

static void poses_main(const char *dir) {
    poses_sheets(dir);
    hold_sheets(dir);
    arm_sheets(dir);
}

int main(int argc, char **argv) {
    if (argc >= 3 && !strcmp(argv[1], "--poses")) { poses_main(argv[2]); return 0; }
    if (argc < 4) { fprintf(stderr, "uso: hands_preview <archivo_stages> <stage> <dir_salida>\n"); return 1; }
    size_t n = 0; uint8_t *d = slurp(argv[1], &n);
    if (!d) { fprintf(stderr, "no se puede leer %s (datos locales del usuario)\n", argv[1]); return 1; }
    const uint16_t ids[2] = {0x36f6, 0x21b6};
    const char *idn[2] = {"36f6", "21b6"};
    for (int mi = 0; mi < 2; mi++) {
        printf("== %s\n", idn[mi]);
        WLoadOpts o; wload_default_opts(&o); o.resident = argv[2]; o.upscale = 2;
        WLoaded m; int r = wload_model(d, n, argv[2], ids[mi], &o, &m);
        if (r) { fprintf(stderr, "wload_model=%d\n", r); return 1; }
        const WCutModel *cm = wcut_find(ids[mi]);
        WCutSet cs; r = wcut_run(&m, cm, &cs);
        if (r) { fprintf(stderr, "wcut_run=%d\n", r); return 1; }
        const WeaponDef *def = NULL;
        for (int i = 0; i < w_registry_count; i++) if (w_registry[i] && cm->weapon && !strcmp(w_registry[i]->id, cm->weapon)) def = w_registry[i];
        if (!def) { fprintf(stderr, "arma no registrada\n"); return 1; }
        WeaponState *st = calloc(1, def->state_size);
        st->def = def; def->init(st);
        weapon_bind(st, cs.meshes, cs.nmeshes, cs.parts, cs.nparts);
        VrHandsRig *rig = vrhands_create(NULL);
        if (vrhands_bind(rig, st, cs.atlas, cs.atlas_w, cs.atlas_h)) { fprintf(stderr, "bind\n"); return 1; }
        WPose g; int dr = vrhands_derive_grip(st, &g);
        printf("  atlas %ux%u; agarre derivado %s: (%.1f, %.1f, %.1f) mm; WP_GRIP del arma: (%.1f, %.1f, %.1f) mm\n", cs.atlas_w, cs.atlas_h,
               dr ? "FALLA" : "ok", g.p.x * 1e3, g.p.y * 1e3, g.p.z * 1e3, def->points[WP_GRIP].p.x * 1e3, def->points[WP_GRIP].p.y * 1e3, def->points[WP_GRIP].p.z * 1e3);
        int mag_i = mesh_index(def, "mag"), case_i = mesh_index(def, "case");

        /* escenario A: pistola en la mano derecha, cargador a medio sacar (40 mm de ~81), corredera hacia atrás 21 mm */
        VrHandsPose hand = mkpose(0.17f, 1.22f, -0.40f, 7.0f, 12.0f);
        st->channel[WCH_MAG] = 0.040f; st->channel[WCH_SLIDE] = 0.021f;
        VrHandsFrame fr; int nm = vrhands_build(rig, st, &hand, &fr);
        printf("  escenario A: %d mallas, %d textura(s)\n", nm, fr.ntex);
        VrHandsMesh all[VRH_MAX_MESHES + 6]; VrHandsFrame fa = fr;
        memcpy(all, fr.meshes, sizeof(VrHandsMesh) * (size_t)nm);
        gizmo(all + nm, &hand); fa.meshes = all; fa.nmeshes = nm + 3;
        V3 head = v3(0, 1.6f, 0);
        Cam ce = look(head, v3(0.10f, 1.18f, -0.50f), v3(0, 1, 0), 55);
        save(argv[3], idn[mi], "eye", &fa, &ce, 1000, 700);
        Cam ceR = look(v3(0.0315f, 1.6f, 0), v3(0.10f, 1.18f, -0.50f), v3(0, 1, 0), 55);
        save(argv[3], idn[mi], "eyeR", &fa, &ceR, 1000, 700);
        WPose Wp = vrhands_weapon_world(rig, st, &hand);
        V3 ctr = v3(Wp.p.x + 0.0f, Wp.p.y + 0.02f, Wp.p.z - 0.10f);   /* aprox. centro del arma */
        WVec3 cw = wpose_apply(Wp, wv(0, 0.03f, -0.13f)); ctr = v3(cw.x, cw.y, cw.z);
        Cam cs1 = look(v3(ctr.x + 0.32f, ctr.y + 0.05f, ctr.z + 0.10f), ctr, v3(0, 1, 0), 34);
        save(argv[3], idn[mi], "side", &fa, &cs1, 1000, 700);
        Cam cs2 = look(v3(ctr.x - 0.22f, ctr.y + 0.14f, ctr.z + 0.30f), ctr, v3(0, 1, 0), 38);
        save(argv[3], idn[mi], "rear", &fa, &cs2, 1000, 700);

        /* escenario B: recarga: cargador viejo fuera (apartado), cargador nuevo en la mano libre, vaina en el aire */
        st->channel[WCH_MAG] = 50.0f; st->channel[WCH_SLIDE] = 0.0f;
        VrHandsPose hand1 = mkpose(-0.02f, 1.14f, -0.36f, -10.0f, 0.0f);
        WPose rel = {{0, 0, -0.02f}, wquat_axis_angle(wv(1, 0, 0), -0.7853982f)};
        if (mag_i >= 0) { st->held[0].active = 1; st->held[0].mesh = mag_i; st->held[0].world = wpose_mul(vrhands_pose_to_w(&hand1), rel); }
        if (case_i >= 0) { st->held[1].active = 1; st->held[1].mesh = case_i; st->held[1].world = (WPose){{0.22f, 1.30f, -0.30f}, wquat_axis_angle(wv(0, 0, 1), 0.8f)}; }
        nm = vrhands_build(rig, st, &hand, &fr);
        printf("  escenario B: %d mallas (cargador en mano libre y vaina)\n", nm);
        memcpy(all, fr.meshes, sizeof(VrHandsMesh) * (size_t)nm);
        gizmo(all + nm, &hand); gizmo(all + nm + 3, &hand1);
        VrHandsFrame fb = fr; fb.meshes = all; fb.nmeshes = nm + 6;
        Cam cf = look(head, v3(0.08f, 1.22f, -1.0f), v3(0, 1, 0), 70);
        Cam cf2 = look(v3(0.05f, 1.35f, -0.05f), v3(0.07f, 1.20f, -0.40f), v3(0, 1, 0), 45);
        save(argv[3], idn[mi], "free", &fb, &cf, 1000, 700);
        save(argv[3], idn[mi], "freeclose", &fb, &cf2, 1000, 700);
        vrhands_destroy(rig); free(st); wcut_free(&cs); wload_free(&m);
    }
    free(d);
    return 0;
}

#endif /* VRH_PREVIEW_BUILD */
