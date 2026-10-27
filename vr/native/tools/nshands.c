/* nshands: vista previa de las manos nuevas con la tubería REAL de la app sobre un volcado (ram.bin, vram.bin, scratch.bin): IK de vr_body sobre la RAM (mando sintético), extractor
 * nativo (ns_extract), jugador (ns_player_bind), manos propias (ns_hand_apply), adaptador (ns_adapt, con texturas de la VRAM y la luz del jugador) y rasterizador de referencia
 * (ns_cpuref). Añade una pistola primitiva (la de test_grip.c) colgada de la mano por el zócalo de vr_grip. Herramienta de escritorio: NO entra en la app.
 *   nshands <dir_volcado> <dir_salida>
 * Escribe hands_<escena>.png: tres vistas (primera persona, lateral, de cerca sobre la costura de la muñeca). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../ns_extract.h"
#include "../ns_adapt.h"
#include "../ns_cpuref.h"
#include "../ns_player.h"
#include "../ns_hand.h"
#include "../ns_wphide.h"
#include "vr_body.h"
#include "vr_pose.h"
#include "vr_grip.h"
#include "vr_hand_skin.h"

static uint8_t RAM[0x200000], RAM0[0x200000], SCR[1024];
static uint16_t VRAM[1024 * 512];
static int load(const char *dir, const char *name, void *d, size_t n) {
    char p[512]; snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "rb"); if (!f) return 0;
    size_t r = fread(d, 1, n, f); fclose(f); return r == n;
}
static uint32_t r32(uint32_t a) { a &= 0x1FFFFF; return RAM[a] | RAM[a + 1] << 8 | RAM[a + 2] << 16 | (uint32_t)RAM[a + 3] << 24; }
static int16_t r16(uint32_t a) { a &= 0x1FFFFF; return (int16_t)(RAM[a] | RAM[a + 1] << 8); }
static void piece_xf(int k, float R[9], float t[3]) {   /* matriz de mundo de la pieza k del jugador */
    uint32_t a = r32(r32(0x800ACBB0u)) + 0x48 + 0x5C * (uint32_t)k;
    for (int i = 0; i < 9; i++) R[i] = r16(a + 2 * (uint32_t)i) / 4096.0f;
    for (int i = 0; i < 3; i++) t[i] = (float)(int32_t)r32(a + 0x14 + 4 * (uint32_t)i);
}

/* vista VR sintética: la cabeza del usuario en p_ref = (0, 1.6, 0) es el ojo del motor (cabeza del jugador); el usuario mira hacia donde mira el torso */
static VrBodyView V;
static int view_fn(VrBodyView *o) { *o = V; return 1; }
static VrHandPose HANDS[2];
int vrpose_get_hands(VrHandPose out[2]) { out[0] = HANDS[0]; out[1] = HANDS[1]; return HANDS[0].valid + HANDS[1].valid; }

static void qmul(float *o, const float *a, const float *b) {
    float r[4] = {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1], a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
                  a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3], a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
    memcpy(o, r, sizeof r);
}
static void qaxis(float *q, char ax, float deg) { float h = deg * 0.00872665f; q[0] = ax == 'x' ? sinf(h) : 0; q[1] = ax == 'y' ? sinf(h) : 0; q[2] = ax == 'z' ? sinf(h) : 0; q[3] = cosf(h); }

/* ---- primitivas (pistola) como triángulos sin textura con sombreado plano ---- */
static NsRTri *XT; static size_t NXT, CXT;
static void push_tri(const float a[3], const float b[3], const float c[3], int r, int g, int bl) {
    if (NXT == CXT) { CXT = CXT ? 2 * CXT : 256; XT = realloc(XT, CXT * sizeof *XT); }
    float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]}, l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    float sh = l > 0 ? 0.55f + 0.45f * fabsf(0.3f * n[0] + 0.85f * n[1] + 0.4f * n[2]) / l : 1.0f;
    NsRTri *t = &XT[NXT++];
    memset(t, 0, sizeof *t);
    const float *p[3] = {a, b, c};
    for (int k = 0; k < 3; k++) { memcpy(t->v[k].p, p[k], 12); t->v[k].r = (uint8_t)(r * sh); t->v[k].g = (uint8_t)(g * sh); t->v[k].b = (uint8_t)(bl * sh); }
}
static void box(const float M[12], const float lo[3], const float hi[3], int r, int g, int b) {   /* caja (mm del modelo) con M modelo -> mundo */
    float P[8][3];
    for (int i = 0; i < 8; i++) {
        float q[3] = {(i & 1) ? hi[0] : lo[0], (i & 2) ? hi[1] : lo[1], (i & 4) ? hi[2] : lo[2]};
        for (int a = 0; a < 3; a++) P[i][a] = M[4 * a] * q[0] + M[4 * a + 1] * q[1] + M[4 * a + 2] * q[2] + M[4 * a + 3];
    }
    static const int F[6][4] = {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
    for (int f = 0; f < 6; f++) { push_tri(P[F[f][0]], P[F[f][1]], P[F[f][2]], r, g, b); push_tri(P[F[f][0]], P[F[f][2]], P[F[f][3]], r, g, b); }
}
static void wpose_to_m(WPose p, float M[12], float tscale) {
    float x = p.q.x, y = p.q.y, z = p.q.z, w = p.q.w, n = sqrtf(x * x + y * y + z * z + w * w); x /= n; y /= n; z /= n; w /= n;
    float r[12] = {1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y), p.p.x * tscale, 2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x), p.p.y * tscale,
                   2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y), p.p.z * tscale};
    memcpy(M, r, sizeof r);
}
static void m_mul(float *o, const float *a, const float *b) {   /* 3x4 */
    float r[12];
    for (int i = 0; i < 3; i++) { for (int j = 0; j < 3; j++) r[4 * i + j] = a[4 * i] * b[j] + a[4 * i + 1] * b[4 + j] + a[4 * i + 2] * b[8 + j]; r[4 * i + 3] = a[4 * i] * b[3] + a[4 * i + 1] * b[7] + a[4 * i + 2] * b[11] + a[4 * i + 3]; }
    memcpy(o, r, sizeof r);
}
static void m_inv(float *o, const float *a) {   /* rígida */
    float r[12];
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) r[4 * i + j] = a[4 * j + i];
    for (int i = 0; i < 3; i++) r[4 * i + 3] = -(r[4 * i] * a[3] + r[4 * i + 1] * a[7] + r[4 * i + 2] * a[11]);
    memcpy(o, r, sizeof r);
}

static void cam_look(NsRCam *c, const float eye[3], const float tg[3], float fov, int w, int h) {
    float f[3] = {tg[0] - eye[0], tg[1] - eye[1], tg[2] - eye[2]}, l = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    for (int i = 0; i < 3; i++) f[i] /= l;
    float up[3] = {0, 1, 0};
    if (fabsf(f[1]) > 0.95f) { up[1] = 0; up[2] = 1; }
    float r[3] = {f[1] * up[2] - f[2] * up[1], f[2] * up[0] - f[0] * up[2], f[0] * up[1] - f[1] * up[0]}, rl = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    for (int i = 0; i < 3; i++) r[i] /= rl;
    float u[3] = {r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]}, d[3] = {-u[0], -u[1], -u[2]};
    ns_cam_basis(c, eye, r, d, f, fov, w, h);
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "uso: nshands <dir_volcado> <dir_salida>\n"); return 1; }
    if (!load(argv[1], "ram.bin", RAM0, sizeof RAM0) || !load(argv[1], "vram.bin", VRAM, sizeof VRAM)) { fprintf(stderr, "volcado ilegible\n"); return 1; }
    load(argv[1], "scratch.bin", SCR, sizeof SCR);
    memcpy(RAM, RAM0, sizeof RAM);
    float Rt[9], tt[3], Rh[9], th[3];
    piece_xf(1, Rt, tt); piece_xf(6, Rh, th);
    /* vista: ojo = cabeza del jugador; el usuario mira (-Z) hacia el frente del torso (+Z local) */
    float fz[3] = {Rt[2], Rt[5], Rt[8]};
    float a = atan2f(-fz[0], -fz[2]) * 57.29578f;
    memset(&V, 0, sizeof V);
    V.active = 1; V.eye[0] = th[0]; V.eye[1] = th[1] + 60.0f; V.eye[2] = th[2];
    V.p_ref[1] = 1.6f; V.theta_deg = a - 180.0f; V.units_per_m = 1000.0f; V.gaze_yaw_deg = atan2f(fz[0], fz[2]) * 57.29578f;
    struct { const char *name; float p[3]; float yaw, pitch, roll; int pose; } SC[6] = {
        {"neutra", {0.16f, 1.22f, -0.40f}, 0, 0, 90, HP_RELAX},
        {"giro_mas90", {0.16f, 1.22f, -0.40f}, 0, 0, 180, HP_RELAX},
        {"giro_menos90", {0.16f, 1.22f, -0.40f}, 0, 0, 0, HP_RELAX},
        {"flexion", {0.16f, 1.22f, -0.40f}, 0, -60, 90, HP_RELAX},
        {"extension", {0.16f, 1.22f, -0.40f}, 0, 50, 90, HP_RELAX},
        {"pistola", {0.16f, 1.25f, -0.42f}, 0, 0, 90, HP_GRIP_PISTOL},
    };
    for (int sc_i = 0; sc_i < 6; sc_i++) {
        memcpy(RAM, RAM0, sizeof RAM);
        VrBodyConfig cfg; vrbody_config_default(&cfg); cfg.torso_follow = 0; cfg.near_hide_m = 0; cfg.near_hide_vtx_m = 0;
        vrbody_init(&cfg); vrbody_set_view_fn(view_fn); vrbody_set_fist_suppressed(1); vrbody_set_armed(SC[sc_i].pose == HP_GRIP_PISTOL);
        /* agarre del mando: deshace el desvío agarre -> mano por defecto (cabeceo -35, alabeo -45 en la derecha) para que la mano quede como apretón de manos, y luego gira la escena */
        float q0[4], q1[4], qy[4], qp[4], qr[4], q[4];
        qaxis(q0, 'z', 45.0f); qaxis(q1, 'x', 35.0f); qmul(q, q0, q1);
        qaxis(qy, 'y', SC[sc_i].yaw); qaxis(qp, 'x', SC[sc_i].pitch); qaxis(qr, 'z', SC[sc_i].roll);
        float t1[4], t2[4]; qmul(t1, qy, qp); qmul(t2, t1, qr); qmul(q, t2, q);
        memset(HANDS, 0, sizeof HANDS);
        HANDS[1].valid = 1; memcpy(HANDS[1].pos, SC[sc_i].p, 12); memcpy(HANDS[1].rot, q, 16);
        HANDS[0].valid = 1; HANDS[0].pos[0] = -0.22f; HANDS[0].pos[1] = 1.05f; HANDS[0].pos[2] = -0.25f; HANDS[0].rot[3] = 1;
        int driven = 0;
        for (int f = 0; f < 60; f++) { memcpy(RAM, RAM0, sizeof RAM); vrbody_set_dt(0.04f); driven = vrbody_apply(RAM); if (f < 59) vrbody_restore(RAM); }   /* fundido completo */
        NsScene S; NsMeshStore *st = ns_store_create();
        ns_scene_alloc_exact(&S, 512, 8192, 256, 2048);
        NsExtractOpts eo; memset(&eo, 0, sizeof eo); eo.player_key = ns_player_key(RAM);
        NsExtractStats es;
        ns_extract_ex(&S, st, RAM, sizeof RAM, SCR, sizeof SCR, &eo, &es);
        NsPlayerMap map; memset(&map, 0, sizeof map);
        int bound = ns_player_bind(&map, &S, RAM, eo.player_key);
        ns_player_hide_part(&map, &S, 6);   /* primera persona: sin cabeza (como la app) */
        ns_wphide_apply(&S, RAM, 1);        /* el arma del juego en la mano de Snake se oculta (como la app) */
        if (sc_i == 0 && getenv("NSHANDS_TEX")) {   /* diagnóstico: texturas de la manopla por rol (luminancia de cada texel del rectángulo) */
            const NsMesh *gm = ns_store_get(st, S.piece[map.part[NS_PART_HAND_R].idx[0]].mesh_id);
            NsHandTex tx[4]; int nf = ns_hand_classify(gm, 1, tx);
            for (int r = 0; r < 4; r++) {
                printf("rol %d ok %d tpage %04x clut %04x u %d..%d t %d..%d\n", r, tx[r].ok, tx[r].tpage, tx[r].clut, tx[r].u[0], tx[r].u[2], tx[r].t[0], tx[r].t[2]);
                if (!tx[r].ok) continue;
                int u0 = tx[r].u[0] < tx[r].u[2] ? tx[r].u[0] : tx[r].u[2], u1 = tx[r].u[0] < tx[r].u[2] ? tx[r].u[2] : tx[r].u[0], v0 = tx[r].t[0] < tx[r].t[2] ? tx[r].t[0] : tx[r].t[2], v1 = tx[r].t[0] < tx[r].t[2] ? tx[r].t[2] : tx[r].t[0];
                for (int v = v0; v <= v1; v++) { for (int u = u0; u <= u1; u++) { uint16_t c = ns_sample_texel(VRAM, tx[r].tpage, tx[r].clut, 0, u, v); int L = (c & 31) + ((c >> 5) & 31) + ((c >> 10) & 31); putchar(" .:-=+*#%@"[L * 10 / 94]); } putchar('\n'); }
            }
            (void)nf;
        }
        NsHandIn hin; memset(&hin, 0, sizeof hin);
        hin.enabled = 1; hin.driven[0] = hin.driven[1] = 1; hin.grip[1] = SC[sc_i].pose == HP_RELAX ? 0.0f : 1.0f; hin.item_pose[1] = SC[sc_i].pose; hin.item_pose[0] = HP_RELAX;
        ns_hand_reset();
        int nh = ns_hand_apply(&S, st, &map, &hin);
        NsHandStats hs; ns_hand_stats(&hs);
        /* mano derecha: su pieza 0 es el anillo de la costura si la hubo; la matriz de la mano es la de la pieza 1 */
        const NsPiece *hp = NULL; float W[12] = {0};
        for (uint32_t i = 0; i < S.n_inst; i++) if (S.inst[i].key == NS_HAND_KEY_R) hp = &S.piece[S.inst[i].first_piece + S.inst[i].n_pieces - 1];
        if (hp) memcpy(W, hp->world.m, sizeof W);
        printf("%s: brazos IK %d, jugador %d, manos %d, costuras %u\n", SC[sc_i].name, driven, bound, nh, hs.seams);
        {   const VrBodyDebug *d = vrbody_debug(1); float gc[3]; vrhs_grip_center(1, gc);
            float gw[3]; for (int i = 0; i < 3; i++) gw[i] = W[4 * i] * gc[0] + W[4 * i + 1] * gc[1] + W[4 * i + 2] * gc[2] + W[4 * i + 3];
            printf("  muñeca IK (%.0f %.0f %.0f) objetivo (%.0f %.0f %.0f) err %.1f; pieza mano t (%.0f %.0f %.0f); centro de agarre (%.0f %.0f %.0f); ojo (%.0f %.0f %.0f)\n", d->sol.wrist[0], d->sol.wrist[1], d->sol.wrist[2],
                   d->target_wrist[0], d->target_wrist[1], d->target_wrist[2], d->sol.err_mm, W[3], W[7], W[11], gw[0], gw[1], gw[2], V.eye[0], V.eye[1], V.eye[2]); }
        NsAdaptOpts ao; memset(&ao, 0, sizeof ao); ao.vram = VRAM; ao.keep_hands = 1;
        NsAdaptSrc *src = NULL; ao.src_out = &src;
        NsAdaptStats ast; NsRTri *tris = NULL;
        size_t nt0 = ns_adapt_scene(&S, st, &ao, &tris, &ast), nt = 0;
        for (size_t i = 0; i < nt0; i++)   /* solo el jugador y las manos (sin el escenario) */
            if (src && (src[i].key == map.key || ns_key_is_hand(src[i].key))) tris[nt++] = tris[i];
        { size_t nhand = 0; for (size_t i = 0; i < nt; i++) nhand += 0; (void)nhand; }
        if (src) { size_t c1 = 0; for (size_t i = 0; i < nt0; i++) c1 += ns_key_is_hand(src[i].key); printf("  triángulos: %zu (manos %zu), piezas %zu/%zu, sin malla %zu, ocultas %zu, degenerados %zu, soldados %zu\n", nt0, c1, ast.pieces_drawn, ast.pieces_in, ast.drop_no_mesh, ast.drop_hidden_piece, ast.drop_degenerate, ast.welded_verts); }
        free(src);
        NXT = 0;
        if (hp && SC[sc_i].pose == HP_GRIP_PISTOL) {   /* pistola: modelo -> mundo = mano * inv(empuñadura * zócalo) (m -> unidades del motor) */
            const VrGripEntry *e = vrgrip_find(0);
            float Sk[12], Hd[12] = {1, 0, 0, 0, 0, 1, 0, -50.0f, 0, 0, 1, 17.5f}, HS[12], iHS[12], M[12];
            wpose_to_m(e->grip, Sk, 1000.0f);
            m_mul(HS, Hd, Sk); m_inv(iHS, HS); m_mul(M, W, iHS);
            const float g0[3] = {-16, -100, 0}, g1[3] = {16, 0, 35}, f0[3] = {-10, -30, -120}, f1[3] = {10, 0, 0}, s0[3] = {-10, 0, -190}, s1[3] = {10, 32, 10}, k0[3] = {-3, -26, -49}, k1[3] = {3, -10, -42};
            box(M, g0, g1, 60, 60, 66); box(M, f0, f1, 60, 60, 66); box(M, s0, s1, 80, 80, 88); box(M, k0, k1, 150, 60, 50);
        }
        NsRTri *all = malloc((nt + NXT) * sizeof *all);
        if (nt) memcpy(all, tris, nt * sizeof *all);
        if (NXT) memcpy(all + nt, XT, NXT * sizeof *all);
        NsRScene rs = {all, nt + NXT, VRAM};
        /* cámaras: primera persona (desde el ojo), desde el exterior del brazo y de cerca sobre la costura de la muñeca */
        float wr[3] = {W[3], W[7], W[11]}, fing[3] = {-W[1], -W[5], -W[9]}, palm[3] = {W[0], W[4], W[8]}, thumb[3] = {W[2], W[6], W[10]};
        float gc[3], hc[3]; vrhs_grip_center(1, gc);
        for (int i = 0; i < 3; i++) hc[i] = W[4 * i] * gc[0] + W[4 * i + 1] * gc[1] + W[4 * i + 2] * gc[2] + W[4 * i + 3];
        float eyes[3][3], tgs[3][3], fovs[3] = {60, 50, 40};
        for (int i = 0; i < 3; i++) {
            eyes[0][i] = V.eye[i]; tgs[0][i] = hc[i];
            eyes[1][i] = hc[i] - palm[i] * 420.0f + thumb[i] * 150.0f; tgs[1][i] = 0.5f * (hc[i] + wr[i]) - fing[i] * 60.0f;
            eyes[2][i] = wr[i] - palm[i] * 140.0f - fing[i] * 40.0f + thumb[i] * 140.0f; tgs[2][i] = wr[i] + fing[i] * 10.0f;
        }
        const int Wd = 640, Hd = 520;
        uint8_t *sheet = calloc((size_t)3 * Wd * Hd * 3, 1);
        for (int v = 0; v < 3; v++) {
            NsRCam cam; cam_look(&cam, eyes[v], tgs[v], fovs[v], Wd, Hd); cam.znear = 20.0f;
            NsRBuf b; ns_buf_alloc(&b, Wd, Hd); ns_buf_clear(&b, 5, 6, 8);
            NsRStats rst; ns_render_view(&rs, &cam, NS_O_FLOAT, &b, &rst);
            uint8_t *rgb = malloc((size_t)Wd * Hd * 3); ns_buf_to_rgb8(&b, rgb, 0, 0);
            for (int y = 0; y < Hd; y++) memcpy(sheet + ((size_t)y * 3 * Wd + (size_t)v * Wd) * 3, rgb + (size_t)y * Wd * 3, (size_t)Wd * 3);
            free(rgb); ns_buf_free(&b);
        }
        char path[512]; snprintf(path, sizeof path, "%s/hands_%s.png", argv[2], SC[sc_i].name);
        if (ns_write_png(path, 3 * Wd, Hd, sheet)) printf("no se pudo escribir %s\n", path); else printf("  %s\n", path);
        free(sheet); free(all); free(tris); ns_scene_free(&S); ns_store_destroy(st);
    }
    return 0;
}
