/* Pruebas sintéticas de vr_fire: geometría (matriz del motor, correspondencia usuario->mundo), lógica de pulsación del
 * botón de ataque contra un "juego falso" que reproduce lo medido en el juego real, sustitución de proyectiles, ocultar el
 * arma sobre una RAM sintética y robustez. Sin datos del juego. Compilar con ASan/UBSan (make test-san). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "vr_fire.h"
#include "vr_camera.h"
#include "bridge_addrs.h"

static int fails, checks;
static int mat_eq(const VrFireMat *a, const VrFireMat *b) { return !memcmp(a->m, b->m, sizeof a->m) && !memcmp(a->t, b->t, sizeof a->t); }
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FALLO %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define PI 3.14159265358979

/* ---------------------------------------------------------------- geometría */
static WQuat qypr(double yaw, double pitch, double roll) {       /* Ry(yaw) * Rx(pitch) * Rz(roll), grados; yaw + = izquierda, pitch + = arriba */
    double y = yaw * PI / 180 / 2, p = pitch * PI / 180 / 2, r = roll * PI / 180 / 2;
    double a[4] = {0, sin(y), 0, cos(y)}, b[4] = {sin(p), 0, 0, cos(p)}, c[4] = {0, 0, sin(r), cos(r)};
    double t[4], o[4];
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
static double ang(const double a[3], const double b[3]) {
    double c[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    return atan2(sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]), a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) * 180 / PI;
}

static void t_matrix(void) {
    /* identidad: delante de usuario -Z con theta 0 = +Z del mundo -> rotación YXZ(-1024, 0, 0) = [[1,0,0],[0,0,1],[0,-1,0]] */
    VrFireMap map = mkmap(1000, 2000, -3000, 0);
    WPose p = {{0.1f, 1.6f, -0.2f}, {0, 0, 0, 1}};
    VrFireMat m;
    CHECK(vrfire_make_matrix(&map, &p, 0, &m) == 1, "matriz válida");
    static const int16_t id[9] = {4096, 0, 0, 0, 0, 4096, 0, -4096, 0};
    CHECK(!memcmp(m.m, id, sizeof id), "yaw 0 pitch 0 = %d %d %d / %d %d %d / %d %d %d", m.m[0], m.m[1], m.m[2], m.m[3], m.m[4], m.m[5], m.m[6], m.m[7], m.m[8]);
    CHECK(m.t[0] == 1000 && m.t[1] == 2000 && m.t[2] == -3000, "t = ojo cuando p = p_ref (%d %d %d)", m.t[0], m.t[1], m.t[2]);
    CHECK(vrfire_make_matrix(&map, &p, 400, &m) && m.t[2] == -3000 + 400 && m.t[0] == 1000 && m.t[1] == 2000, "shift 400 por delante (+Z)");
    /* barrido de poses: dirección del motor frente a la fórmula (cos p sin(theta+yaw), sin p, cos p cos(theta+yaw)) */
    double worst = 0, worstn = 0, worstdet = 1;
    for (int th = -180; th <= 180; th += 45) for (int yaw = -180; yaw < 180; yaw += 20) for (int pit = -85; pit <= 85; pit += 17) for (int roll = -90; roll <= 90; roll += 45) {
        VrFireMap mp = mkmap(500, -700, 900, (float)th);
        WPose q = {{0.3f, 1.2f, -0.7f}, qypr(yaw, pit, roll)};
        VrFireMat mm;
        if (!vrfire_make_matrix(&mp, &q, 400.0f, &mm)) { CHECK(0, "matriz inválida th=%d yaw=%d pit=%d", th, yaw, pit); continue; }
        double pr = pit * PI / 180, h = (th + yaw) * PI / 180;
        double e[3] = {cos(pr) * sin(h), sin(pr), cos(pr) * cos(h)};
        float f[3]; vrfire_mat_forward(&mm, f);
        double fd[3] = {f[0], f[1], f[2]};
        double a = ang(e, fd); if (a > worst) worst = a;
        /* columnas ortonormales (4096 = 1,0) y orientación */
        double c[3][3];
        for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) c[j][i] = mm.m[3 * i + j] / 4096.0;
        for (int j = 0; j < 3; j++) { double n = sqrt(c[j][0] * c[j][0] + c[j][1] * c[j][1] + c[j][2] * c[j][2]); if (fabs(n - 1) > worstn) worstn = fabs(n - 1); }
        double det = c[0][0] * (c[1][1] * c[2][2] - c[1][2] * c[2][1]) - c[0][1] * (c[1][0] * c[2][2] - c[1][2] * c[2][0]) + c[0][2] * (c[1][0] * c[2][1] - c[1][1] * c[2][0]);
        if (fabs(det - 1) > fabs(worstdet - 1)) worstdet = det;
        /* t = boca + 400 f: la boca se obtiene restando */
        float w[3], pp[3] = {q.p.x, q.p.y, q.p.z}; vrfire_pos_to_world(&mp, pp, w);
        double dx = mm.t[0] - 400.0 * f[0] - w[0], dy = mm.t[1] - 400.0 * f[1] - w[1], dz = mm.t[2] - 400.0 * f[2] - w[2];
        CHECK(sqrt(dx * dx + dy * dy + dz * dz) < 2.0, "origen de la bala = boca (%.2f)", sqrt(dx * dx + dy * dy + dz * dz));
    }
    CHECK(worst < 0.02, "dirección frente a la fórmula independiente: %.4f grados", worst);
    CHECK(worstn < 0.001, "columnas unitarias: %.5f", worstn);
    CHECK(fabs(worstdet - 1) < 0.002, "orientación derecha (det %.4f)", worstdet);
    /* pitch hacia arriba = +Y del mundo; yaw a la izquierda = giro +X (yaw 1024) con theta 0 */
    WPose up = {{0, 1.6f, 0}, qypr(0, 90, 0)};
    VrFireMap m0 = mkmap(0, 0, 0, 0);
    vrfire_make_matrix(&m0, &up, 0, &m);
    float f[3]; vrfire_mat_forward(&m, f);
    CHECK(fabs(f[1] - 1) < 0.001, "pitch +90 mira arriba (+Y): %.3f", f[1]);
    WPose left = {{0, 1.6f, 0}, qypr(90, 0, 0)};
    vrfire_make_matrix(&m0, &left, 0, &m); vrfire_mat_forward(&m, f);
    CHECK(fabs(f[0] - 1) < 0.001 && fabs(f[2]) < 0.001, "yaw +90 (izquierda) con theta 0 mira a +X (%.3f %.3f)", f[0], f[2]);
    /* el desplazamiento: 0,5 m a la derecha del usuario con theta 0 = -X del mundo */
    WPose right = {{0.1f + 0.5f, 1.6f, -0.2f}, {0, 0, 0, 1}};
    vrfire_make_matrix(&m0, &right, 0, &m);
    CHECK(m.t[0] == -500 && m.t[1] == 0 && m.t[2] == 0, "0,5 m a la derecha = -500 en X del motor (%d %d %d)", m.t[0], m.t[1], m.t[2]);
    WPose high = {{0.1f, 1.6f + 0.25f, -0.2f}, {0, 0, 0, 1}};
    vrfire_make_matrix(&m0, &high, 0, &m); CHECK(m.t[1] == 250, "0,25 m más alto = +250 en Y (%d)", m.t[1]);
    VrFireMap m90 = mkmap(0, 0, 0, 90);
    WPose fw = {{0.1f, 1.6f, -0.2f - 1.0f}, {0, 0, 0, 1}};      /* 1 m hacia delante del usuario */
    vrfire_make_matrix(&m90, &fw, 0, &m);
    CHECK(abs(m.t[0] - 1000) <= 1 && abs(m.t[2]) <= 1, "theta 90: delante del usuario = +X del mundo (%d %d)", m.t[0], m.t[2]);
    /* RAM: ida y vuelta */
    static uint8_t ram[0x200000];
    VrFireMat a, b; vrfire_make_matrix(&map, &p, 400, &a);
    vrfire_store_mat(ram, 0x80123450u, &a); vrfire_load_mat(ram, 0x80123450u, &b);
    CHECK(mat_eq(&a, &b) && ram[0x123450 + 18] == 0 && ram[0x123450 + 19] == 0, "matriz en RAM (32 B, relleno 0)");
    int16_t rpad; memcpy(&rpad, ram + 0x123450 + 18, 2); CHECK(rpad == 0, "relleno");
    /* entradas degeneradas */
    VrFireMap bad = map; bad.valid = 0; CHECK(!vrfire_make_matrix(&bad, &p, 0, &m), "mapa inválido");
    bad = map; bad.units_per_m = 0; CHECK(!vrfire_make_matrix(&bad, &p, 0, &m), "unidades 0");
    WPose z = p; z.q.x = z.q.y = z.q.z = z.q.w = 0; CHECK(!vrfire_make_matrix(&map, &z, 0, &m), "cuaternión nulo");
    z = p; z.p.x = NAN; CHECK(!vrfire_make_matrix(&map, &z, 0, &m), "posición NaN");
    z = p; z.q.w = INFINITY; CHECK(!vrfire_make_matrix(&map, &z, 0, &m), "cuaternión inf");
    z = p; z.p.y = 1e30f; CHECK(vrfire_make_matrix(&map, &z, 0, &m) == 1 && m.t[1] <= 1000000000 && m.t[1] >= -1000000000, "posición enorme saturada");
    CHECK(!vrfire_make_matrix(&map, NULL, 0, &m) && !vrfire_make_matrix(NULL, &p, 0, &m) && !vrfire_make_matrix(&map, &p, 0, NULL), "NULL");
    /* desde el registro de la cámara */
    VrCamRecord r; memset(&r, 0, sizeof r);
    VrFireMap fm; vrfire_map_from_cam(&fm, &r); CHECK(!fm.valid, "registro inactivo");
    r.active = 1; r.eye[0] = 5; r.eye[1] = 6; r.eye[2] = 7; r.theta_play_deg = 33; r.p_ref[1] = 1.5f; r.units_per_m = 1000;
    vrfire_map_from_cam(&fm, &r); CHECK(fm.valid && fm.eye[2] == 7 && fm.theta_play_deg == 33 && fm.p_ref[1] == 1.5f, "mapa desde el registro");
    vrfire_map_from_cam(&fm, NULL); CHECK(!fm.valid, "registro NULL");
}

/* ---------------------------------------------------------------- juego falso para la lógica de pulsación */
typedef struct {
    int vb;
    int sq_seen;                          /* nivel de cuadrado muestreado (cada 2 VBlanks) */
    int sq_prev;
    int arm_t, armed, idle_t;             /* 17 VBlanks desde el primer toque; decae 22 VBlanks sin cuadrado */
    int pending;                          /* disparos en cola por soltar */
    int shot_at[8], nshot_at;
    int mag, weapon_hold;                 /* hold_fires: dispara cada 6 VBlanks mientras se mantiene */
    int next_auto;
    int bullets;                          /* proyectiles creados */
    int kind;                             /* tipo de proyectil que crea */
    VrFireMat last; int last_res;
    int16_t gpos[3], gvel[3];             /* kind 3: argumentos con los que el juego creó la granada */
    int pass_n, skip_n, subst_n;
    int first_vb;
} FGame;
static void fg_init(FGame *g, int mag, int hold) { memset(g, 0, sizeof *g); g->mag = mag; g->weapon_hold = hold; }
static void fg_projectile(FGame *g) {
    VrFireMat m; memset(&m, 0, sizeof m);
    int r = g->kind == 3 ? (vrfire_take_throw(g->gpos, g->gvel) ? VRFIRE_SUBST : VRFIRE_PASS) : vrfire_take_projectile(g->kind, &m);
    g->last_res = r; g->last = m;
    if (r == VRFIRE_SUBST) g->subst_n++; else if (r == VRFIRE_SKIP) g->skip_n++; else g->pass_n++;
    if (r != VRFIRE_SKIP) { g->bullets++; if (!g->first_vb) g->first_vb = g->vb; }
    g->mag--;                              /* el juego descuenta aunque no se cree la bala */
}
static void fg_tick(FGame *g, int square) {
    g->vb++;
    if (g->vb % 2 == 0) {
        int prev = g->sq_seen; g->sq_seen = square;
        if (square && !prev) { if (!g->arm_t && !g->armed) g->arm_t = 1; g->idle_t = 0; }
        if (!square && prev) { if (g->pending < 4) g->pending++; }
    }
    if (g->arm_t > 0 && !g->armed) { if (++g->arm_t >= 18) { g->armed = 1; g->arm_t = 0; g->idle_t = 0; } }
    if (g->armed) {
        if (!g->sq_seen) { if (++g->idle_t > 22) { g->armed = 0; g->pending = 0; } } else g->idle_t = 0;
        if (g->pending > 0 && !g->weapon_hold && g->nshot_at < 8) { g->shot_at[g->nshot_at++] = g->vb + 3; g->pending--; }
    }
    for (int i = 0; i < g->nshot_at; i++) if (g->shot_at[i] == g->vb) { if (g->mag > 0) fg_projectile(g); }
    if (g->weapon_hold && g->armed && g->sq_seen && g->vb >= g->next_auto && g->mag > 0) { fg_projectile(g); g->next_auto = g->vb + 6; }
}

static VrFireGame mkgame(const FGame *g) {
    VrFireGame r; memset(&r, 0, sizeof r);
    r.ok = 1; r.weapon = 0; r.armed = g->armed && g->sq_seen >= 0; r.hold_fires = g->weapon_hold; r.mag = g->mag; r.mag_max = 13; r.total = 40;
    return r;
}
static VrFireIn mkin(const VrFireMap *map) {
    VrFireIn in; memset(&in, 0, sizeof in);
    in.active = 1; in.map = map;
    in.muzzle.p.x = 0.2f; in.muzzle.p.y = 1.3f; in.muzzle.p.z = -0.5f; in.muzzle.q = qypr(20, 10, 0);
    in.fire_pose = in.muzzle;
    return in;
}
typedef struct { int min_press, min_rel, run, gap, last, presses; } PW;
static void pw_note(PW *p, int sq) {
    if (sq) { if (!p->last && p->gap > 0 && p->gap < p->min_rel) p->min_rel = p->gap; p->run++; p->gap = 0; }
    else { if (p->last && p->run > 0 && p->run < p->min_press) p->min_press = p->run; if (p->last) p->run = 0; p->gap++; }
    p->last = sq;
}
/* una pasada: llama a vrfire_frame y mueve el juego; devuelve el nivel de cuadrado */
static int vstep(FGame *g, const VrFireIn *in, PW *pw) {
    VrFireGame gs = mkgame(g); VrFireOut o;
    vrfire_frame(&gs, in, &o);
    int sq = o.engaged ? o.square : 0;
    if (pw) pw_note(pw, sq);
    fg_tick(g, sq);
    return sq;
}
static void settle(FGame *g, VrFireIn in, int n, PW *pw) { in.fire = 0; for (int i = 0; i < n; i++) vstep(g, &in, pw); }

static void init_trigger(void) { VrFireCfg c; vrfire_cfg_default(&c); vrfire_init(&c); }
static void init_raise(void) { VrFireCfg c; vrfire_cfg_default(&c); c.arm_mode = VRFIRE_ARM_ON_RAISE; vrfire_init(&c); }

static void t_single(void) {
    init_trigger();
    VrFireMap map = mkmap(100, 200, 300, 40); FGame g; fg_init(&g, 12, 0); PW pw = {1000, 1000, 0, 0, 0, 0};
    VrFireIn in = mkin(&map);
    settle(&g, in, 6, &pw);
    CHECK(g.bullets == 0 && pw.run == 0, "sin petición no se pulsa");
    in.fire = 1; vstep(&g, &in, &pw); in.fire = 0;
    settle(&g, in, 80, &pw);
    VrFireStats st; vrfire_stats(&st);
    CHECK(g.bullets == 1 && g.subst_n == 1 && g.pass_n == 0 && g.skip_n == 0, "una petición = un proyectil sustituido (%d/%d/%d)", g.subst_n, g.pass_n, g.skip_n);
    CHECK(st.requests == 1 && st.expired == 0 && st.subst == 1, "contadores %u %u %u", st.requests, st.expired, st.subst);
    CHECK(pw.min_press >= 2 && pw.min_rel >= 2, "pulsación >= 2 VBlanks (%d) y soltado >= 2 (%d)", pw.min_press, pw.min_rel);
    int sq_after = 0; for (int i = 0; i < 100; i++) sq_after += vstep(&g, &in, NULL);
    CHECK(sq_after == 0 && g.bullets == 1, "queda libre: sin pulsaciones ni disparos sobrantes");
    /* la matriz usa la pose de la petición con la bala adelantada 400 */
    VrFireMat m; WPose pz = in.muzzle; vrfire_make_matrix(&map, &pz, 400, &m);
    CHECK(mat_eq(&m, &g.last), "matriz = pose de la petición (+400)");
    CHECK(g.first_vb > 15 && g.first_vb < 50, "primer disparo a los %d VBlanks de la petición (levantar ~17 + 3)", g.first_vb - 6);
}
static void t_snapshot(void) {
    init_trigger();
    VrFireMap map = mkmap(0, 0, 0, 0); FGame g; fg_init(&g, 12, 0);
    VrFireIn in = mkin(&map);
    in.fire = 1; vstep(&g, &in, NULL); in.fire = 0;
    in.muzzle.q = qypr(-80, 0, 0); in.muzzle.p.y = 2.0f;         /* el brazo se mueve mientras el juego levanta el arma */
    settle(&g, in, 60, NULL);
    VrFireMat m; WPose p0 = mkin(&map).muzzle; vrfire_make_matrix(&map, &p0, 400, &m);
    CHECK(g.bullets == 1 && mat_eq(&m, &g.last), "la bala sale con la pose del instante del disparo, no con la actual");
}
static void t_burst(void) {
    init_trigger();
    VrFireMap map = mkmap(0, 0, 0, 0); FGame g; fg_init(&g, 12, 0); PW pw = {1000, 1000, 0, 0, 0, 0};
    VrFireIn in = mkin(&map);
    for (int k = 0; k < 3; k++) { in.fire = 1; vstep(&g, &in, &pw); in.fire = 0; settle(&g, in, 3, &pw); }
    settle(&g, in, 120, &pw);
    CHECK(g.bullets == 3 && g.subst_n == 3 && g.pass_n == 0 && g.skip_n == 0, "ráfaga de 3 = 3 proyectiles (%d)", g.bullets);
    CHECK(pw.min_press >= 2 && pw.min_rel >= 2, "anchos mínimos %d %d", pw.min_press, pw.min_rel);
    /* disparos seguidos con el arma ya levantada: cadencia alta */
    FGame h; fg_init(&h, 12, 0); in.fire = 1; vstep(&h, &in, NULL); in.fire = 0; settle(&h, in, 60, NULL);
    int b0 = h.bullets, v0 = h.vb;
    for (int k = 0; k < 6; k++) { in.fire = 1; vstep(&h, &in, NULL); in.fire = 0; settle(&h, in, 7, NULL); }
    settle(&h, in, 30, NULL);
    CHECK(h.bullets - b0 == 6, "6 disparos con el arma ya levantada = 6 proyectiles (%d)", h.bullets - b0);
    CHECK(h.vb - v0 < 6 * 8 + 30 + 1, "sin latencia de levantar (%d VBlanks)", h.vb - v0);
}
static void t_dry(void) {
    init_trigger();
    VrFireMap map = mkmap(0, 0, 0, 0); FGame g; fg_init(&g, 0, 0); PW pw = {1000, 1000, 0, 0, 0, 0};
    VrFireIn in = mkin(&map);
    in.fire = 1; vstep(&g, &in, &pw); in.fire = 0;
    int sq_total = 0;
    for (int i = 0; i < 400; i++) sq_total += vstep(&g, &in, &pw);
    VrFireStats st; vrfire_stats(&st);
    CHECK(g.bullets == 0 && st.expired == 1, "sin munición: la petición caduca (%u)", st.expired);
    CHECK(sq_total < 120, "no martillea el botón sin fin (%d VBlanks pulsado)", sq_total);
    int late = 0; for (int i = 0; i < 100; i++) late += vstep(&g, &in, NULL);
    CHECK(late == 0, "tras caducar queda libre");
}
static void t_raise(void) {
    init_raise();
    VrFireMap map = mkmap(0, 0, 0, 0); FGame g; fg_init(&g, 12, 0); PW pw = {1000, 1000, 0, 0, 0, 0};
    VrFireIn in = mkin(&map); in.ready = 1;
    settle(&g, in, 60, &pw);
    CHECK(g.sq_seen == 1 && g.armed, "con el arma levantada se mantiene el botón y el juego queda apuntando");
    int b0 = g.bullets, v0 = g.vb;
    in.fire = 1; vstep(&g, &in, &pw); in.fire = 0; settle(&g, in, 20, &pw);
    CHECK(g.bullets - b0 == 1 && g.vb - v0 <= 21, "disparo con latencia mínima (%d proyectil, %d VBlanks)", g.bullets - b0, g.vb - v0);
    CHECK(g.sq_seen == 1 || g.armed, "vuelve a mantener el botón");
    /* bajar el arma: el soltar sin petición dispara un tiro sobrante que se suprime */
    in.ready = 0; int skip0 = g.skip_n, pass0 = g.pass_n, mag0 = g.mag;
    settle(&g, in, 60, &pw);
    CHECK(g.skip_n - skip0 == 1 && g.pass_n == pass0, "el tiro de desarmado se suprime (%d) y no sale al mundo", g.skip_n - skip0);
    CHECK(g.mag == mag0 - 1, "el juego descontó 1 (coste del desarmado, documentado)");
    VrFireStats st; vrfire_stats(&st); CHECK(st.skipped == 1, "contador de suprimidos %u", st.skipped);
    int late = 0; for (int i = 0; i < 100; i++) late += vstep(&g, &in, NULL);
    CHECK(late == 0, "desarmado: botón libre");
    init_raise(); FGame h; fg_init(&h, 12, 0); in.ready = 1; settle(&h, in, 60, NULL); in.ready = 0; h.mag = 0;
    settle(&h, in, 60, NULL); CHECK(h.bullets == 0, "sin balas el desarmado no crea nada");
}
static void t_hold_auto(void) {
    init_trigger();
    VrFireMap map = mkmap(0, 0, 0, 0); FGame g; fg_init(&g, 25, 1); PW pw = {1000, 1000, 0, 0, 0, 0};
    VrFireIn in = mkin(&map); in.trigger_held = 1;
    settle(&g, in, 80, &pw);
    CHECK(g.bullets >= 4 && g.pass_n == 0 && g.skip_n == 0 && g.subst_n == g.bullets, "automática: todas las balas con la pose viva (%d)", g.bullets);
    VrFireMat m; WPose p = in.muzzle; vrfire_make_matrix(&map, &p, 400, &m);
    CHECK(mat_eq(&m, &g.last), "pose viva");
    in.trigger_held = 0; int b0 = g.bullets, p0 = g.pass_n;
    for (int i = 0; i < 4; i++) vstep(&g, &in, &pw);
    g.next_auto = g.vb; g.sq_seen = 1;                           /* una bala más sale justo tras soltar (cola del juego) */
    fg_projectile(&g);
    CHECK(g.pass_n == p0 && g.bullets == b0 + 1, "una bala tardía tras soltar usa aún la pose viva");
    settle(&g, in, 40, &pw);
    g.kind = 0; fg_projectile(&g);
    CHECK(g.pass_n == p0 + 1, "pasada la cola, un proyectil ajeno sigue su camino original");
    CHECK(pw.min_press >= 2, "pulsación >= 2");
    /* semiautomático sobre arma de mantener: una petición = un proyectil y se suelta */
    FGame h; fg_init(&h, 25, 1); in.trigger_held = 0;
    in.fire = 1; vstep(&h, &in, NULL); in.fire = 0; settle(&h, in, 100, NULL);
    CHECK(h.bullets == 1 && h.pass_n == 0 && h.skip_n == 0, "semi sobre arma de mantener: exactamente 1 (%d)", h.bullets);
}
static void t_kinds(void) {
    init_trigger();
    VrFireMap map = mkmap(10, 20, 30, 0); VrFireIn in = mkin(&map);
    float f[3]; VrFireMat m, mm;
    for (int kind = 0; kind < 3; kind++) {
        FGame g; fg_init(&g, 5, 0); g.kind = kind;
        in.fire = 1; vstep(&g, &in, NULL); in.fire = 0; settle(&g, in, 60, NULL);
        WPose p = in.muzzle; vrfire_make_matrix(&map, &p, kind == 0 ? 400.0f : 0.0f, &m);
        CHECK(g.bullets == 1 && mat_eq(&m, &g.last), "tipo %d: origen %s", kind, kind == 0 ? "boca + 400 por delante" : "la boca");
        (void)f; (void)mm;
    }
    /* resplandor: pistola = t - (400+130) f ; fusil = t - 400 f (tras una bala sustituida) */
    FGame g; fg_init(&g, 5, 0); in.fire = 1; vstep(&g, &in, NULL); in.fire = 0;
    for (int i = 0; i < 80 && !g.bullets; i++) vstep(&g, &in, NULL);          /* hasta que sale la bala */
    CHECK(g.bullets == 1, "salió la bala");
    VrFireMat fa, fb; CHECK(vrfire_take_flash(0, &fa) && vrfire_take_flash(1, &fb), "hay resplandor");
    vrfire_mat_forward(&g.last, f);
    for (int i = 0; i < 3; i++) {
        CHECK(abs((int)lrintf(g.last.t[i] - 530.0f * f[i]) - fa.t[i]) <= 1, "resplandor de pistola en la boca (eje %d)", i);
        CHECK(abs((int)lrintf(g.last.t[i] - 400.0f * f[i]) - fb.t[i]) <= 1, "resplandor de fusil en la boca (eje %d)", i);
    }
    CHECK(!memcmp(fa.m, g.last.m, sizeof fa.m), "misma orientación");
    CHECK(!vrfire_take_flash(0, NULL), "NULL");
    settle(&g, in, 10, NULL);
    VrFireMat st0; CHECK(!vrfire_take_flash(0, &st0) && !vrfire_take_flash(1, &st0), "resplandor de otro tirador (tarde) no se mueve a la boca VR");
    vrfire_init(NULL); VrFireMat z; CHECK(!vrfire_take_flash(0, &z), "sin bala sustituida no hay resplandor");
    /* proyectil sin cola ni petición: pasa; sin mapa: pasa; NULL */
    CHECK(vrfire_take_projectile(0, &z) == VRFIRE_PASS && vrfire_take_projectile(0, NULL) == VRFIRE_PASS, "sin petición pasa");
}
static void t_throw(void) {
    init_trigger();
    VrFireMap map = mkmap(0, 0, 0, 0); VrFireIn in = mkin(&map);
    FGame g; fg_init(&g, 5, 0);
    vstep(&g, &in, NULL);
    float p[3] = {0.1f + 0.3f, 1.6f + 0.2f, -0.2f - 0.4f}, v[3] = {1.0f, 2.0f, -5.0f};
    CHECK(vrfire_set_throw(p, v, 0.04f) == 1, "lanzamiento admitido");
    in.fire = 1; vstep(&g, &in, NULL); in.fire = 0;
    int16_t pos[3], vel[3];
    CHECK(vrfire_take_throw(pos, vel) == 1, "datos de lanzamiento");
    /* 1 VBlank de retardo (medio tick): la posición avanza v*n y la gravedad del juego (16 unidades/tick^2 sobre Y del motor) la curva; la velocidad pierde 16*n */
    CHECK(pos[0] == -320 && pos[1] >= 237 && pos[1] <= 239 && pos[2] == 500, "posición del motor con 1 VBlank de retardo (%d %d %d)", pos[0], pos[1], pos[2]);
    CHECK(vel[0] == -40 && vel[1] >= 71 && vel[1] <= 73 && vel[2] == 200, "velocidad por tick (%d %d %d)", vel[0], vel[1], vel[2]);
    /* 16 VBlanks de retardo (la granada sale tras la animación): 0,32 s de trayectoria */
    CHECK(vrfire_set_throw(p, v, 0.04f) == 1, "segundo lanzamiento");
    settle(&g, in, 16, NULL);
    CHECK(vrfire_take_throw(pos, vel) == 1, "datos con 16 VBlanks");
    /* 16 VBlanks = 8 ticks: la gravedad del juego (16 unidades/tick^2, medida en el paso del actor de la granada) da 0,5*16*64 = 512 y 16*8 = 128 */
    CHECK(pos[0] == -620 && pos[2] == 2000 && pos[1] >= 200 + 640 - 512 - 2 && pos[1] <= 200 + 640 - 512 + 2, "posición adelantada 8 ticks con la gravedad del juego (%d %d %d)", pos[0], pos[1], pos[2]);
    CHECK(vel[1] >= 80 - 128 - 2 && vel[1] <= 80 - 128 + 2, "velocidad vertical menos 16 por tick (%d)", vel[1]);
    CHECK(vrfire_take_throw(pos, vel) == 0, "se consume una vez");
    CHECK(!vrfire_set_throw(NULL, v, 0.04f) && !vrfire_set_throw(p, NULL, 0.04f) && !vrfire_set_throw(p, v, 0) && !vrfire_set_throw(p, v, NAN), "argumentos inválidos");
    CHECK(vrfire_set_throw(p, v, 0.04f) == 1, "de nuevo");
    settle(&g, in, 100, NULL);
    CHECK(vrfire_take_throw(pos, vel) == 0, "datos de lanzamiento no consumidos caducan (no se aplican a una granada ajena)");
    float big[3] = {1e9f, 0, 0}; CHECK(vrfire_set_throw(big, v, 0.04f) == 1 && vrfire_take_throw(pos, vel) == 1 && pos[0] == -32768, "saturado a s16");
    float nan3[3] = {NAN, 0, 0}; CHECK(vrfire_set_throw(nan3, v, 0.04f) == 0, "NaN rechazado");
    /* una petición pendiente se atiende con el lanzamiento aunque no haya datos */
    in.fire = 1; vstep(&g, &in, NULL); in.fire = 0;
    CHECK(vrfire_take_throw(pos, vel) == 0, "sin datos: el juego lanza con los suyos"); 
    VrFireStats st; vrfire_stats(&st); CHECK(st.passed >= 1, "contado como pasado");
}
/* Granada preparada (botón mantenido) que se suelta sin lanzar (arma fuera / seguimiento perdido): el juego no la lanza con su velocidad por defecto, cae suave desde la boca. */
static void t_grenade_cancel(void) {
    for (int cas = 0; cas < 2; cas++) {
        init_trigger();
        VrFireMap map = mkmap(0, 0, 0, 0); VrFireIn in = mkin(&map);
        FGame g; fg_init(&g, 5, 0); g.kind = 3;
        in.raise = 1; in.ready = 1;
        settle(&g, in, 40, NULL);
        CHECK(g.armed && g.bullets == 0, "granada lista: botón mantenido, sin lanzar (armado %d, %d)", g.armed, g.bullets);
        if (cas == 0) { in.ready = 0; settle(&g, in, 60, NULL); }               /* deja de estar lista (arma cambiada) con seguimiento */
        else { in.active = 0; settle(&g, in, 60, NULL); }                       /* seguimiento perdido */
        CHECK(g.bullets == 1 && g.subst_n == 1 && g.pass_n == 0, "caso %d: la suelta se sustituye, no sale con la velocidad del juego (%d/%d)", cas, g.subst_n, g.pass_n);
        CHECK(g.gvel[0] == 0 && g.gvel[1] == 0 && g.gvel[2] == 0, "caso %d: sin velocidad (%d %d %d)", cas, g.gvel[0], g.gvel[1], g.gvel[2]);
        CHECK(g.gpos[0] == -100 && g.gpos[1] == -300 && g.gpos[2] == 300, "caso %d: cae desde la mano (%d %d %d)", cas, g.gpos[0], g.gpos[1], g.gpos[2]);
    }
}
/* ---- granada: conversión velocidad de la mano -> unidades/tick del motor, contra la física del propio actor del juego ----
 * Medido en el código del juego (0x8006a8f0, paso del actor; 0x8006af98 lo crea con (pos*, vel*, ...)): cada tick pos += vel y, en el aire, vel.y -= 16 (unidades/tick^2).
 * Su lanzamiento por defecto sale con (0, 128, 150) unidades/tick en el marco del jugador (arriba, delante) = 3,2 y 3,75 m/s a 1000 unidades/m y 0,04 s/tick. */
static double sim_game(const int16_t p0[3], const int16_t v0[3], double floor_y, double *lx, double *lz) {
    double p[3] = {p0[0], p0[1], p0[2]}, v[3] = {v0[0], v0[1], v0[2]};
    for (int t = 0; t < 2000; t++) { for (int i = 0; i < 3; i++) p[i] += v[i]; v[1] -= 16.0; if (p[1] <= floor_y) break; }
    if (lx) *lx = p[0];
    if (lz) *lz = p[2];
    return sqrt((p[0] - p0[0]) * (p[0] - p0[0]) + (p[2] - p0[2]) * (p[2] - p0[2]));
}
static void t_throw_game(void) {
    init_trigger();
    VrFireMap map = mkmap(0, 0, 0, 0); VrFireIn in = mkin(&map);
    FGame g; fg_init(&g, 5, 0);
    vstep(&g, &in, NULL);
    const double FLOOR = -1600;                                  /* ojo del usuario a 1,6 m del suelo; la mano (y usuario 1,2) a 1,2 m */
    float p[3] = {0.1f, 1.2f, -0.2f}; int16_t pos[3], vel[3];
    /* lanzamiento por defecto del juego reproducido con la mano: 3,2 m/s arriba y 3,75 m/s hacia delante (-Z del usuario) = (0, 128, 150) por tick */
    float vd[3] = {0.0f, 3.2f, -3.75f};
    CHECK(vrfire_set_throw(p, vd, 0.04f) && vrfire_take_throw(pos, vel), "lanzamiento");
    CHECK(vel[0] == 0 && vel[1] == 128 && vel[2] == 150, "m/s -> unidades/tick: 40 por m/s (%d %d %d)", vel[0], vel[1], vel[2]);
    CHECK(pos[0] == 0 && pos[1] == -400 && pos[2] == 0, "posición de la mano en el motor (%d %d %d)", pos[0], pos[1], pos[2]);
    const int16_t refv[3] = {0, 128, 150};
    double r0 = sim_game(pos, refv, FLOOR, NULL, NULL), r1 = sim_game(pos, vel, FLOOR, NULL, NULL);
    CHECK(r0 > 2500 && r0 < 4500, "alcance del lanzamiento por defecto del juego %.0f unidades (~3,4 m)", r0);
    CHECK(fabs(r1 - r0) < 1.0, "el lanzamiento por defecto reproducido con la mano cae igual (%.0f / %.0f)", r1, r0);
    /* lanzamiento medio de una mano real: 5 m/s a 45 grados -> alcance del orden del por defecto (v^2 manda: x1,5 como mucho) */
    float vm[3] = {0.0f, 3.54f, -3.54f};
    CHECK(vrfire_set_throw(p, vm, 0.04f) && vrfire_take_throw(pos, vel), "lanzamiento medio");
    double rm = sim_game(pos, vel, FLOOR, NULL, NULL);
    CHECK(rm > 0.8 * r0 && rm < 1.5 * r0, "alcance de un lanzamiento medio %.0f frente al del juego %.0f", rm, r0);
    /* barrido sintético: módulo = 40 unidades/tick por m/s, dirección conservada (rotación del mapa), la vertical sigue siendo +Y del motor */
    srand(7);
    for (int k = 0; k < 200; k++) {
        VrFireMap mp = mkmap(rand() % 2000 - 1000, rand() % 2000 - 1000, rand() % 2000 - 1000, (float)(rand() % 360 - 180));
        vstep(&g, &(VrFireIn){ .active = 1, .map = &mp, .muzzle = in.muzzle, .fire_pose = in.fire_pose }, NULL);
        float v[3] = {(float)(rand() % 2000 - 1000) / 100.0f, (float)(rand() % 2000 - 1000) / 100.0f, (float)(rand() % 2000 - 1000) / 100.0f};
        if (!(vrfire_set_throw(p, v, 0.04f) && vrfire_take_throw(pos, vel))) { CHECK(0, "lanzamiento %d", k); continue; }
        double ml = sqrt((double)v[0] * v[0] + (double)v[1] * v[1] + (double)v[2] * v[2]) * 40.0, ol = sqrt((double)vel[0] * vel[0] + (double)vel[1] * vel[1] + (double)vel[2] * vel[2]);
        CHECK(fabs(ol - ml) <= 1.0, "módulo %.1f frente a %.1f", ol, ml);
        CHECK(abs(vel[1] - (int)lrintf(v[1] * 40.0f)) <= 1, "la vertical del usuario es la Y del motor (%d frente a %.1f)", vel[1], v[1] * 40.0f);
    }
    /* retardo hasta la creación: la trayectoria extrapolada y la continuada con la física del juego aterrizan en el mismo punto (tolerancia: discretización por tick) */
    init_trigger(); fg_init(&g, 5, 0); in = mkin(&map); vstep(&g, &in, NULL);
    float vt[3] = {1.0f, 2.0f, -4.0f};
    int16_t p0[3], v0[3];
    CHECK(vrfire_set_throw(p, vt, 0.04f) && vrfire_take_throw(p0, v0), "sin retardo");
    CHECK(vrfire_set_throw(p, vt, 0.04f), "otra vez");
    settle(&g, in, 12, NULL);
    CHECK(vrfire_take_throw(pos, vel), "con 12 VBlanks de retardo");
    double lx0, lz0, lx1, lz1;
    sim_game(p0, v0, FLOOR, &lx0, &lz0); sim_game(pos, vel, FLOOR, &lx1, &lz1);
    CHECK(fabs(lx0 - lx1) < 150 && fabs(lz0 - lz1) < 150, "aterriza en el mismo sitio (%.0f,%.0f) / (%.0f,%.0f)", lx0, lz0, lx1, lz1);
}
/* Velocidad de la mano -> alcance de la granada en el motor, con el retardo real de 16 VBlanks hasta la creación: a más velocidad, más lejos (estrictamente); la mano quieta o casi quieta cae cerca; la fuerte llega lejos.
 * Nada en la cadena iguala los lanzamientos ni los recorta a cero. */
static void t_throw_prop(void) {
    init_trigger();
    VrFireMap map = mkmap(0, 0, 0, 0); VrFireIn in = mkin(&map);
    FGame g; fg_init(&g, 5, 0);
    vstep(&g, &in, NULL);
    const double FLOOR = -1600;
    float p[3] = {0.1f, 1.2f, -0.2f};
    int16_t pos[3], vel[3], pos0[3], vel0[3];
    const float sp[] = {0.0f, 0.3f, 0.6f, 1.0f, 2.0f, 4.0f, 6.0f, 9.0f, 13.0f, 18.0f, 22.0f};
    double prev = -1, r_of[11]; int mono = 1;
    for (unsigned k = 0; k < sizeof sp / sizeof *sp; k++) {
        float v[3] = {0.0f, sp[k] * 0.70710678f, -sp[k] * 0.70710678f};
        CHECK(vrfire_set_throw(p, v, 0.04f) && vrfire_take_throw(pos0, vel0), "v=%.1f: lanzamiento sin retardo", sp[k]);
        CHECK(vrfire_set_throw(p, v, 0.04f), "v=%.1f: lanzamiento", sp[k]);
        settle(&g, in, 16, NULL);
        CHECK(vrfire_take_throw(pos, vel), "v=%.1f: datos tras 16 VBlanks", sp[k]);
        double lx, lz; sim_game(pos, vel, FLOOR, &lx, &lz);
        double r = sqrt((lx - pos0[0]) * (lx - pos0[0]) + (lz - pos0[2]) * (lz - pos0[2]));      /* desde la mano */
        r_of[k] = r;
        if (!(r > prev)) mono = 0;
        prev = r;
        double hv = sqrt((double)vel[0] * vel[0] + (double)vel[2] * vel[2]);
        CHECK(fabs(hv - sp[k] * 0.70710678 * 40.0) <= 1.0, "v=%.1f: velocidad horizontal por tick %.1f (esperada %.1f)", sp[k], hv, sp[k] * 0.70710678 * 40.0);
    }
    CHECK(mono, "a más velocidad de la mano, más alcance (estrictamente): %.0f %.0f %.0f %.0f ... %.0f", r_of[0], r_of[1], r_of[2], r_of[3], r_of[10]);
    CHECK(r_of[0] < 10.0, "mano quieta: cae a la mano (%.0f unidades)", r_of[0]);
    CHECK(r_of[3] < 1500.0, "suave (1 m/s): cerca (%.0f unidades)", r_of[3]);
    CHECK(r_of[7] > 8000.0 && r_of[8] > 1.5 * r_of[7], "fuerte (9 m/s): lejos (%.0f) y 13 m/s más lejos aún (%.0f)", r_of[7], r_of[8]);
}
/* Gesto de la granada: mano cerrada = botón del juego mantenido (el juego queda "apuntando"); abrirla lanza con la velocidad de la mano al soltar el botón. */
static void t_grenade_gesture(void) {
    init_trigger();
    VrFireMap map = mkmap(0, 0, 0, 0); FGame g; fg_init(&g, 3, 0); g.kind = 3;
    VrFireIn in = mkin(&map); in.raise = 1; in.ready = 1;
    settle(&g, in, 60, NULL);
    CHECK(g.sq_seen == 1 && g.armed && g.bullets == 0, "mano cerrada: botón mantenido, el juego listo y sin lanzar");
    float p[3] = {0.1f, 1.2f, -0.2f}, v[3] = {0.0f, 3.2f, -3.75f};
    CHECK(vrfire_set_throw(p, v, 0.04f) == 1, "velocidad de la mano");
    in.ready = 0; in.fire = 1; int v0 = g.vb; vstep(&g, &in, NULL); in.fire = 0; settle(&g, in, 30, NULL);
    CHECK(g.bullets == 1 && g.subst_n == 1 && g.pass_n == 0 && g.skip_n == 0, "un lanzamiento sustituido (%d/%d/%d)", g.subst_n, g.pass_n, g.skip_n);
    CHECK(g.vb - v0 <= 31, "la creación sale al soltar el botón (sin otra espera de apuntado)");
    CHECK(g.gvel[1] >= 128 - 16 * 5 && g.gvel[1] <= 128 && g.gvel[2] >= 148 && g.gvel[2] <= 150, "velocidad del motor (%d,%d,%d)", g.gvel[0], g.gvel[1], g.gvel[2]);
    VrFireStats st; vrfire_stats(&st); CHECK(st.presses == 1 && st.releases == 1, "una pulsación y un soltar (%u/%u)", st.presses, st.releases);
    int late = 0; for (int i = 0; i < 80; i++) late += vstep(&g, &in, NULL);
    CHECK(late == 0 && g.bullets == 1, "después, botón libre y sin lanzamientos sobrantes");
}
/* Seguimiento diagnóstico del actor de la granada: una línea cada 0,25 s durante 3 s. */
static uint8_t gram[0x200000];
static void gw16(uint32_t a, int v) { int16_t x = (int16_t)v; memcpy(gram + (a & 0x1FFFFF), &x, 2); }
static void gw32(uint32_t a, uint32_t v) { memcpy(gram + (a & 0x1FFFFF), &v, 4); }
static int gtrack(float dt, int n, int *lines_out, char *buf, size_t bufn) {
    fflush(stderr);
    FILE *f = tmpfile(); int sv = dup(2); dup2(fileno(f), 2);
    int lines = 0;
    for (int i = 0; i < n; i++) lines += vrfire_grenade_track(gram, dt);
    fflush(stderr); dup2(sv, 2); close(sv);
    rewind(f); size_t got = fread(buf, 1, bufn - 1, f); buf[got] = 0; fclose(f);
    *lines_out = lines;
    return (int)got;
}
static void t_grenade_track(void) {
    const uint32_t A = 0x80150000u, OBJ = A + 0x20, Q = 0x80160000u;
    memset(gram, 0, sizeof gram);
    gw16(OBJ, 100); gw16(OBJ + 2, 200); gw16(OBJ + 4, 300); gw32(OBJ + 0x28, 0x1b); gw16(OBJ + 0x2C, 1); gw32(A + 0x100, 168);
    gw16(A + 0x108, 0); gw16(A + 0x10A, 128); gw16(A + 0x10C, 150);
    gw32(0x800ACAF8u, 1); gw16(0x800B2B7Cu + 0x56, 2); gw32(0x800B2B7Cu + 0x58, Q); gw32(Q, 0x80170000u); gw32(Q + 4, OBJ);
    static char buf[16384]; int lines;
    VrFireMat m; memset(&m, 0, sizeof m);
    vrfire_init(NULL);
    gtrack(0.02f, 10, &lines, buf, sizeof buf);
    CHECK(lines == 0, "sin granada creada no escribe");
    vrfire_probe(3, 1, 0, &m);
    gtrack(0.02f, 10, &lines, buf, sizeof buf);
    CHECK(lines == 0, "creación fallida (actor 0): no sigue nada");
    vrfire_probe(3, 1, A, &m);
    gtrack(0.02f, 200, &lines, buf, sizeof buf);
    CHECK(lines == 12, "una línea cada 0,25 s durante 3 s = 12 (%d)", lines);
    CHECK(strstr(buf, "[granada] t=0.26 actor=80150000 pos=(100,200,300) vel=(0,128,150) mecha=168 | objeto bandera=01b oculto=0 grupo_ok=1 en_cola=1") != NULL, "contenido de la primera línea: %.200s", buf);
    CHECK(strstr(buf, "t=3.0") != NULL, "la última línea es la de 3 s");
    gtrack(0.02f, 50, &lines, buf, sizeof buf);
    CHECK(lines == 0, "pasados 3 s deja de escribir");
    /* lo que impediría dibujarla se ve en la línea: oculta por el juego, fuera de grupo, fuera de la cola */
    gw32(OBJ + 0x28, 0x9b); gw32(0x800ACAF8u, 2); gw32(Q + 4, 0);
    vrfire_probe(3, 1, A, &m);
    gtrack(0.25f, 1, &lines, buf, sizeof buf);
    CHECK(lines == 1 && strstr(buf, "oculto=1 grupo_ok=0 en_cola=0") != NULL, "diagnostica oculta / grupo / cola: %.200s", buf);
    vrfire_probe(3, 1, A, &m);       /* un lanzamiento nuevo reinicia el seguimiento */
    gtrack(0.25f, 1, &lines, buf, sizeof buf);
    CHECK(lines == 1 && strstr(buf, "t=0.25") != NULL, "otro lanzamiento reinicia la cuenta");
    CHECK(vrfire_grenade_track(NULL, 0.02f) == 0, "RAM nula");
    gtrack(0.25f, 20, &lines, buf, sizeof buf);          /* termina el seguimiento en curso */
    vrfire_probe(3, 1, 0x80200000u, &m);   /* actor fuera de RAM */
    gtrack(0.25f, 8, &lines, buf, sizeof buf);
    CHECK(lines == 0, "actor fuera de RAM: no sigue nada");
}
/* Petición de vida larga (mina: el juego la crea ~70 VBlanks después, al terminar de agacharse): sustituye en vez de caducar, y la liberación en curso
 * no provoca una segunda pulsación mientras espera. Con la vida por defecto (60) la petición caduca y el proyectil pasa. */
static void t_long_life(void) {
    for (int life = 0; life <= 150; life += 150) {
        init_trigger();
        VrFireMap map = mkmap(100, 200, 300, 40); VrFireIn in = mkin(&map);
        VrFireGame gs; memset(&gs, 0, sizeof gs); gs.ok = 1; gs.armed = 1; gs.mag = 1; gs.mag_max = 13; gs.total = 4;
        in.fire = 1; in.fire_life = life;
        VrFireOut o; vrfire_frame(&gs, &in, &o); in.fire = 0;
        for (int i = 0; i < 100; i++) vrfire_frame(&gs, &in, &o);
        VrFireStats st; vrfire_stats(&st);
        VrFireMat m; int r = vrfire_take_projectile(2, &m);
        if (life) CHECK(r == VRFIRE_SUBST && st.expired == 0 && st.presses == 1, "vida larga: sustituye tras 100 VBlanks (r=%d exp=%u pulsaciones=%u)", r, st.expired, st.presses);
        else CHECK(r == VRFIRE_PASS && st.expired == 1, "vida por defecto: caduca (r=%d exp=%u)", r, st.expired);
        in.fire = 1; in.fire_life = -5; vrfire_frame(&gs, &in, &o); in.fire = 0;       /* vida negativa = la de la configuración */
        for (int i = 0; i < 70; i++) vrfire_frame(&gs, &in, &o);
        vrfire_stats(&st);
        CHECK(vrfire_take_projectile(2, &m) == VRFIRE_PASS && st.expired >= 1, "vida negativa = por defecto");
    }
}
static void t_invalid_input(void) {
    init_trigger();
    FGame g; fg_init(&g, 5, 0);
    VrFireMap bad; memset(&bad, 0, sizeof bad);
    VrFireIn in = mkin(&bad); in.fire = 1;
    for (int i = 0; i < 100; i++) { vstep(&g, &in, NULL); in.fire = 0; }
    CHECK(g.bullets == 0 && g.sq_seen == 0, "sin correspondencia no se pulsa ni se sustituye");
    VrFireMap map = mkmap(0, 0, 0, 0); in = mkin(&map); in.active = 0; in.fire = 1;
    for (int i = 0; i < 100; i++) { vstep(&g, &in, NULL); in.fire = 0; }
    CHECK(g.bullets == 0, "inactivo no hace nada");
    in.active = 1; in.map = NULL; in.fire = 1; for (int i = 0; i < 10; i++) vstep(&g, &in, NULL);
    VrFireGame gs = mkgame(&g); gs.ok = 0; in.map = &map; in.fire = 1; VrFireOut o; vrfire_frame(&gs, &in, &o);
    CHECK(!o.square && !o.engaged, "lectura del juego no válida");
    vrfire_frame(NULL, &in, &o); CHECK(!o.engaged, "NULL");
    vrfire_frame(&gs, NULL, &o); CHECK(!o.engaged, "NULL (2)");
}
/* ---------------------------------------------------------------- RAM sintética: leer el juego y ocultar el arma */
static uint8_t ram[0x200000];
static void w32(uint32_t a, uint32_t v) { memcpy(ram + (a & 0x1FFFFF), &v, 4); }
static void w16(uint32_t a, int v) { int16_t x = (int16_t)v; memcpy(ram + (a & 0x1FFFFF), &x, 2); }
static uint32_t r32(uint32_t a) { uint32_t v; memcpy(&v, ram + (a & 0x1FFFFF), 4); return v; }
static int r16(uint32_t a) { int16_t v; memcpy(&v, ram + (a & 0x1FFFFF), 2); return v; }
#define PW_BASE 0x80100000u
#define WACT 0x80110000u
#define OBJS 0x80120000u
#define PRIM1 0x80130000u
#define PRIM2 0x80131000u
static void build_ram(int n, int weapon) {
    memset(ram, 0, sizeof ram);
    w32(BA_PLAYER_CTRL, PW_BASE + 0x20);
    w32(PW_BASE + 0x904, WACT); w32(PW_BASE + 0x910, 1); w32(PW_BASE + 0x91C, 0x9042);
    w32(WACT + 0x20, OBJS); w32(WACT + 0x58, PRIM1); w32(WACT + 0x10C, PRIM2);
    w16(OBJS + 0x2E, n);
    for (int i = 0; i < n; i++) w16(OBJS + 0x48 + 0x5C * (uint32_t)i + 0x4C, 0x1234 + i);
    w32(PRIM1 + 0x24, 0x15); w32(PRIM2 + 0x24, 0x409);
    w16(BA_VARS + 2 * BRIDGE_V_WEAPON, weapon);
}
static void t_hide(void) {
    build_ram(4, 0);
    CHECK(vrfire_hide_weapon(ram) == 4, "oculta los 4 modelos");
    for (int i = 0; i < 4; i++) CHECK(r16(OBJS + 0x48 + 0x5C * (uint32_t)i + 0x4C) == 0, "límite %d a 0", i);
    CHECK(r32(PRIM1 + 0x24) == (0x15u | 0x100u) && r32(PRIM2 + 0x24) == (0x409u | 0x100u), "haz y punto de la pistola invisibles");
    CHECK(vrfire_hide_weapon(ram) == 4 && r32(PRIM1 + 0x24) == 0x115u, "idempotente");
    build_ram(3, 1);
    CHECK(vrfire_hide_weapon(ram) == 3 && r32(PRIM1 + 0x24) == 0x15u, "solo la pistola tiene haz/punto");
    build_ram(2, 0); w32(WACT + 0x58, 0); w32(WACT + 0x10C, 0x00000123);
    CHECK(vrfire_hide_weapon(ram) == 2, "primitivas nulas/ inválidas se ignoran");
    build_ram(2, 0); w32(PW_BASE + 0x904, 0); CHECK(vrfire_hide_weapon(ram) == 0, "sin arma equipada");
    build_ram(2, 0); w32(BA_PLAYER_CTRL, 0); CHECK(vrfire_hide_weapon(ram) == 0, "sin jugador");
    build_ram(2, 0); w32(BA_PLAYER_CTRL, 0x801FFFF0u); CHECK(vrfire_hide_weapon(ram) == 0, "puntero al final de la RAM");
    build_ram(2, 0); w32(WACT + 0x20, 0x801FFFF0u); CHECK(vrfire_hide_weapon(ram) == 0, "objs al final de la RAM");
    build_ram(0, 0); CHECK(vrfire_hide_weapon(ram) == 0, "0 modelos");
    build_ram(200, 0); CHECK(vrfire_hide_weapon(ram) == 0, "nº de modelos absurdo");
    build_ram(2, 0); w16(OBJS + 0x2E, -5); CHECK(vrfire_hide_weapon(ram) == 0, "nº negativo");
    build_ram(63, 0); w32(WACT + 0x20, 0x801FFFF0u - 0x5C * 30u); CHECK(vrfire_hide_weapon(ram) == 0, "los modelos no caben en la RAM");
    build_ram(2, 0); w32(WACT + 0x20, 0xA0120000u); CHECK(vrfire_hide_weapon(ram) == 0, "puntero KSEG1 rechazado (solo KSEG0)");
    build_ram(2, 0); w32(WACT + 0x20, 0x80200000u + 0x120000u); CHECK(vrfire_hide_weapon(ram) == 0, "espejo de la RAM por encima de 2 MB rechazado");
    build_ram(2, 0); w32(BA_PLAYER_CTRL, 0x80200000u + PW_BASE + 0x20 - 0x80000000u); CHECK(vrfire_hide_weapon(ram) == 0, "control en el espejo rechazado");
    build_ram(2, 0); w32(PW_BASE + 0x904, 0x801FFF80u); CHECK(vrfire_hide_weapon(ram) == 0, "actor demasiado cerca del final (lectura de la pistola no cabe)");
    build_ram(2, 0); w32(PW_BASE + 0x904, 0x80140000u); w16(0x80140000u + 0x20u, 100); w16(0x80140000u + 0x22u, 200); w16(0x80140000u + 0x24u, 300);
    CHECK(vrfire_hide_weapon(ram) == 0, "actor del arma reciclado como granada (+0x20 = posición del proyectil): no se toca");
    CHECK(!vrfire_hide_enabled(), "desactivado por defecto"); vrfire_set_hide(1); CHECK(vrfire_hide_enabled(), "activado"); vrfire_set_hide(0);
}
static void t_read_game(void) {
    build_ram(2, 0);
    BridgeState bs; memset(&bs, 0, sizeof bs); bs.weapon = 3; bs.weapon_ammo[3] = 7; bs.magazine = 4; bs.magazine_max = 12;
    VrFireGame g;
    vrfire_read_game(ram, &bs, &g);
    CHECK(g.ok && g.weapon == 3 && g.mag == 4 && g.mag_max == 12 && g.total == 7 && g.armed && !g.hold_fires, "lectura del juego (arma %d armed %d hold %d)", g.weapon, g.armed, g.hold_fires);
    w32(PW_BASE + 0x910, 0); w32(PW_BASE + 0x91C, 0x9043);
    vrfire_read_game(ram, &bs, &g); CHECK(g.ok && !g.armed && g.hold_fires, "armed/hold_fires de la RAM");
    bs.weapon = -1; vrfire_read_game(ram, &bs, &g); CHECK(g.weapon == -1 && g.total == 0, "sin arma");
    w32(BA_PLAYER_CTRL, 0); vrfire_read_game(ram, &bs, &g); CHECK(!g.ok, "sin jugador");
    vrfire_read_game(NULL, &bs, &g); CHECK(!g.ok, "RAM NULL"); vrfire_read_game(ram, NULL, &g); CHECK(!g.ok, "bs NULL");
}
/* ---------------------------------------------------------------- robustez */
static void t_fuzz(void) {
    srand(99);
    for (int mode = 0; mode < 2; mode++) {
        if (mode) init_raise(); else init_trigger();
        FGame g; fg_init(&g, 30, 0);
        VrFireMap map = mkmap(1, 2, 3, 0);
        for (int it = 0; it < 300000; it++) {
            VrFireIn in = mkin(&map);
            if (rand() % 3 == 0) map.theta_play_deg = (float)(rand() % 720 - 360);
            if (rand() % 50 == 0) map.valid = rand() & 1;
            if (rand() % 40 == 0) in.muzzle.q = qypr(rand() % 360, rand() % 180 - 90, rand() % 360);
            if (rand() % 200 == 0) in.muzzle.p.x = (rand() & 1) ? NAN : 1e20f;
            if (rand() % 400 == 0) in.muzzle.q.w = (rand() & 1) ? INFINITY : 0.0f;
            in.fire = rand() % 15 == 0; in.ready = rand() % 100 < 50; in.trigger_held = rand() % 100 < 30; in.active = rand() % 20 != 0;
            VrFireGame gs = mkgame(&g); gs.hold_fires = (it / 20000) & 1; if (rand() % 100 == 0) gs.ok = rand() & 1;
            if (it % 5000 == 0) g.mag = 30;
            VrFireOut o; vrfire_frame(&gs, &in, &o);
            CHECK(o.square == 0 || o.square == 1, "square 0/1");
            CHECK(!o.square || o.engaged, "pulsa solo si controla");
            fg_tick(&g, o.engaged ? o.square : 0);
            if (it % 61 == 0) { VrFireMat m; int r = vrfire_take_projectile(rand() % 4, &m); CHECK(r == VRFIRE_PASS || r == VRFIRE_SUBST || r == VRFIRE_SKIP, "decisión válida"); }
            if (it % 97 == 0) { VrFireMat m; vrfire_take_flash(rand() & 1, &m); }
            if (it % 211 == 0) { float p[3] = {(float)rand() / 1000, 1, 0}, v[3] = {1, 2, 3}; int16_t a[3], b[3]; vrfire_set_throw(p, v, 0.04f); vrfire_take_throw(a, b); }
        }
    }
    VrFireStats s; vrfire_stats(&s);
    CHECK(s.requests > 0, "hubo peticiones");
}
static void t_cfg(void) {
    VrFireCfg c; memset(&c, 0, sizeof c); c.press_vbl = -3; c.release_vbl = 0; c.req_window = 0; c.arm_timeout = 0;
    vrfire_init(&c);
    CHECK(vrfire_config()->press_vbl >= 2 && vrfire_config()->release_vbl >= 2 && vrfire_config()->req_window >= 8 && vrfire_config()->arm_timeout >= 4, "configuración saneada");
    vrfire_init(NULL); CHECK(vrfire_config()->press_vbl == 2 && vrfire_config()->bullet_shift == 400.0f && vrfire_config()->arm_mode == VRFIRE_ARM_ON_TRIGGER, "defecto");
}

/* inversas mundo -> usuario: ida y vuelta con cualquier giro y ojo; el misil guiado las usa para colocar la cámara de la pantalla */
static void t_inverse(void) {
    for (int th = -180; th <= 180; th += 15) {
        VrFireMap map = mkmap(1234, -567, 8910, (float)th);
        for (int i = 0; i < 40; i++) {
            float p[3] = {(float)(i % 7) * 0.9f - 2.0f, 0.3f + (float)(i % 5) * 0.5f, (float)(i % 9) * 1.1f - 4.0f}, w[3], b[3], d[3] = {0.3f, -0.5f, 0.8f}, dw[3], db[3];
            vrfire_pos_to_world(&map, p, w); vrfire_pos_from_world(&map, w, b);
            vrfire_dir_to_world(&map, d, dw); vrfire_dir_from_world(&map, dw, db);
            for (int k = 0; k < 3; k++) { CHECK(fabsf(b[k] - p[k]) < 2e-3f, "pos inversa"); CHECK(fabsf(db[k] - d[k]) < 1e-5f, "dir inversa"); }
        }
    }
    uint32_t ser = 0, a0 = vrfire_last_missile(&ser), ser0 = ser; (void)a0;
    VrFireMat m; memset(&m, 0, sizeof m);
    vrfire_probe(0, 0, 0x801701B8u, &m); vrfire_last_missile(&ser); CHECK(ser == ser0, "inv");          /* una bala no cuenta */
    vrfire_probe(1, 1, 0x801701B8u, &m); CHECK(vrfire_last_missile(&ser) == 0x801701B8u && ser == ser0 + 1, "inv");
    vrfire_probe(1, 0, 0, &m); vrfire_last_missile(&ser); CHECK(ser == ser0 + 1, "inv");                 /* creación fallida (0) no cuenta */
}

/* ---------------------------------------------------------------- vr_hit: cápsulas por pieza, zonas, línea de tiro (balas del jugador y de los enemigos) y puños
 * Mundo sintético con las estructuras del juego (tabla de blancos, cola de objetos, piezas y modelos, suelos de colisión) y un "juego falso" que reproduce el tick de la bala
 * medido en el desensamblado (0x800771ac -> 0x8002ebd8/0x8002e7f8 -> 0x8002e208): segmento contra UNA caja por blanco con bit 0x10 y bando distinto, daño a los blancos con
 * bit 4 que solapan la caja de 100 de la bala (modo 1: vida -= daño; modo 0: vida = 0), bandera de daño y tipo de impacto. */
#include "vr_hit.h"
#define HT 0x800B7660u
#define HQ 0x80160000u
#define HB 0x80150000u
#define HFL 0x80102400u
static int hs16(uint32_t a) { return r16(a); }
static void h_target(int i, int cls, int side, int cx, int cy, int cz, int hx, int hy, int hz, int mode, int vital) {
    uint32_t b = HT + 72u * (uint32_t)i;
    w16(b, cls); w16(b + 2, side); w16(b + 4, 1); w16(b + 6, 0); w16(b + 8, cx); w16(b + 10, cy); w16(b + 12, cz); w16(b + 16, hx); w16(b + 18, hy); w16(b + 20, hz);
    w16(b + 36, mode); w16(b + 38, vital); w16(b + 0x3e, -1);
}
static void h_mat(uint32_t a, int x, int y, int z) { for (int i = 0; i < 9; i++) w16(a + 2u * (uint32_t)i, i % 4 == 0 ? 4096 : 0); w32(a + 20, (uint32_t)x); w32(a + 24, (uint32_t)y); w32(a + 28, (uint32_t)z); }
/* soldado sintético de 7 piezas: la cabeza es la 3 (no la 6) y la 6 es una mano levantada por encima de la cabeza; el brazo (4) cuelga a x+400, fuera de la caja de +-300 */
static const int SP[7] = {-1, 0, 1, 2, 1, 0, 4};
static const int SJ[7][3] = {{0, 0, 0}, {0, 0, 0}, {0, 329, 0}, {0, 74, 0}, {400, 265, 0}, {-90, -230, 0}, {0, -270, 0}};
static const int SBX[7][6] = {{-160, -250, -150, 160, 9, 85}, {-180, 9, -170, 180, 374, 75}, {-60, -15, -40, 50, 74, 79}, {-74, -60, -79, 74, 169, 80},
                              {-54, -250, -70, 35, 44, 50}, {-79, -705, -120, 85, 45, 79}, {-44, -104, -40, 31, 17, 50}};
static void h_soldier(int ti, int slot, int cx, int cy, int cz, int vital) {
    uint32_t D = 0x80130000u + 0x1000u * (uint32_t)slot, O = 0x80140000u + 0x1000u * (uint32_t)slot;
    h_target(ti, 0xDF, 2, cx, cy, cz, 300, 1000, 300, 1, vital);
    w32(D, 7); w32(D + 4, 7);
    h_mat(O, cx, cy, cz); w32(O + 0x24, D); w32(O + 0x28, 0x32d); w16(O + 0x2C, 1); w16(O + 0x2E, 7);
    for (int k = 0; k < 7; k++) {
        uint32_t M = D + 0x20u + 0x58u * (uint32_t)k, P = O + 0x48u + 0x5Cu * (uint32_t)k;
        for (int i = 0; i < 6; i++) w32(M + 8u + 4u * (uint32_t)i, (uint32_t)SBX[k][i]);
        for (int i = 0; i < 3; i++) w32(M + 0x20u + 4u * (uint32_t)i, (uint32_t)SJ[k][i]);
        w32(M + 0x2C, (uint32_t)SP[k]);
        int p[3] = {cx, cy, cz};
        for (int j = k; j >= 0; j = SP[j]) for (int i = 0; i < 3; i++) p[i] += SJ[j][i];
        if (k == 6) { p[0] = cx + 400; p[1] = cy + 900; p[2] = cz; }
        h_mat(P, p[0], p[1], p[2]); w32(P + 0x40, M);
    }
    int n = r16(0x800B2B7Cu + 0x56);
    w32(HQ + 4u * (uint32_t)n, O); w16(0x800B2B7Cu + 0x56, n + 1);
}
static int h_nfloor;
static void h_world(void) {
    memset(ram, 0, sizeof ram);
    w32(0x800ACB9Cu, 1); w32(0x800B2B7Cu + 0x58, HQ);
    w32(0x800ACB84u, 0x80101000u); w32(0x80101000u + 0x2C, 0x80102000u); w32(0x80102000u + 8, 0x80102100u); w32(0x80102100u, 0x80102200u);
    w16(0x80102200u + 10, 1); w32(0x80102200u + 16, 0x80102300u); w32(0x80102300u + 12, HFL); w32(0x800ACB38u, 1);
    h_nfloor = 0;
}
/* suelo de colisión horizontal (cuadrilátero de 48 B: caja de 2 puntos + 4 esquinas x,z,y,h) */
static void h_floor(int x0, int z0, int x1, int z1, int y) {
    uint32_t f = HFL + 48u * (uint32_t)h_nfloor;
    const int q[6][2] = {{x0, z0}, {x1, z1}, {x0, z0}, {x1, z0}, {x1, z1}, {x0, z1}};
    for (int k = 0; k < 6; k++) { w16(f + 8u * (uint32_t)k, q[k][0]); w16(f + 8u * (uint32_t)k + 2, q[k][1]); w16(f + 8u * (uint32_t)k + 4, y); }
    w16(0x80102300u + 4, ++h_nfloor);
}
static float hw_wall = -1.0f; static int hw_cam_on, hw_cam_hits; static float hw_cam_t; static uint32_t hw_cam_tg, hw_cam_last;
static float hw_wallf(const uint8_t *r, const float o[3], const float d[3], float max) { (void)r; (void)o; (void)d; return hw_wall >= 0 && hw_wall <= max ? hw_wall : -1.0f; }
static int hw_camf(uint8_t *r, const float o[3], const float d[3], float max, float *t, uint32_t *tg) { (void)r; (void)o; (void)d; if (!hw_cam_on || hw_cam_t > max) return 0; *t = hw_cam_t; *tg = hw_cam_tg; return 1; }
static void hw_camhit(uint8_t *r, uint32_t tg, const int16_t pt[3]) { (void)r; (void)pt; hw_cam_hits++; hw_cam_last = tg; }
static float hw_solid = -1.0f;
static float hw_solidf(const uint8_t *r, const float o[3], const float d[3], float max) { (void)r; (void)o; (void)d; return hw_solid >= 0 && hw_solid <= max ? hw_solid : -1.0f; }
static void h_reg(void) { VrHitWorld w = {1000.0f, hw_wallf, hw_camf, hw_camhit, hw_solidf}; vrhit_set_world(&w); hw_wall = -1.0f; hw_solid = -1.0f; hw_cam_on = 0; hw_cam_hits = 0; }
/* bala como la deja el creador del juego: origen, velocidad 5000 por tick, alcance, final del trazado (pared del juego a gwall, o el alcance) y su blanco de daño 64 */
static void h_bullet(int side, const float o[3], const float to[3], int range, int gwall) {
    double d[3] = {to[0] - o[0], to[1] - o[1], to[2] - o[2]}, l = sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    int lim = gwall >= 0 ? gwall : range;
    memset(ram + (HB & 0x1FFFFF), 0, 0x170);
    for (int i = 0; i < 3; i++) {
        double e = o[i] + d[i] / l * lim;
        w16(HB + 0x110 + 2u * (uint32_t)i, (int)lrint(o[i])); w16(HB + 0x120 + 2u * (uint32_t)i, (int)lrint(d[i] / l * 5000.0));
        w16(HB + 0x118 + 2u * (uint32_t)i, (int)(e > 32767 ? 32767 : e < -32768 ? -32768 : lrint(e)));
    }
    w32(HB + 0x138, (uint32_t)lim); w32(HB + 0x140, gwall >= 0); w32(HB + 0x148, (uint32_t)side); w32(HB + 0x154, 64); w32(HB + 0x158, (uint32_t)range); w32(HB + 0x15C, 5000);
    w16(HB + 0x44, 4); w16(HB + 0x44 + 2, side); w16(HB + 0x44 + 4, 1); w16(HB + 0x44 + 16, 100); w16(HB + 0x44 + 18, 100); w16(HB + 0x44 + 20, 100); w16(HB + 0x44 + 38, 64); w16(HB + 0x44 + 62, 1);
}
static int g_axis(const int f[3], const int t[3], const int c[3], const int h[3], int ax, int b1, int b2, int out[3]) {
    int t1 = t[ax] - f[ax]; if (!t1) return 0;
    int t0;
    if (c[ax] < f[ax]) { t0 = c[ax] + h[ax]; if (f[ax] < t0) t0 = f[ax]; } else { t0 = c[ax] - h[ax]; if (t0 < f[ax]) t0 = f[ax]; }
    int t5 = t0 - f[ax];
    int p1 = f[b1] + (t[b1] - f[b1]) * t5 / t1; if (p1 < c[b1] - h[b1] || p1 > c[b1] + h[b1]) return 0;
    int p2 = f[b2] + (t[b2] - f[b2]) * t5 / t1; if (p2 < c[b2] - h[b2] || p2 > c[b2] + h[b2]) return 0;
    out[ax] = t0; out[b1] = p1; out[b2] = p2; return 1;
}
/* un tick de la bala del juego. Devuelve el índice del blanco sellado (-1 ninguno) */
static int h_tick(void) {
    int f[3], t[3], side = (int)r32(HB + 0x148), trav = (int)r32(HB + 0x13C) + (int)r32(HB + 0x15C), best = -1;
    w32(HB + 0x13C, (uint32_t)trav);
    for (int i = 0; i < 3; i++) { f[i] = hs16(HB + 0x110 + 2u * (uint32_t)i); t[i] = (int)r32(HB + 0x138) < trav ? hs16(HB + 0x118 + 2u * (uint32_t)i) : f[i] + hs16(HB + 0x120 + 2u * (uint32_t)i); }
    for (int i = 0; i < 16; i++) {
        uint32_t b = HT + 72u * (uint32_t)i; int c[3], h[3], o[3], ov = 1;
        if (hs16(b + 2) == side || !(hs16(b) & 0x10) || !(hs16(b + 4) & (int)r32(0x800ACB9Cu))) continue;
        for (int k = 0; k < 3; k++) { c[k] = hs16(b + 8 + 2u * (uint32_t)k); h[k] = hs16(b + 16 + 2u * (uint32_t)k); if (abs((f[k] + t[k]) / 2 - c[k]) > abs(f[k] - t[k]) / 2 + h[k]) ov = 0; }
        if (!ov) continue;
        if (g_axis(f, t, c, h, 0, 1, 2, o) || g_axis(f, t, c, h, 1, 2, 0, o) || g_axis(f, t, c, h, 2, 0, 1, o)) { memcpy(t, o, sizeof t); best = i; }
    }
    if (best < 0) { for (int i = 0; i < 3; i++) w16(HB + 0x110 + 2u * (uint32_t)i, t[i]); return -1; }
    int dmg = hs16(HB + 0x44 + 38);
    for (int i = 0; i < 16; i++) {
        uint32_t b = HT + 72u * (uint32_t)i; int ov = 1;
        if (!(hs16(b) & 4) || (hs16(b + 2) & side)) continue;
        for (int k = 0; k < 3; k++) if (abs(t[k] - hs16(b + 8 + 2u * (uint32_t)k)) > 100 + hs16(b + 16 + 2u * (uint32_t)k)) ov = 0;
        if (!ov) continue;
        w16(b + 6, hs16(b + 6) | 4); w16(b + 0x3e, hs16(HB + 0x44 + 62));
        int m = hs16(b + 36) & 3; if (m == 1) w16(b + 38, hs16(b + 38) - dmg); else if (m == 0) w16(b + 38, 0);
        dmg = 0;
    }
    return best;
}
static int h_shoot(int side, float ox, float oy, float oz, float tx, float ty, float tz, int range, int gwall, VrHitShot *sh) {
    float o[3] = {ox, oy, oz}, to[3] = {tx, ty, tz};
    memset(sh, 0, sizeof *sh);
    h_bullet(side, o, to, range, gwall);
    vrhit_bullet(ram, HB, sh);
    for (int k = 0; k < 8; k++) {            /* ticks del juego hasta que sella algo o agota su recorrido */
        int r = h_tick();
        if (r >= 0) return r;
        if ((int)r32(HB + 0x13C) > (int)r32(HB + 0x138)) break;
    }
    return -1;
}
static int h_vital(int i) { return hs16(HT + 72u * (uint32_t)i + 38); }

static void t_hit_zones(void) {
    VrHitShot sh; VrHitTarget T[VRHIT_MAX_T];
    h_world(); h_reg(); h_soldier(0, 0, 0, 1000, 5000, 128);
    int n = vrhit_scan(ram, 1, 0x10, 1, T, VRHIT_MAX_T);
    CHECK(n == 1 && T[0].ncap == 7 && T[0].head == 3, "soldado sintético: 7 cápsulas y cabeza = pieza hoja más alta en reposo (n=%d ncap=%d cabeza=%d)", n, n ? T[0].ncap : -1, n ? T[0].head : -1);
    CHECK(n == 1 && T[0].cap[3].zone == VRHIT_Z_HEAD && T[0].cap[2].zone == VRHIT_Z_BODY && T[0].cap[1].zone == VRHIT_Z_BODY && T[0].cap[0].zone == VRHIT_Z_BODY && T[0].cap[4].zone == VRHIT_Z_LIMB && T[0].cap[5].zone == VRHIT_Z_LIMB && T[0].cap[6].zone == VRHIT_Z_LIMB, "zonas por pieza");
    /* cabeza: letal de un tiro */
    CHECK(h_shoot(1, 0, 1300, 0, 0, 1457, 5000, 10000, -1, &sh) == 0 && h_vital(0) <= 0, "tiro a la cabeza: sellado y letal (vida %d)", h_vital(0));
    CHECK(sh.zone == VRHIT_Z_HEAD && sh.damage == 128 && sh.vital == 128 && sh.target == 0 && sh.action == VRHIT_ACT_SEAL, "zona cabeza, daño = vida (zona %d daño %d)", sh.zone, sh.damage);
    /* la cabeza solo es letal en un soldado normal (dañable, bando enemigo, caja 300x1000x300, vida <= 3 balas): jefes y demás humanoides reciben el daño del juego */
    h_world(); h_soldier(0, 0, 0, 1000, 5000, 1000);
    CHECK(h_shoot(1, 0, 1300, 0, 0, 1457, 5000, 10000, -1, &sh) == 0 && h_vital(0) == 936 && sh.zone == VRHIT_Z_HEAD && sh.damage == 64, "vida alta (jefe): cabeza sin daño letal (vida %d daño %d)", h_vital(0), sh.damage);
    h_world(); h_soldier(0, 0, 0, 1000, 5000, 128); w16(HT + 16, 400); w16(HT + 20, 400);
    CHECK(h_shoot(1, 0, 1300, 0, 0, 1457, 5000, 10000, -1, &sh) == 0 && h_vital(0) == 64 && sh.zone == VRHIT_Z_HEAD, "caja distinta de la del soldado: cabeza sin daño letal (vida %d)", h_vital(0));
    h_world(); h_soldier(0, 0, 0, 1000, 5000, 128); w16(HT + 2, 4);
    CHECK(h_shoot(1, 0, 1300, 0, 0, 1457, 5000, 10000, -1, &sh) == 0 && h_vital(0) == 64, "bando que no es el enemigo: cabeza sin daño letal (vida %d)", h_vital(0));
    h_world(); h_soldier(0, 0, 0, 1000, 5000, 192);
    CHECK(h_shoot(1, 0, 1300, 0, 0, 1457, 5000, 10000, -1, &sh) == 0 && h_vital(0) <= 0 && sh.damage == 192, "soldado de vida 192: cabeza letal");
    /* cuerpo: 64 (dos tiros con vida 128) */
    h_world(); h_soldier(0, 0, 0, 1000, 5000, 128);
    CHECK(h_shoot(1, 0, 1300, 0, 0, 1190, 5000, 10000, -1, &sh) == 0 && h_vital(0) == 64 && sh.zone == VRHIT_Z_BODY && sh.damage == 64, "tiro al cuerpo: 64 (vida %d zona %d)", h_vital(0), sh.zone);
    CHECK(h_shoot(1, 0, 1300, 0, 0, 1190, 5000, 10000, -1, &sh) == 0 && h_vital(0) == 0, "segundo tiro al cuerpo: muere (vida %d)", h_vital(0));
    /* extremidad FUERA de la caja del juego (brazo a x+390; la caja llega a +300): el juego fallaba */
    h_world(); h_soldier(0, 0, 0, 1000, 5000, 128);
    CHECK(h_shoot(1, 390, 1162, 0, 390, 1162, 5000, 10000, -1, &sh) == 0 && h_vital(0) == 64 && sh.zone == VRHIT_Z_LIMB, "brazo fuera de la caja: impacta (sellado, vida %d zona %d)", h_vital(0), sh.zone);
    /* dentro de la caja pero sin tocar ninguna pieza (por encima del hombro): fallo real, el juego no sella */
    h_world(); h_soldier(0, 0, 0, 1000, 5000, 128);
    CHECK(h_shoot(1, 250, 1900, 0, 250, 1900, 5000, 10000, -1, &sh) == -1 && h_vital(0) == 128, "rayo por la caja fuera de las cápsulas: no sella (vida %d)", h_vital(0));
    CHECK(sh.zone == VRHIT_Z_MISS && sh.action == VRHIT_ACT_VOID && sh.target == -1, "zona fallo (zona %d acción %d)", sh.zone, sh.action);
    /* pared delante */
    h_world(); h_soldier(0, 0, 0, 1000, 5000, 128); hw_wall = 2000.0f;
    CHECK(h_shoot(1, 0, 1300, 0, 0, 1457, 5000, 10000, 2000, &sh) == -1 && h_vital(0) == 128 && sh.zone == VRHIT_Z_WALL && fabsf(sh.dist - 2000.0f) < 1.0f, "pared delante: pared (zona %d dist %.0f vida %d)", sh.zone, sh.dist, h_vital(0));
    hw_wall = -1.0f;
    /* alcance: 13 m (el juego acababa a 10000) y más allá del alcance */
    h_world(); h_soldier(0, 0, 8000, 1000, 13000, 128);
    CHECK(h_shoot(1, 8000, 1300, 0, 8000, 1457, 13000, 10000, -1, &sh) == 0 && h_vital(0) <= 0 && sh.zone == VRHIT_Z_HEAD, "enemigo a 13000 unidades: impacta (vida %d)", h_vital(0));
    h_world(); h_soldier(0, 0, 8000, 1000, 15500, 128);
    CHECK(h_shoot(1, 8000, 1300, 0, 8000, 1190, 15500, 10000, -1, &sh) == 0 && h_vital(0) == 64, "enemigo a 15500 unidades (15 m del láser): impacta (vida %d)", h_vital(0));
    h_world(); h_soldier(0, 0, 8000, 1000, 17500, 128);
    CHECK(h_shoot(1, 8000, 1300, 0, 8000, 1190, 17500, 10000, -1, &sh) == -1 && h_vital(0) == 128 && sh.zone == VRHIT_Z_MISS && sh.action == VRHIT_ACT_NONE, "fuera de alcance: fallo y la bala no se toca");
    /* registro */
    h_world(); h_soldier(0, 0, 0, 1000, 5000, 128);
    { fflush(stderr); FILE *f = tmpfile(); int sv = dup(2); dup2(fileno(f), 2);
      h_shoot(1, 0, 1300, 0, 0, 1457, 5000, 10000, -1, &sh);
      fflush(stderr); dup2(sv, 2); close(sv); static char lb[1024]; rewind(f); size_t got = fread(lb, 1, sizeof lb - 1, f); lb[got] = 0; fclose(f);
      CHECK(strstr(lb, "[impacto] blanco=0 zona=cabeza dist=4.92 m vida=128") != NULL && strstr(lb, "vida=128") != NULL, "línea [impacto]: %.200s", lb); }
}
/* la geometría de render de la escena (cristales con huecos de textura, rejillas, marcos) NO es colisión de balas: no bloquea; el registro [impacto] dice qué bloqueó (motivo) y qué habría bloqueado la escena */
static char *h_shoot_log(int side, float ox, float oy, float oz, float tx, float ty, float tz, int range, int gwall, VrHitShot *sh, int *res) {
    static char lb[2048];
    fflush(stderr); FILE *f = tmpfile(); int sv = dup(2); dup2(fileno(f), 2);
    *res = h_shoot(side, ox, oy, oz, tx, ty, tz, range, gwall, sh);
    fflush(stderr); dup2(sv, 2); close(sv); rewind(f); size_t got = fread(lb, 1, sizeof lb - 1, f); lb[got] = 0; fclose(f);
    return lb;
}
static void t_hit_motivo(void) {
    VrHitShot sh; int r; char *lg;
    h_world(); h_reg(); h_soldier(0, 0, 0, 1000, 5000, 128); hw_solid = 2000.0f;
    lg = h_shoot_log(1, 0, 1300, 0, 0, 1457, 5000, 10000, -1, &sh, &r);
    CHECK(r == 0 && h_vital(0) <= 0 && sh.zone == VRHIT_Z_HEAD, "sólido de la escena entre medias: la bala llega (zona %d vida %d)", sh.zone, h_vital(0));
    CHECK(strstr(lg, "escena=2.00 m") != NULL && strstr(lg, "accion=sellada") != NULL, "el registro cita el sólido de la escena que no bloqueó: %.300s", lg);
    h_world(); h_reg(); h_soldier(0, 0, 0, 1000, 5000, 128); hw_wall = 2000.0f;
    lg = h_shoot_log(1, 0, 1300, 0, 0, 1457, 5000, 10000, -1, &sh, &r);
    CHECK(r == -1 && sh.zone == VRHIT_Z_WALL && strstr(lg, "motivo=pared_hzd") != NULL, "pared HZD: motivo pared_hzd: %.300s", lg);
    h_world(); h_reg(); h_soldier(0, 0, 0, 1000, 5000, 128);
    lg = h_shoot_log(1, 0, 1300, 0, 0, 1457, 5000, 10000, 2000, &sh, &r);
    CHECK(r == -1 && sh.zone == VRHIT_Z_WALL && strstr(lg, "motivo=juego") != NULL, "trazado del juego: motivo juego: %.300s", lg);
    h_world(); h_reg(); h_soldier(0, 0, 0, -3000, 9000, 128); h_floor(-2000, 500, 2000, 12000, 0);
    lg = h_shoot_log(1, 0, 1300, 0, 0, -3000.0f + 457, 9000, 10000, -1, &sh, &r);
    CHECK(r == -1 && sh.zone == VRHIT_Z_WALL && strstr(lg, "motivo=suelo_hzd") != NULL, "suelo HZD: motivo suelo_hzd: %.300s", lg);
    h_world(); h_reg(); h_soldier(0, 0, 0, 1000, 5000, 128);
    lg = h_shoot_log(1, 400, 1300, 0, 400, 1457, 5000, 10000, -1, &sh, &r);
    CHECK(sh.zone == VRHIT_Z_MISS && strstr(lg, "motivo=ninguno") != NULL && strstr(lg, "o=(400,1300,0)") != NULL && strstr(lg, "mas_cercano=blanco 0") != NULL, "fallo: origen, dirección y blanco más cercano: %.300s", lg);
}
static void t_hit_camera(void) {
    VrHitShot sh;
    h_world(); h_reg(); h_target(2, 0x14, 2, 3000, 3000, 6000, 300, 300, 300, 0, 0);
    uint32_t b = HT + 72u * 2u; int hits = 0;
    for (int k = 0; k < 2; k++) {
        CHECK(h_shoot(1, 0, 1300, 0, 3000, 3000, 6000, 10000, -1, &sh) == 2 && sh.zone == VRHIT_Z_BOX && sh.action == VRHIT_ACT_SEAL, "cámara: la bala la sella (tiro %d)", k + 1);
        if ((hs16(b + 6) & 4) && hs16(b + 0x3e) == 1) { hits++; w16(b + 6, 0); }      /* lo que recoge vr_cameras cada fotograma */
    }
    CHECK(hits == 2 && hw_cam_hits == 0, "cámara: 2 tiros = 2 impactos del juego (%d), sin impacto directo duplicado (%d)", hits, hw_cam_hits);
    /* cámara a 14 m: el juego no llegaba */
    h_world(); h_target(2, 0x14, 2, 0, 3000, 14000, 300, 300, 300, 0, 0);
    CHECK(h_shoot(1, 0, 1300, 0, 0, 3000, 14000, 10000, -1, &sh) == 2 && (hs16(b + 6) & 4), "cámara a 14000 unidades: sellada");
    /* blanco de cámara sin el bit de daño: el juego la sella sin dañarla -> impacto directo (uno por tiro) */
    h_world(); h_target(2, 0x10, 2, 3000, 3000, 6000, 300, 300, 300, 0, 0);
    for (int k = 0; k < 2; k++) h_shoot(1, 0, 1300, 0, 3000, 3000, 6000, 10000, -1, &sh);
    CHECK(hw_cam_hits == 0, "blanco no dañable que el anfitrión no reconoce como cámara: sin impacto directo (%d)", hw_cam_hits);
    hw_cam_on = 1; hw_cam_t = 6400.0f; hw_cam_tg = b;
    for (int k = 0; k < 2; k++) h_shoot(1, 0, 1300, 0, 3000, 3000, 6000, 10000, -1, &sh);
    CHECK(hw_cam_hits == 2 && hw_cam_last == b, "cámara no dañable por el juego: 2 impactos directos (%d)", hw_cam_hits);
    /* una bala enemiga nunca cuenta impactos en una cámara */
    hw_cam_hits = 0; w16(b + 2, 1); h_shoot(2, 0, 1300, 0, 3000, 3000, 6000, 10000, -1, &sh); w16(b + 2, 2);
    CHECK(hw_cam_hits == 0, "bala enemiga: sin impacto directo de cámara (%d)", hw_cam_hits);
    /* cámara articulada (con piezas): es cámara por su blanco, no por tener solo caja */
    h_world(); h_reg(); h_soldier(0, 0, 0, 1000, 5000, 0); w16(HT, 0x10); hw_cam_on = 1; hw_cam_t = 4700.0f; hw_cam_tg = HT;
    CHECK(h_shoot(1, 0, 1300, 0, 0, 1457, 5000, 10000, -1, &sh) == 0 && hw_cam_hits == 1 && hw_cam_last == HT, "cámara articulada no dañable: impacto directo (%d)", hw_cam_hits);
    hw_cam_on = 0;
    /* cámara que el juego no sella (sin bit 0x10): la da el rayo de cámaras del anfitrión */
    h_world(); h_reg(); h_target(2, 0x04, 2, 3000, 3000, 6000, 300, 300, 300, 0, 0); hw_cam_on = 1; hw_cam_t = 6500.0f; hw_cam_tg = b;
    CHECK(h_shoot(1, 0, 1300, 0, 3000, 3000, 6000, 10000, -1, &sh) == -1 && hw_cam_hits == 1 && sh.zone == VRHIT_Z_BOX, "cámara fuera de la prueba del juego: impacto directo (%d)", hw_cam_hits);
    hw_cam_on = 0;
}
/* otra planta: 4 m más abajo / más arriba. Línea despejada -> impacto con su zona; suelo de colisión entre medias -> pared */
static void t_hit_floors(void) {
    VrHitShot sh;
    for (int up = 0; up < 2; up++) {
        int cy = up ? 5000 : -3000, fy = up ? 3000 : 0;
        h_world(); h_reg(); h_soldier(0, 0, 0, cy, 9000, 128);
        CHECK(h_shoot(1, 0, 1300, 0, 0, (float)cy + 457, 9000, 10000, -1, &sh) == 0 && h_vital(0) <= 0 && sh.zone == VRHIT_Z_HEAD, "planta %s, línea despejada: cabeza (zona %d vida %d)", up ? "superior" : "inferior", sh.zone, h_vital(0));
        /* el trazado del propio juego dio en su colisión (suelo, puerta cerrada...) antes del blanco: pared */
        h_world(); h_soldier(0, 0, 0, cy, 9000, 128);
        CHECK(h_shoot(1, 0, 1300, 0, 0, (float)cy + 457, 9000, 10000, 4000, &sh) == -1 && h_vital(0) == 128 && sh.zone == VRHIT_Z_WALL, "planta %s, cortada por la colisión del juego: pared (zona %d)", up ? "superior" : "inferior", sh.zone);
        h_world(); h_soldier(0, 0, 0, cy, 9000, 128);
        CHECK(h_shoot(1, 0, 1300, 0, 0, (float)cy + 190, 9000, 10000, -1, &sh) == 0 && h_vital(0) == 64 && sh.zone == VRHIT_Z_BODY, "planta %s: cuerpo (zona %d)", up ? "superior" : "inferior", sh.zone);
        h_world(); h_soldier(0, 0, 0, cy, 9000, 128); h_floor(-2000, 500, 2000, 12000, fy);
        CHECK(h_shoot(1, 0, 1300, 0, 0, (float)cy + 457, 9000, 10000, -1, &sh) == -1 && h_vital(0) == 128 && sh.zone == VRHIT_Z_WALL, "planta %s con suelo sólido entre medias: pared (zona %d vida %d)", up ? "superior" : "inferior", sh.zone, h_vital(0));
        /* el suelo no llega hasta la línea de tiro (hueco): despejada */
        h_world(); h_soldier(0, 0, 0, cy, 9000, 128); h_floor(1000, 500, 4000, 12000, fy);
        CHECK(h_shoot(1, 0, 1300, 0, 0, (float)cy + 457, 9000, 10000, -1, &sh) == 0 && sh.zone == VRHIT_Z_HEAD, "planta %s con el suelo a un lado: impacta", up ? "superior" : "inferior");
    }
    float o[3] = {0, 1300, 0}, d[3] = {0, -1, 0};
    h_world(); h_floor(-500, -500, 500, 500, 0);
    CHECK(fabsf(vrhit_floor_ray(ram, o, d, 5000.0f) - 1300.0f) < 1.0f, "rayo vertical contra el suelo");
    CHECK(vrhit_floor_ray(ram, o, d, 1000.0f) < 0 && vrhit_floor_ray(NULL, o, d, 5000.0f) < 0, "suelo fuera de alcance / RAM nula");
}
/* balas enemigas: lo único que se toca es su alcance (hasta VRHIT_RANGE_MIN) cuando su línea al jugador está despejada; quién sella es el tick del juego, nunca se recolocan */
static void t_hit_enemy(void) {
    VrHitShot sh; int16_t p0[3];
    for (int up = 0; up < 2; up++) {
        float ey = up ? 4500.0f : -3500.0f; int fy = up ? 2500 : -1500;
        /* 11,7 m con línea despejada: el juego la acababa a 10000; ahora llega y la sella el juego en su tercer tick */
        h_world(); h_reg(); h_target(6, 0x9F, 1, 0, 500, 0, 300, 500, 300, 1, 256);
        CHECK(h_shoot(2, 0, ey, 11000, 0, 500, 0, 10000, -1, &sh) == 6 && h_vital(6) == 192 && sh.side == 2 && sh.action == VRHIT_ACT_RANGE, "bala enemiga desde %s a 11,7 m, despejada: alcanza al jugador (vida %d acción %d)", up ? "arriba" : "abajo", h_vital(6), sh.action);
        CHECK((int)r32(HB + 0x158) == 16000 && (int)r32(HB + 0x13C) == 15000, "alcance ampliado a 16000 y sellada por el tick del juego en vuelo (recorrido %d)", (int)r32(HB + 0x13C));
        /* nunca se recoloca: la posición inicial de la bala no cambia */
        h_world(); h_target(6, 0x9F, 1, 0, 500, 0, 300, 500, 300, 1, 256);
        { float o[3] = {0, ey, 11000}, to[3] = {0, 500, 0}; h_bullet(2, o, to, 10000, -1); memcpy(p0, ram + ((HB + 0x110) & 0x1FFFFF), 6); vrhit_bullet(ram, HB, &sh);
          CHECK(!memcmp(p0, ram + ((HB + 0x110) & 0x1FFFFF), 6), "la bala enemiga no se recoloca"); }
        /* dentro del alcance del juego no se toca nada: decide su trazado */
        h_world(); h_target(6, 0x9F, 1, 0, 500, 0, 300, 500, 300, 1, 256);
        CHECK(h_shoot(2, 0, ey, 8000, 0, 500, 0, 10000, -1, &sh) == 6 && sh.action == VRHIT_ACT_NONE && (int)r32(HB + 0x158) == 10000, "bala enemiga dentro del alcance del juego: intacta");
        h_world(); h_target(6, 0x9F, 1, 0, 500, 0, 300, 500, 300, 1, 256);
        CHECK(h_shoot(2, 0, ey, 8000, 0, 500, 0, 10000, 3000, &sh) == -1 && h_vital(6) == 256 && sh.action == VRHIT_ACT_NONE, "bala enemiga cortada por el trazado del juego: no impacta y no se toca");
        /* el trazado del juego ya dio en su colisión: no se amplía */
        h_world(); h_target(6, 0x9F, 1, 0, 500, 0, 300, 500, 300, 1, 256);
        CHECK(h_shoot(2, 0, ey, 11000, 0, 500, 0, 10000, 6000, &sh) == -1 && h_vital(6) == 256 && sh.action == VRHIT_ACT_NONE && sh.zone == VRHIT_Z_WALL, "bala enemiga lejana cortada por el juego: no se amplía");
        /* suelo o pared del anfitrión antes del jugador: no se amplía (la geometría del anfitrión solo resta) */
        h_world(); h_target(6, 0x9F, 1, 0, 500, 0, 300, 500, 300, 1, 256); h_floor(-2000, 1000, 2000, 12000, fy);
        CHECK(h_shoot(2, 0, ey, 11000, 0, 500, 0, 10000, -1, &sh) == -1 && h_vital(6) == 256 && sh.zone == VRHIT_Z_WALL && sh.action == VRHIT_ACT_NONE, "bala enemiga lejana con suelo entre medias: no se amplía (vida %d zona %d)", h_vital(6), sh.zone);
        h_world(); h_target(6, 0x9F, 1, 0, 500, 0, 300, 500, 300, 1, 256); hw_wall = 1500.0f;
        CHECK(h_shoot(2, 0, ey, 11000, 0, 500, 0, 10000, -1, &sh) == -1 && h_vital(6) == 256 && sh.action == VRHIT_ACT_NONE, "bala enemiga lejana con pared entre medias: no se amplía");
        hw_wall = -1.0f;
    }
    /* la puntería de la IA no se toca: una bala que no cruza la caja del jugador sigue intacta, tampoco las de más de 16000 */
    h_world(); h_target(6, 0x9F, 1, 0, 500, 0, 300, 500, 300, 1, 256);
    CHECK(h_shoot(2, 0, 1000, 8000, 3000, 1000, 0, 10000, -1, &sh) == -1 && sh.action == VRHIT_ACT_NONE && h_vital(6) == 256 && (int)r32(HB + 0x158) == 10000, "bala enemiga desviada: intacta");
    h_world(); h_target(6, 0x9F, 1, 0, 500, 0, 300, 500, 300, 1, 256);
    CHECK(h_shoot(2, 0, 500, 17000, 0, 500, 0, 10000, -1, &sh) == -1 && sh.action == VRHIT_ACT_NONE && (int)r32(HB + 0x158) == 10000, "bala enemiga a 17 m: intacta");
    /* y las balas enemigas no sellan a otros enemigos ni usan sus cápsulas */
    h_world(); h_soldier(0, 0, 0, 1000, 5000, 128);
    CHECK(h_shoot(2, 0, 1300, 0, 0, 1457, 5000, 10000, -1, &sh) == -1 && h_vital(0) == 128, "bala enemiga contra enemigo: nada");
}
/* puntería anotada en el gatillo: el blanco se mueve durante el retardo del juego y la bala impacta en él */
static void t_hit_aim(void) {
    VrHitShot sh; float o[3] = {0, 1300, 0}, d[3] = {0, 157, 5000};
    h_world(); h_reg(); h_soldier(0, 0, 0, 1000, 5000, 128);
    vrhit_aim(ram, o, d);
    for (int i = 0; i < 20; i++) vrhit_frame();
    h_world(); h_soldier(0, 0, 700, 1000, 5200, 128);                 /* 0,7 m a un lado al crearse la bala */
    CHECK(h_shoot(1, 0, 1300, 0, 0, 1457, 5000, 10000, -1, &sh) == 0 && h_vital(0) <= 0 && sh.zone == VRHIT_Z_HEAD, "blanco movido tras el gatillo: cabeza (zona %d vida %d)", sh.zone, h_vital(0));
    h_world(); h_soldier(0, 0, 700, 1000, 5200, 128);
    CHECK(h_shoot(1, 0, 1300, 0, 0, 1457, 5000, 10000, -1, &sh) == -1 && h_vital(0) == 128, "la puntería se consume con su bala");
    h_world(); h_soldier(0, 0, 0, 1000, 5000, 128); vrhit_aim(ram, o, d);
    for (int i = 0; i < VRHIT_AIM_LIFE + 5; i++) vrhit_frame();
    h_world(); h_soldier(0, 0, 700, 1000, 5200, 128);
    CHECK(h_shoot(1, 0, 1300, 0, 0, 1457, 5000, 10000, -1, &sh) == -1, "puntería caducada: no vale");
    h_world(); h_soldier(0, 0, 0, 1000, 5000, 128); vrhit_aim(ram, o, d);
    h_world(); h_soldier(0, 0, 700, 1000, 5200, 128);
    CHECK(h_shoot(1, 300, 1300, 0, 300, 1457, 5000, 10000, -1, &sh) == -1, "otro rayo no usa la puntería anotada");
    /* el blanco se movió tras una esquina (pared o suelo en la línea boca -> blanco de AHORA): pared */
    h_world(); h_soldier(0, 0, 0, 1000, 5000, 128); vrhit_aim(ram, o, d);
    h_world(); h_soldier(0, 0, 700, 1000, 5200, 128); hw_wall = 3000.0f;
    CHECK(h_shoot(1, 0, 1300, 0, 0, 1457, 5000, 10000, -1, &sh) == -1 && h_vital(0) == 128 && sh.zone == VRHIT_Z_WALL, "puntería anotada, blanco ahora tras una pared: pared (zona %d vida %d)", sh.zone, h_vital(0));
    hw_wall = -1.0f;
    h_world(); h_soldier(0, 0, 0, 1000, 5000, 128); vrhit_aim(ram, o, d);
    h_world(); h_soldier(0, 0, 0, -3000, 9000, 128); h_floor(-2000, 500, 2000, 12000, 0);
    CHECK(h_shoot(1, 0, 1300, 0, 0, 1457, 5000, 10000, -1, &sh) == -1 && h_vital(0) == 128 && sh.zone == VRHIT_Z_WALL, "puntería anotada, blanco ahora bajo un suelo: pared (zona %d)", sh.zone);
    h_world(); h_soldier(0, 0, 0, 1000, 5000, 128); vrhit_aim(ram, o, d);
    h_world(); h_soldier(0, 0, 700, 1000, 5200, 128);
    CHECK(h_shoot(1, 0, 1300, 0, 0, 1457, 5000, 10000, 2500, &sh) == -1 && h_vital(0) == 128, "puntería anotada pero el trazado del juego corta el rayo antes (puerta): no impacta");
}
/* puños: esfera de 0,12 m barrida contra las cápsulas */
static void t_hit_fist(void) {
    VrHitTarget T[VRHIT_MAX_T]; int z = 0; float pt[3], q[3];
    h_world(); h_soldier(0, 0, 0, 1000, 5000, 128);
    int n = vrhit_scan(ram, 1, 0x01, 1, T, VRHIT_MAX_T);
    float a[3] = {0, 1457, 4500}, b[3] = {0, 1457, 4950};
    CHECK(vrhit_sweep(T, n, a, b, 126.0f, &z, pt) == 0 && z == VRHIT_Z_HEAD && pt[2] > 4780 && pt[2] < 4800, "puño a la cabeza: contacto en la superficie (zona %d z %.0f)", z, pt[2]);
    float a2[3] = {0, 1190, 4500}, b2[3] = {0, 1190, 4900};
    CHECK(vrhit_sweep(T, n, a2, b2, 126.0f, &z, pt) == 0 && z == VRHIT_Z_BODY, "puño al cuerpo (zona %d)", z);
    float a3[3] = {100, 1800, 4500}, b3[3] = {100, 1800, 5500};
    CHECK(vrhit_sweep(T, n, a3, b3, 126.0f, &z, pt) == -1, "puño por el aire dentro de la caja: sin contacto");
    float a4[3] = {0, 1457, 4500}, b4[3] = {0, 1457, 4600};
    CHECK(vrhit_sweep(T, n, a4, b4, 126.0f, &z, pt) == -1, "puño que no llega: sin contacto");
    float far[3] = {900, 1162, 4990};
    vrhit_clamp_box(&T[0], far, q);
    CHECK(q[0] <= 298.0f && q[0] >= 290.0f && q[1] == 1162.0f && q[2] == 4990.0f, "contacto fuera de la caja proyectado dentro (%.0f %.0f %.0f)", q[0], q[1], q[2]);
    CHECK(vrhit_sweep(T, 0, a, b, 126.0f, &z, pt) == -1 && vrhit_scan(NULL, 1, 1, 1, T, VRHIT_MAX_T) == 0, "sin blancos / RAM nula");
}
/* RAM real (volcados locales, nunca en el repositorio; MV_DUMPS=<carpeta>, por defecto la de trabajo; sin ellos se omite): cada soldado tiene sus 16 cápsulas, la cabeza
 * sale la pieza 6 por la regla (no por índice fijo) y los rayos hacia cada pieza dan la zona de esa pieza */
static void t_hit_real(void) {
    static const char *nm[] = {"d0008_20261103-001918_stage", "d0009_20261103-002117_stage"};
    const char *dir = getenv("MV_DUMPS"); if (!dir) dir = "/home/claude/work/adj24/Dumps";
    for (int k = 0; k < 2; k++) {
        char p[512]; snprintf(p, sizeof p, "%s/%s/ram.bin", dir, nm[k]);
        FILE *f = fopen(p, "rb");
        if (!f) { printf("t_hit_real: sin %s (omitido)\n", p); continue; }
        size_t got = fread(ram, 1, sizeof ram, f); fclose(f);
        CHECK(got == sizeof ram, "volcado completo");
        VrHitTarget T[VRHIT_MAX_T]; int n = vrhit_scan(ram, 1, 0x10, 1, T, VRHIT_MAX_T), sold = 0, cams = 0, rays = 0;
        for (int i = 0; i < n; i++) {
            if (T[i].cls == 0x14) { cams++; CHECK(T[i].ncap == 0, "la cámara no tiene cápsulas"); continue; }
            if (T[i].cls != 0xDF) continue;
            sold++;
            CHECK(T[i].ncap == 16 && T[i].head == 6, "%s soldado %d: 16 cápsulas, cabeza = pieza %d", nm[k], T[i].index, T[i].head);
            int nz[8] = {0};
            for (int c = 0; c < T[i].ncap; c++) {
                const VrHitCap *q = &T[i].cap[c]; nz[q->zone]++;
                float mid[3] = {(q->a[0] + q->b[0]) / 2, (q->a[1] + q->b[1]) / 2, (q->a[2] + q->b[2]) / 2};
                CHECK(q->r >= 30.0f && q->r <= 200.0f && fabsf(mid[0] - T[i].c[0]) < 1300.0f && fabsf(mid[1] - T[i].c[1]) < 1300.0f, "cápsula %d razonable (r %.0f)", c, q->r);
                /* 6 rayos desde 3 m hacia el centro de la cápsula: todos dan en ESTE soldado no más lejos que la pieza (ni antes que el cuerpo entero, 1,9 m de alto), y alguno da la zona de la pieza */
                int same = 0;
                for (int dd = 0; dd < 6; dd++) {
                    float dir3[3] = {dd == 0 ? 1.0f : dd == 1 ? -1.0f : 0.0f, dd == 4 ? 1.0f : dd == 5 ? -1.0f : 0.0f, dd == 2 ? 1.0f : dd == 3 ? -1.0f : 0.0f};
                    float o[3] = {mid[0] - 3000 * dir3[0], mid[1] - 3000 * dir3[1], mid[2] - 3000 * dir3[2]}, dist = 0; int z = 0;
                    int hit = vrhit_ray(&T[i], 1, o, dir3, 6000.0f, &dist, &z); rays++;
                    CHECK(hit == 0 && dist <= 3000.0f + 1.0f && dist > 3000.0f - 1900.0f, "rayo %d a la pieza %d: impacta (%d, %.0f)", dd, c, hit, dist);
                    if (z == q->zone) same++;
                }
                CHECK(same >= 1, "pieza %d: algún rayo da su zona (%s)", c, vrhit_zone_name(q->zone));
            }
            CHECK(nz[VRHIT_Z_HEAD] == 1 && nz[VRHIT_Z_BODY] == 3 && nz[VRHIT_Z_LIMB] == 12, "zonas: 1 cabeza, 3 cuerpo, 12 extremidad (%d %d %d)", nz[VRHIT_Z_HEAD], nz[VRHIT_Z_BODY], nz[VRHIT_Z_LIMB]);
            /* desde arriba y por los 4 lados a la altura de la cabeza: cabeza */
            const VrHitCap *hc = &T[i].cap[T[i].head];
            float hm[3] = {(hc->a[0] + hc->b[0]) / 2, (hc->a[1] + hc->b[1]) / 2, (hc->a[2] + hc->b[2]) / 2};
            static const float dirs[5][3] = {{0, -1, 0}, {1, 0, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 0, -1}};
            for (int dd = 0; dd < 5; dd++) {
                float o[3] = {hm[0] - 2000 * dirs[dd][0], hm[1] - 2000 * dirs[dd][1], hm[2] - 2000 * dirs[dd][2]}, dist; int z = 0;
                CHECK(vrhit_ray(&T[i], 1, o, dirs[dd], 4000.0f, &dist, &z) == 0 && z == VRHIT_Z_HEAD, "rayo %d a la cabeza: zona %s", dd, vrhit_zone_name(z));
            }
            /* la cabeza es la cápsula cuyo centro queda más alto entre cabeza y cuerpo, y por encima de la raíz */
            CHECK(hm[1] > T[i].c[1] + 300.0f, "cabeza por encima de la raíz (%.0f / %.0f)", hm[1], T[i].c[1]);
            /* a la altura de las espinillas, fuera de la caja del juego por abajo (la caja empieza en raíz-1000): extremidad */
            const VrHitCap *sc = &T[i].cap[11];
            float sm[3] = {(sc->a[0] + sc->b[0]) / 2, (sc->a[1] + sc->b[1]) / 2, (sc->a[2] + sc->b[2]) / 2}, o[3] = {sm[0], sm[1], sm[2] - 2500}, dz[3] = {0, 0, 1}, dist; int z = 0;
            CHECK(vrhit_ray(&T[i], 1, o, dz, 5000.0f, &dist, &z) == 0 && z == VRHIT_Z_LIMB, "espinilla: extremidad (%s)", vrhit_zone_name(z));
        }
        CHECK(sold == 2 && cams == 1, "%s: 2 soldados y 1 cámara en el mapa activo (%d, %d)", nm[k], sold, cams);
        printf("t_hit_real: %s soldados=%d camaras=%d rayos=%d\n", nm[k], sold, cams, rays);
    }
}

int main(void) {
    t_hit_zones(); t_hit_motivo(); t_hit_camera(); t_hit_floors(); t_hit_enemy(); t_hit_aim(); t_hit_fist(); t_hit_real();
    t_matrix(); t_single(); t_snapshot(); t_burst(); t_dry(); t_raise(); t_hold_auto(); t_kinds(); t_throw(); t_throw_game(); t_throw_prop(); t_grenade_gesture(); t_grenade_cancel(); t_grenade_track(); t_long_life(); t_inverse(); t_invalid_input(); t_hide(); t_read_game(); t_fuzz(); t_cfg();
    printf("test_fire: %d comprobaciones, %d fallos\n", checks, fails);
    return fails ? 1 : 0;
}
