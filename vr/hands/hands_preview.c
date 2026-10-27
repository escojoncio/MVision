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
#include "vr_hand_pose.h"
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


/* ---- modo --poses: la mano nueva en cada pose de vr_hand_pose ---- */
static const char *const POSE_NAME[HP_COUNT] = {"open", "relax", "fist", "point", "trigger", "grip_pistol", "grip_rifle", "grip_support", "grip_sphere", "grip_placed", "grip_box"};
static VrHandsVertex hp_v[2][6 * 320]; static uint16_t hp_i[6 * 320];

/* Mallas de la mano en el marco de la mano (metros, centrada en c) con las soldaduras aplicadas; color por rol (guante gris oscuro, uña y yema algo más claras). */
static int hand_mesh(const VrHand *h, const float xf[VRHAND_BONES][12], VrHandsVertex *out, uint16_t *idx, const float c[3], float um) {
    static float sk[VRHAND_BONES][VRHAND_MAX_V][3];
    vrhand_skin(h, xf, sk);
    static const uint8_t col[VRHAND_ROLES][3] = {{46, 48, 52}, {60, 62, 68}, {54, 56, 62}, {84, 88, 96}};
    int nv = 0;
    for (int k = 0; k < VRHAND_BONES; k++) {
        const VrHandBoneMesh *m = &h->mesh[k];
        const float *R = xf[k];
        for (int f = 0; f < m->n_faces; f++) {
            static const int tri[6] = {0, 1, 3, 1, 2, 3};
            for (int q = 0; q < 6 && nv + 6 <= 6 * 320; q++) {
                int vi = m->f[f][tri[q]];
                VrHandsVertex *o = &out[nv];
                for (int a = 0; a < 3; a++) o->pos[a] = (sk[k][vi][a] - c[a]) * um;
                float n[3] = {m->n[vi][0] / 4096.0f, m->n[vi][1] / 4096.0f, m->n[vi][2] / 4096.0f};
                for (int a = 0; a < 3; a++) o->nrm[a] = -(R[4 * a] * n[0] + R[4 * a + 1] * n[1] + R[4 * a + 2] * n[2]);
                o->uv[0] = o->uv[1] = 0;
                const uint8_t *cc = col[m->role[f] < VRHAND_ROLES ? m->role[f] : 0];
                o->rgba[0] = cc[0]; o->rgba[1] = cc[1]; o->rgba[2] = cc[2]; o->rgba[3] = 255;
                idx[nv] = (uint16_t)nv; nv++;
            }
        }
    }
    return nv;
}

static void poses_main(const char *dir) {
    VrHand hs[2];
    vrhand_build(&hs[0], NULL, 1); vrhand_build(&hs[1], NULL, -1);
    const float um = vrhand_unit_m(&hs[0]);
    const int W = 420, H = 520;
    printf("mano: %d triángulos, %.3f mm por unidad\n", vrhand_tri_count(&hs[0]), um * 1000.0f);
    for (int pose = 0; pose < HP_COUNT; pose++) for (int pulled = 0; pulled <= 1; pulled++) {
        if (pulled && !vrhandpose_has_trigger(pose)) continue;
        VrHandPoseIn in = {pose, pose == HP_RELAX ? 0.0f : 1.0f, pulled ? 1.0f : 0.0f, 0, 0};
        WImage sheet = wimg_new(3 * W, 2 * H);
        for (int side = 0; side < 2; side++) {
            float xf[VRHAND_BONES][12];
            vrhandpose_blend(&hs[side], &in, xf);
            float c[3] = {0, 0, 0}, lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
            static float sk[VRHAND_BONES][VRHAND_MAX_V][3];
            vrhand_skin(&hs[side], xf, sk);
            for (int k = 0; k < VRHAND_BONES; k++) for (int i = 0; i < hs[side].mesh[k].n_verts; i++) for (int a = 0; a < 3; a++) { if (sk[k][i][a] < lo[a]) lo[a] = sk[k][i][a]; if (sk[k][i][a] > hi[a]) hi[a] = sk[k][i][a]; }
            for (int a = 0; a < 3; a++) c[a] = 0.5f * (lo[a] + hi[a]);
            int nv = hand_mesh(&hs[side], xf, hp_v[0], hp_i, c, um);
            VrHandsMesh m; memset(&m, 0, sizeof m);
            m.mesh_id = 1; m.verts = hp_v[0]; m.nverts = (uint32_t)nv; m.idx = hp_i; m.nidx = (uint32_t)nv;
            m.model[0] = m.model[5] = m.model[10] = m.model[15] = 1.0f;
            /* vistas: ojo en +X (palma de la derecha), en -X (dorso) y en +Z (perfil del pulgar); arriba = -Y (los dedos hacia arriba). El espejo de la izquierda muestra la palma en -X. */
            const float sgn = side == 0 ? 1.0f : -1.0f;
            const V3 eyes[3] = {v3(sgn * 0.46f, 0, 0), v3(-sgn * 0.46f, 0, 0), v3(0, 0, 0.46f)};
            for (int vw = 0; vw < 3; vw++) {
                Cam cam = look(eyes[vw], v3(0, 0, 0), v3(0, -1, 0), 30);
                V3 l = norm(v3(-cam.f.x * 0.8f - cam.r.x * 0.35f + cam.u.x * 0.45f, -cam.f.y * 0.8f - cam.r.y * 0.35f + cam.u.y * 0.45f, -cam.f.z * 0.8f - cam.r.z * 0.35f + cam.u.z * 0.45f));
                VrHandsFrame fr; memset(&fr, 0, sizeof fr);
                fr.nmeshes = 1; fr.meshes = &m; fr.light_dir[0] = l.x; fr.light_dir[1] = l.y; fr.light_dir[2] = l.z; fr.ambient = 0.55f; fr.diffuse = 0.9f;
                WImage im; render_view(&fr, &cam, W, H, 2, &im);
                wimg_blit(&sheet, &im, vw * W, side * H);
                wimg_free(&im);
            }
        }
        char path[512]; snprintf(path, sizeof path, "%s/pose_%02d_%s%s.png", dir, pose, POSE_NAME[pose], pulled ? "_pulled" : "");
        if (wimg_write_png(&sheet, path)) printf("no se pudo escribir %s\n", path); else printf("  %s\n", path);
        wimg_free(&sheet);
    }
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
