/* Escena nativa SINTÉTICA para el banco del renderer Metal (ningún dato del juego): una sala cerrada con suelo y paredes de mapa (color precalculado),
 * pilares y esfera sombreados con luz, un modelo articulado con vértices welded, semitransparentes de los 4 modos mezclados con opacos, triángulos
 * y niebla. Se genera por código en un fichero de prueba, se vuelve a leer, se publica por el puente (ns_bridge_*) y se adapta a ns_cpuref para
 * tener la imagen de referencia. El adaptador reimplementa a propósito, de forma independiente, la lógica de color/niebla/welded del renderer. */
#include "native_synth.h"
#include "ns_bridge.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NSG_NO_WELD_C 0xFF

/* ---------------------------------------------------------------------------------------------------- VRAM sintética */
static uint16_t g_vram[1024 * 512];

static uint16_t rgb15(int r, int g, int b, int stp) { return (uint16_t)((r & 31) | ((g & 31) << 5) | ((b & 31) << 10) | (stp ? 0x8000 : 0)); }

#define TP_A ((uint16_t)(0 | (0 << 7)))      /* 4 bpp en x=0 */
#define TP_B ((uint16_t)(2 | (1 << 7)))      /* 8 bpp en x=128 */
#define TP_C ((uint16_t)(4 | (2 << 7)))      /* 15 bpp en x=256 */
#define TP_D ((uint16_t)(8 | (2 << 7)))      /* 15 bpp en x=512 */
#define CL_A ((uint16_t)(0 | (480 << 6)))
#define CL_B ((uint16_t)(0 | (481 << 6)))

static void make_vram(uint16_t *vr) {
    memset(vr, 0, 1024 * 512 * 2);
    /* A: 4 bpp, ladrillos; paleta de 16 (índice 0 transparente, 14 con bit15) */
    for (int i = 0; i < 16; i++) vr[480 * 1024 + i] = rgb15(8 + i, 5 + i / 2, 3 + i / 4, i == 14);
    vr[480 * 1024 + 0] = 0;
    for (int y = 0; y < 256; y++)
        for (int xw = 0; xw < 64; xw++) {
            uint16_t w = 0;
            for (int k = 0; k < 4; k++) {
                int u = xw * 4 + k;
                int idx = ((y & 31) < 3 || (((u + ((y >> 5) & 1) * 32) & 63) < 3)) ? 1 : 2 + (((u >> 3) + 3 * (y >> 3)) & 11);
                w = (uint16_t)(w | (idx << (4 * k)));
            }
            vr[y * 1024 + xw] = w;
        }
    /* B: 8 bpp, damero con degradado; paleta de 256 (0 transparente; algunos con bit15) */
    for (int i = 0; i < 256; i++) vr[481 * 1024 + i] = rgb15((i >> 3) & 31, (i * 3 >> 3) & 31, 31 - ((i >> 3) & 31), (i % 61) == 7);
    vr[481 * 1024 + 0] = 0;
    for (int y = 0; y < 256; y++)
        for (int xw = 0; xw < 128; xw++) {
            int u0 = xw * 2;
            uint16_t w = 0;
            for (int k = 0; k < 2; k++) {
                int u = u0 + k;
                int idx = (((u >> 4) ^ (y >> 4)) & 1) ? 40 + ((u + y) & 63) : 140 + ((u * 3 + y) & 63);
                w = (uint16_t)(w | (idx << (8 * k)));
            }
            vr[y * 1024 + 128 + xw] = w;
        }
    /* C: 15 bpp, degradado con rejilla; huecos transparentes (texel 0) y franjas con bit15 */
    for (int y = 0; y < 256; y++)
        for (int x = 0; x < 256; x++) {
            int stp = (((x + y) >> 5) & 1);
            uint16_t t = rgb15(x >> 3, y >> 3, ((x ^ y) >> 3) & 31, stp);
            if ((x & 15) == 0 || (y & 15) == 0) t = rgb15(4, 4, 4, stp);
            if ((((x >> 5) + (y >> 5)) % 7) == 0 && (x & 31) < 12) t = 0;
            if (t == 0 && !((((x >> 5) + (y >> 5)) % 7) == 0 && (x & 31) < 12)) t = 1;
            vr[y * 1024 + 256 + x] = t;
        }
    /* D: 15 bpp, para sombras/cristales: mitad con bit15 */
    for (int y = 0; y < 256; y++)
        for (int x = 0; x < 256; x++) {
            int stp = x < 128;
            vr[y * 1024 + 512 + x] = rgb15(20 + (x >> 5), 10 + (y >> 5), 28 - (x >> 6), stp);
        }
}

/* ---------------------------------------------------------------------------------------------------- constructores de mallas */
typedef struct {
    NsVert v[128]; NsNorm n[128]; NsFace f[96]; uint32_t paint[96 * 4];
    int nv, nn, nf;
} MB;

static int mb_v(MB *m, int x, int y, int z, int weld) {
    m->v[m->nv].x = (int16_t)x; m->v[m->nv].y = (int16_t)y; m->v[m->nv].z = (int16_t)z;
    m->v[m->nv].weld = (uint8_t)weld; m->v[m->nv].pad = 0;
    return m->nv++;
}
static int mb_n(MB *m, float x, float y, float z) {
    float l = sqrtf(x * x + y * y + z * z);
    if (l > 0) { x /= l; y /= l; z /= l; }
    m->n[m->nn].x = (int16_t)lrintf(x * 4096.0f); m->n[m->nn].y = (int16_t)lrintf(y * 4096.0f); m->n[m->nn].z = (int16_t)lrintf(z * 4096.0f); m->n[m->nn].pad = 0;
    return m->nn++;
}
static uint32_t pk(int r, int g, int b) { return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | 0xFF000000u; }

/* v[4], nrm[4] (índices de vértice/normal en orden perímetro), uv[8] = (u,t) de A,B,C,D, pc = 4 colores o NULL */
static void mb_f(MB *m, const int v[4], const int nr[4], const uint8_t uv[8], uint16_t tpage, uint16_t clut, uint16_t texid, int tri, const uint32_t *pc) {
    NsFace *f = &m->f[m->nf];
    memset(f, 0, sizeof *f);
    for (int k = 0; k < 4; k++) { f->v[k] = (uint8_t)v[k]; f->n[k] = (uint8_t)nr[k]; f->u[k] = uv[2 * k]; f->t[k] = uv[2 * k + 1]; }
    f->tpage = tpage; f->clut = clut; f->texid = texid;
    f->flags = (uint8_t)((tri ? NS_FF_TRI : 0) | (pc ? NS_FF_BAKED_OK : 0));
    for (int k = 0; k < 4; k++) m->paint[4 * m->nf + k] = pc ? pc[k] : 0;
    m->nf++;
}
static const uint8_t UVQ[8] = {0, 0, 255, 0, 255, 255, 0, 255};

typedef struct { NsMesh m; MB *b; } Built;
static Built g_built[16];
static int g_nbuilt;

static void finish_mesh(MB *b, uint32_t flags, int16_t depth_bias, uint32_t addr) {
    (void)addr;
    Built *o = &g_built[g_nbuilt++];
    MB *c = (MB *)malloc(sizeof *c);
    memcpy(c, b, sizeof *c);
    memset(&o->m, 0, sizeof o->m);
    o->b = c;
    o->m.flags = flags; o->m.depth_bias = depth_bias; o->m.epoch = 1;
    o->m.n_verts = (uint16_t)c->nv; o->m.n_norms = (uint16_t)c->nn; o->m.n_faces = (uint16_t)c->nf;
    o->m.verts = c->v; o->m.norms = c->n; o->m.faces = c->f;
    o->m.id = ns_mesh_content_hash(&o->m);
}

enum { M_FLOOR, M_ROOM, M_PILLAR, M_SPHERE, M_TORSO, M_ARM, M_SPIKE, M_SEMI_A, M_SEMI_B, M_BACK, M_DECAL_BASE, M_DECAL, M_COUNT };

static void build_floor(void) {
    MB b; memset(&b, 0, sizeof b);
    int id[7][7];
    for (int j = 0; j < 7; j++) for (int i = 0; i < 7; i++) id[j][i] = mb_v(&b, -6000 + 2000 * i, 0, -6000 + 2000 * j, NSG_NO_WELD_C);
    int nu = mb_n(&b, 0, 1, 0);
    for (int j = 0; j < 6; j++)
        for (int i = 0; i < 6; i++) {
            int v[4] = {id[j][i], id[j][i + 1], id[j + 1][i + 1], id[j + 1][i]};
            int ij[4][2] = {{i, j}, {i + 1, j}, {i + 1, j + 1}, {i, j + 1}};
            uint32_t pc[4];
            for (int k = 0; k < 4; k++) { int val = 90 + (((ij[k][0] * 5 + ij[k][1] * 3) % 9) * 10); pc[k] = pk(val, val * 9 / 10, val * 8 / 10); }
            int nr[4] = {nu, nu, nu, nu};
            mb_f(&b, v, nr, UVQ, TP_A, CL_A, 1, 0, pc);
        }
    finish_mesh(&b, NS_MF_BAKED, 0, 0x1000u * (M_FLOOR + 1));
}

static void build_room(void) {
    MB b; memset(&b, 0, sizeof b);
    int nrm = mb_n(&b, 0, 1, 0);
    /* paredes -z, +z, -x, +x (texturizadas 15 bpp) y techo (sin textura, gouraud fuerte) */
    const int X = 6000, Y = 4000;
    int W[4][4][3] = {
        {{-X, Y, -X}, {X, Y, -X}, {X, 0, -X}, {-X, 0, -X}},
        {{X, Y, X}, {-X, Y, X}, {-X, 0, X}, {X, 0, X}},
        {{-X, Y, X}, {-X, Y, -X}, {-X, 0, -X}, {-X, 0, X}},
        {{X, Y, -X}, {X, Y, X}, {X, 0, X}, {X, 0, -X}},
    };
    for (int w = 0; w < 4; w++) {
        int v[4];
        for (int k = 0; k < 4; k++) v[k] = mb_v(&b, W[w][k][0], W[w][k][1], W[w][k][2], NSG_NO_WELD_C);
        uint32_t pc[4] = {pk(70 + 15 * w, 80, 110), pk(70 + 15 * w, 80, 110), pk(150, 140 + 10 * w, 120), pk(150, 140 + 10 * w, 120)};
        int nr[4] = {nrm, nrm, nrm, nrm};
        mb_f(&b, v, nr, UVQ, (uint16_t)(TP_C), 0, 2, 0, pc);
    }
    int v[4];
    int C[4][3] = {{-X, Y, -X}, {X, Y, -X}, {X, Y, X}, {-X, Y, X}};
    for (int k = 0; k < 4; k++) v[k] = mb_v(&b, C[k][0], C[k][1], C[k][2], NSG_NO_WELD_C);
    uint32_t pc[4] = {pk(230, 60, 60), pk(60, 230, 60), pk(60, 60, 230), pk(230, 230, 60)};
    int nr[4] = {nrm, nrm, nrm, nrm};
    mb_f(&b, v, nr, UVQ, 0, 0, 0, 0, pc);
    finish_mesh(&b, NS_MF_BAKED, 0, 0x1000u * (M_ROOM + 1));
}

/* caja con normales por cara; 8 vértices; texturizada */
static void box_faces(MB *b, const int vid[8], uint16_t tpage, uint16_t clut, uint16_t texid, const int skip[6]) {
    /* vid: 0..3 abajo (-y) en orden perímetro, 4..7 arriba */
    int nn[6] = {mb_n(b, 0, -1, 0), mb_n(b, 0, 1, 0), mb_n(b, 0, 0, -1), mb_n(b, 1, 0, 0), mb_n(b, 0, 0, 1), mb_n(b, -1, 0, 0)};
    const int F[6][4] = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7}};
    for (int f = 0; f < 6; f++) {
        if (skip && skip[f]) continue;
        int v[4], nr[4] = {nn[f], nn[f], nn[f], nn[f]};
        for (int k = 0; k < 4; k++) v[k] = vid[F[f][k]];
        mb_f(b, v, nr, UVQ, tpage, clut, texid, 0, NULL);
    }
}

static void build_pillar(void) {
    MB b; memset(&b, 0, sizeof b);
    int vid[8], k = 0;
    int P[8][3] = {{-200, 0, -200}, {200, 0, -200}, {200, 0, 200}, {-200, 0, 200}, {-200, 1800, -200}, {200, 1800, -200}, {200, 1800, 200}, {-200, 1800, 200}};
    for (k = 0; k < 8; k++) vid[k] = mb_v(&b, P[k][0], P[k][1], P[k][2], NSG_NO_WELD_C);
    box_faces(&b, vid, TP_B, CL_B, 3, NULL);
    finish_mesh(&b, 0, 0, 0x1000u * (M_PILLAR + 1));
}

static void build_sphere(void) {
    MB b; memset(&b, 0, sizeof b);
    enum { NLAT = 8, NLON = 12 };
    const float R = 700.0f;
    int id[NLAT + 1][NLON + 1], nid[NLAT + 1][NLON + 1];
    for (int i = 0; i <= NLAT; i++)
        for (int j = 0; j <= NLON; j++) {
            float th = (float)M_PI * (float)i / NLAT, ph = 2.0f * (float)M_PI * (float)j / NLON;
            float x = sinf(th) * cosf(ph), y = cosf(th), z = sinf(th) * sinf(ph);
            id[i][j] = mb_v(&b, (int)lrintf(R * x), (int)lrintf(R * y), (int)lrintf(R * z), NSG_NO_WELD_C);
            nid[i][j] = mb_n(&b, x, y, z);
        }
    for (int i = 0; i < NLAT; i++)
        for (int j = 0; j < NLON; j++) {
            uint8_t uv[8];
            int u0 = j * 255 / NLON, u1 = (j + 1) * 255 / NLON, t0 = i * 255 / NLAT, t1 = (i + 1) * 255 / NLAT;
            if (i == 0) {
                int v[4] = {id[0][j], id[1][j + 1], id[1][j], id[1][j]}, nr[4] = {nid[0][j], nid[1][j + 1], nid[1][j], nid[1][j]};
                uint8_t uvt[8] = {(uint8_t)u0, (uint8_t)t0, (uint8_t)u1, (uint8_t)t1, (uint8_t)u0, (uint8_t)t1, (uint8_t)u0, (uint8_t)t1};
                mb_f(&b, v, nr, uvt, TP_D, 0, 4, 1, NULL);
            } else if (i == NLAT - 1) {
                int v[4] = {id[i][j], id[i][j + 1], id[NLAT][j], id[NLAT][j]}, nr[4] = {nid[i][j], nid[i][j + 1], nid[NLAT][j], nid[NLAT][j]};
                uint8_t uvt[8] = {(uint8_t)u0, (uint8_t)t0, (uint8_t)u1, (uint8_t)t0, (uint8_t)u0, (uint8_t)t1, (uint8_t)u0, (uint8_t)t1};
                mb_f(&b, v, nr, uvt, TP_D, 0, 4, 1, NULL);
            } else {
                int v[4] = {id[i][j], id[i][j + 1], id[i + 1][j + 1], id[i + 1][j]}, nr[4] = {nid[i][j], nid[i][j + 1], nid[i + 1][j + 1], nid[i + 1][j]};
                uv[0] = (uint8_t)u0; uv[1] = (uint8_t)t0; uv[2] = (uint8_t)u1; uv[3] = (uint8_t)t0; uv[4] = (uint8_t)u1; uv[5] = (uint8_t)t1; uv[6] = (uint8_t)u0; uv[7] = (uint8_t)t1;
                mb_f(&b, v, nr, uv, TP_C, 0, 4, 0, NULL);
            }
        }
    finish_mesh(&b, 0, 0, 0x1000u * (M_SPHERE + 1));
}

static int g_torso_face_verts[4];   /* vértices de la cara +x del torso (los que sueldan al brazo) */

static void build_robot(void) {
    MB b; memset(&b, 0, sizeof b);
    int vid[8];
    int P[8][3] = {{-150, -350, -150}, {150, -350, -150}, {150, -350, 150}, {-150, -350, 150}, {-150, 350, -150}, {150, 350, -150}, {150, 350, 150}, {-150, 350, 150}};
    for (int k = 0; k < 8; k++) vid[k] = mb_v(&b, P[k][0], P[k][1], P[k][2], NSG_NO_WELD_C);
    box_faces(&b, vid, TP_C, 0, 5, NULL);
    /* cara +x: vértices 1,2,6,5 */
    g_torso_face_verts[0] = vid[1]; g_torso_face_verts[1] = vid[2]; g_torso_face_verts[2] = vid[6]; g_torso_face_verts[3] = vid[5];
    finish_mesh(&b, 0, 0, 0x1000u * (M_TORSO + 1));

    MB a; memset(&a, 0, sizeof a);
    int aid[8];
    /* extremo cercano (x = 0) soldado al torso: 4 vértices con weld = vértices de la cara +x del torso; extremo lejano libre */
    int AP[8][4] = {{0, -100, -100, 0}, {500, -100, -100, 0xFF}, {500, -100, 100, 0xFF}, {0, -100, 100, 0}, {0, 100, -100, 0}, {500, 100, -100, 0xFF}, {500, 100, 100, 0xFF}, {0, 100, 100, 0}};
    /* weld[k] = índice (en el torso) de la esquina del torso que sustituye */
    int WT[8] = {g_torso_face_verts[0], 0, 0, g_torso_face_verts[1], g_torso_face_verts[3], 0, 0, g_torso_face_verts[2]};
    for (int k = 0; k < 8; k++) aid[k] = mb_v(&a, AP[k][0], AP[k][1], AP[k][2], AP[k][3] == 0xFF ? NSG_NO_WELD_C : WT[k]);
    box_faces(&a, aid, TP_B, CL_B, 6, NULL);
    finish_mesh(&a, NS_MF_WELDED, 0, 0x1000u * (M_ARM + 1));
}

static void build_spike(void) {
    MB b; memset(&b, 0, sizeof b);
    int v0 = mb_v(&b, -500, 0, -500, NSG_NO_WELD_C), v1 = mb_v(&b, 500, 0, -500, NSG_NO_WELD_C), v2 = mb_v(&b, 500, 0, 500, NSG_NO_WELD_C),
        v3 = mb_v(&b, -500, 0, 500, NSG_NO_WELD_C), v4 = mb_v(&b, 0, 1400, 0, NSG_NO_WELD_C);
    int nr[4] = {mb_n(&b, 0, 1, 0), mb_n(&b, 0, 1, 0), mb_n(&b, 0, 1, 0), mb_n(&b, 0, 1, 0)};
    uint32_t c0 = pk(250, 40, 40), c1 = pk(40, 250, 40), c2 = pk(40, 40, 250), c3 = pk(250, 250, 40), c4 = pk(255, 255, 255);
    int s[4][3] = {{v0, v1, v4}, {v1, v2, v4}, {v2, v3, v4}, {v3, v0, v4}};
    uint32_t cs[5] = {c0, c1, c2, c3, c4};
    for (int f = 0; f < 4; f++) {
        int v[4] = {s[f][0], s[f][1], s[f][2], s[f][2]};
        uint32_t pc[4] = {cs[f], cs[(f + 1) % 4], cs[4], cs[4]};
        mb_f(&b, v, nr, UVQ, 0, 0, 0, 1, pc);
    }
    int vb[4] = {v0, v3, v2, v1};
    uint32_t pcb[4] = {c0, c3, c2, c1};
    mb_f(&b, vb, nr, UVQ, 0, 0, 0, 0, pcb);
    finish_mesh(&b, NS_MF_BAKED | NS_MF_UNLIT, 0, 0x1000u * (M_SPIKE + 1));
}

static void quad_at(MB *b, int x0, int y0, int x1, int y1, int z, uint16_t tpage, uint16_t texid, uint32_t col) {
    int v[4] = {mb_v(b, x0, y1, z, NSG_NO_WELD_C), mb_v(b, x1, y1, z, NSG_NO_WELD_C), mb_v(b, x1, y0, z, NSG_NO_WELD_C), mb_v(b, x0, y0, z, NSG_NO_WELD_C)};
    int n0 = mb_n(b, 0, 0, -1);
    int nr[4] = {n0, n0, n0, n0};
    uint32_t pc[4] = {col, col, col, col};
    mb_f(b, v, nr, UVQ, tpage, 0, texid, 0, pc);
}

static void build_semi(void) {
    /* A: cuatro cuadriláteros sin textura, uno por modo de mezcla, a distintas profundidades */
    MB a; memset(&a, 0, sizeof a);
    quad_at(&a, -300, 700, 1300, 2000, 3200, (uint16_t)(0 << 5), 0, pk(200, 70, 70));      /* modo 0 (50 %), el más lejano */
    quad_at(&a, 100, 900, 1700, 2300, 2300, (uint16_t)(2 << 5), 0, pk(60, 70, 40));        /* modo 2 (resta) */
    quad_at(&a, -700, 500, 800, 1800, 1700, (uint16_t)(3 << 5), 0, pk(60, 200, 255));      /* modo 3 (+25 %) */
    quad_at(&a, 700, 1000, 1900, 1900, 1200, (uint16_t)(1 << 5), 0, pk(50, 60, 110));      /* modo 1 (aditivo), el más cercano */
    finish_mesh(&a, NS_MF_BLEND | NS_MF_BAKED, 0, 0x1000u * (M_SEMI_A + 1));
    /* B: dos texturizados (página D, texels con bit15 la mitad) intercalados en profundidad con los de A */
    MB b; memset(&b, 0, sizeof b);
    quad_at(&b, -100, 600, 1500, 2100, 2700, (uint16_t)(TP_D | (1 << 5)), 7, pk(128, 128, 128));   /* aditivo */
    quad_at(&b, -500, 800, 600, 1900, 1450, (uint16_t)(TP_D | (0 << 5)), 8, pk(160, 150, 140));    /* 50 % */
    finish_mesh(&b, NS_MF_BLEND | NS_MF_BAKED, 0, 0x1000u * (M_SEMI_B + 1));
}

/* Prueba del sesgo de orden (decals): un cuadrilátero base y otro coplanar con raise > 0 (se publica en otra escena, ns_synth_publish_decal). */
static void build_decal(void) {
    MB b; memset(&b, 0, sizeof b);
    int n0 = mb_n(&b, 0, 1, 0);
    int v[4] = {mb_v(&b, -1500, 0, -1500, NSG_NO_WELD_C), mb_v(&b, 1500, 0, -1500, NSG_NO_WELD_C), mb_v(&b, 1500, 0, 1500, NSG_NO_WELD_C), mb_v(&b, -1500, 0, 1500, NSG_NO_WELD_C)};
    int nr[4] = {n0, n0, n0, n0};
    uint32_t pc[4] = {pk(200, 40, 40), pk(200, 40, 40), pk(200, 40, 40), pk(200, 40, 40)};
    mb_f(&b, v, nr, UVQ, 0, 0, 0, 0, pc);
    finish_mesh(&b, NS_MF_BAKED, 0, 0x1000u * (M_DECAL_BASE + 1));
    MB d; memset(&d, 0, sizeof d);
    n0 = mb_n(&d, 0, 1, 0);
    int w[4] = {mb_v(&d, -500, 0, -500, NSG_NO_WELD_C), mb_v(&d, 500, 0, -500, NSG_NO_WELD_C), mb_v(&d, 500, 0, 500, NSG_NO_WELD_C), mb_v(&d, -500, 0, 500, NSG_NO_WELD_C)};
    int nr2[4] = {n0, n0, n0, n0};
    uint32_t pc2[4] = {pk(40, 200, 40), pk(40, 200, 40), pk(40, 200, 40), pk(40, 200, 40)};
    mb_f(&d, w, nr2, UVQ, 0, 0, 0, 0, pc2);
    finish_mesh(&d, NS_MF_BAKED | NS_MF_BIAS_UP, 250, 0x1000u * (M_DECAL + 1));
}

/* telón de fondo: caja grande sin textura detrás de la sala (lo que se ve por los texels transparentes de las paredes); así la escena no tiene huecos */
static void build_back(void) {
    MB b; memset(&b, 0, sizeof b);
    int nrm = mb_n(&b, 0, 1, 0);
    const int X = 9000, Y0 = -3000, Y1 = 9000;
    int P[8][3] = {{-X, Y0, -X}, {X, Y0, -X}, {X, Y0, X}, {-X, Y0, X}, {-X, Y1, -X}, {X, Y1, -X}, {X, Y1, X}, {-X, Y1, X}};
    int vid[8];
    for (int k = 0; k < 8; k++) vid[k] = mb_v(&b, P[k][0], P[k][1], P[k][2], NSG_NO_WELD_C);
    const int F[6][4] = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7}};
    for (int f = 0; f < 6; f++) {
        int v[4], nr[4] = {nrm, nrm, nrm, nrm};
        uint32_t pc[4];
        for (int k = 0; k < 4; k++) { v[k] = vid[F[f][k]]; pc[k] = pk(40 + 30 * f + 10 * k, 60 + 10 * k, 90 + 20 * f); }
        mb_f(&b, v, nr, UVQ, 0, 0, 0, 0, pc);
    }
    finish_mesh(&b, NS_MF_BAKED, 0, 0x1000u * (M_BACK + 1));
}

/* ---------------------------------------------------------------------------------------------------- escena */
static NsXf xf_rot_y(float deg, float tx, float ty, float tz) {
    float a = deg * (float)M_PI / 180.0f, c = cosf(a), s = sinf(a);
    NsXf x = {{c, 0, s, tx, 0, 1, 0, ty, -s, 0, c, tz}};
    return x;
}
static NsXf xf_mul(const NsXf *a, const NsXf *b) {   /* a * b */
    NsXf o;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) o.m[4 * i + j] = a->m[4 * i] * b->m[j] + a->m[4 * i + 1] * b->m[4 + j] + a->m[4 * i + 2] * b->m[8 + j];
        o.m[4 * i + 3] = a->m[4 * i] * b->m[3] + a->m[4 * i + 1] * b->m[7] + a->m[4 * i + 2] * b->m[11] + a->m[4 * i + 3];
    }
    return o;
}
static NsXf xf_rot_z(float deg, float tx, float ty, float tz) {
    float a = deg * (float)M_PI / 180.0f, c = cosf(a), s = sinf(a);
    NsXf x = {{c, -s, 0, tx, s, c, 0, ty, 0, 0, 1, tz}};
    return x;
}

typedef struct {
    float to_vr[12];
    NsCamera cam;
    NsFog fog;
    NsLight global_light;
    uint32_t group_mask;
    float bg[3];
    uint32_t n_inst, n_piece, n_light, n_mesh;
} Hdr;

static NsInstance g_inst[64];
static NsPiece g_piece[128];
static NsLight g_light[8];
static Hdr g_hdr;

static void add_inst(uint16_t flags, int16_t light, const uint64_t *ids, const NsXf *xfs, const int16_t *parents, uint32_t np, uint16_t pflag0) {
    NsInstance *in = &g_inst[g_hdr.n_inst];
    memset(in, 0, sizeof *in);
    in->key = 0x2000u + g_hdr.n_inst * 0x100u;
    in->first_piece = g_hdr.n_piece; in->n_pieces = (uint16_t)np; in->flags = flags; in->light = light; in->vis_state = 2;
    for (uint32_t k = 0; k < np; k++) {
        NsPiece *p = &g_piece[g_hdr.n_piece++];
        p->mesh_id = ids[k]; p->world = xfs[k]; p->parent = parents ? parents[k] : -1; p->flags = (k == 0) ? pflag0 : 0;
        for (int q = 0; q < g_nbuilt; q++) if (g_built[q].m.id == ids[k] && (g_built[q].m.flags & NS_MF_BAKED)) p->baked_ver = 1;
    }
    g_hdr.n_inst++;
}

static int g_no_missing;   /* 1: la escena no incluye la instancia con malla ausente (el renderer puede declarar «mallas completas» y decidir nativo) */
void ns_synth_set_complete(int on) { g_no_missing = on != 0; }

static void build_scene(void) {
    memset(&g_hdr, 0, sizeof g_hdr);
    g_nbuilt = 0;
    build_floor(); build_room(); build_pillar(); build_sphere(); build_robot(); build_spike(); build_semi(); build_back(); build_decal();   /* M_TORSO y M_ARM ocupan dos entradas */
    /* mapeo: g_built[] en orden de construcción: floor, room, pillar, sphere, torso, arm, spike, semi_a, semi_b */
    const uint64_t idFloor = g_built[0].m.id, idRoom = g_built[1].m.id, idPillar = g_built[2].m.id, idSphere = g_built[3].m.id, idTorso = g_built[4].m.id,
                   idArm = g_built[5].m.id, idSpike = g_built[6].m.id, idSemiA = g_built[7].m.id, idSemiB = g_built[8].m.id, idBack = g_built[9].m.id;

    float tv[12] = {0.001f, 0, 0, 0, 0, 0.001f, 0, 0, 0, 0, -0.001f, 0};   /* mm -> m, espejo Z */
    memcpy(g_hdr.to_vr, tv, sizeof tv);
    g_hdr.cam.eye[0] = 300; g_hdr.cam.eye[1] = 1600; g_hdr.cam.eye[2] = -500;
    float rot[9] = {1, 0, 0, 0, -1, 0, 0, 0, 1};   /* vista (x der, y abajo, z delante) -> mundo: adelante = +Z, abajo = -Y */
    memcpy(g_hdr.cam.rot, rot, sizeof rot);
    g_hdr.cam.h = 320; g_hdr.cam.overscan_y = 58.0f / 64.0f; g_hdr.cam.vr_active = 1;
    g_hdr.fog.active = 1; g_hdr.fog.start = 3000; g_hdr.fog.len = 7000;
    /* luz global: principal oblicua cálida, una puntual azulada, ambiente */
    NsLight *L = &g_hdr.global_light;
    memset(L, 0, sizeof *L);
    L->dir[0][0] = 1500; L->dir[0][1] = 3200; L->dir[0][2] = -1800;
    L->dir[1][0] = -2200; L->dir[1][1] = -600; L->dir[1][2] = 1500;
    L->col[0][0] = 1600; L->col[1][0] = 1500; L->col[2][0] = 1300;
    L->col[0][1] = 0; L->col[1][1] = 800; L->col[2][1] = 1600;
    L->amb[0] = 40; L->amb[1] = 40; L->amb[2] = 50;
    g_light[0] = *L;                                   /* luz 0: ambiente propio para el robot */
    g_light[0].amb[0] = 90; g_light[0].amb[1] = 20; g_light[0].amb[2] = 20; g_light[0].flags = 2;
    g_light[0].dir[0][0] = -1000; g_light[0].dir[0][1] = 3000; g_light[0].dir[0][2] = 2000;
    g_hdr.n_light = 1;

    uint64_t id1[1];
    NsXf x1[1];
    id1[0] = idFloor; x1[0] = xf_rot_y(0, 0, 0, 0); add_inst(NS_IF_STATIC, -1, id1, x1, NULL, 1, 0);
    id1[0] = idRoom; add_inst(NS_IF_STATIC, -1, id1, x1, NULL, 1, 0);
    id1[0] = idBack; add_inst(NS_IF_STATIC, -1, id1, x1, NULL, 1, 0);
    const float PX[5] = {-3000, 3500, 3000, -3500, 300}, PZ[5] = {3000, 2500, -2000, -2500, 4200};
    for (int i = 0; i < 5; i++) { id1[0] = idPillar; x1[0] = xf_rot_y(20.0f * (float)i, PX[i], 0, PZ[i]); add_inst(NS_IF_LIT, -1, id1, x1, NULL, 1, 0); }
    id1[0] = idSphere; x1[0] = xf_rot_y(30, -1200, 1500, 2400); add_inst(NS_IF_LIT, -1, id1, x1, NULL, 1, 0);
    /* robot: torso (pieza 0) y brazo (pieza 1, padre 0) con vértices welded */
    {
        uint64_t ids[2] = {idTorso, idArm};
        NsXf torso = xf_rot_y(-25, 1800, 900, 1500);
        NsXf jointLocal = xf_rot_z(35, 150, 250, 0);
        NsXf arm = xf_mul(&torso, &jointLocal);
        NsXf xs[2] = {torso, arm};
        int16_t par[2] = {-1, 0};
        add_inst(NS_IF_LIT | NS_IF_OWN_AMBIENT, 0, ids, xs, par, 2, 0);
    }
    id1[0] = idSpike; x1[0] = xf_rot_y(10, -2500, 0, 3500); add_inst(0, -1, id1, x1, NULL, 1, 0);
    /* semitransparentes: dos piezas (mallas A y B) en una misma instancia para ordenar entre piezas */
    {
        uint64_t ids[2] = {idSemiA, idSemiB};
        NsXf xs[2] = {xf_rot_y(0, 0, 0, 0), xf_rot_y(0, 0, 0, 0)};
        add_inst(NS_IF_BLEND, -1, ids, xs, NULL, 2, 0);
    }
    /* malla ausente del almacén y pieza oculta: no se dibujan */
    if (!g_no_missing) { id1[0] = 0xDEADBEEF00000001ull; x1[0] = xf_rot_y(0, 0, 0, 0); add_inst(NS_IF_LIT, -1, id1, x1, NULL, 1, 0); }
    id1[0] = idPillar; x1[0] = xf_rot_y(0, 300, 0, 1000); add_inst(NS_IF_LIT, -1, id1, x1, NULL, 1, NS_PF_HIDDEN);
    g_hdr.n_mesh = (uint32_t)g_nbuilt;
}

int ns_synth_write(const char *path) {
    build_scene();
    make_vram(g_vram);
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fwrite("NSY1", 1, 4, f);
    fwrite(&g_hdr, sizeof g_hdr, 1, f);
    fwrite(g_inst, sizeof(NsInstance), g_hdr.n_inst, f);
    fwrite(g_piece, sizeof(NsPiece), g_hdr.n_piece, f);
    fwrite(g_light, sizeof(NsLight), g_hdr.n_light, f);
    for (uint32_t i = 0; i < g_hdr.n_mesh; i++) {
        const NsMesh *m = &g_built[i].m;
        fwrite(&m->id, 8, 1, f); fwrite(&m->flags, 4, 1, f); fwrite(&m->depth_bias, 2, 1, f);
        fwrite(&m->n_verts, 2, 1, f); fwrite(&m->n_norms, 2, 1, f); fwrite(&m->n_faces, 2, 1, f);
        fwrite(m->verts, sizeof(NsVert), m->n_verts, f);
        fwrite(m->norms, sizeof(NsNorm), m->n_norms, f);
        fwrite(m->faces, sizeof(NsFace), m->n_faces, f);
        fwrite(g_built[i].b->paint, 4, (size_t)m->n_faces * 4, f);
    }
    fwrite(g_vram, 2, 1024 * 512, f);
    return fclose(f) == 0 ? 0 : -1;
}

/* ---------------------------------------------------------------------------------------------------- carga y almacén */
static NsMesh g_lm[32];
static uint32_t g_nlm;
static uint16_t g_lvram[1024 * 512];
static int g_loaded;
static NsBridge *g_br;
NsBridge *ns_synth_bridge(void) { return g_br; }
static float g_head[3] = {0.3f, 1.6f, 0.5f};

static NsBaked g_lb[32];
static uint32_t g_epoch = 1, g_acked;
const NsMesh *ns_store_get(const NsMeshStore *st, uint64_t id) {
    (void)st;
    for (uint32_t i = 0; i < g_nlm; i++) if (g_lm[i].id == id) return &g_lm[i];
    return NULL;
}
const NsBaked *ns_store_get_baked(const NsMeshStore *st, uint64_t mesh_id, uint32_t ver) {
    (void)st;
    for (uint32_t i = 0; i < g_nlm; i++) if (g_lm[i].id == mesh_id && g_lb[i].rgba && ver == g_lb[i].ver) return &g_lb[i];
    return NULL;
}
uint32_t ns_store_epoch(const NsMeshStore *st) { (void)st; return g_epoch; }
void ns_store_ack(NsMeshStore *st, uint32_t epoch) { (void)st; g_acked = epoch; }
uint32_t ns_synth_acked(void) { return g_acked; }

#define RD(p, n, sz) do { if (fread((p), (sz), (n), f) != (size_t)(n)) { fclose(f); return -2; } } while (0)

static NsInstance li[64]; static NsPiece lp[128]; static NsLight ll[8];
static Hdr g_lh;

/* Publica la escena cargada (cabecera `h`, listas li/lp/ll) por el puente g_br. */
static int publish_loaded(const Hdr *h) {
    NsScene *sc = ns_bridge_begin(g_br);
    if (!sc) return -6;
    g_epoch = 1; sc->game_frame = 1; sc->mesh_epoch = 1; sc->valid = NS_SV_GAME | NS_SV_LISTS_OK | NS_SV_MAP_OK | NS_SV_TOVR_OK | NS_SV_PLAYER_OK; sc->units_per_m = 1000;
    memcpy(sc->to_vr, h->to_vr, sizeof h->to_vr);
    sc->cam = h->cam; sc->fog = h->fog; sc->global_light = h->global_light; sc->group_mask = h->group_mask;
    memcpy(sc->bg_rgb, h->bg, sizeof h->bg);
    memcpy(sc->inst, li, sizeof(NsInstance) * h->n_inst); sc->n_inst = h->n_inst;
    memcpy(sc->piece, lp, sizeof(NsPiece) * h->n_piece); sc->n_piece = h->n_piece;
    memcpy(sc->lights, ll, sizeof(NsLight) * h->n_light); sc->n_light = h->n_light;
    ns_bridge_publish(g_br, (NsMeshStore *)&g_nlm);
    return 0;
}
int ns_synth_republish(void) { return (g_loaded && g_br) ? publish_loaded(&g_lh) : -1; }

int ns_synth_load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    char mg[4];
    RD(mg, 4, 1);
    if (memcmp(mg, "NSY1", 4)) { fclose(f); return -3; }
    Hdr h;
    RD(&h, 1, sizeof h);
    if (h.n_inst > 64 || h.n_piece > 128 || h.n_light > 8 || h.n_mesh > 32) { fclose(f); return -4; }
    RD(li, h.n_inst, sizeof(NsInstance));
    RD(lp, h.n_piece, sizeof(NsPiece));
    RD(ll, h.n_light, sizeof(NsLight));
    g_nlm = 0;
    for (uint32_t i = 0; i < h.n_mesh; i++) {
        NsMesh *m = &g_lm[g_nlm];
        memset(m, 0, sizeof *m);
        RD(&m->id, 1, 8); RD(&m->flags, 1, 4); RD(&m->depth_bias, 1, 2); m->epoch = 1;
        RD(&m->n_verts, 1, 2); RD(&m->n_norms, 1, 2); RD(&m->n_faces, 1, 2);
        NsVert *v = (NsVert *)malloc(sizeof(NsVert) * (m->n_verts + 1u));
        NsNorm *n = (NsNorm *)malloc(sizeof(NsNorm) * (m->n_norms + 1u));
        NsFace *fc = (NsFace *)malloc(sizeof(NsFace) * (m->n_faces + 1u));
        uint32_t *pt = (uint32_t *)malloc(4u * 4u * (m->n_faces + 1u));
        RD(v, m->n_verts, sizeof(NsVert)); RD(n, m->n_norms, sizeof(NsNorm)); RD(fc, m->n_faces, sizeof(NsFace)); RD(pt, (size_t)m->n_faces * 4, 4);
        m->verts = v; m->norms = n; m->faces = fc;
        if (ns_mesh_content_hash(m) != m->id) { fclose(f); return -7; }
        memset(&g_lb[g_nlm], 0, sizeof g_lb[0]);
        if (m->flags & NS_MF_BAKED) { g_lb[g_nlm].mesh_id = m->id; g_lb[g_nlm].ver = 1; g_lb[g_nlm].n_colors = 4u * m->n_faces; g_lb[g_nlm].rgba = pt; }
        g_nlm++;
    }
    RD(g_lvram, 1024 * 512, 2);
    fclose(f);

    if (g_br) ns_bridge_destroy(g_br);
    g_br = ns_bridge_create(NULL);
    if (!g_br) return -5;
    g_lh = h;
    { int rc = publish_loaded(&g_lh); if (rc) return rc; }
    g_loaded = 1;
    return 0;
}

const uint16_t *ns_synth_vram(void) { return g_loaded ? g_lvram : NULL; }
void ns_synth_head(float pos[3]) { memcpy(pos, g_head, sizeof g_head); }

/* ---------------------------------------------------------------------------------------------------- adaptador a la referencia */
static void xf_pt(const NsXf *x, const float v[3], float o[3]) { ns_xf_apply(x, v, o); }
static void to_vr_pt(const float m[12], const float p[3], float o[3]) {
    o[0] = m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3];
    o[1] = m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7];
    o[2] = m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11];
}

typedef struct { NsRTri *t; size_t n, cap; } TriList;
static void tl_push(TriList *l, const NsRTri *t) {
    if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 1024; l->t = (NsRTri *)realloc(l->t, l->cap * sizeof(NsRTri)); }
    l->t[l->n++] = *t;
}

static const float FACE[6][9] = {
    {1, 0, 0, 0, 1, 0, 0, 0, 1}, {0, 0, -1, 0, 1, 0, 1, 0, 0}, {-1, 0, 0, 0, 1, 0, 0, 0, -1},
    {0, 0, 1, 0, 1, 0, -1, 0, 0}, {1, 0, 0, 0, 0, 1, 0, -1, 0}, {1, 0, 0, 0, 0, -1, 0, 1, 0},
};
const float *ns_synth_face_basis(int f) { return FACE[f]; }
static const float POSE[9] = {1, 0, 0, 0, -1, 0, 0, 0, -1};   /* derecha, abajo, delante en el mundo VR (mira a -Z) */
const float *ns_synth_pose_basis(void) { return POSE; }

static void adapt(const NsScene *sc, TriList *out) {
    for (uint32_t ii = 0; ii < sc->n_inst; ii++) {
        const NsInstance *in = &sc->inst[ii];
        const NsLight *Lsel = (in->light >= 0 && (uint32_t)in->light < sc->n_light) ? &sc->lights[in->light] : &sc->global_light;
        NsLight Leff = *Lsel;
        if (!(in->flags & NS_IF_OWN_AMBIENT)) memcpy(Leff.amb, sc->global_light.amb, sizeof Leff.amb);
        /* niebla del objeto: profundidad de la pieza raíz */
        float objs = 1.0f;
        if (sc->fog.active) {
            const NsPiece *r = &sc->piece[in->first_piece];
            float rp[3] = {r->world.m[3], r->world.m[7], r->world.m[11]};
            float z = 0;
            for (int k = 0; k < 3; k++) z += sc->cam.rot[3 * k + 2] * (rp[k] - sc->cam.eye[k]);
            float f = (z - sc->fog.start) / sc->fog.len;
            f = f < 0 ? 0 : f > 1 ? 1 : f;
            objs = 1.0f - f;
        }
        for (uint32_t pi = 0; pi < in->n_pieces; pi++) {
            const NsPiece *p = &sc->piece[in->first_piece + pi];
            if (p->flags & NS_PF_HIDDEN) continue;
            const NsMesh *m = ns_store_get(NULL, p->mesh_id);
            if (!m) continue;
            const NsMesh *pm = NULL; const NsPiece *pp = NULL;
            if (p->parent >= 0 && (uint32_t)p->parent < in->n_pieces) { pp = &sc->piece[in->first_piece + (uint32_t)p->parent]; pm = ns_store_get(NULL, pp->mesh_id); }
            const float rw[9] = {p->world.m[0], p->world.m[1], p->world.m[2], p->world.m[4], p->world.m[5], p->world.m[6], p->world.m[8], p->world.m[9], p->world.m[10]};
            /* posiciones VR de los vértices */
            float pos[128][3];
            float wpos[128][3];
            for (uint32_t k = 0; k < m->n_verts; k++) {
                float lv[3] = {(float)m->verts[k].x, (float)m->verts[k].y, (float)m->verts[k].z}, w[3];
                if (m->verts[k].weld != 0xFF && pp && pm && m->verts[k].weld < pm->n_verts) {
                    float pv[3] = {(float)pm->verts[m->verts[k].weld].x, (float)pm->verts[m->verts[k].weld].y, (float)pm->verts[m->verts[k].weld].z};
                    xf_pt(&pp->world, pv, w);
                } else xf_pt(&p->world, lv, w);
                memcpy(wpos[k], w, sizeof w);
                to_vr_pt(sc->to_vr, w, pos[k]);
            }
            const NsBaked *bk = (p->baked_ver && (m->flags & NS_MF_BAKED)) ? ns_store_get_baked(NULL, p->mesh_id, p->baked_ver) : NULL;
            const int shade = (in->flags & NS_IF_LIT) && !(m->flags & NS_MF_UNLIT);
            for (uint32_t f = 0; f < m->n_faces; f++) {
                const NsFace *fc = &m->faces[f];
                NsRVert rv[4];
                for (int k = 0; k < 4; k++) {
                    uint32_t vi = fc->v[k], ni = fc->n[k];
                    if (vi >= m->n_verts) vi = 0;
                    memset(&rv[k], 0, sizeof rv[k]);
                    memcpy(rv[k].p, pos[vi], sizeof rv[k].p);
                    rv[k].u = fc->u[k]; rv[k].v = fc->t[k];
                    float raw[3];
                    if (bk && (fc->flags & NS_FF_BAKED_OK) && 4u * f + (uint32_t)k < bk->n_colors) {
                        uint32_t c = bk->rgba[4u * f + (uint32_t)k];
                        raw[0] = (float)(c & 255u); raw[1] = (float)((c >> 8) & 255u); raw[2] = (float)((c >> 16) & 255u);
                    } else if (shade) {
                        float n[3] = {0, 0, 0};
                        if (ni < m->n_norms) { n[0] = m->norms[ni].x / 4096.0f; n[1] = m->norms[ni].y / 4096.0f; n[2] = m->norms[ni].z / 4096.0f; }
                        ns_shade_normal(&Leff, rw, n, raw);
                    } else raw[0] = raw[1] = raw[2] = 128.0f;
                    float s = objs;
                    if (sc->fog.active && (in->flags & NS_IF_STATIC)) {
                        float z = 0;
                        for (int a = 0; a < 3; a++) z += sc->cam.rot[3 * a + 2] * (wpos[vi][a] - sc->cam.eye[a]);
                        float ff = (z - sc->fog.start) / sc->fog.len;
                        ff = ff < 0 ? 0 : ff > 1 ? 1 : ff;
                        s = 1.0f - ff;
                    }
                    float c3[3];
                    for (int a = 0; a < 3; a++) { float c = floorf(raw[a] * s); c3[a] = c < 0 ? 0 : c > 255 ? 255 : c; }
                    rv[k].r = (uint8_t)c3[0]; rv[k].g = (uint8_t)c3[1]; rv[k].b = (uint8_t)c3[2];
                }
                static const int idxq[2][3] = {{0, 1, 3}, {1, 2, 3}};   /* diagonal B-D: la del hardware (igual que ns_gpu_pack) */
                static const int idxt[1][3] = {{0, 1, 2}};
                const int (*idx)[3] = (fc->flags & NS_FF_TRI) ? idxt : idxq;
                const int ntri = (fc->flags & NS_FF_TRI) ? 1 : 2;
                for (int t = 0; t < ntri; t++) {
                    NsRTri tr;
                    memset(&tr, 0, sizeof tr);
                    for (int k = 0; k < 3; k++) tr.v[k] = rv[idx[t][k]];
                    tr.tpage = fc->tpage; tr.clut = fc->clut;
                    tr.flags = (uint8_t)((fc->texid ? NS_TF_TEXTURED : 0) | ((m->flags & NS_MF_BLEND) ? NS_TF_SEMI : 0));
                    tr.abr = (uint8_t)((fc->tpage >> 5) & 3u);
                    tl_push(out, &tr);
                }
            }
        }
    }
}

size_t ns_synth_ref_tris(NsRTri **out) {
    NsView view; memset(&view, 0, sizeof view);
    const NsScene *sc = (g_br && ns_bridge_acquire(g_br, ns_now_ns(), &view)) ? view.cur : NULL;
    *out = NULL;
    if (!sc) return 0;
    TriList tl; memset(&tl, 0, sizeof tl);
    adapt(sc, &tl);
    *out = tl.t;
    return tl.n;
}

/* Escena de prueba de decals (otra publicación con mesh_epoch distinto: obliga al renderer a vaciar y resubir su caché). */
int ns_synth_publish_decal(uint32_t epoch) {
    if (!g_loaded) return -1;
    uint64_t idBase = 0, idDecal = 0;
    for (uint32_t i = 0; i < g_nlm; i++) {
        if (i == M_DECAL_BASE) idBase = g_lm[i].id;
        if (i == M_DECAL) idDecal = g_lm[i].id;
    }
    if (!idBase || !idDecal) return -2;
    NsScene *sc = ns_bridge_begin(g_br);
    if (!sc) return -3;
    g_epoch = epoch; sc->game_frame = 2; sc->mesh_epoch = epoch; sc->valid = NS_SV_GAME | NS_SV_LISTS_OK | NS_SV_MAP_OK | NS_SV_TOVR_OK | NS_SV_PLAYER_OK; sc->units_per_m = 1000;
    float tv[12] = {0.001f, 0, 0, 0, 0, 0.001f, 0, 0, 0, 0, -0.001f, 0};
    memcpy(sc->to_vr, tv, sizeof tv);
    sc->cam.rot[0] = 1; sc->cam.rot[4] = -1; sc->cam.rot[8] = 1; sc->cam.h = 320;
    uint64_t ids[2] = {idBase, idDecal};
    NsXf id = xf_rot_y(0, 0, 0, 0);
    for (int k = 0; k < 2; k++) {
        NsInstance *in = &sc->inst[sc->n_inst++];
        memset(in, 0, sizeof *in);
        in->key = 0x100u + (uint32_t)k; in->first_piece = sc->n_piece; in->n_pieces = 1; in->flags = NS_IF_STATIC; in->light = -1;
        NsPiece *p = &sc->piece[sc->n_piece++];
        p->mesh_id = ids[k]; p->world = id; p->parent = -1; p->baked_ver = 1;
    }
    ns_bridge_publish(g_br, (NsMeshStore *)&g_nlm);
    return 0;
}

long ns_synth_ref_cube(int n, float znear, NsRBuf faces[6], NsRStats *st) {
    NsView view; memset(&view, 0, sizeof view);
    const NsScene *sc = (g_br && ns_bridge_acquire(g_br, ns_now_ns(), &view)) ? view.cur : NULL;
    if (!sc) return -1;
    TriList tl; memset(&tl, 0, sizeof tl);
    adapt(sc, &tl);
    NsRScene rs = {tl.t, tl.n, g_lvram};
    NsRCam pose;
    ns_cam_basis(&pose, g_head, &POSE[0], &POSE[3], &POSE[6], 90.0f, n, n);
    for (int f = 0; f < 6; f++) if (ns_buf_alloc(&faces[f], n, n)) { free(tl.t); return -2; }
    ns_render_cube(&rs, &pose, n, znear, NS_O_FLOAT, faces, st);
    long cnt = (long)tl.n;
    free(tl.t);
    return cnt;
}
