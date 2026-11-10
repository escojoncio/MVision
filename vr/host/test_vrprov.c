/* Prueba de vrprov.h: etiqueta de vista, acumulador/registro [prov] y selección del volcado automático por procedencia. make -C vr/host test */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vrprov.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "FALLO %d: ", __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); fails++; } } while (0)

int main(void) {
    /* vista */
    CHECK(vrprov_view(VRV_K_WORLD, VRV_R_WORLD) == VP_V_WORLD, "mundo");
    CHECK(vrprov_view(VRV_K_WORLD, VRV_R_SELECTOR) == VP_V_SELECTOR && vrprov_view(VRV_K_UI, VRV_R_MENU) == VP_V_SELECTOR, "selector");
    CHECK(vrprov_view(VRV_K_WORLD, VRV_R_PAUSE_WORLD) == VP_V_PAUSE && vrprov_view(VRV_K_UI, VRV_R_PAUSED) == VP_V_PAUSE, "pausa");
    CHECK(vrprov_view(VRV_K_UI, VRV_R_COMMS) == VP_V_CODEC, "codec");
    CHECK(vrprov_view(VRV_K_ENGINE, VRV_R_CINEMATIC) == VP_V_CINE, "cine");
    CHECK(vrprov_view(VRV_K_VIDEO, VRV_R_STOPPED) == VP_V_OTHER, "otra");

    /* acumulador: id 1 = scope.c (3 ops, uno semitransparente abr 2), id 2 = otro (1 op), sin id (1 op) */
    GpuOp ops[5]; memset(ops, 0, sizeof ops);
    ProvEntry tab[3]; memset(tab, 0, sizeof tab);
    tab[1].id = 1; tab[1].fn = 0x80012345u; strcpy(tab[1].name, "scope.c");
    tab[2].id = 2; tab[2].fn = 0x80023456u;
    for (int i = 0; i < 3; i++) { ops[i].type = GPU_OP_RECT; ops[i].prov = 1; ops[i].v[0].x = (int16_t)(10 * i); ops[i].v[0].y = 5; ops[i].rw = 4; ops[i].rh = 6; }
    ops[1].flags = GPU_F_SEMI; ops[1].abr = 2;
    ops[3].type = GPU_OP_POLY; ops[3].nv = 3; ops[3].prov = 2; ops[3].v[0].x = -3; ops[3].v[1].x = 7; ops[3].v[2].y = 9;
    ops[4].type = GPU_OP_FILL;
    GpuDrawList l; memset(&l, 0, sizeof l); l.ops = ops; l.nops = 5; l.prov = tab; l.nprov = 3;
    static VrProvAcc A;
    vrprov_acc(&A, &l);
    vrprov_acc(&A, &l);
    char b[1024];
    int n = vrprov_report(&A, VP_V_SELECTOR, b, sizeof b);
    CHECK(n > 0 && n == (int)strlen(b), "longitud de la línea");
    CHECK(strstr(b, "[prov] vista=selector listas=2 ops=10 |"), "cabecera: %s", b);
    CHECK(strstr(b, "scope.c|80012345: ops=6 box=(0,5)-(24,11) abr=2;"), "scope.c: %s", b);
    CHECK(strstr(b, "?|80023456: ops=2 box=(-3,0)-(7,9) abr=-;"), "otro: %s", b);
    CHECK(strstr(b, "-|00000000: ops=2 box=") , "sin id: %s", b);
    CHECK(strstr(b, "scope.c") < strstr(b, "?|8002"), "orden por ops");
    CHECK(vrprov_report(&A, VP_V_WORLD, b, sizeof b) == 0, "vacío tras el informe");
    /* id efectivo: lo que encadena la ordenación del canal 3D (0x8001da18) se agrupa por quien escribió el paquete (prov2), no por la ordenación; sin escritor, sin id */
    {
        GpuOp co[6]; memset(co, 0, sizeof co);
        ProvEntry ct[4]; memset(ct, 0, sizeof ct);
        ct[1].id = 1; ct[1].fn = 0x8001da18u;
        ct[2].id = 2; ct[2].fn = 0x800c5000u; strcpy(ct[2].name, "chaff.c");
        ct[3].id = 3; ct[3].fn = 0x800394f8u; strcpy(ct[3].name, "menuman.c");
        for (int i = 0; i < 6; i++) { co[i].type = GPU_OP_RECT; co[i].prov = 1; co[i].v[0].x = (int16_t)(10 * i); co[i].rw = 2; co[i].rh = 2; }
        co[0].prov2 = co[1].prov2 = co[2].prov2 = 2;   /* efecto encadenado por la ordenación */
        co[3].prov = 3; co[3].prov2 = 2;               /* ámbito normal: manda quien encadenó */
        GpuDrawList cl; memset(&cl, 0, sizeof cl); cl.ops = co; cl.nops = 6; cl.prov = ct; cl.nprov = 4;
        vrprov_acc(&A, &cl);
        char cb[1024]; int cn = vrprov_report(&A, VP_V_WORLD, cb, sizeof cb);
        CHECK(cn > 0 && strstr(cb, "chaff.c|800c5000: ops=3 "), "encadenado por la ordenación: se agrupa por el escritor: %s", cb);
        CHECK(strstr(cb, "menuman.c|800394f8: ops=1 "), "ámbito normal: por quien encadenó: %s", cb);
        CHECK(strstr(cb, "-|00000000: ops=2 ") && !strstr(cb, "8001da18"), "ordenación sin escritor: sin id; la ordenación no aparece: %s", cb);
    }
    /* top 16 */
    {
        static GpuOp many[40]; static ProvEntry tb[40]; memset(many, 0, sizeof many); memset(tb, 0, sizeof tb);
        for (int i = 0; i < 40; i++) { many[i].type = GPU_OP_RECT; many[i].prov = (uint16_t)(i + 1); tb[i].id = (uint16_t)i; }
        GpuDrawList l2; memset(&l2, 0, sizeof l2); l2.ops = many; l2.nops = 40;
        vrprov_acc(&A, &l2);
        char big[4096]; int m = vrprov_report(&A, VP_V_WORLD, big, sizeof big);
        int cnt = 0; for (const char *p = big; (p = strstr(p, "ops=1 box")); p++) cnt++;
        CHECK(m > 0 && cnt == 16, "solo 16 entradas (%d)", cnt);
        vrprov_acc(&A, &l2);
        char small[64]; int m2 = vrprov_report(&A, VP_V_WORLD, small, sizeof small);
        CHECK(m2 > 0 && m2 < 64 && strlen(small) == (size_t)m2, "buffer pequeño: recortado sin desbordar");
    }

    /* volcado automático: cada actor una vez y las vistas selector/pausa/códec una vez */
    CHECK(vrprov_actor_idx("scope.c") == 0 && vrprov_actor_idx("cinema.c") == 1 && vrprov_actor_idx("telop.c") == 2 && vrprov_actor_idx("motse.c") == 3, "nombres");
    CHECK(vrprov_actor_idx("hud.c") == -1 && vrprov_actor_idx("") == -1 && vrprov_actor_idx(NULL) == -1, "otros nombres");
    VrProvTrig t; memset(&t, 0, sizeof t);
    CHECK(vrprov_pick(&t, 0, VP_V_WORLD) == -1, "nada presente en el mundo");
    int c = vrprov_pick(&t, 1u << 2 | 1u << 0, VP_V_WORLD);
    CHECK(c == 0, "primero scope.c (%d)", c);
    CHECK(vrprov_pick(&t, 1u << 2 | 1u << 0, VP_V_WORLD) == 0, "sin marcar se repite (volcado no pedido)");
    vrprov_done(&t, c);
    c = vrprov_pick(&t, 1u << 2 | 1u << 0, VP_V_WORLD);
    CHECK(c == 2, "luego telop.c (%d)", c);
    vrprov_done(&t, c);
    CHECK(vrprov_pick(&t, 1u << 2 | 1u << 0, VP_V_WORLD) == -1, "ya hechos");
    c = vrprov_pick(&t, 0, VP_V_SELECTOR);
    CHECK(c == VP_T_VIEW0 + VP_V_SELECTOR && !strcmp(vrprov_code_name(c), "view_selector"), "selector");
    vrprov_done(&t, c);
    CHECK(vrprov_pick(&t, 0, VP_V_SELECTOR) == -1, "selector una sola vez");
    c = vrprov_pick(&t, 0, VP_V_CODEC); CHECK(c == VP_T_VIEW0 + VP_V_CODEC && !strcmp(vrprov_code_name(c), "view_codec"), "codec"); vrprov_done(&t, c);
    c = vrprov_pick(&t, 0, VP_V_PAUSE); CHECK(c == VP_T_VIEW0 + VP_V_PAUSE && !strcmp(vrprov_code_name(c), "view_pause"), "pausa"); vrprov_done(&t, c);
    CHECK(vrprov_pick(&t, 0, VP_V_CINE) == -1 && vrprov_pick(&t, 0, VP_V_WORLD) == -1, "cine y mundo no disparan");
    CHECK(!strcmp(vrprov_code_name(1), "cinema") && !strcmp(vrprov_code_name(3), "motse"), "nombres de motivo");

    printf(fails ? "vrprov: %d FALLOS\n" : "vrprov: OK\n", fails);
    return fails ? 1 : 0;
}
