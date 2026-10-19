/* Pruebas de vr_activity con datos sintéticos (RAM de 2 MB construida a mano); ASan/UBSan en `make test-activity-san`. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vr_activity.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FALLO %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static uint8_t ram[0x200000];
static BridgeState st;
static VrActivity A;

static void w32(uint32_t a, uint32_t v) { memcpy(ram + (a & 0x1FFFFF), &v, 4); }
static void w16(uint32_t a, int v) { uint16_t x = (uint16_t)v; memcpy(ram + (a & 0x1FFFFF), &x, 2); }
static int r16(uint32_t a) { int16_t v; memcpy(&v, ram + (a & 0x1FFFFF), 2); return v; }

/* Hashes de las clases (los mismos que la tabla; los nombres no se escriben en el repo). */
#define H_SOLDIER 0x7f1a6140u
#define H_SENSOR  0x848a7679u   /* solo lista >= 4 */
#define H_SENSOR2 0x909c58f7u

/* bloque de objeto 3D válido en `p` con modelo `def` (def: nº de modelos = n) */
static void mk_objs(uint32_t p, uint32_t def, int n, uint32_t flag, uint32_t group, int bound, int32_t x, int32_t y, int32_t z) {
    w32(def, (uint32_t)n); w32(def + 4, (uint32_t)n);
    w32(p + 0x14, (uint32_t)x); w32(p + 0x18, (uint32_t)y); w32(p + 0x1c, (uint32_t)z);
    w32(p + 0x24, def); w32(p + 0x28, flag); w16(p + 0x2c, (int)group); w16(p + 0x2e, n);
    w16(p + 0x30, 0); w16(p + 0x32, bound);
    w32(p + 0x48 + 0x40, def + 0x20);
}

static void mk_map(unsigned i, int used, uint32_t haz, uint32_t def, uint32_t zones, int nz, int x0, int z0) {
    uint32_t m = VRA_A_MAPS + i * VRA_MAP_STRIDE;
    w32(m, 1u << i); w16(m + 4, 1); w16(m + 6, used); w32(m + 8, haz); w32(haz, def);
    w16(def + 12, nz); w32(def + 20, zones);
    for (int z = 0; z < nz; z++) {
        uint32_t q = zones + (uint32_t)z * 24;
        w16(q, x0 + 1000 * z); w16(q + 2, z0 + 2000 * z); w16(q + 4, 100 * z); w16(q + 6, 500); w16(q + 8, 750);
    }
}

static void actor(int i, uint32_t addr, int level, uint32_t hash) {
    (void)hash;
    st.actors[i].addr = addr;
    st.actors[i].level = (uint8_t)level;
}

static void setup(void) {
    memset(ram, 0, sizeof ram);
    memset(&st, 0, sizeof st);
    vra_init(&A);
    w32(VRA_A_CUR_GROUP, 1);
    mk_map(0, 1, 0x80100000u, 0x80100100u, 0x80100200u, 10, -4000, -20000);
    st.valid = BRIDGE_OK_ALL_CORE; st.has_player = 1;
    st.player_pos.x = 0; st.player_pos.y = 1000; st.player_pos.z = 0;
}

static void test_hash_and_names(void) {
    CHECK(vra_name_hash("abc") == 0x1A47E90Bu);          /* FNV-1a 32 de "abc" */
    CHECK(vra_name_hash("") == 0x811c9dc5u);
    CHECK(vra_classify_hash(H_SOLDIER, 4) == VRA_CLASS_SOLDIER);
    CHECK(vra_classify_hash(H_SOLDIER, 8) == VRA_CLASS_OTHER);
    CHECK(vra_classify_hash(H_SENSOR, 4) == VRA_CLASS_SENSOR);
    CHECK(vra_classify_hash(H_SENSOR, 2) == VRA_CLASS_OTHER);   /* misma cadena que la cámara de juego (lista 2) */
    CHECK(vra_classify_hash(H_SENSOR2, 3) == VRA_CLASS_SENSOR);
    CHECK(vra_classify_hash(0x12345678u, 4) == VRA_CLASS_OTHER);
    CHECK(vra_classify_name(NULL, 4) == VRA_CLASS_OTHER);
    CHECK(vra_classify_name("", 4) == VRA_CLASS_OTHER);
    CHECK(vra_classify_name("zzz-no-existe", 4) == VRA_CLASS_OTHER);
}

static void test_zone(void) {
    setup();
    int32_t box[6];
    int32_t d = vra_zone_diag(ram, box);
    CHECK(box[0] == -4500 && box[3] == -4000 + 9000 + 500);
    CHECK(box[2] == -20000 - 750 && box[5] == -20000 + 18000 + 750);
    int64_t dx = box[3] - box[0], dy = box[4] - box[1], dz = box[5] - box[2];
    double e = (double)(dx * dx + dy * dy + dz * dz);
    CHECK(d > 0 && (double)d * d <= e && (double)(d + 1) * (d + 1) > e);
    /* mapa no usado se ignora */
    mk_map(1, 0, 0x80101000u, 0x80101100u, 0x80101200u, 5, 30000, 30000);
    CHECK(vra_zone_diag(ram, NULL) == d);
    /* mapa usado con bit de índice erróneo: se ignora */
    w32(VRA_A_MAPS + 20, 4); w16(VRA_A_MAPS + 26, 1);
    CHECK(vra_zone_diag(ram, NULL) == d);
    /* sin mapas -> 0; punteros fuera de RAM */
    w32(VRA_A_MAPS + 8, 0x90000000u); w32(VRA_A_MAPS, 1);
    CHECK(vra_zone_diag(ram, NULL) == 0);
    w32(VRA_A_MAPS + 8, 0x80100000u); w16(0x80100100u + 12, 5000);   /* nº de zonas absurdo */
    CHECK(vra_zone_diag(ram, NULL) == 0);
    w16(0x80100100u + 12, 10); w32(0x80100100u + 20, 0x801FFFF8u);   /* zonas que se salen de la RAM */
    CHECK(vra_zone_diag(ram, NULL) == 0);
}

static const char *NM_S, *NM_C;

static void test_frame_and_decisions(void) {
    setup();
    /* nombres: se usa vra_test_register para mapear dos cadenas neutras a las clases (tabla de pruebas) */
    vra_register_name("tst_sold", VRA_CLASS_SOLDIER, 3, 6);
    vra_register_name("tst_sens", VRA_CLASS_SENSOR, 3, 6);
    vra_register_name("tst_game", VRA_CLASS_SENSOR, 4, 6);
    CHECK(vra_register_name("x", VRA_CLASS_OTHER, 0, 8) == -1 && vra_register_name("", VRA_CLASS_SENSOR, 0, 8) == -1);
    CHECK(vra_register_name("x", VRA_CLASS_SENSOR, 5, 2) == -1 && vra_register_name("x", VRA_CLASS_SENSOR, 0, 9) == -1);
    st.n_actors = 6;
    /* actor 0: soldado cerca; objs en +156 y arma en +400 */
    uint32_t a0 = 0x80160000u, a1 = 0x80162000u, a2 = 0x80164000u, a3 = 0x80166000u, a4 = 0x80168000u, a5 = 0x8016a000u;
    actor(0, a0, 4, 0); strcpy(st.actors[0].name, "tst_sold");
    actor(1, a1, 4, 0); strcpy(st.actors[1].name, "tst_sold");
    actor(2, a2, 4, 0); strcpy(st.actors[2].name, "tst_sens");
    actor(3, a3, 2, 0); strcpy(st.actors[3].name, "tst_game");     /* lista 2: no es sensor */
    actor(4, a4, 5, 0); strcpy(st.actors[4].name, "otro");
    actor(5, a5, 4, 0); strcpy(st.actors[5].name, "tst_sold");     /* sin objs válido */
    mk_objs(0x80170000u, 0x80171000u, 16, 0x32d, 1, 0, 3000, 1000, -4000);
    w32(a0 + 156, 0x80170000u);
    mk_objs(0x80172000u, 0x80173000u, 1, 0x6d, 1, 0, 3000, 1100, -4000);
    w32(a0 + 400, 0x80172000u);
    mk_objs(0x80174000u, 0x80175000u, 16, 0x32d, 1, 0, 30000, 1000, -4000);   /* soldado lejano (> diagonal?) */
    w32(a1 + 156, 0x80174000u);
    mk_objs(0x80176000u, 0x80177000u, 4, 0x2d, 1, 0, 6000, 1000, 2000);
    w32(a2 + 200, 0x80176000u);
    mk_objs(0x80178000u, 0x80179000u, 4, 0x2d, 1, 0, 100, 1000, 100);
    w32(a3 + 200, 0x80178000u);                                                 /* cámara de juego: ignorada */
    w32(a4 + 8, 0x80170000u);                                                   /* otro actor, apunta a un objs ya seguido */
    st.player_pos.x = 0; st.player_pos.y = 1000; st.player_pos.z = 0;

    /* calentamiento: inactivo hasta VRA_WARMUP fotogramas válidos */
    for (int i = 0; i < VRA_WARMUP - 1; i++) { vra_frame(&A, ram, &st); CHECK(!A.active); }
    int n = vra_frame(&A, ram, &st);
    CHECK(A.active);
    CHECK(n == 4);   /* a0, a1, a2, a5 */
    CHECK(A.n_class[VRA_CLASS_SOLDIER] == 3 && A.n_class[VRA_CLASS_SENSOR] == 1);
    CHECK(A.tracked[0].n_objs == 2 && A.tracked[0].objs[0] == 0x80170000u && A.tracked[0].objs[1] == 0x80172000u);
    CHECK(A.tracked[3].n_objs == 0);
    CHECK(A.zone_diag > 20000);
    CHECK(vra_radius(&A, VRA_CLASS_SOLDIER) == A.zone_diag);
    CHECK(A.tracked[0].pos[0] == 3000 && A.tracked[0].pos[2] == -4000);
    CHECK(A.tracked[0].in_range == 1);
    CHECK(A.tracked[1].dist == 30265);   /* sqrt(30000^2 + 4000^2) */
    CHECK(A.tracked[1].in_range == (A.tracked[1].dist <= A.zone_diag));
    /* clip_state */
    CHECK(vra_clip_state(&A, 0x80170000u, 0x32d, 1, 0) == 2);      /* cuerpo del soldado cercano: 0 -> 2 */
    CHECK(vra_clip_state(&A, 0x80172000u, 0x6d, 1, 0) == 2);       /* su arma */
    CHECK(vra_clip_state(&A, 0x80170000u, 0x32d, 1, 1) == 1);      /* ya distinto de 0: intacto */
    CHECK(vra_clip_state(&A, 0x80170000u, 0x32d | 0x80, 1, 0) == 0);   /* invisible */
    CHECK(vra_clip_state(&A, 0x80170000u, 0x32d, 2, 0) == 0);      /* otro grupo de mapas */
    CHECK(vra_clip_state(&A, 0x80170000u, 0x32d, 0, 0) == 2);      /* grupo 0 = siempre */
    CHECK(vra_clip_state(&A, 0x80199000u, 0x32d, 1, 0) == 0);      /* objeto no seguido */
    CHECK(vra_clip_state(&A, 0x80178000u, 0x2d, 1, 0) == 0);       /* cámara de juego: no seguida */
    CHECK(vra_class_of_objs(&A, 0x80176000u) == VRA_CLASS_SENSOR);
    CHECK(vra_class_of_objs(&A, 0) == VRA_CLASS_OTHER);

    /* conmutadores separados */
    vra_set_mode(&A, VRA_CLASS_SENSOR, VRA_ORIGINAL);
    vra_frame(&A, ram, &st);
    CHECK(vra_clip_state(&A, 0x80176000u, 0x2d, 1, 0) == 0);       /* sensor en ORIGINAL: intacto */
    CHECK(vra_clip_state(&A, 0x80170000u, 0x32d, 1, 0) == 2);      /* soldado en VR */
    vra_set_mode(&A, VRA_CLASS_SENSOR, VRA_VR);
    vra_set_mode(&A, VRA_CLASS_SOLDIER, VRA_ORIGINAL);
    vra_frame(&A, ram, &st);
    CHECK(vra_clip_state(&A, 0x80170000u, 0x32d, 1, 0) == 0);
    CHECK(vra_clip_state(&A, 0x80176000u, 0x2d, 1, 0) == 2);
    CHECK(vra_get_mode(&A, VRA_CLASS_SOLDIER) == VRA_ORIGINAL && vra_get_mode(&A, VRA_CLASS_SENSOR) == VRA_VR);
    vra_set_mode(&A, VRA_CLASS_SOLDIER, VRA_VR);

    /* radios independientes */
    vra_set_radius(&A, VRA_CLASS_SOLDIER, 3500);
    vra_set_radius(&A, VRA_CLASS_SENSOR, 100000);
    vra_frame(&A, ram, &st);
    CHECK(A.tracked[0].dist == 5000 && A.tracked[0].in_range == 0);   /* 5000 > 3500 */
    int32_t pnear[3] = {2000, 1000, -2000}, pfar[3] = {9000, 1000, 0};
    CHECK(vra_point_active(&A, VRA_CLASS_SOLDIER, pnear) == 1);
    CHECK(vra_point_active(&A, VRA_CLASS_SOLDIER, pfar) == 0);
    CHECK(vra_point_active(&A, VRA_CLASS_SENSOR, pfar) == 1);
    CHECK(vra_point_active(&A, VRA_CLASS_OTHER, pnear) == 0);
    CHECK(vra_point_active(&A, (VraClass)7, pnear) == 0);
    vra_set_radius(&A, VRA_CLASS_SOLDIER, -5);   /* negativo -> automático */
    CHECK(vra_radius(&A, VRA_CLASS_SOLDIER) == A.zone_diag);

    /* altura máxima y LOS */
    vra_set_max_dy(&A, VRA_CLASS_SOLDIER, 500);
    int32_t phigh[3] = {1000, 2000, 0};
    CHECK(vra_point_active(&A, VRA_CLASS_SOLDIER, phigh) == 0);
    vra_set_max_dy(&A, VRA_CLASS_SOLDIER, 0);
    CHECK(vra_point_active(&A, VRA_CLASS_SOLDIER, phigh) == 1);

    /* observador forzado (cabeza) */
    int32_t head[3] = {9000, 1000, 0};
    vra_set_radius(&A, VRA_CLASS_SOLDIER, 2000);
    vra_set_viewer(&A, head);
    vra_frame(&A, ram, &st);
    CHECK(vra_point_active(&A, VRA_CLASS_SOLDIER, pfar) == 1);
    CHECK(vra_point_active(&A, VRA_CLASS_SOLDIER, pnear) == 0);
    vra_set_viewer(&A, NULL);
    vra_frame(&A, ram, &st);
    CHECK(vra_point_active(&A, VRA_CLASS_SOLDIER, pfar) == 0);

    /* estado de partida no válido: todo inactivo y vacío */
    st.valid = 0;
    CHECK(vra_frame(&A, ram, &st) == 0);
    CHECK(!A.active && vra_clip_state(&A, 0x80170000u, 0x32d, 1, 0) == 0 && vra_class_of_objs(&A, 0x80170000u) == VRA_CLASS_OTHER);
    st.valid = BRIDGE_OK_ALL_CORE; st.has_player = 0;
    CHECK(vra_frame(&A, ram, &st) == 0 && !A.active);
    st.has_player = 1;
    CHECK(vra_frame(&A, ram, NULL) == 0);
    CHECK(vra_frame(&A, NULL, &st) == 0);
    (void)NM_S; (void)NM_C;
}

static int los_calls;
static int los_block(void *u, const int32_t f[3], const int32_t t[3]) { (void)f; los_calls++; return t[0] != *(int32_t *)u; }
static void test_los_and_audit(void) {
    setup();
    vra_register_name("tst_sold", VRA_CLASS_SOLDIER, 3, 6);
    st.n_actors = 2;
    uint32_t a0 = 0x80160000u, a1 = 0x80162000u;
    actor(0, a0, 4, 0); strcpy(st.actors[0].name, "tst_sold");
    actor(1, a1, 4, 0); strcpy(st.actors[1].name, "tst_sold");
    mk_objs(0x80170000u, 0x80171000u, 16, 0x32d, 1, 0, 3000, 1000, -4000);
    w32(a0 + 156, 0x80170000u);
    mk_objs(0x80174000u, 0x80175000u, 16, 0x32d, 1, 0, 5000, 1000, -4000);
    w32(a1 + 156, 0x80174000u);
    int32_t blocked = 5000;
    vra_set_los(&A, los_block, &blocked);
    for (int i = 0; i < VRA_WARMUP; i++) vra_frame(&A, ram, &st);
    CHECK(los_calls > 0);
    CHECK(A.tracked[0].in_range == 1 && A.tracked[1].in_range == 0);
    VraAudit au[8];
    int n = vra_audit(&A, ram, au, 8);
    CHECK(n == 2);
    CHECK(au[0].why == VRA_WHY_CLIP_ZERO);        /* activo en rango pero clip_state==0: el gancho aún no actuó */
    CHECK(au[1].why & VRA_WHY_NO_LOS);
    w16(0x80170000u + 0x32, 2);                    /* el gancho ya escribió 2 */
    vra_audit(&A, ram, au, 8);
    CHECK(au[0].why == 0 && au[0].clip_state == 2);
    w32(0x80170000u + 0x28, 0x32d | 0x80);
    vra_audit(&A, ram, au, 8);
    CHECK(au[0].why & VRA_WHY_INVISIBLE);
    w32(0x80170000u + 0x28, 0x32d); w16(0x80170000u + 0x2c, 4);
    vra_audit(&A, ram, au, 8);
    CHECK(au[0].why & VRA_WHY_NOT_IN_GROUP);
    w16(0x80170000u + 0x2c, 1);
    st.pause_level = 2; st.game_status = 0x200;
    vra_frame(&A, ram, &st);
    vra_audit(&A, ram, au, 8);
    CHECK((au[0].why & VRA_WHY_PAUSED) && (au[0].why & VRA_WHY_ENEMY_OFF));
    vra_set_mode(&A, VRA_CLASS_SOLDIER, VRA_ORIGINAL);
    vra_audit(&A, ram, au, 8);
    CHECK(au[0].why & VRA_WHY_NOT_VR);
    CHECK(vra_audit(&A, ram, au, 1) == 1);
    CHECK(vra_audit(&A, ram, au, 0) == 0);
}

static void test_pointcheck(void) {
    setup();
    for (int i = 0; i < VRA_WARMUP; i++) vra_frame(&A, ram, &st);
    CHECK(A.active);
    uint32_t pts = 0x80180000u;
    int16_t v[4][4] = {{1000, 1000, 1000, 0}, {30000, 0, 30000, 0}, {-1000, 500, 0, 1}, {2000, 0, 0, 5}};
    for (int i = 0; i < 4; i++) for (int k = 0; k < 4; k++) w16(pts + 8u * (unsigned)i + 2u * (unsigned)k, v[i][k]);
    vra_set_radius(&A, VRA_CLASS_SOLDIER, 5000);
    int ch = vra_pointcheck_fixup(&A, ram, pts, 4);
    CHECK(ch == 2);                                  /* 0 y 5 pasan a 1 si están en rango; el 1 ya valía 1 */
    CHECK(r16(pts + 6) == 1);                        /* en rango */
    CHECK(r16(pts + 14) == 0);                       /* lejos: queda 0 (el original decide) */
    CHECK(r16(pts + 22) == 1);                       /* ya 1: intacto */
    CHECK(r16(pts + 30) == 1);                       /* 5 -> 1 (en rango) */
    CHECK(r16(pts + 0) == 1000 && r16(pts + 8) == 30000);   /* no toca coordenadas */
    vra_set_mode(&A, VRA_CLASS_SOLDIER, VRA_ORIGINAL);
    w16(pts + 6, 0);
    CHECK(vra_pointcheck_fixup(&A, ram, pts, 4) == 0 && r16(pts + 6) == 0);
    vra_set_mode(&A, VRA_CLASS_SOLDIER, VRA_VR);
    vra_set_mode(&A, VRA_CLASS_SENSOR, VRA_VR);
    CHECK(vra_pointcheck_fixup(&A, ram, pts, 0) == 0);
    CHECK(vra_pointcheck_fixup(&A, ram, pts, -3) == 0);
    CHECK(vra_pointcheck_fixup(&A, ram, pts, 100000) == 0);
    /* final de RAM: no se sale */
    CHECK(vra_pointcheck_fixup(&A, ram, 0x801FFFF0u, 8) >= 0);
    CHECK(vra_pointcheck_fixup(&A, ram, 0x00001000u, 2) == 0);   /* no KSEG0 */
    A.active = 0;
    w16(pts + 6, 0);
    CHECK(vra_pointcheck_fixup(&A, ram, pts, 1) == 0 && r16(pts + 6) == 0);
}


/* pausa/enemigos apagados/histéresis/KSEG1: el original manda */
static void test_respect_original(void) {
    setup();
    vra_register_name("tst_sold", VRA_CLASS_SOLDIER, 3, 6);
    vra_register_name("tst_sens", VRA_CLASS_SENSOR, 3, 6);
    st.n_actors = 2;
    uint32_t a0 = 0x80160000u, a1 = 0x80162000u;
    actor(0, a0, 4, 0); strcpy(st.actors[0].name, "tst_sold");
    actor(1, a1, 4, 0); strcpy(st.actors[1].name, "tst_sens");
    mk_objs(0x80170000u, 0x80171000u, 16, 0x32d, 1, 0, 3000, 1000, -4000);
    w32(a0 + 156, 0x80170000u);
    mk_objs(0x80174000u, 0x80175000u, 16, 0x32d, 1, 0, 3000, 1000, -3000);
    w32(a1 + 156, 0x80174000u);
    for (int i = 0; i < VRA_WARMUP; i++) vra_frame(&A, ram, &st);
    CHECK(vra_clip_state(&A, 0x80170000u, 0x32d, 1, 0) == 2 && vra_clip_state(&A, 0x80174000u, 0x32d, 1, 0) == 2);
    st.game_status = 0x200;                      /* enemigos apagados: solo afecta a soldados */
    vra_frame(&A, ram, &st);
    CHECK(vra_clip_state(&A, 0x80170000u, 0x32d, 1, 0) == 0 && vra_clip_state(&A, 0x80174000u, 0x32d, 1, 0) == 2);
    st.game_status = 0; st.pause_level = 2;      /* pausa: ninguno */
    vra_frame(&A, ram, &st);
    CHECK(vra_clip_state(&A, 0x80170000u, 0x32d, 1, 0) == 0 && vra_clip_state(&A, 0x80174000u, 0x32d, 1, 0) == 0);
    w16(0x80180000u + 6, 0); w16(0x80180000u, 100);
    CHECK(vra_pointcheck_fixup(&A, ram, 0x80180000u, 1) == 0 && r16(0x80180006u) == 0);
    st.pause_level = 0;
    vra_frame(&A, ram, &st);
    CHECK(vra_pointcheck_fixup(&A, ram, 0x80180000u, 1) == 1 && r16(0x80180006u) == 1);
    CHECK(vra_pointcheck_fixup(&A, ram, 0x80180001u, 1) == 0);          /* SVECTOR desalineado: no se toca */
    /* histéresis: entra a r, sale a r*1.0625; el soldado está a 5000 */
    vra_set_radius(&A, VRA_CLASS_SOLDIER, 1000);
    vra_frame(&A, ram, &st);
    vra_set_radius(&A, VRA_CLASS_SOLDIER, 4900);
    vra_frame(&A, ram, &st);
    CHECK(A.tracked[0].dist == 5000 && !A.tracked[0].in_range);
    vra_set_radius(&A, VRA_CLASS_SOLDIER, 5100);
    vra_frame(&A, ram, &st);
    CHECK(A.tracked[0].in_range);
    vra_set_radius(&A, VRA_CLASS_SOLDIER, 4800);  /* 4800*1.0625 = 5100 >= 5000: se mantiene */
    vra_frame(&A, ram, &st);
    CHECK(A.tracked[0].in_range);
    vra_set_radius(&A, VRA_CLASS_SOLDIER, 4600);  /* 4887 < 5000: sale */
    vra_frame(&A, ram, &st);
    CHECK(!A.tracked[0].in_range);
    /* punteros KSEG1 / KSEG2 no se aceptan en las estructuras del juego */
    vra_set_radius(&A, VRA_CLASS_SOLDIER, 0);
    w16(0x80180006u, 0);
    CHECK(vra_pointcheck_fixup(&A, ram, 0xA0180000u, 1) == 1);
    w16(0x80180006u, 0);
    CHECK(vra_pointcheck_fixup(&A, ram, 0xC0180000u, 1) == 0);
}

static unsigned seed = 12345;
static uint32_t rnd(void) { seed = seed * 1103515245u + 12345u; return (seed >> 3) ^ (seed << 7); }
/* RAM y estado aleatorios: no debe salirse de la RAM ni colgarse. */
static void test_fuzz(void) {
#define RND() rnd()
    vra_register_name("tst_sold", VRA_CLASS_SOLDIER, 3, 6);
    for (int it = 0; it < 300; it++) {
        for (size_t i = 0; i < sizeof ram; i += 4) { uint32_t v = RND(); memcpy(ram + i, &v, 4); }
        memset(&st, 0, sizeof st);
        vra_init(&A);
        st.valid = BRIDGE_OK_ALL_CORE; st.has_player = 1;
        st.n_actors = (int)(RND() % 40);
        for (int i = 0; i < st.n_actors; i++) {
            st.actors[i].addr = 0x80000000u | (RND() & 0x1FFFFC);
            st.actors[i].level = (uint8_t)(RND() % 9);
            if (RND() & 1) strcpy(st.actors[i].name, "tst_sold");
        }
        for (int f = 0; f < VRA_WARMUP + 1; f++) vra_frame(&A, ram, &st);
        VraAudit au[VRA_MAX_TRACKED];
        vra_audit(&A, ram, au, VRA_MAX_TRACKED);
        for (int i = 0; i < 20; i++) vra_clip_state(&A, RND(), RND(), RND(), (int)(RND() % 3));
        vra_pointcheck_fixup(&A, ram, RND(), (int)(RND() % 300));
        vra_zone_diag(ram, NULL);
    }
    CHECK(1);
}

int main(void) {
    test_hash_and_names();
    test_zone();
    test_frame_and_decisions();
    test_los_and_audit();
    test_pointcheck();
    test_respect_original();
    test_fuzz();
    printf("%s: %d comprobaciones, %d fallos\n", fails ? "FALLO" : "OK", checks, fails);
    return fails ? 1 : 0;
}
