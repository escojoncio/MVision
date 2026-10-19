/* Prueba sintética (sin datos del juego): con una escena nativa y una lista de la GPU del MISMO mundo, el render nativo cae en el mismo sitio que la ruta de la GPU.
 *   ruta GPU:    vr_scene_build con el mapa de usuario (igual que build_one de vr/host/vrhost.c) sobre polígonos con posiciones de vista conocidas;
 *   ruta nativa: mundo del motor W = M v + ojo, llevado a VR con el MISMO vrcam_native_to_vr que usa el anfitrión (NsScene.to_vr).
 * Además comprueba ns_coherence (la red de seguridad del renderer): 0 con la misma cámara y distancias reales con otra (fotograma distinto, giro).
 * make -C vr/camera test-native-coh */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "vr_camera.h"
#include "vr_scene.h"
#include "vr_usermap.h"
#include "ns_bridge.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; fprintf(stderr, "FALLO %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

static GpuOp ops[64];
static GpuDrawList dl;
static void setv3(GpuVtx *v, const float p[3]) { v->fvalid = 1; v->vvalid = 1; v->vx = p[0]; v->vy = p[1]; v->vz = p[2]; v->fz = p[2]; }
static void tri(const float a[3], const float b[3], const float c[3]) {
    GpuOp *o = &ops[dl.nops++];
    memset(o, 0, sizeof *o);
    o->type = GPU_OP_POLY; o->nv = 3; o->flags = (uint16_t)GPU_F_TEXTURED; o->tp = 1; o->tx = 320; o->ty = 256; o->clx = 48; o->cly = 500;
    setv3(&o->v[0], a); setv3(&o->v[1], b); setv3(&o->v[2], c);
    for (int i = 0; i < 3; i++) { o->v[i].u = (uint8_t)(10 * i); o->v[i].v = (uint8_t)(5 * i); o->v[i].r = 128; o->v[i].g = 100; o->v[i].b = 60; }
}

/* Un caso completo: cámara del motor (M, ojo), mapa de usuario (theta, p_ref); mirror = mundo zurdo del puente (la lista espeja Z, el nativo no). */
static void run_case(const char *name, const float M[9], const int32_t eye[3], float theta_deg, const float pref[3]) {
    VrCamRecord r;
    memset(&r, 0, sizeof r);
    r.active = 1; r.serial = 1; r.screen = 320; r.units_per_m = 1000.0f; r.theta_play_deg = theta_deg;
    for (int i = 0; i < 3; i++) { r.eye[i] = eye[i]; r.p_ref[i] = pref[i]; }
    memcpy(r.M, M, sizeof r.M);

    BridgeState bs;
    memset(&bs, 0, sizeof bs);
    bs.valid = BRIDGE_OK_ALL_CORE; bs.has_player = 1; bs.eye_focal = 320;
    for (int i = 0; i < 3; i++) { bs.eye_pos[i] = eye[i]; for (int j = 0; j < 3; j++) bs.eye_axes[i][j] = (int16_t)lrintf(M[3 * i + j] * 4096.0f); }
    bs.player_pos = (BVec3){eye[0] + 30, eye[1] - 150, eye[2] - 20};   /* el jugador no coincide con el ojo: el origen sale de player_pos.y - ctl_height */
    bs.ctl_height = 1104;

    /* igual que build_one (VR activa): origen bajo el ojo VR, mapa mundo -> usuario */
    VrSceneOpts o = vr_scene_opts_default();
    float org[3];
    vr_scene_player_origin(&bs, &o, org);
    o.origin_mode = 1; o.origin_game[0] = (float)eye[0]; o.origin_game[1] = org[1]; o.origin_game[2] = (float)eye[2];
    vrcam_scene_user_map(&o, &r, org[1]);

    /* puntos en el espacio de vista del motor (x der, y abajo, z adelante), dentro y fuera del eje */
    const float P[6][3] = {{0, 0, 2500}, {800, -300, 4000}, {-1200, 500, 3000}, {300, 900, 6000}, {-2500, -700, 7000}, {2000, 100, 1800}};
    memset(ops, 0, sizeof ops); memset(&dl, 0, sizeof dl); dl.ops = ops;
    tri(P[0], P[1], P[2]);
    tri(P[3], P[4], P[5]);
    VrScene s;
    CHECK(vr_scene_build(&dl, &bs, &o, &s) == 0, "%s: build", name);
    CHECK(s.has_3d && s.world_valid && s.user_space && s.nv >= 6, "%s: has_3d=%d world_valid=%d user_space=%d nv=%u", name, s.has_3d, s.world_valid, s.user_space, s.nv);

    /* ruta nativa: el mismo mapa que usa el anfitrión (ns_world_to_vr: origen = origin_game de la lista) */
    float to_vr[12];
    CHECK(vrcam_native_to_vr(&o, o.origin_game, to_vr) == 1, "%s: to_vr", name);

    /* 1) cada punto de vista, llevado al mundo del motor (W = M v + ojo) y de ahí a VR por el nativo, es un vértice de la lista de la GPU */
    for (int k = 0; k < 6; k++) {
        float W[3], n[3];
        for (int i = 0; i < 3; i++) W[i] = M[3 * i] * P[k][0] + M[3 * i + 1] * P[k][1] + M[3 * i + 2] * P[k][2] + (float)eye[i];
        for (int i = 0; i < 3; i++) n[i] = to_vr[4 * i] * W[0] + to_vr[4 * i + 1] * W[1] + to_vr[4 * i + 2] * W[2] + to_vr[4 * i + 3];
        float best = 1e9f;
        for (uint32_t j = 0; j < s.nv; j++) {
            float d = sqrtf((s.v[j].pos[0] - n[0]) * (s.v[j].pos[0] - n[0]) + (s.v[j].pos[1] - n[1]) * (s.v[j].pos[1] - n[1]) + (s.v[j].pos[2] - n[2]) * (s.v[j].pos[2] - n[2]));
            if (d < best) best = d;
        }
        CHECK(best < 0.01f, "%s: el punto %d cae a %.4f m de su vértice de la GPU (nativo %.3f %.3f %.3f)", name, k, best, n[0], n[1], n[2]);
    }

    /* 2) la cámara: el ojo nativo en VR es VrScene.eye_vr (la cabeza), y la vista del juego de la GPU tiene la misma orientación que la cámara nativa */
    NsCohIn in;
    NsCohOut out;
    memset(&in, 0, sizeof in);
    memcpy(in.user_R, s.user_R, sizeof in.user_R); in.k = s.user_k * s.scale; in.user_eye_h = s.user_eye_h; memcpy(in.user_p_ref, s.user_p_ref, sizeof in.user_p_ref);
    memcpy(in.origin, s.origin_game, sizeof in.origin); memcpy(in.game_view, s.game_view, sizeof in.game_view);
    memcpy(in.to_vr, to_vr, sizeof in.to_vr);
    for (int i = 0; i < 3; i++) in.cam_eye[i] = (float)eye[i];
    memcpy(in.cam_rot, M, sizeof in.cam_rot);
    in.units_per_m = 1000.0f;
    CHECK(ns_coherence(&in, &out) == 1 && out.ok, "%s: coherencia evaluable", name);
    CHECK(out.d_eye < 0.005f && out.d_probe < 0.01f && out.d_rot < 0.2f, "%s: misma cámara: d_eye=%.4f d_probe=%.4f d_rot=%.3f", name, out.d_eye, out.d_probe, out.d_rot);
    {   /* la versión con punteros planos (la que llama Swift) da lo mismo */
        NsCohOut of;
        float kk = s.user_k * s.scale, ecam[3] = {(float)eye[0], (float)eye[1], (float)eye[2]};
        CHECK(ns_coherence_flat(s.user_R, kk, s.user_eye_h, s.user_p_ref, s.origin_game, s.game_view, to_vr, ecam, M, 1000.0f, &of) == 1 &&
              fabsf(of.d_probe - out.d_probe) < 1e-6f && fabsf(of.d_rot - out.d_rot) < 1e-4f, "%s: ns_coherence_flat", name);
    }
    CHECK(sqrtf((out.nat_eye[0] - s.eye_vr[0]) * (out.nat_eye[0] - s.eye_vr[0]) + (out.nat_eye[1] - s.eye_vr[1]) * (out.nat_eye[1] - s.eye_vr[1]) +
                (out.nat_eye[2] - s.eye_vr[2]) * (out.nat_eye[2] - s.eye_vr[2])) < 0.005f, "%s: ojo nativo %.3f %.3f %.3f frente a eye_vr %.3f %.3f %.3f", name,
          out.nat_eye[0], out.nat_eye[1], out.nat_eye[2], s.eye_vr[0], s.eye_vr[1], s.eye_vr[2]);

    /* 3) la red de seguridad SÍ detecta una escena nativa de otro fotograma: el jugador se movió 0,5 m y giró 12 grados (se recalcula su mapa como lo hace el anfitrión) */
    {
        VrCamRecord r2 = r;
        float c = cosf(12.0f * 3.14159265f / 180.0f), sn = sinf(12.0f * 3.14159265f / 180.0f);
        r2.eye[0] += 500;
        for (int col = 0; col < 3; col++) {     /* M2 = Ry(12) * M */
            r2.M[0 * 3 + col] = c * M[0 * 3 + col] + sn * M[2 * 3 + col];
            r2.M[1 * 3 + col] = M[1 * 3 + col];
            r2.M[2 * 3 + col] = -sn * M[0 * 3 + col] + c * M[2 * 3 + col];
        }
        VrSceneOpts o2 = vr_scene_opts_default();
        float org2[3] = {(float)r2.eye[0], org[1], (float)r2.eye[2]};
        o2.origin_mode = 1; memcpy(o2.origin_game, org2, sizeof org2);
        vrcam_scene_user_map(&o2, &r2, org[1]);
        float to_vr2[12];
        CHECK(vrcam_native_to_vr(&o2, org2, to_vr2) == 1, "%s: to_vr2", name);
        NsCohIn in2 = in;
        memcpy(in2.to_vr, to_vr2, sizeof in2.to_vr);
        for (int i = 0; i < 3; i++) in2.cam_eye[i] = (float)r2.eye[i];
        memcpy(in2.cam_rot, r2.M, sizeof in2.cam_rot);
        CHECK(ns_coherence(&in2, &out) == 1, "%s: coherencia 2", name);
        CHECK(out.d_eye > 0.3f && out.d_rot > 8.0f, "%s: otro fotograma: d_eye=%.3f (esperado ~0,5) d_rot=%.2f (esperado ~12)", name, out.d_eye, out.d_rot);
    }
    printf("  %-10s ok (6 puntos del mundo coinciden con la lista de la GPU; coherencia 0 con la misma cámara)\n", name);
}

static void rot_y(const float R[9], float rad, float out[9]) {
    float c = cosf(rad), sn = sinf(rad);
    for (int col = 0; col < 3; col++) {
        out[0 * 3 + col] = c * R[0 * 3 + col] + sn * R[2 * 3 + col];
        out[1 * 3 + col] = R[1 * 3 + col];
        out[2 * 3 + col] = -sn * R[0 * 3 + col] + c * R[2 * 3 + col];
    }
}

int main(void) {
    /* columnas = der, abajo, adelante en el mundo del motor (Y arriba): mirando a -Z; luego girada */
    const float base[9] = {1, 0, 0, 0, -1, 0, 0, 0, -1};
    const float pref[3] = {0.12f, 1.62f, -0.31f};
    float M[9];
    const int32_t e1[3] = {1200, 3000, -800}, e2[3] = {-5200, 2500, 14000}, e3[3] = {0, 1700, 0};
    run_case("recto", base, e1, 0.0f, pref);
    rot_y(base, 0.7f, M);
    run_case("girado", M, e2, 33.0f, pref);
    rot_y(base, -2.4f, M);
    run_case("atras", M, e3, 215.0f, pref);
    /* mundo zurdo (det < 0): la lista de la GPU espeja Z internamente y el mapa de usuario lo deshace; el nativo no espeja nada */
    const float left[9] = {1, 0, 0, 0, -1, 0, 0, 0, 1};
    rot_y(left, 0.4f, M);
    run_case("zurdo", M, e2, 80.0f, pref);
    if (fails) { fprintf(stderr, "test_native_coh: %d fallos\n", fails); return 1; }
    printf("test_native_coh: todo correcto\n");
    return 0;
}
