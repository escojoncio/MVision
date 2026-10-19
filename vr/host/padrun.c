/* padrun: recorre el juego real (host completo en MV_NOGEN, sin escenas ni render) con acciones de mandos Sense sintéticas, traducidas con la
 * MISMA tabla en C que usa la app (vr_input_update en modo Sense), y comprueba que se llega del título a la partida.
 *   padrun <exe> <disco> [--frames N] [--seq "T:acc+acc[:dur],..."] [--log]
 * T y dur en VBlanks absolutos (dur por defecto 6). Acciones: rtrig ltrig rgrip lgrip rA rB lA lB rmenu lmenu rclick lclick
 * y sticks rup rdown rleft rright lup ldown lleft lright. El indicador de menú de vr_input es "jugador fuera de control" del puente,
 * igual que en el host. Sin guion usa el estándar del ejemplo de la bitácora. MV_SIGS = firmas. No lleva datos del juego. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include "bridge.h"
#include "host.h"
#include "sys.h"
#include "vr_camera.h"
#include "vr_input.h"
#include "vrgame.h"
#include "vrhost.h"

enum { A_RTRIG, A_LTRIG, A_RGRIP, A_LGRIP, A_RA, A_RB, A_LA, A_LB, A_RMENU, A_LMENU, A_RCLICK, A_LCLICK,
       A_RUP, A_RDOWN, A_RLEFT, A_RRIGHT, A_LUP, A_LDOWN, A_LLEFT, A_LRIGHT, A_N };
static const char *AN[A_N] = {"rtrig", "ltrig", "rgrip", "lgrip", "rA", "rB", "lA", "lB", "rmenu", "lmenu", "rclick", "lclick",
                              "rup", "rdown", "rleft", "rright", "lup", "ldown", "lleft", "lright"};
typedef struct { uint32_t t, dur, m; } Ev;
static Ev ev[512];
static int nev;
static uint32_t nframes = 9000, t_ctl;
static int verbose = 1;

static uint32_t amask(const char *s, size_t len) {
    uint32_t m = 0;
    while (len) {
        size_t k = 0;
        while (k < len && s[k] != '+') k++;
        int ok = 0;
        for (int i = 0; i < A_N; i++) if (strlen(AN[i]) == k && !strncmp(s, AN[i], k)) { m |= 1u << i; ok = 1; }
        if (!ok) { fprintf(stderr, "[padrun] acción desconocida '%.*s'\n", (int)k, s); exit(1); }
        s += k; len -= k;
        if (len) { s++; len--; }
    }
    return m;
}
static int add_seq(const char *g) {
    char *dup = strdup(g), *sv = NULL;
    for (char *tok = strtok_r(dup, ",", &sv); tok; tok = strtok_r(NULL, ",", &sv)) {
        char *e;
        uint32_t t = (uint32_t)strtoul(tok, &e, 0);
        if (*e != ':' || nev >= 512) { free(dup); return -1; }
        char *b = e + 1, *c2 = strchr(b, ':');
        uint32_t d = c2 ? (uint32_t)strtoul(c2 + 1, 0, 0) : 6;
        ev[nev++] = (Ev){t, d, amask(b, c2 ? (size_t)(c2 - b) : strlen(b))};
    }
    free(dup);
    return 0;
}

static const char *PNAME[16] = {"SELECT", "L3", "R3", "START", "ARRIBA", "DERECHA", "ABAJO", "IZQUIERDA",
                                "L2", "R2", "L1", "R1", "TRIANGULO", "CIRCULO", "CRUZ", "CUADRADO"};
static vr_input_t st;
static uint16_t last_b = 0xFFFF;
static int last_menu = -1;

static uint16_t driver(uint32_t vbl, uint16_t pad, const BridgeState *bs) {
    (void)pad;
    int core = bs && (bs->valid & BRIDGE_OK_ALL_CORE) == BRIDGE_OK_ALL_CORE && bs->has_player;
    int ctl = core && bs->in_control;
    static uint32_t run;
    run = ctl ? run + 1 : 0;
    if (run >= 150 && !t_ctl) { t_ctl = vbl; fprintf(stderr, "[padrun] jugador al mando (150 VBlanks seguidos) en el VBlank %u (zona %.8s)\n", vbl, bs->stage_code); }
    if (verbose && vbl % 250 == 0) fprintf(stderr, "[padrun] vbl=%u valid=%x jugador=%d mando=%d camara=%d estado=%08x zona=%.8s\n", vbl, bs ? bs->valid : 0, bs ? bs->has_player : 0, bs ? bs->in_control : 0, vrcam_active(), bs ? (unsigned)bs->game_status : 0, bs ? bs->stage_code : "");
    uint32_t m = 0;
    for (int i = 0; i < nev; i++) if (vbl >= ev[i].t && vbl < ev[i].t + ev[i].dur) m |= ev[i].m;
    vr_input_frame_t f;
    memset(&f, 0, sizeof f);
    f.dt = 0.02f; f.game_posture = -1; f.menu = !(core && bs->in_control);
    f.head_valid = 1; f.head_pos[1] = 1.65f; f.head_quat[3] = 1;
    f.hand[0].quat[3] = f.hand[1].quat[3] = 1;
    /* derecha */
    if (m & (1u << A_RTRIG)) f.hand[1].trigger = 1;
    if (m & (1u << A_RGRIP)) f.hand[1].grip = 1;
    if (m & (1u << A_RA)) f.hand[1].buttons |= VRI_H_LOW;
    if (m & (1u << A_RB)) f.hand[1].buttons |= VRI_H_UP;
    if (m & (1u << A_RMENU)) f.hand[1].buttons |= VRI_H_MENU;
    if (m & (1u << A_RCLICK)) f.hand[1].buttons |= VRI_H_STICK;
    if (m & (1u << A_RUP)) f.hand[1].stick_y = 1;
    if (m & (1u << A_RDOWN)) f.hand[1].stick_y = -1;
    if (m & (1u << A_RLEFT)) f.hand[1].stick_x = -1;
    if (m & (1u << A_RRIGHT)) f.hand[1].stick_x = 1;
    /* izquierda */
    if (m & (1u << A_LTRIG)) f.hand[0].trigger = 1;
    if (m & (1u << A_LGRIP)) f.hand[0].grip = 1;
    if (m & (1u << A_LA)) f.hand[0].buttons |= VRI_H_LOW;
    if (m & (1u << A_LB)) f.hand[0].buttons |= VRI_H_UP;
    if (m & (1u << A_LMENU)) f.hand[0].buttons |= VRI_H_MENU;
    if (m & (1u << A_LCLICK)) f.hand[0].buttons |= VRI_H_STICK;
    if (m & (1u << A_LUP)) f.hand[0].stick_y = 1;
    if (m & (1u << A_LDOWN)) f.hand[0].stick_y = -1;
    if (m & (1u << A_LLEFT)) f.hand[0].stick_x = -1;
    if (m & (1u << A_LRIGHT)) f.hand[0].stick_x = 1;
    vr_pad_t o;
    vr_input_update(&st, &f, &o);
    if (verbose && (o.buttons != last_b || f.menu != last_menu)) {
        char nm[160] = "";
        for (int i = 0; i < 16; i++) if (o.buttons & (1u << i)) { strcat(nm, nm[0] ? " " : ""); strcat(nm, PNAME[i]); }
        fprintf(stderr, "[padrun] vbl=%u menu=%d botones=0x%04X [%s]\n", vbl, f.menu, o.buttons, nm);
        last_b = o.buttons; last_menu = f.menu;
    }
    return o.buttons;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "uso: %s <exe> <disco> [--frames N] [--seq guion]\n", argv[0]); return 1; }
    /* secuencia estándar: Menú derecho (saltar logos y pantalla de título), A (nuevo juego, dificultad), cruceta izquierda + A (continuar sin tarjeta), A/gatillo (saltar la escena y avanzar el radio) */
    const char *def = "120:rmenu,200:rmenu,302:rmenu,450:rmenu,540:rA,640:rA,740:lleft,790:rA,1500:rA";
    int custom = 0;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) nframes = (uint32_t)strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--quiet")) verbose = 0;
        else if (!strcmp(argv[i], "--seq") && i + 1 < argc) { if (add_seq(argv[++i])) { fprintf(stderr, "--seq inválido\n"); return 1; } custom = 1; }
    }
    if (!custom) {
        add_seq(def);
        /* tras la escena: gatillo derecho (confirmar y saltar) cada 83 VBlanks hasta tomar el mando */
        for (uint32_t t = 2000; t < 4600; t += 83) { char b[32]; snprintf(b, sizeof b, "%u:rtrig", t); add_seq(b); }
    }
    vr_input_init(&st);
    const char *sigs = getenv("MV_SIGS");
    if (sigs) mv_host_set_sigs(sigs);
    mkdir("build-host", 0755);
    mkdir("build-host/card", 0755);
    mv_vrhost_install();
    vrgame_set_driver(driver);
    mv_host_set_turbo(1);
    int r = mv_host_start(argv[1], argv[2], "build-host/card");
    if (r) { fprintf(stderr, "mv_host_start = %d: %s\n", r, mv_host_error()); return 1; }
    for (;;) {
        if (mv_host_state() == MV_HOST_ERROR) { fprintf(stderr, "error del motor: %s\n", mv_host_error()); return 1; }
        if (mv_host_frame_count() >= nframes || (t_ctl && mv_host_frame_count() > t_ctl + 60)) break;
        usleep(2000);
    }
    mv_host_pause();
    usleep(100000);
    if (!t_ctl) { fprintf(stderr, "[padrun] FALLO: no se llegó a la partida en %u VBlanks\n", nframes); return 2; }
    fprintf(stderr, "[padrun] OK: partida alcanzada (primer control en el VBlank %u)\n", t_ctl);
    return 0;
}
