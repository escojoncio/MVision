/* Prueba de vr_melee con las rutinas REALES del motor interpretadas sobre una instantánea de RAM (VRM_DUMPS=<dir>, fichero body.bin; sin ella se omite).
 * La instantánea nunca se sube al repositorio. Qué comprueba: (1) el golpe con el puño VR dentro de la caja del enemigo hace que el motor aplique el
 * aturdimiento original (contador +0x2A, bandera de daño) aunque la comprobación devuelva 0 (vida quitada = 0 en un puñetazo); (2) con el puño lejos no pasa
 * nada; (3) el combo golpe-golpe-patada con las peticiones fuera de las ventanas del botón (encoladas + espera) llega a la patada. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu.h"
#include "rtpatch.h"
#include "vr_melee_rt.h"

const FuncEntry rt_func_table[] = {{0, 0}};
const unsigned rt_func_count = 0;
const FuncName rt_func_names[] = {{0xFFFFFFFFu, ""}};
const unsigned rt_func_name_count = 0;
const uint32_t rt_entry = 0;
extern int interp_log_enabled;

static int fails, checks;
#define CK(c) do { checks++; if (!(c)) { fails++; printf("FALLO %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
static uint32_t R32(uint32_t a) { uint32_t v; memcpy(&v, emu_ram + (a & 0x1FFFFF), 4); return v; }
static int R16(uint32_t a) { int16_t v; memcpy(&v, emu_ram + (a & 0x1FFFFF), 2); return v; }
static void W16(uint32_t a, int v) { int16_t x = (int16_t)v; memcpy(emu_ram + (a & 0x1FFFFF), &x, 2); }
static void W32(uint32_t a, uint32_t v) { memcpy(emu_ram + (a & 0x1FFFFF), &v, 4); }
static uint8_t SAVE[EMU_RAM_SIZE];

static void call(uint32_t fn, uint32_t a0, uint32_t a1) {
    Cpu c; memset(&c, 0, sizeof c); c.r[29] = 0x801FFE00u; c.r[31] = 0xDEAD0000u; c.r[28] = 0x800AC488u; c.r[4] = a0; c.r[5] = a1;
    interp_call(&c, fn);
}
static void set_gte(int px, int py, int pz) {      /* matriz de rotación = identidad (rumbo 0) y traslación = posición del jugador, como la deja el motor al animar */
    Cpu c; memset(&c, 0, sizeof c); gte_reset();
    gte_ctc2(&c, 0, 4096); gte_ctc2(&c, 1, 0); gte_ctc2(&c, 2, 4096); gte_ctc2(&c, 3, 0); gte_ctc2(&c, 4, 4096);
    gte_ctc2(&c, 5, (uint32_t)px); gte_ctc2(&c, 6, (uint32_t)py); gte_ctc2(&c, 7, (uint32_t)pz);
}
#define PL_X -5762
#define PL_Y 1417
#define PL_Z -1310
static uint32_t WORK;
#define T1 (VRM_TARGETS + 72u)
static void reset_world(void) {
    memcpy(emu_ram, SAVE, EMU_RAM_SIZE);
    WORK = R32(VRM_PLAYER_CTRL) - 0x20;
    W32(0x800ACB40u, 1);                                  /* máscara de mapa del jugador (la instantánea la trae a 0) */
    W32(WORK + VRM_W_ANIMT, 0);
    W32(WORK + VRM_W_WPN, 6u); W16(WORK + VRM_W_LOCAL, 0);
    vrm_reset(vrmelee_instance());
}
static void place_enemy(int x, int y, int z) { W16(T1 + 8, x); W16(T1 + 10, y); W16(T1 + 12, z); W16(T1 + 0x2A, 20); W16(T1 + 6, 0); }
static VrmFrame frm(void) { VrmFrame f; memset(&f, 0, sizeof f); f.punch_side = f.knock_side = -1; f.grab_side = -1; return f; }
/* un fotograma del motor: despachador (tiempo, state, update) tras la decisión de acción */
static void engine_frame(int *time_out) {
    uint32_t t = R32(WORK + VRM_W_ANIMT);
    W32(R32(WORK + 0x9ac), 0);                            /* sin botones físicos (la instantánea trae pulsaciones viejas): solo cuentan las peticiones VR */
    W32(WORK + VRM_W_ANIMT, t + 1);
    call(R32(WORK + 0x9b4), WORK, t);
    call(R32(WORK + VRM_W_UPD), WORK, t);
    if (time_out) *time_out = (int)t;
}

int main(void) {
    const char *dd = getenv("VRM_DUMPS");
    char p[512]; FILE *fp;
    if (!dd) { printf("test_vr_melee_real: sin VRM_DUMPS, omitida\n"); return 0; }
    snprintf(p, sizeof p, "%s/body.bin", dd);
    fp = fopen(p, "rb");
    if (!fp || fread(SAVE, 1, EMU_RAM_SIZE, fp) != EMU_RAM_SIZE) { printf("test_vr_melee_real: sin %s, omitida\n", p); return 0; }
    fclose(fp);
    interp_log_enabled = 0;
    memcpy(emu_ram, SAVE, EMU_RAM_SIZE);
    int ri = vrmelee_rt_install(); CK(ri == 0);
    if (ri != 0) { printf("no se pudo enganchar (ejecutable distinto)\n"); return 1; }
    Vrm *V = vrmelee_instance();

    /* 1) puño VR dentro de la caja: la comprobación original se cumple (el origen del motor, delante del cuerpo, queda sustituido por el puño) */
    for (int far = 0; far < 2; far++) {
        reset_world(); set_gte(PL_X, PL_Y, PL_Z);
        place_enemy(PL_X + 150, 1104, PL_Z + 900);          /* a 0,9 m delante-a un lado: el origen del motor (0,6 m) NO lo alcanzaría sin el puño */
        VrmFrame f = frm(); f.punch_side = 1; f.fist_ok[1] = f.fist_closed[1] = 1;
        f.fist[1][0] = far ? (float)(PL_X + 3000) : (float)(PL_X + 150); f.fist[1][1] = 1300; f.fist[1][2] = (float)(PL_Z + 900);
        vrmelee_rt_frame(&f);
        call(VRM_FN_ACTION_IN, WORK, 0);
        CK(R32(WORK + VRM_W_ANIMFN) == VRM_FN_PUNCH);
        VrmFrame n = frm(); n.fist_ok[1] = n.fist_closed[1] = 1; memcpy(n.fist[1], f.fist[1], sizeof n.fist[1]);
        for (int i = 0; i < 6; i++) { vrmelee_rt_frame(&n); set_gte(PL_X, PL_Y, PL_Z); engine_frame(NULL); }
        if (far) { CK(R16(T1 + 0x2A) == 20); CK(V->n_hit == 0); }
        else { CK(R16(T1 + 0x2A) < 20); CK(R16(T1 + 6) & 4); CK(V->n_hit == 1); }
    }

    /* 2) combo golpe-golpe-patada con peticiones tardías (fuera de las ventanas del botón): sin espera acaba en un golpe, con la cola y la espera llega a la patada */
    for (int mode = 0; mode < 2; mode++) {
        reset_world(); set_gte(PL_X, PL_Y, PL_Z);
        place_enemy(PL_X, 1104, PL_Z + 600);
        VrmFrame f = frm(); f.punch_side = 1; f.fist_ok[1] = f.fist_closed[1] = 1; f.fist[1][0] = (float)PL_X; f.fist[1][1] = 1300; f.fist[1][2] = (float)(PL_Z + 600);
        vrmelee_rt_frame(&f); call(VRM_FN_ACTION_IN, WORK, 0);
        int req_at[2] = {8, 17};          /* VBlanks tras el inicio: la 2.ª petición cae después del instante 6 y la 3.ª después del 13 de un combo sin espera */
        int vb = 0, sawkick = 0, time = 0, maxt = 0;
        W16(T1 + 0x2A, 40);
        for (int i = 0; i < 80; i++) {
            VrmFrame n = frm(); n.fist_ok[1] = n.fist_closed[1] = 1; memcpy(n.fist[1], f.fist[1], sizeof n.fist[1]);
            n.swinging = mode ? (vb < 40) : 0;
            if (vb == req_at[0] || vb == req_at[1]) n.punch_side = 1;
            vb++;
            vrmelee_rt_frame(&n);
            /* el motor corre un fotograma de juego cada VBlank en este arnés */
            set_gte(PL_X, PL_Y, PL_Z); engine_frame(&time);
            if (time > maxt) maxt = time;
            if (V->stage >= 3) sawkick = 1;
            if (R32(WORK + VRM_W_ANIMFN) != VRM_FN_PUNCH && vb > 3) break;
        }
        if (mode == 0) CK(!sawkick); else { CK(sawkick); CK(maxt >= 22); }
        CK(R16(T1 + 0x2A) < 40);
    }
    vrmelee_rt_uninstall();
    printf("test_vr_melee_real: %d comprobaciones, %d fallos\n", checks, fails);
    return fails != 0;
}
