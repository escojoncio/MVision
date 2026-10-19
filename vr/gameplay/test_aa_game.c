/* Pruebas sintéticas de vr_aa: matriz del visor para el buscador del juego (convenciones del motor), gesto del visor con
 * histéresis, lectura del estado del buscador sobre una RAM sintética, modo "raise" de vr_fire y robustez. Sin datos del juego.
 * Compilar con ASan/UBSan (make test-san). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vr_aa.h"
#include "vr_fire.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FALLO %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define PI 3.14159265358979

static WQuat qypr(double yaw, double pitch, double roll) {       /* Ry(yaw) * Rx(pitch) * Rz(roll); yaw + = izquierda, pitch + = arriba */
    double y = yaw * PI / 360, p = pitch * PI / 360, r = roll * PI / 360;
    double a[4] = {0, sin(y), 0, cos(y)}, b[4] = {sin(p), 0, 0, cos(p)}, c[4] = {0, 0, sin(r), cos(r)}, t[4], o[4];
#define QM(o, a, b) do { o[0] = a[3]*b[0] + a[0]*b[3] + a[1]*b[2] - a[2]*b[1]; o[1] = a[3]*b[1] - a[0]*b[2] + a[1]*b[3] + a[2]*b[0]; \
                         o[2] = a[3]*b[2] + a[0]*b[1] - a[1]*b[0] + a[2]*b[3]; o[3] = a[3]*b[3] - a[0]*b[0] - a[1]*b[1] - a[2]*b[2]; } while (0)
    QM(t, a, b); QM(o, t, c);
    WQuat q = {(float)o[0], (float)o[1], (float)o[2], (float)o[3]};
    return q;
}
static VrFireMap mkmap(int ex, int ey, int ez, float theta) {
    VrFireMap m; memset(&m, 0, sizeof m);
    m.valid = 1; m.eye[0] = ex; m.eye[1] = ey; m.eye[2] = ez; m.theta_play_deg = theta; m.p_ref[0] = 0.1f; m.p_ref[1] = 1.6f; m.p_ref[2] = -0.2f; m.units_per_m = 1000.0f;
    return m;
}

/* ---------------------------------------------------------------- matriz del visor */
static void t_matrix(void) {
    /* Caso medido en el juego (cámara de primera persona mirando a -X): filas = ejes (derecha, abajo, delante) en mundo,
     * t = -R*ojo. Con theta 0, rumbo 270 grados (= -X) y la pose del visor en el punto de referencia. */
    VrFireMap map = mkmap(-5756, 1343, -1298, 0);
    map.p_ref[0] = 0; map.p_ref[1] = 1.6f; map.p_ref[2] = 0;
    WPose s = {{0, 1.6f, 0}, qypr(270, 0, 0)};
    VrFireMat v;
    CHECK(vraa_view_matrix(&map, &s, &v) == 1, "matriz válida");
    static const int16_t ref[9] = {0, 0, -4096, 0, -4096, 0, -4096, 0, 0};
    CHECK(!memcmp(v.m, ref, sizeof ref), "R = %d %d %d / %d %d %d / %d %d %d", v.m[0], v.m[1], v.m[2], v.m[3], v.m[4], v.m[5], v.m[6], v.m[7], v.m[8]);
    CHECK(v.t[0] == -1298 && v.t[1] == 1343 && v.t[2] == -5756, "t = (%d,%d,%d)", v.t[0], v.t[1], v.t[2]);
    /* rumbo 0 (+Z): derecha = -X, abajo = -Y, delante = +Z */
    WPose s0 = {{0, 1.6f, 0}, qypr(0, 0, 0)};
    CHECK(vraa_view_matrix(&map, &s0, &v) && v.m[0] == -4096 && v.m[4] == -4096 && v.m[8] == 4096 && !v.m[1] && !v.m[2] && !v.m[3] && !v.m[5] && !v.m[6] && !v.m[7],
          "rumbo 0: %d %d %d / %d %d %d / %d %d %d", v.m[0], v.m[1], v.m[2], v.m[3], v.m[4], v.m[5], v.m[6], v.m[7], v.m[8]);

    /* barrido: un punto delante del visor cae en el eje de la vista (x = y = 0, z = distancia); uno a la izquierda en x < 0;
     * uno arriba en y < 0; R ortonormal con determinante +1 */
    double worst_off = 0, worst_det = 0, worst_orth = 0;
    int n = 0;
    for (int th = -180; th <= 180; th += 60) for (int yaw = -180; yaw < 180; yaw += 30) for (int pit = -80; pit <= 80; pit += 20) for (int roll = -90; roll <= 90; roll += 45) {
        VrFireMap mp = mkmap(500, -700, 900, (float)th);
        WPose q = {{0.3f, 1.2f, -0.7f}, qypr(yaw, pit, roll)};
        if (!vraa_view_matrix(&mp, &q, &v)) { CHECK(0, "matriz inválida th=%d yaw=%d pit=%d roll=%d", th, yaw, pit, roll); continue; }
        n++;
        double R[9]; for (int i = 0; i < 9; i++) R[i] = v.m[i] / 4096.0;
        double det = R[0] * (R[4] * R[8] - R[5] * R[7]) - R[1] * (R[3] * R[8] - R[5] * R[6]) + R[2] * (R[3] * R[7] - R[4] * R[6]);
        if (fabs(det - 1) > worst_det) worst_det = fabs(det - 1);
        for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) {
            double d = R[3 * i] * R[3 * j] + R[3 * i + 1] * R[3 * j + 1] + R[3 * i + 2] * R[3 * j + 2] - (i == j);
            if (fabs(d) > worst_orth) worst_orth = fabs(d);
        }
        /* posición del ojo del visor en mundo: t = -R E  ->  E = -R^T t */
        double E[3];
        for (int j = 0; j < 3; j++) E[j] = -(R[j] * v.t[0] + R[3 + j] * v.t[1] + R[6 + j] * v.t[2]);
        float pw[3], pp[3] = {q.p.x, q.p.y, q.p.z};
        vrfire_pos_to_world(&mp, pp, pw);
        for (int j = 0; j < 3; j++) if (fabs(E[j] - pw[j]) > worst_off) worst_off = fabs(E[j] - pw[j]);
        /* punto a 5 m por delante de la pose */
        float fu[3] = {0, 0, -1}, fw[3];
        WQuat qq = q.q; float nn = sqrtf(qq.x * qq.x + qq.y * qq.y + qq.z * qq.z + qq.w * qq.w); qq.x /= nn; qq.y /= nn; qq.z /= nn; qq.w /= nn;
        fu[0] = -2 * (qq.x * qq.z + qq.w * qq.y); fu[1] = -2 * (qq.y * qq.z - qq.w * qq.x); fu[2] = -(1 - 2 * (qq.x * qq.x + qq.y * qq.y));
        vrfire_dir_to_world(&mp, fu, fw);
        double W[3] = {pw[0] + 5000 * fw[0], pw[1] + 5000 * fw[1], pw[2] + 5000 * fw[2]}, V[3];
        for (int i = 0; i < 3; i++) V[i] = R[3 * i] * W[0] + R[3 * i + 1] * W[1] + R[3 * i + 2] * W[2] + v.t[i];
        CHECK(fabs(V[0]) < 3 && fabs(V[1]) < 3 && fabs(V[2] - 5000) < 6, "punto delante: V=(%.1f,%.1f,%.1f) th=%d yaw=%d pit=%d roll=%d", V[0], V[1], V[2], th, yaw, pit, roll);
    }
    CHECK(n > 1000, "casos del barrido: %d", n);
    CHECK(worst_det < 2e-3, "determinante: %g", worst_det);
    CHECK(worst_orth < 2e-3, "ortonormalidad: %g", worst_orth);
    CHECK(worst_off < 2.0, "el ojo recuperado de t difiere %g unidades", worst_off);

    /* izquierda del visor -> x de vista negativa; arriba -> y de vista negativa (Y abajo) */
    {
        VrFireMap mp = mkmap(0, 0, 0, 0);
        mp.p_ref[0] = 0; mp.p_ref[1] = 1.6f; mp.p_ref[2] = 0;
        WPose q = {{0, 1.6f, 0}, qypr(0, 0, 0)};   /* mira a +Z; +X es la izquierda */
        vraa_view_matrix(&mp, &q, &v);
        double R[9]; for (int i = 0; i < 9; i++) R[i] = v.m[i] / 4096.0;
        double left[3] = {1000, 0, 5000}, up[3] = {0, 1000, 5000}, vl[3], vu[3];
        for (int i = 0; i < 3; i++) { vl[i] = R[3 * i] * left[0] + R[3 * i + 1] * left[1] + R[3 * i + 2] * left[2] + v.t[i]; vu[i] = R[3 * i] * up[0] + R[3 * i + 1] * up[1] + R[3 * i + 2] * up[2] + v.t[i]; }
        CHECK(vl[0] < -900 && fabs(vl[1]) < 5, "izquierda: (%.0f,%.0f,%.0f)", vl[0], vl[1], vl[2]);
        CHECK(vu[1] < -900 && fabs(vu[0]) < 5, "arriba: (%.0f,%.0f,%.0f)", vu[0], vu[1], vu[2]);
    }
    /* entradas inválidas */
    VrFireMap bad = mkmap(0, 0, 0, 0); bad.valid = 0;
    WPose nan = {{NAN, 0, 0}, {0, 0, 0, 1}}, zq = {{0, 0, 0}, {0, 0, 0, 0}};
    CHECK(!vraa_view_matrix(&bad, &s, &v), "mapa inválido");
    CHECK(!vraa_view_matrix(NULL, &s, &v) && !vraa_view_matrix(&map, NULL, &v) && !vraa_view_matrix(&map, &s, NULL), "nulos");
    CHECK(!vraa_view_matrix(&map, &nan, &v), "posición NaN");
    CHECK(!vraa_view_matrix(&map, &zq, &v), "cuaternión nulo");
}

/* ---------------------------------------------------------------- vista para el HLE */
static void t_take_view(void) {
    vraa_init(NULL);
    VrFireMap map = mkmap(100, 200, 300, 10);
    WPose s = {{0.2f, 1.5f, -0.4f}, qypr(30, 5, 0)};
    VrFireMat v;
    CHECK(!vraa_take_view(&v), "sin visor activo no se sustituye");
    vraa_set_scope(1, &map, &s);
    CHECK(vraa_take_view(&v) == 1, "con visor activo se sustituye");
    VrFireMat v2; vraa_view_matrix(&map, &s, &v2);
    CHECK(!memcmp(&v, &v2, sizeof v), "misma matriz que vraa_view_matrix");
    vraa_set_scope(0, &map, &s);
    CHECK(!vraa_take_view(&v), "apagado");
    VrFireMap inv = map; inv.valid = 0;
    vraa_set_scope(1, &inv, &s);
    CHECK(!vraa_take_view(&v), "mapa inválido no sustituye");
    WPose nan = s; nan.q.w = NAN;
    vraa_set_scope(1, &map, &nan);
    CHECK(!vraa_take_view(&v), "pose NaN no sustituye");
    vraa_set_scope(1, NULL, &s); CHECK(!vraa_take_view(&v), "mapa nulo");
    /* H del juego */
    VrAaCfg c; vraa_cfg_default(&c); c.h_scale = 2.0f; vraa_init(&c);
    CHECK(vraa_view_focal(160) == 320, "H = H del motor x escala (%d)", vraa_view_focal(160));
    CHECK(vraa_view_focal(0) == 16 && vraa_view_focal(30000) == 32767, "H acotada");
    c.h_scale = 0.5f; vraa_init(&c);
    CHECK(vraa_config()->h_scale == 0.5f && vraa_view_focal(320) == 160, "escala < 1 válida (engine_h > 320)");
    c.h_scale = 0.0f; vraa_init(&c); CHECK(vraa_config()->h_scale == 1.0f, "escala 0 vuelve a 1");
    c.h_scale = 100.0f; vraa_init(&c); CHECK(vraa_config()->h_scale == 1.0f, "escala enorme vuelve a 1");
    vraa_set_h_scale(2.0f); CHECK(vraa_view_focal(160) == 320, "set_h_scale");
    vraa_set_h_scale(NAN); CHECK(vraa_view_focal(160) == 160, "set_h_scale NaN -> 1");
    vraa_set_h_scale(1.0f / 1000.0f); CHECK(vraa_view_focal(160) == 160, "set_h_scale fuera de rango -> 1");
}

/* Proyección de puntos conocidos como la GTE (sx = x*H/z, sy = y*H/z; x + = derecha, y + = ABAJO en pantalla), con la convención de
 * la pose de usuario (+X local = derecha, +Y = arriba, -Z = delante) y sin usar las filas del visor para construir los puntos:
 * un punto a la derecha/arriba del eje del visor, expresado con los ejes de la pose, debe caer a la derecha/arriba en pantalla. */
static void t_project(void) {
    int bad = 0, n = 0;
    for (int th = -150; th <= 180; th += 90) for (int yaw = -170; yaw <= 170; yaw += 40) for (int pit = -60; pit <= 60; pit += 30) for (int roll = -60; roll <= 60; roll += 60) {
        VrFireMap mp = mkmap(321, -45, 987, (float)th);
        WPose q = {{-0.2f, 1.4f, 0.35f}, qypr(yaw, pit, roll)};
        VrFireMat v;
        if (!vraa_view_matrix(&mp, &q, &v)) { bad++; continue; }
        WQuat k = q.q; float nn = sqrtf(k.x * k.x + k.y * k.y + k.z * k.z + k.w * k.w); k.x /= nn; k.y /= nn; k.z /= nn; k.w /= nn;
        /* ejes locales -> usuario (matriz de rotación del cuaternión): columnas X, Y, Z */
        float X[3] = {1 - 2 * (k.y * k.y + k.z * k.z), 2 * (k.x * k.y + k.w * k.z), 2 * (k.x * k.z - k.w * k.y)};
        float Y[3] = {2 * (k.x * k.y - k.w * k.z), 1 - 2 * (k.x * k.x + k.z * k.z), 2 * (k.y * k.z + k.w * k.x)};
        float Z[3] = {2 * (k.x * k.z + k.w * k.y), 2 * (k.y * k.z - k.w * k.x), 1 - 2 * (k.x * k.x + k.y * k.y)};
        const float dr = 0.6f, du = 0.45f, df = 4.0f;       /* 0,6 m a la derecha, 0,45 m arriba, 4 m delante (-Z) */
        float pu[3] = {q.p.x + dr * X[0] + du * Y[0] - df * Z[0], q.p.y + dr * X[1] + du * Y[1] - df * Z[1], q.p.z + dr * X[2] + du * Y[2] - df * Z[2]}, pw[3];
        vrfire_pos_to_world(&mp, pu, pw);
        double V[3], R[9];
        for (int i = 0; i < 9; i++) R[i] = v.m[i] / 4096.0;
        for (int i = 0; i < 3; i++) V[i] = R[3 * i] * pw[0] + R[3 * i + 1] * pw[1] + R[3 * i + 2] * pw[2] + v.t[i];
        const double H = 320.0;
        double sx = V[0] * H / V[2], sy = V[1] * H / V[2];
        double ex = dr * 1000.0 * H / (df * 1000.0), ey = -du * 1000.0 * H / (df * 1000.0);     /* esperado: +x derecha, y negativa = arriba */
        n++;
        if (!(V[2] > 3900 && V[2] < 4100) || fabs(sx - ex) > 1.0 || fabs(sy - ey) > 1.0) {
            bad++;
            CHECK(0, "proyección th=%d yaw=%d pit=%d roll=%d: (%.1f,%.1f) esperado (%.1f,%.1f) z=%.0f", th, yaw, pit, roll, sx, sy, ex, ey, V[2]);
        }
    }
    CHECK(n > 100 && !bad, "puntos proyectados: %d, erróneos: %d", n, bad);
}

/* ---------------------------------------------------------------- gesto */
static void t_gesture(void) {
    VrAaCfg c; vraa_cfg_default(&c); vraa_init(&c);
    WPose head = {{0, 1.6f, 0}, qypr(0, 0, 0)};
    /* lanzador al hombro: punto del visor 0,1 m por delante del ojo y ligeramente bajo, tubo hacia delante */
    WPose sight = {{0.05f, 1.55f, -0.10f}, qypr(0, 0, 0)};
    WPose far = {{0.3f, 1.0f, -0.6f}, qypr(0, 0, 0)};
    CHECK(!vraa_gesture(&head, &far, 1), "lejos: no apunta");
    int on = 0;
    for (int i = 0; i < 2; i++) on = vraa_gesture(&head, &sight, 1);
    CHECK(!on, "histéresis de entrada: 2 VBlanks no bastan");
    on = vraa_gesture(&head, &sight, 1);
    CHECK(on == 1 && vraa_aiming(), "a los 3 VBlanks apunta");
    /* sale a más de leave_m, no a medio camino */
    WPose mid = {{0.05f, 1.55f, -0.35f}, qypr(0, 0, 0)};     /* ojo a 0,25 m del apoyo: entre entrada (0,20) y salida (0,30) */
    CHECK(vraa_gesture(&head, &mid, 1) == 1, "entre los dos umbrales se mantiene");
    CHECK(vraa_gesture(&head, &far, 1) == 0, "lejos: deja de apuntar");
    CHECK(vraa_gesture(&head, &mid, 1) == 0, "tras salir no vuelve a entrar con el umbral de salida");
    /* eje del tubo desviado más de 40 grados de la mirada */
    vraa_init(&c);
    WPose askew = {{0.05f, 1.55f, -0.10f}, qypr(70, 0, 0)};
    for (int i = 0; i < 6; i++) on = vraa_gesture(&head, &askew, 1);
    CHECK(!on, "tubo de través: no apunta");
    /* no empuñado */
    vraa_init(&c);
    for (int i = 0; i < 6; i++) on = vraa_gesture(&head, &sight, 0);
    CHECK(!on, "no empuñado: no apunta");
    /* entrada rota */
    WPose nan = sight; nan.p.x = NAN;
    for (int i = 0; i < 6; i++) on = vraa_gesture(&head, &nan, 1);
    CHECK(!on, "pose NaN");
    CHECK(!vraa_gesture(NULL, &sight, 1) && !vraa_gesture(&head, NULL, 1), "nulos");
    /* configuración fuera de rango */
    VrAaCfg bad; memset(&bad, 0, sizeof bad); bad.enter_m = -1; bad.leave_m = -5; bad.axis_deg = 500; bad.sight_back_m = 9; bad.on_vbl = -3; bad.h_scale = NAN;
    vraa_init(&bad);
    const VrAaCfg *k = vraa_config();
    CHECK(k->enter_m > 0 && k->leave_m > k->enter_m && k->axis_deg <= 90 && k->sight_back_m <= 0.5f && k->on_vbl >= 1 && k->h_scale == 1.0f, "configuración saneada");
}

/* ---------------------------------------------------------------- estado del buscador */
static uint8_t ram[0x200000];
static void w32(uint32_t a, uint32_t v) { memcpy(ram + (a & 0x1FFFFF), &v, 4); }
static void t_lock_state(void) {
    vraa_init(NULL);
    memset(ram, 0, sizeof ram);
    uint32_t actor = 0x80120000u;
    VrAaLock l; vraa_lock_info(&l);
    CHECK(!l.alive && !l.target, "inicio");
    w32(actor + 0x30, 0x800B8818u); w32(actor + 0x34, 5); w32(actor + 0x38, 0); w32(actor + 0x21AC, 3);
    vraa_after_step(ram, actor, 1);
    vraa_lock_info(&l);
    CHECK(l.alive && l.actor == actor && l.target == 0x800B8818u && l.frames == 5 && l.candidates == 3 && !l.converged && l.steps == 1 && l.swapped == 1, "primer paso");
    w32(actor + 0x34, 12);
    vraa_after_step(ram, actor, 0);
    vraa_lock_info(&l);
    CHECK(l.converged && l.steps == 2 && l.swapped == 1, "cerrado a los 12 fotogramas");
    w32(0x800BF080u, 0x800B8818u);
    CHECK(vraa_game_target(ram) == 0x800B8818u, "puntero global del juego");
    VrAaSight sg; vraa_set_scope(1, NULL, NULL); vraa_sight(&sg);
    CHECK(!sg.active && sg.locked && sg.converged, "sin visor VR: no activo pero con blanco");
    /* sin blanco: frames a 0 aunque la RAM tenga basura */
    w32(actor + 0x30, 0); w32(actor + 0x34, 9);
    vraa_after_step(ram, actor, 1);
    vraa_lock_info(&l);
    CHECK(!l.target && l.frames == 0 && !l.converged, "sin blanco");
    /* caducidad: sin pasos el buscador deja de estar vivo, los contadores históricos se conservan */
    int steps = l.steps;
    for (int i = 0; i < 3; i++) vraa_tick();
    vraa_lock_info(&l); CHECK(l.alive, "3 VBlanks: aún vivo");
    for (int i = 0; i < 3; i++) vraa_tick();
    vraa_lock_info(&l); CHECK(!l.alive && !l.target && l.steps == steps, "caducado");
    /* actor inválido */
    vraa_after_step(ram, 0, 0); vraa_after_step(ram, 0x80000000u, 0); vraa_after_step(ram, 0x801FFFFCu, 0); vraa_after_step(NULL, actor, 0);
    vraa_lock_info(&l); CHECK(!l.alive, "actores inválidos ignorados");
    CHECK(vraa_game_target(NULL) == 0, "RAM nula");
}

/* ---------------------------------------------------------------- vr_fire con raise */
static void t_fire_raise(void) {
    VrFireCfg cfg; vrfire_cfg_default(&cfg);       /* ON_TRIGGER por defecto */
    vrfire_init(&cfg);
    VrFireMap map = mkmap(0, 0, 0, 0);
    WPose mz = {{0, 1.6f, -0.6f}, {0, 0, 0, 1}};
    VrFireGame g; memset(&g, 0, sizeof g); g.ok = 1; g.weapon = 4; g.total = 4;
    VrFireIn in; memset(&in, 0, sizeof in);
    in.active = 1; in.muzzle = mz; in.fire_pose = mz; in.map = &map;
    VrFireOut o;
    int pressed = 0;
    for (int i = 0; i < 6; i++) { in.ready = 1; in.raise = 0; vrfire_frame(&g, &in, &o); pressed |= o.square; }
    CHECK(!pressed, "ready sin raise en ON_TRIGGER: no pulsa");
    vrfire_init(&cfg);
    pressed = 0;
    for (int i = 0; i < 6; i++) { in.ready = 1; in.raise = 1; vrfire_frame(&g, &in, &o); pressed |= o.square; }
    CHECK(pressed && o.engaged, "ready con raise: mantiene el botón");
    /* el juego pasa a "apuntando" y el gatillo pide el disparo: suelta (el juego lanza el misil) */
    g.armed = 1;
    in.fire = 1; vrfire_frame(&g, &in, &o); in.fire = 0;
    int released = !o.square;
    for (int i = 0; i < 2 && !released; i++) { vrfire_frame(&g, &in, &o); released = !o.square; }
    CHECK(released, "el disparo suelta el botón");
    VrFireStats st; vrfire_stats(&st);
    CHECK(st.requests == 1 && st.releases >= 1, "estadísticas: peticiones=%u soltados=%u", st.requests, st.releases);
    /* el misil sale con la matriz de la petición */
    VrFireMat m; int r = vrfire_take_projectile(1, &m);
    CHECK(r == VRFIRE_SUBST, "el proyectil usa la pose del visor");
    float f[3]; vrfire_mat_forward(&m, f);
    CHECK(fabsf(f[2] - 1.0f) < 1e-3f, "delante +Z");
    /* bajar el visor sin disparar: el tiro sobrante del juego se suprime */
    vrfire_init(&cfg);
    for (int i = 0; i < 6; i++) { in.ready = 1; in.raise = 1; g.armed = 1; vrfire_frame(&g, &in, &o); }
    for (int i = 0; i < 6; i++) { in.ready = 0; in.raise = 1; vrfire_frame(&g, &in, &o); }
    CHECK(vrfire_take_projectile(1, &m) == VRFIRE_SKIP, "desarmar: el tiro sobrante se suprime");
}

int main(void) {
    t_matrix();
    t_take_view();
    t_project();
    t_gesture();
    t_lock_state();
    t_fire_raise();
    printf("test_aa_game: %d comprobaciones, %d fallos\n", checks, fails);
    return fails ? 1 : 0;
}
