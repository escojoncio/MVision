/* Pruebas de la visión por la cabeza VR y del factor de alcance (vr_perception) con RAM falsa y un «motor» sintético:
 * el soldado mira a +Z, cono de semiángulo `ang`, muro vertical con altura finita (línea de visión 3D), alcance `len`.
 * Reproduce la forma de la rutina real (lee la posición del personaje de VRP_A_PLAYER_POS; escribe distancia, rumbo y resultado). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vr_perception.h"
#include "vr_ai_scan.h"

static int nfail, nok;
#define CHECK(c, ...) do { if (c) nok++; else { nfail++; printf("FALLO %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint8_t RAM[VAI_RAM_SIZE];
#define ACTOR 0x80100000u
#define GLOBAL_EYE 0x800E3104u
#define VISION 0x800CB1ACu
#define SOLD_Y 1000
static int wall_on = 1, wall_z = 2000, wall_x0 = -600, wall_x1 = 600, wall_top = 1500;   /* muro en el plano z = wall_z */
static int hidden_status, calls;
static const VrpLayout LA = {"A", 2956, 2958, 2960, 2962, 3108, 3112};
static int16_t pp(int i) { return vai_r16(RAM, VRP_A_PLAYER_POS + 2u * (unsigned)i); }

/* línea de visión 3D del soldado (0, SOLD_Y, 0) al punto: choca con el muro si lo cruza por debajo de su altura */
static int blocked(int px, int py, int pz) {
    if (!wall_on || pz <= wall_z) return 0;
    float t = (float)wall_z / (float)pz, x = t * (float)px, y = SOLD_Y + t * (float)(py - SOLD_Y);
    return x >= wall_x0 && x <= wall_x1 && y <= wall_top;
}
static void engine(void *u, uint32_t addr, uint32_t a0) {
    (void)u;
    if (addr != VISION) return;
    calls++;
    int dx = pp(0), dy = pp(1) - SOLD_Y, dz = pp(2);
    if (pp(0) != 0 || pp(1) != 1000 || pp(2) != 0) {}
    float dist = sqrtf((float)dx * dx + (float)dy * dy + (float)dz * dz);
    int dir = (int)lroundf(atan2f((float)dx, (float)dz) * 4096.0f / 6.2831853f) & 4095;
    vai_w32(RAM, a0 + 3108, (uint32_t)(int32_t)dist); vai_w32(RAM, a0 + 3112, (uint32_t)dir);
    if (hidden_status) { vai_w16(RAM, a0 + 2962, 0); return; }          /* estado oculto del jugador (caja...) */
    int face = vai_r16(RAM, a0 + 2956), ang = vai_r16(RAM, a0 + 2958), len = vai_r16(RAM, a0 + 2960);
    int diff = (dir - face) & 4095; if (diff > 2048) diff = 4096 - diff;
    int g = (int32_t)vai_r32(RAM, GLOBAL_EYE);
    int16_t f = 0;
    if (dist <= g + 2000 && (dist < 500 || diff < ang) && !blocked(dx, pp(1), dz)) f = dist > len ? 1 : 2;
    vai_w16(RAM, a0 + 2962, f);
}
static void guard(void *u, int e) { (void)u; (void)e; }

static void setup(VrpCtx *x, VrpPreset p, int px, int py, int pz) {
    memset(RAM, 0, sizeof RAM);
    VrpHost h = {NULL, engine, guard};
    vrp_init(x, RAM, &h, p);
    x->sites.found = 1; x->sites.vision = VISION; x->sites.lay = &LA; x->sites.eye_global = GLOBAL_EYE;
    vai_w16(RAM, ACTOR + 2960, 4000); vai_w16(RAM, ACTOR + 2958, 512); vai_w16(RAM, ACTOR + 2956, 0);
    vai_w32(RAM, GLOBAL_EYE, 4000);
    vai_w16(RAM, VRP_A_PLAYER_POS, (int16_t)px); vai_w16(RAM, VRP_A_PLAYER_POS + 2, (int16_t)py); vai_w16(RAM, VRP_A_PLAYER_POS + 4, (int16_t)pz);
    x->cfg.range_scale = 1.0f;
    hidden_status = 0; calls = 0; wall_on = 1;
    vrp_set_time(x, 1.0f);
}
static int result(void) { return vai_r16(RAM, ACTOR + 2962); }
static const float NOHEAD[3] = {0, 0, 0};

static void eval(VrpCtx *x, const float *off, float t) {
    vrp_set_time(x, t);
    if (off) { vrp_set_head(x, off); } else x->head_valid = 0;
    vrp_vision(x, ACTOR);
}

static void t_peek(void) {
    VrpCtx x;
    /* cuerpo detrás del muro (z=3000 > wall_z, y=1000 < altura), sin cabeza: no ve */
    setup(&x, VRP_VR, 0, 1000, 3000);
    eval(&x, NULL, 1.0f);
    CHECK(result() == 0, "cuerpo tras el muro, sin cabeza: no ve (%d)", result());
    /* cabeza dentro de la zona muerta: igual */
    setup(&x, VRP_VR, 0, 1000, 3000);
    eval(&x, (float[3]){0, 60, 0}, 1.0f);
    CHECK(result() == 0 && x.st.head_evals == 0, "cabeza casi quieta: ni se evalúa");
    /* cabeza asomada por ENCIMA del muro: ve (y la distancia/rumbo quedan los del punto elegido) */
    setup(&x, VRP_VR, 0, 1000, 3000);
    eval(&x, (float[3]){0, 1400, 0}, 1.0f);
    CHECK(result() == 2 && x.st.head_evals == 1 && x.st.seen_head == 1 && x.st.seen_body == 0, "asoma por encima: ve por la cabeza (%d)", result());
    CHECK(pp(0) == 0 && pp(1) == 1000 && pp(2) == 3000, "posición del personaje restaurada");
    CHECK(vai_r16(RAM, ACTOR + 2960) == 4000 && (int32_t)vai_r32(RAM, GLOBAL_EYE) == 4000, "alcance restaurado");
    const VrpEntry *e = vrp_entry(&x, ACTOR, 0);
    CHECK(e && e->via == 2, "entrada: visto por la cabeza");
    /* cabeza asomada por el lado del muro: ve */
    setup(&x, VRP_VR, 0, 1000, 3000);
    eval(&x, (float[3]){1500, 0, 0}, 1.0f);
    CHECK(result() == 2 && x.st.seen_head == 1, "asoma por el lado: ve");
    /* detrás de la pared con la cabeza que NO sale de detrás: no ve (muro sigue bloqueando) */
    setup(&x, VRP_VR, 0, 1000, 3000);
    eval(&x, (float[3]){300, 300, 0}, 1.0f);
    CHECK(result() == 0, "cabeza detrás del muro: no ve (%d)", result());
    /* cuerpo visible, cabeza oculta: ve por el cuerpo (la cabeza ni se evalúa) */
    setup(&x, VRP_VR, 0, 2000, 3000);
    eval(&x, (float[3]){0, -900, 0}, 1.0f);
    CHECK(result() == 2 && x.st.seen_body == 1 && x.st.head_evals == 0, "cuerpo a la vista: ve por el cuerpo");
    /* sin muro, fuera del cono, ni cuerpo ni cabeza ven; con una gran asomada tampoco si sigue fuera del cono */
    setup(&x, VRP_VR, 3000, 1000, 100);
    wall_on = 0;
    eval(&x, (float[3]){200, 0, 0}, 1.0f);
    CHECK(result() == 0, "fuera del cono: no ve");
    setup(&x, VRP_VR, 3000, 1000, 100); wall_on = 0;
    eval(&x, (float[3]){0, 900, 0}, 1.0f);
    CHECK(result() == 0, "fuera del cono, cabeza arriba: no ve");
    /* asomarse hacia dentro del cono sí cuenta */
    setup(&x, VRP_VR, 3500, 1000, 3000); wall_on = 0;
    eval(&x, NULL, 1.0f);
    CHECK(result() == 0, "justo fuera del cono: no ve");
    setup(&x, VRP_VR, 3500, 1000, 3000); wall_on = 0;
    eval(&x, (float[3]){-900, 0, 0}, 1.0f);
    CHECK(result() >= 1 && x.st.head_evals == 1, "la cabeza entra en el cono: lo ve el motor (%d)", result());
}

static void t_modes(void) {
    VrpCtx x;
    /* ORIGINAL: ni envoltorio ni cabeza (RAM idéntica a la original) */
    static uint8_t ref[VAI_RAM_SIZE];
    setup(&x, VRP_ORIGINAL, 0, 1000, 3000);
    engine(NULL, VISION, ACTOR); memcpy(ref, RAM, sizeof RAM);
    setup(&x, VRP_ORIGINAL, 0, 1000, 3000);
    eval(&x, (float[3]){0, 900, 0}, 1.0f);
    CHECK(!memcmp(ref, RAM, sizeof RAM) && result() == 0, "ORIGINAL: nada cambia");
    /* interruptor del usuario */
    setup(&x, VRP_VR, 0, 1000, 3000);
    vrp_set_head_enabled(&x, 0);
    eval(&x, (float[3]){0, 900, 0}, 1.0f);
    CHECK(result() == 0 && x.st.head_evals == 0, "cabeza desactivada: no ve");
    /* desplazamiento caducado (0,25 s) o inválido */
    setup(&x, VRP_VR, 0, 1000, 3000);
    vrp_set_head(&x, (float[3]){0, 900, 0}); vrp_set_time(&x, 1.5f); vrp_vision(&x, ACTOR);
    CHECK(result() == 0 && x.st.head_evals == 0, "desplazamiento caducado: ignorado");
    setup(&x, VRP_VR, 0, 1000, 3000);
    vrp_set_head(&x, (float[3]){NAN, 0, 0});
    CHECK(!x.head_valid, "NaN rechazado");
    /* cabeza con la que el cuerpo no es visible pero 'no le tocaba' evaluar: se restaura el campo y no se evalúa la cabeza */
    setup(&x, VRP_VR, 0, 1000, 3000);
    calls = 0; vai_w16(RAM, ACTOR + 2962, 5);
    /* el motor falso siempre evalúa; la ruta «no le tocaba» está cubierta en test_perception */
}

static void t_stance(void) {
    VrpCtx x;
    /* Agachado (personaje y=496 como en el volcado) y usuario sentado SIN moverse: desplazamiento 0 -> resultado y RAM idénticos a la original */
    static uint8_t ref[VAI_RAM_SIZE];
    setup(&x, VRP_VR, 0, 496, 3000);
    vai_w32(RAM, VRP_A_PLAYER_STATUS, VRP_PS_SQUAT);
    engine(NULL, VISION, ACTOR); memcpy(ref, RAM, sizeof RAM);
    setup(&x, VRP_VR, 0, 496, 3000);
    vai_w32(RAM, VRP_A_PLAYER_STATUS, VRP_PS_SQUAT);
    eval(&x, NOHEAD, 1.0f);
    CHECK(result() == 0 && x.st.head_evals == 0, "agachado y quieto: oculto como el original");
    CHECK(vai_r16(RAM, ACTOR + 2962) == vai_r16(ref, ACTOR + 2962) && !memcmp(RAM + 0x100000, ref + 0x100000, 0x1000), "agachado: estado del soldado igual al original");
    /* pequeñas oscilaciones del usuario sentado (<8 cm) no lo delatan */
    setup(&x, VRP_VR, 0, 496, 3000);
    vai_w32(RAM, VRP_A_PLAYER_STATUS, VRP_PS_SQUAT);
    eval(&x, (float[3]){40, 50, 30}, 1.0f);
    CHECK(result() == 0, "agachado con balanceo: sigue oculto");
    /* levantarse de verdad (cabeza muy por encima del ancla) sí asoma */
    setup(&x, VRP_VR, 0, 496, 3000);
    vai_w32(RAM, VRP_A_PLAYER_STATUS, VRP_PS_SQUAT);
    eval(&x, (float[3]){0, 1500, 0}, 1.0f);
    CHECK(result() == 2, "agachado pero con la cabeza física por encima del muro: ve");
    /* tumbado + estado oculto del juego (caja): ni cuerpo ni cabeza */
    setup(&x, VRP_VR, 0, 2000, 3000);
    hidden_status = 1;
    eval(&x, (float[3]){0, 900, 0}, 1.0f);
    CHECK(result() == 0, "estado oculto del juego manda también con la cabeza");
}

static void t_scale(void) {
    VrpCtx x;
    int sc[3] = {100, 150, 200};
    for (int k = 0; k < 3; k++) {
        float s = sc[k] / 100.0f;
        setup(&x, VRP_VR, 0, 1000, 5500); wall_on = 0; x.cfg.range_scale = s;
        vrp_set_time(&x, 1.0f); vrp_vision(&x, ACTOR);
        int expect = (4000.0f * s >= 4500.0f) ? 2 : 1;       /* distancia 3D = 4500: dentro del alcance escalado -> alerta inmediata */
        CHECK(result() == expect, "factor %.1f: dist 4500 -> %d (esperado %d)", s, result(), expect);
        CHECK(vai_r16(RAM, ACTOR + 2960) == 4000, "alcance restaurado (factor %.1f)", s);
    }
    /* el motor ve el alcance escalado y la zona de sospecha escalada */
    setup(&x, VRP_VR, 0, 1000, 5500); wall_on = 0; x.cfg.range_scale = 2.0f;
    int seen_len = 0;
    vrp_vision(&x, ACTOR);
    CHECK(result() == 2, "factor 2: alerta a 4500");
    /* ORIGINAL ignora el factor */
    setup(&x, VRP_ORIGINAL, 0, 1000, 5500); wall_on = 0; x.cfg.range_scale = 2.0f;
    vrp_vision(&x, ACTOR);
    CHECK(result() == 1, "ORIGINAL: sin factor");
    (void)seen_len;
    /* factor 1: lejos sigue siendo sospecha gradual hasta 2,4 L */
    setup(&x, VRP_VR, 0, 1000, 8000); wall_on = 0;
    vrp_vision(&x, ACTOR);
    CHECK(result() == 1, "factor 1: 8000 = zona de sospecha, aún no alerta");
}

static void t_offset(void) {
    float off[3], pref[3] = {-0.155f, 0.932f, -0.128f};
    vrp_head_offset(25.0f, 1070.0f, pref, pref, NULL, off);
    CHECK(fabsf(off[0]) + fabsf(off[1]) + fabsf(off[2]) < 1e-3f, "cabeza en el ancla: cero");
    float h[3] = {pref[0], pref[1] + 0.3f, pref[2]};
    vrp_head_offset(0.0f, 1000.0f, pref, h, NULL, off);
    CHECK(fabsf(off[1] - 300.0f) < 0.5f && fabsf(off[0]) < 0.5f && fabsf(off[2]) < 0.5f, "0,3 m arriba = 300 unidades");
    /* horizontal: la norma se conserva con cualquier giro, y el giro es el inverso del de vr_headcol (R^T) */
    float hh[3] = {pref[0] + 0.2f, pref[1], pref[2] + 0.1f};
    for (float th = 0; th < 360; th += 45) {
        vrp_head_offset(th, 1000.0f, pref, hh, NULL, off);
        float n = sqrtf(off[0] * off[0] + off[2] * off[2]);
        CHECK(fabsf(n - 223.607f) < 0.5f, "norma horizontal conservada (th=%.0f: %.2f)", th, n);
    }
    /* th=0: vr_headcol usa R con th+180 = 180 grados: W - eye = upm*R^T d = (-d0, d1, -d2) */
    vrp_head_offset(0.0f, 1000.0f, pref, hh, NULL, off);
    CHECK(fabsf(off[0] + 200.0f) < 0.5f && fabsf(off[2] + 100.0f) < 0.5f, "theta 0: media vuelta respecto al usuario (%.1f %.1f)", off[0], off[2]);
    /* el recorte se suma */
    float corr[3] = {-0.2f, 0, 0};
    vrp_head_offset(0.0f, 1000.0f, pref, hh, corr, off);
    CHECK(fabsf(off[0]) < 0.5f, "recorte contra el escenario aplicado");
    VrpConfig c; vrp_config_preset(&c, VRP_VR);
    int16_t b[3] = {100, 200, 300}, o[3];
    CHECK(!vrp_head_point(&c, b, (float[3]){30, 40, 0}, o), "dentro de la zona muerta: 0");
    CHECK(vrp_head_point(&c, b, (float[3]){0, 580, 0}, o) && o[1] == 200 + 500 && o[0] == 100, "zona muerta restada (continuo)");
    CHECK(vrp_head_point(&c, b, (float[3]){0, 9000, 0}, o) && o[1] == (int16_t)(200 + 9000 * 1500.0f / 9000.0f), "tope del desplazamiento");
    int16_t big[3] = {32000, 0, 0};
    CHECK(vrp_head_point(&c, big, (float[3]){2000, 0, 0}, o) && o[0] > 32000, "saturación sin desbordar");
}

static void t_log(void) {
    VrpCtx x; char buf[200];
    setup(&x, VRP_VR, 0, 1000, 3000);
    x.log_t = 0;
    eval(&x, (float[3]){0, 1400, 0}, 1.0f);                       /* ve por la cabeza */
    CHECK(vrp_log_tick(&x, buf, sizeof buf) && strstr(buf, "[ia]") && strstr(buf, "ven=1") && strstr(buf, "cabeza_vr=1") && strstr(buf, "cuerpo=0"), "línea [ia]: %s", buf);
    vrp_set_time(&x, 1.5f);
    CHECK(!vrp_log_tick(&x, buf, sizeof buf), "como mucho una por segundo");
    vrp_set_time(&x, 2.2f);
    CHECK(vrp_log_tick(&x, buf, sizeof buf) && strstr(buf, "ven=0"), "una última con 0 al dejar de verse: %s", buf);
    vrp_set_time(&x, 5.0f);
    CHECK(!vrp_log_tick(&x, buf, sizeof buf), "y después silencio");
    vrp_set_time(&x, 7.0f);
    CHECK(!vrp_log_tick(&x, buf, sizeof buf), "sin soldados que ven: silencio");
    /* cuerpo */
    setup(&x, VRP_VR, 0, 2000, 3000); x.log_t = 0;
    eval(&x, NULL, 1.0f);
    CHECK(vrp_log_tick(&x, buf, sizeof buf) && strstr(buf, "cuerpo=1") && strstr(buf, "cabeza_vr=0"), "por el cuerpo: %s", buf);
}

int main(void) {
    t_peek(); t_modes(); t_stance(); t_scale(); t_offset(); t_log();
    printf("%d comprobaciones OK, %d fallos\n", nok, nfail);
    return nfail != 0;
}
