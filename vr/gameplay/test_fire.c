/* Pruebas sintéticas de vr_fire: geometría (matriz del motor, correspondencia usuario->mundo), lógica de pulsación del
 * botón de ataque contra un "juego falso" que reproduce lo medido en el juego real, sustitución de proyectiles, ocultar el
 * arma sobre una RAM sintética y robustez. Sin datos del juego. Compilar con ASan/UBSan (make test-san). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    /* identidad: delante de usuario -Z con theta 0 = +Z del mundo -> RotMatrixYXZ(-1024, 0, 0) = [[1,0,0],[0,0,1],[0,-1,0]] */
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
    int pass_n, skip_n, subst_n;
    int first_vb;
} FGame;
static void fg_init(FGame *g, int mag, int hold) { memset(g, 0, sizeof *g); g->mag = mag; g->weapon_hold = hold; }
static void fg_projectile(FGame *g) {
    VrFireMat m; memset(&m, 0, sizeof m);
    int r = vrfire_take_projectile(g->kind, &m);
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
    in.fire = 1; vstep(&g, &in, NULL); in.fire = 0; settle(&g, in, 10, NULL);
    int16_t pos[3], vel[3];
    CHECK(vrfire_take_throw(pos, vel) == 1, "datos de lanzamiento");
    CHECK(pos[0] == -300 && pos[1] == 200 && pos[2] == 400, "posición del motor (%d %d %d)", pos[0], pos[1], pos[2]);
    CHECK(vel[0] == -40 && vel[1] == 80 && vel[2] == 200, "velocidad por tick (%d %d %d)", vel[0], vel[1], vel[2]);
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

int main(void) {
    t_matrix(); t_single(); t_snapshot(); t_burst(); t_dry(); t_raise(); t_hold_auto(); t_kinds(); t_throw(); t_invalid_input(); t_hide(); t_read_game(); t_fuzz(); t_cfg();
    printf("test_fire: %d comprobaciones, %d fallos\n", checks, fails);
    return fails ? 1 : 0;
}
