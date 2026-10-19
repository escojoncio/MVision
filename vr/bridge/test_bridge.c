/* Pruebas del puente RAM sobre volcados sintéticos (no necesitan datos del juego). make -C vr/bridge test[-san] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bridge.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FALLO %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static uint8_t *ram;   /* exactamente 2 MB en el montón: ASan detecta cualquier lectura fuera */

static uint32_t off(uint32_t a) { return a & 0x1FFFFFu; }
static void p8(uint32_t a, uint8_t v) { ram[off(a)] = v; }
static void p16(uint32_t a, int v) { uint16_t x = (uint16_t)v; memcpy(ram + off(a), &x, 2); }
static void p32(uint32_t a, uint32_t v) { memcpy(ram + off(a), &v, 4); }
static void pv3(uint32_t a, int x, int y, int z) { p16(a, x); p16(a + 2, y); p16(a + 4, z); }
static void pstr(uint32_t a, const char *s) { memcpy(ram + off(a), s, strlen(s) + 1); }
static void var(int i, int v) { p16(BA_VARS + 2u * (uint32_t)i, v); }

#define CTRL_BASE 0x80100000u
#define ACT_BASE  0x80110000u
#define STR_BASE  0x80120000u

static void put_control(int idx, uint32_t addr, int name, int x, int y, int z, int ry, int radar, int cone_dir) {
    p32(BA_CONTROL_LIST + 4u * (uint32_t)idx, addr);
    p16(addr + BC_NAME, name);
    pv3(addr + BC_MOV, x, y, z);
    pv3(addr + BC_ROT, 0, ry, 0);
    p16(addr + BC_HEIGHT, 500);
    p16(addr + BC_RADAR, radar);
    p16(addr + BC_CONE, cone_dir); p16(addr + BC_CONE + 2, 3000); p16(addr + BC_CONE + 4, 600);
    p32(addr + BC_MAP, 0x80100800u);      /* "mapa": primer s32 = bit de mapa */
}
static void put_target(int i, int cls, int side, int map, int x, int y, int z, int vital, int faint) {
    uint32_t t = BA_TARGET_ARRAY + (uint32_t)BT_SIZE * (uint32_t)i;
    p16(t + BT_CLASS, cls); p16(t + BT_SIDE, side); p16(t + BT_MAP, map);
    pv3(t + BT_CENTER, x, y, z); pv3(t + BT_BOX, 100, 300, 100);
    p16(t + BT_VITAL, vital); p16(t + BT_FAINT, faint); p32(t + BT_WEAPON, (uint32_t)-1);
}
/* Escribe el nodo de lista `lv` con n actores en ACT_BASE + lv*0x1000 + i*0x40 */
static void put_actors(int lv, int n, const char *name) {
    uint32_t base = BA_ACTOR_LISTS + 68u * (uint32_t)lv, endn = base + 32;
    uint32_t prev = base;
    for (int i = 0; i < n; i++) {
        uint32_t a = ACT_BASE + (uint32_t)lv * 0x1000u + (uint32_t)i * 0x40u;
        p32(prev + 4, a);
        p32(a + 0, prev); p32(a + 8, 0x800C0000u + (uint32_t)i); p32(a + 12, 0); p32(a + 20, STR_BASE + (uint32_t)lv * 0x100u);
        p32(a + 24, 7); p32(a + 28, 9);
        prev = a;
    }
    p32(prev + 4, endn); p32(endn + 0, prev);
    pstr(STR_BASE + (uint32_t)lv * 0x100u, name);
}

static void build_game(void) {
    memset(ram, 0, BRIDGE_RAM_SIZE);
    /* variables de partida */
    var(1, 1); var(4, 1); var(8, -5800); var(9, 700); var(10, -1300);
    var(BRIDGE_V_LIFE, 200); var(BRIDGE_V_LIFE_MAX, 512); var(BRIDGE_V_WEAPON, 1); var(BRIDGE_V_ITEM, 13);
    var(BRIDGE_V_STANCE, 1);
    for (int i = 0; i < BRIDGE_N_WEAPONS; i++) { var(BRIDGE_V_WEAPON_AMMO + i, -1); var(BRIDGE_V_WEAPON_MAX + i, 100 + i); }
    var(BRIDGE_V_WEAPON_AMMO + 0, 12);                 /* arma 0: 12 */
    var(BRIDGE_V_WEAPON_AMMO + 1, 0x8000 | 90);        /* arma 1 bloqueada, 90 */
    for (int i = 0; i < BRIDGE_N_ITEMS; i++) var(BRIDGE_V_ITEM_COUNT + i, -1);
    var(BRIDGE_V_ITEM_COUNT + 13, 3);                  /* consumible 13: 3 unidades */
    var(BRIDGE_V_ITEM_COUNT + 0, 1);
    var(BRIDGE_V_ITEM_MAX, 5);
    /* jugador */
    p32(BA_PLAYER_CTRL, CTRL_BASE);
    pv3(BA_PLAYER_POS, -5800, 700, -1300);
    p32(BA_PLAYER_STATUS, BRIDGE_PS_SQUAT | BRIDGE_PS_MOVE);
    p32(BA_PLAYER_ACTION, 35);
    p32(BA_PLAYER_MAP, 1);
    p16(BA_O2, 1024); p16(BA_MAGAZINE, 8); p16(BA_MAGAZINE_MAX, 15);
    put_control(0, CTRL_BASE, BRIDGE_PLAYER_HASH, -5800, 701, -1300, 1024, 1, 0);
    put_control(1, CTRL_BASE + 0x100, 0x1467, 2500, 1080, -8500, 2048, 0xD, 1024);
    put_control(2, CTRL_BASE + 0x200, 0x1466, -5750, 1080, -16250, 0, 0x100D, 0);   /* sospecha */
    put_control(3, CTRL_BASE + 0x300, 0xEEBA, 8000, 500, -3500, 0, 0, 0);          /* objeto, no enemigo */
    p32(BA_N_CONTROLS, 4);
    p32(0x80100800u, 1);
    /* blancos */
    put_target(0, 0x9F, 1, 1, -5800, 701, -1300, 200, 0);
    put_target(1, 0xDF, 2, 1, 2510, 1080, -8490, 192, 7);
    put_target(2, 0xDF, 2, 1, -5750, 1080, -16250, 150, 20);
    put_target(3, 0, 0, 0, 0, 0, 0, 0, 0);          /* hueco libre */
    p32(BA_N_TARGETS, 4);
    /* cámara */
    int16_t m[9] = {4096, 0, 0, 0, -1736, -3716, 0, 3725, -1731};
    uint32_t e = BA_VIEW_CHANNEL + BCH_CAM;
    for (int i = 0; i < 9; i++) p16(e + 2u * (uint32_t)i, m[i]);
    p32(e + 20, (uint32_t)-5800); p32(e + 24, 8188); p32(e + 28, 1807);
    p16(BA_VIEW_CHANNEL + BCH_SCREEN, 320);
    pv3(BA_CAMERA + BCAM_POS, -5800, 8188, 1807); pv3(BA_CAMERA + BCAM_TARGET, -5800, 1390, -1358);
    p16(BA_CAMERA + BCAM_ZOOM, 320); p16(BA_CAMERA + BCAM_FIRST, 0); p16(BA_CAMERA + BCAM_TYPE, 2);
    p32(BA_CAMERA + BCAM_TRACK, 7500);
    /* zona */
    pstr(BA_STAGE_CODE, "q00a"); p16(BA_AREA_ID, 0x469B);
    p16(BA_AREA_HISTORY, 0x469B);
    /* alerta, modo */
    p32(BA_ALERT_MODE, 2); p32(BA_ALERT_LEVEL, 1200);
    p32(BA_GAME_STATUS, 0); p32(BA_PAUSE_LEVEL, 0); p8(BA_MENU_WORK + 0x2A, 0);
    /* actores */
    put_actors(0, 2, "uno.c"); put_actors(4, 3, "dos.c");
    for (int lv = 0; lv < BRIDGE_N_ACTOR_LISTS; lv++) if (lv != 0 && lv != 4) put_actors(lv, 0, "x");
    /* un actor cuyo control está en actor+0x20 */
    p32(BA_CONTROL_LIST + 4, ACT_BASE + 0x4000u + 0x20u); /* se sustituye abajo en el test de asociación */
    put_control(1, CTRL_BASE + 0x100, 0x1467, 2500, 1080, -8500, 2048, 0xD, 1024);
}

static void test_basic(void) {
    static BridgeState s;
    build_game();
    uint32_t v = bridge_read(ram, &s);
    CHECK(v == s.valid);
    CHECK((v & BRIDGE_OK_ALL_CORE) == BRIDGE_OK_ALL_CORE);
    CHECK(s.player_pos.x == -5800 && s.player_pos.y == 700 && s.player_pos.z == -1300);
    CHECK(s.ctl_pos.y == 701 && s.ctl_rot.y == 1024 && s.ctl_height == 500 && s.ctl_addr == CTRL_BASE);
    CHECK(s.life == 200 && s.life_max == 512);
    CHECK(s.weapon == 1 && s.item == 13);
    CHECK(s.weapon_ammo[0] == 12 && s.weapon_ammo[1] == 90 && s.weapon_ammo[2] == -1);
    CHECK(s.weapon_disabled == 2 && s.item_disabled == 0);
    CHECK(s.weapon_max[3] == 103);
    CHECK(s.item_count[13] == 3 && s.item_count[0] == 1 && s.item_count[5] == -1 && s.item_max[0] == 5);
    CHECK(s.stance == 1 && s.crouching && !s.prone && s.moving && !s.first_person && !s.on_wall);
    CHECK(s.player_action == 35 && s.magazine == 8 && s.magazine_max == 15 && s.o2 == 1024);
    CHECK(s.eye_pos[0] == -5800 && s.eye_pos[1] == 8188 && s.eye_pos[2] == 1807 && s.eye_focal == 320);
    CHECK(s.eye_axes[1][2] == -3716);
    CHECK(s.cam_zoom == 320 && s.cam_track == 7500 && s.cam_type == 2 && s.cam_target.y == 1390);
    CHECK(s.n_controls == 4 && s.controls[0].is_player && !s.controls[1].is_player);
    CHECK(s.n_targets == 3);                       /* el hueco libre se omite */
    CHECK(s.n_enemies == 2);                       /* control visible en el radar y no jugador */
    CHECK(s.enemies[0].control == 1 && s.enemies[0].target == 1 && s.enemies[0].alert_state == 0 && s.enemies[0].has_cone);
    CHECK(s.enemies[0].vital == 192 && s.enemies[0].faint == 7 && s.enemies[0].rot.y == 2048);
    CHECK(s.enemies[1].control == 2 && s.enemies[1].target == 2 && s.enemies[1].alert_state == 1);
    CHECK(strcmp(s.stage_code, "q00a") == 0 && s.area_id == 0x469B && s.area_history[0] == 0x469B);
    CHECK(s.alert_mode == 2 && s.alert_level == 1200);
    CHECK(s.n_actors == 5);
    CHECK(strcmp(s.actors[0].name, "uno.c") == 0 && s.actors[2].level == 4 && strcmp(s.actors[2].name, "dos.c") == 0);
    CHECK(s.actors[0].act == 0x800C0000u && s.actors[0].runtime == 7);
    CHECK(s.in_control && !s.cinematic && !s.comms && !s.paused && !s.menu_open);
    CHECK(strcmp(bridge_mode_name(&s), "juego") == 0);
}

static void test_modes(void) {
    static BridgeState s;
    build_game();
    p32(BA_PAUSE_LEVEL, 1); p8(BA_MENU_WORK + 0x2A, 4);
    bridge_read(ram, &s);
    CHECK(s.comms && s.stopped && !s.in_control && !s.cam_ctl && strcmp(bridge_mode_name(&s), "comms") == 0);
    build_game();
    p32(BA_PAUSE_LEVEL, 2);
    bridge_read(ram, &s);
    CHECK(s.paused && !s.in_control && s.cam_ctl && strcmp(bridge_mode_name(&s), "pausa") == 0);
    build_game();
    p8(BA_MENU_WORK + 0x2A, 2);
    bridge_read(ram, &s);
    CHECK(s.menu_open && !s.comms && !s.in_control && s.cam_ctl);     /* ventana de armas: la cabeza sigue llevando la cámara */
    build_game();
    p8(BA_MENU_WORK + 0x2A, 1);
    bridge_read(ram, &s);
    CHECK(s.menu_open && !s.comms && !s.in_control && s.cam_ctl);     /* ventana de objetos */
    build_game();
    p8(BA_MENU_WORK + 0x2A, 4 | 1);
    bridge_read(ram, &s);
    CHECK(s.comms && !s.cam_ctl);                                     /* con radio no hay cámara de jugador */
    build_game();
    p8(BA_MENU_WORK + 0x2A, 2); p32(BA_GAME_STATUS, 0x104A0000u);
    bridge_read(ram, &s);
    CHECK(s.cinematic && !s.cam_ctl);                                 /* escena con menú: sin cámara de jugador */
    build_game();
    p8(BA_MENU_WORK + 0x2A, 2); p16(BA_VARS + 2u * BRIDGE_V_LIFE, 0);
    bridge_read(ram, &s);
    CHECK(!s.cam_ctl);                                                /* muerte con menú: tampoco */
    build_game();
    p8(BA_MENU_WORK + 0x2A, 2); p32(BA_PAUSE_LEVEL, 1);
    bridge_read(ram, &s);
    CHECK(s.stopped && !s.cam_ctl);                                   /* juego detenido: tampoco */
    build_game();
    bridge_read(ram, &s);
    CHECK(s.in_control && s.cam_ctl);
    build_game();
    p32(BA_GAME_STATUS, 0x104A0000u);   /* escena con mando retirado */
    bridge_read(ram, &s);
    CHECK(s.cinematic && !s.in_control && strcmp(bridge_mode_name(&s), "escena") == 0);
    build_game();
    p32(BA_GAME_STATUS, BRIDGE_GS_DEMO);
    bridge_read(ram, &s);
    CHECK(s.cinematic);
    build_game();
    p16(BA_VARS + 2u * BRIDGE_V_LIFE, 0);   /* vida a cero: muerte en curso, sin control */
    bridge_read(ram, &s);
    CHECK(s.life == 0 && s.has_player && !s.in_control);
    build_game();
    p32(BA_GAME_STATUS, BRIDGE_GS_VOX_STREAM);
    bridge_read(ram, &s);
    CHECK(s.on_screen_text && s.in_control);
    build_game();
    p32(BA_PLAYER_STATUS, BRIDGE_PS_WATCH | BRIDGE_PS_WATCH_PEEK | BRIDGE_PS_MENU_DISABLE);
    bridge_read(ram, &s);
    CHECK(s.first_person);
    build_game();
    p32(BA_PLAYER_STATUS, BRIDGE_PS_WALL | BRIDGE_PS_BOX); var(BRIDGE_V_STANCE, 0);
    bridge_read(ram, &s);
    CHECK(s.on_wall && s.in_box && !s.crouching && s.stance == 0);
    build_game();
    p32(BA_PLAYER_STATUS, BRIDGE_PS_GROUND); var(BRIDGE_V_STANCE, 2);
    bridge_read(ram, &s);
    CHECK(s.prone && s.stance == 2);
}

static void test_invalid(void) {
    static BridgeState s;
    CHECK(bridge_read(NULL, &s) == 0);
    CHECK(bridge_read(ram, NULL) == 0);
    memset(ram, 0, BRIDGE_RAM_SIZE);
    CHECK(bridge_read(ram, &s) == 0 && s.valid == 0 && s.n_actors == 0);   /* RAM vacía: no hay partida */
    memset(ram, 0xFF, BRIDGE_RAM_SIZE);
    CHECK(bridge_read(ram, &s) == 0);
    /* vida absurda */
    build_game(); var(BRIDGE_V_LIFE, 30000);
    bridge_read(ram, &s);
    CHECK(!(s.valid & BRIDGE_OK_LIFE) && s.life == 0 && (s.valid & BRIDGE_OK_VARS));
    /* arma equipada que no tiene */
    build_game(); var(BRIDGE_V_WEAPON, 4);
    bridge_read(ram, &s);
    CHECK(!(s.valid & BRIDGE_OK_EQUIP) && s.weapon == 0 && s.weapon_ammo[0] == 0);
    /* arma fuera de rango */
    build_game(); var(BRIDGE_V_WEAPON, 99);
    bridge_read(ram, &s);
    CHECK(!(s.valid & BRIDGE_OK_EQUIP));
    /* puntero de control fuera de RAM o con otro identificador */
    build_game(); p32(BA_PLAYER_CTRL, 0xFFFFFFF0u);
    bridge_read(ram, &s);
    CHECK(!(s.valid & BRIDGE_OK_PLAYER_CTL) && s.ctl_addr == 0 && !(s.valid & BRIDGE_OK_PLAYER_POS) && !s.has_player &&
          !s.in_control && s.player_pos.x == 0 && strcmp(bridge_mode_name(&s), "sin_jugador") == 0);
    /* puntero de control en KSEG1 de la zona del kernel: no vale */
    build_game(); p32(BA_PLAYER_CTRL, 0xA0010000u | (CTRL_BASE & 0xFFFF));
    bridge_read(ram, &s);
    CHECK(!(s.valid & BRIDGE_OK_PLAYER_CTL));
    build_game(); p32(BA_PLAYER_CTRL, 0xA0000000u | (CTRL_BASE & 0x1FFFFF));
    bridge_read(ram, &s);
    CHECK(!(s.valid & BRIDGE_OK_PLAYER_CTL));
    /* título/carga: variables de partida válidas con la posición inicial pero sin control del jugador */
    build_game(); p32(BA_PLAYER_CTRL, 0); pv3(BA_PLAYER_POS, -3952, 104, -534);
    bridge_read(ram, &s);
    CHECK(!s.has_player && !s.in_control && !(s.valid & BRIDGE_OK_PLAYER_POS) && s.player_pos.y == 0);
    /* posición del jugador que no coincide con su bloque de control */
    build_game(); pv3(BA_PLAYER_POS, -5800 + 1000, 700, -1300);
    bridge_read(ram, &s);
    CHECK(!(s.valid & BRIDGE_OK_PLAYER_POS) && !s.has_player && !s.in_control);
    /* jugador no controlable / fin de partida */
    build_game(); p32(BA_PLAYER_STATUS, BRIDGE_PS_NOT_PLAYABLE);
    bridge_read(ram, &s);
    CHECK(s.has_player && !s.in_control);
    build_game(); p16(CTRL_BASE + BC_NAME, 0x1234);
    bridge_read(ram, &s);
    CHECK(!(s.valid & BRIDGE_OK_PLAYER_CTL));
    build_game(); p32(BA_PLAYER_CTRL, 0x801FFFF0u);   /* el bloque no cabe */
    bridge_read(ram, &s);
    CHECK(!(s.valid & BRIDGE_OK_PLAYER_CTL));
    /* lista de controles con puntero basura */
    build_game(); p32(BA_CONTROL_LIST + 8, 0x12345678u);
    bridge_read(ram, &s);
    CHECK(!(s.valid & BRIDGE_OK_CONTROLS) && s.n_controls == 0 && s.n_enemies == 0 && !(s.valid & BRIDGE_OK_ENEMIES));
    build_game(); p32(BA_N_CONTROLS, 97);
    bridge_read(ram, &s);
    CHECK(!(s.valid & BRIDGE_OK_CONTROLS) && s.n_controls == 0 && s.n_enemies == 0);
    /* blancos: contador absurdo */
    build_game(); p32(BA_N_TARGETS, 65);
    bridge_read(ram, &s);
    CHECK(!(s.valid & BRIDGE_OK_TARGETS) && s.n_targets == 0);
    /* matriz de cámara degenerada */
    build_game(); p16(BA_VIEW_CHANNEL + BCH_CAM, 0);
    bridge_read(ram, &s);
    CHECK(!(s.valid & BRIDGE_OK_EYE) && (s.valid & BRIDGE_OK_CAMERA) && s.eye_focal == 0 && s.eye_pos[1] == 0);
    /* columnas no ortogonales / posición de ojo absurda */
    build_game(); p16(BA_VIEW_CHANNEL + BCH_CAM + 2, 3000);
    bridge_read(ram, &s);
    CHECK(!(s.valid & BRIDGE_OK_EYE));
    build_game(); p32(BA_VIEW_CHANNEL + BCH_CAM + 20, 0x7FFFFFFFu);
    bridge_read(ram, &s);
    CHECK(!(s.valid & BRIDGE_OK_EYE));
    build_game(); p16(BA_CAMERA + BCAM_FIRST, 7);
    bridge_read(ram, &s);
    CHECK(!(s.valid & BRIDGE_OK_CAMERA));
    /* alerta fuera de rango, zona vacía */
    build_game(); p32(BA_ALERT_MODE, 9);
    bridge_read(ram, &s);
    CHECK(!(s.valid & BRIDGE_OK_ALERT));
    build_game(); var(BRIDGE_V_STANCE, 9);
    CHECK(bridge_read(ram, &s) == 0);
    build_game(); memset(ram + off(BA_STAGE_CODE), 'a', 8);   /* sin NUL */
    bridge_read(ram, &s);
    CHECK(!(s.valid & BRIDGE_OK_AREA) && s.stage_code[0] == 0);
    build_game(); p8(BA_STAGE_CODE, 0);
    bridge_read(ram, &s);
    CHECK(!(s.valid & BRIDGE_OK_AREA));
    build_game(); p8(BA_STAGE_CODE + 1, 7);   /* carácter no imprimible */
    bridge_read(ram, &s);
    CHECK(!(s.valid & BRIDGE_OK_AREA) && s.stage_code[0] == 0);
    /* modo: nivel de pausa basura */
    build_game(); p32(BA_PAUSE_LEVEL, 0x7FFFFFFF);
    bridge_read(ram, &s);
    CHECK(!(s.valid & BRIDGE_OK_MODE) && !s.in_control);
}

static void test_actor_lists(void) {
    static BridgeState s;
    /* bucle: el nodo apunta a sí mismo */
    build_game();
    uint32_t a0 = ACT_BASE;
    p32(a0 + 4, a0);
    bridge_read(ram, &s);
    CHECK(!(s.valid & BRIDGE_OK_ACTORS));
    CHECK(s.n_actors <= BRIDGE_MAX_ACTORS);
    /* siguiente fuera de RAM */
    build_game(); p32(ACT_BASE + 4, 0x90000000u);
    bridge_read(ram, &s);
    CHECK(!(s.valid & BRIDGE_OK_ACTORS));
    /* nodo de inicio con prev != 0 */
    build_game(); p32(BA_ACTOR_LISTS, 0x80100000u);
    bridge_read(ram, &s);
    CHECK(!(s.valid & BRIDGE_OK_ACTORS));
    /* nombre no imprimible o sin terminar: se descarta el nombre, el actor se conserva */
    build_game(); p8(STR_BASE, 3);
    bridge_read(ram, &s);
    CHECK((s.valid & BRIDGE_OK_ACTORS) && s.n_actors == 5 && s.actors[0].name[0] == 0 && s.actors[0].name_ptr == 0);
    build_game(); memset(ram + off(STR_BASE), 'a', 40);
    bridge_read(ram, &s);
    CHECK(strlen(s.actors[0].name) == 19);
    /* más actores que el tope: lista larga, se trunca sin fallar */
    build_game();
    {
        uint32_t base = BA_ACTOR_LISTS + 68u * 5u, endn = base + 32, prev = base;
        for (int i = 0; i < 1000; i++) {
            uint32_t a = 0x80130000u + (uint32_t)i * 0x40u;
            p32(prev + 4, a); p32(a, prev); p32(a + 8, 0x800C0000u);
            prev = a;
        }
        p32(prev + 4, endn); p32(endn, prev);
        bridge_read(ram, &s);
        CHECK(s.n_actors == 0);   /* grupo no válido: a cero */
        CHECK(!(s.valid & BRIDGE_OK_ACTORS));      /* lista truncada = incompleta */
    }
}

static void test_association(void) {
    static BridgeState s;
    build_game();
    /* control 1 colgando de un actor: actor + 0x20 == control */
    uint32_t actor = CTRL_BASE + 0x100 - 0x20;
    uint32_t base = BA_ACTOR_LISTS + 68u * 3u, endn = base + 32;
    p32(base + 4, actor); p32(actor, base); p32(actor + 8, 0x800C0010u); p32(actor + 20, STR_BASE);
    p32(actor + 4, endn); p32(endn, actor);
    pstr(STR_BASE, "enemigo.c");
    bridge_read(ram, &s);
    CHECK(s.controls[1].actor >= 0 && strcmp(s.actors[s.controls[1].actor].name, "enemigo.c") == 0);
    CHECK(s.controls[0].actor == -1);
    /* sin blanco cercano: target = -1 */
    build_game(); put_target(1, 0xDF, 2, 1, 6000, 1080, 6000, 192, 7);
    bridge_read(ram, &s);
    CHECK(s.enemies[0].target == -1 && s.enemies[0].vital == 0);
    /* blanco de otro mapa: no se asocia */
    build_game(); put_target(1, 0xDF, 2, 4, 2510, 1080, -8490, 192, 7);
    bridge_read(ram, &s);
    CHECK(s.enemies[0].target == -1);
    /* caído */
    build_game(); put_target(1, 0xDF | 32, 2, 1, 2510, 1080, -8490, 0, 90);
    bridge_read(ram, &s);
    CHECK(s.enemies[0].down && s.enemies[0].faint == 90);
}

static void test_exclusive_assoc(void) {
    static BridgeState s;
    /* dos controles de enemigo y un solo blanco entre ambos: solo el más cercano se lo queda */
    build_game();
    put_control(1, CTRL_BASE + 0x100, 0x1467, 2500, 1080, -8500, 0, 0xD, 0);
    put_control(2, CTRL_BASE + 0x200, 0x1466, 2700, 1080, -8500, 0, 0xD, 0);
    put_target(1, 0xDF, 2, 1, 2680, 1080, -8500, 192, 7);
    put_target(2, 0, 0, 0, 0, 0, 0, 0, 0);
    bridge_read(ram, &s);
    CHECK(s.n_enemies == 2 && s.enemies[0].target == -1 && s.enemies[1].target >= 0 && s.enemies[1].vital == 192);
    /* ángulos */
    CHECK(bridge_deg_norm(-1024) == 270.0f && bridge_deg_norm(5120) == 90.0f && bridge_deg_norm(0) == 0.0f);
}

static void test_peek_and_table(void) {
    uint32_t v32 = 0; int16_t v16 = 0;
    build_game();
    CHECK(bridge_peek32(ram, BA_ALERT_MODE, &v32) && v32 == 2);
    CHECK(bridge_peek16(ram, BA_AREA_ID, &v16) && v16 == 0x469B);
    CHECK(bridge_peek32(ram, 0x80200000u, &v32) == 0);
    CHECK(bridge_peek32(ram, 0x801FFFFDu, &v32) == 0);      /* cruza el final */
    CHECK(bridge_peek32(ram, 0x801FFFFCu, &v32) == 1);
    CHECK(bridge_peek16(ram, 0x801FFFFFu, &v16) == 0);
    CHECK(bridge_peek32(ram, 0x00001000u, &v32) == 1);      /* KUSEG */
    CHECK(bridge_peek32(ram, 0xA0001000u, &v32) == 1);      /* KSEG1 */
    CHECK(bridge_peek32(ram, 0x40000000u, &v32) == 0);
    CHECK(bridge_peek32(NULL, BA_ALERT_MODE, &v32) == 0);
    CHECK(bridge_addr_ok(0x80000000u, BRIDGE_RAM_SIZE) && !bridge_addr_ok(0x80000000u, BRIDGE_RAM_SIZE + 1));
    /* tabla: direcciones dentro de RAM, ids únicos, textos presentes */
    CHECK(bridge_addr_count > 10);
    for (int i = 0; i < bridge_addr_count; i++) {
        const BridgeAddr *a = &bridge_addr_table[i];
        CHECK(a->id && a->tipo && a->metodo && a->metodo[0]);
        CHECK(a->conf >= 1 && a->conf <= 3);
        CHECK(a->addr >= 0x80010000u && bridge_addr_ok(a->addr, a->size));
        for (int j = i + 1; j < bridge_addr_count; j++) CHECK(strcmp(a->id, bridge_addr_table[j].id) != 0);
    }
    CHECK(bridge_deg(1024) == 90.0f && bridge_deg(-2048) == -180.0f);
}

/* Fuzz: bytes aleatorios en las zonas que lee el puente (y alrededor); no debe fallar ni salirse de ram. */
static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)(rng >> 16); }

static void test_fuzz(void) {
    static BridgeState s;
    for (int it = 0; it < 3000; it++) {
        build_game();
        int n = 1 + (int)(rnd() % 40);
        for (int k = 0; k < n; k++) {
            /* sesgo hacia las zonas del puente */
            uint32_t base;
            switch (rnd() % 8) {
            case 0: base = BA_VARS; break;
            case 1: base = BA_ACTOR_LISTS; break;
            case 2: base = BA_CONTROL_LIST; break;
            case 3: base = BA_TARGET_ARRAY; break;
            case 4: base = BA_CAMERA; break;
            case 5: base = 0x80100000u; break;
            case 6: base = 0x800ACA00u; break;
            default: base = 0x80000000u + (rnd() & 0x1FFFFCu); break;
            }
            uint32_t a = base + (rnd() % 0x400u);
            if ((rnd() & 3) == 0) p32(a, rnd()); else p8(a, (uint8_t)rnd());
        }
        uint32_t v = bridge_read(ram, &s);
        /* invariantes siempre ciertos */
        CHECK(v == s.valid);
        CHECK(s.n_actors >= 0 && s.n_actors <= BRIDGE_MAX_ACTORS);
        CHECK(s.n_controls >= 0 && s.n_controls <= BRIDGE_MAX_CONTROLS);
        CHECK(s.n_targets >= 0 && s.n_targets <= BRIDGE_MAX_TARGETS);
        CHECK(s.n_enemies >= 0 && s.n_enemies <= BRIDGE_MAX_ENEMIES);
        for (int i = 0; i < s.n_enemies; i++) CHECK(s.enemies[i].control >= 0 && s.enemies[i].control < s.n_controls);
        for (int i = 0; i < s.n_controls; i++) CHECK(s.controls[i].actor >= -1 && s.controls[i].actor < s.n_actors);
        CHECK(strlen(s.stage_code) <= 8);
        for (int i = 0; i < s.n_actors; i++) CHECK(strlen(s.actors[i].name) <= 19);
        if (!(v & BRIDGE_OK_VARS)) CHECK(v == 0);
    }
    /* RAM totalmente aleatoria */
    for (int it = 0; it < 50; it++) {
        for (uint32_t i = 0; i < BRIDGE_RAM_SIZE; i += 4) { uint32_t r = rnd(); memcpy(ram + i, &r, 4); }
        bridge_read(ram, &s);
        CHECK(s.n_actors <= BRIDGE_MAX_ACTORS);
    }
}

/* Opcional: BRIDGE_RAM_DUMP=fichero de 2 MB real (de bridge_dump --ram) -> el puente debe dar todos los grupos válidos. */
static void test_real_dump(void) {
    const char *p = getenv("BRIDGE_RAM_DUMP");
    if (!p) return;
    FILE *f = fopen(p, "rb");
    if (!f) { printf("FALLO: no se pudo abrir %s\n", p); fails++; return; }
    size_t n = fread(ram, 1, BRIDGE_RAM_SIZE, f);
    fclose(f);
    CHECK(n == BRIDGE_RAM_SIZE);
    static BridgeState s;
    bridge_read(ram, &s);
    CHECK((s.valid & BRIDGE_OK_ALL_CORE) == BRIDGE_OK_ALL_CORE);
    CHECK(s.n_actors > 5 && s.n_controls >= 1 && s.controls[0].is_player && s.ctl_addr != 0);
    CHECK(s.player_pos.x == s.ctl_pos.x || (s.player_pos.x - s.ctl_pos.x) * (s.player_pos.x - s.ctl_pos.x) < 100 * 100);
    printf("volcado real %s: zona '%s' modo %s pos (%d,%d,%d) vida %d/%d %d actores %d controles %d enemigos\n", p,
           s.stage_code, bridge_mode_name(&s), s.player_pos.x, s.player_pos.y, s.player_pos.z, s.life, s.life_max,
           s.n_actors, s.n_controls, s.n_enemies);
}

int main(void) {
    ram = malloc(BRIDGE_RAM_SIZE);
    if (!ram) return 2;
    test_basic();
    test_modes();
    test_invalid();
    test_actor_lists();
    test_association();
    test_exclusive_assoc();
    test_peek_and_table();
    test_fuzz();
    test_real_dump();
    free(ram);
    printf("%d comprobaciones, %d fallos\n", checks, fails);
    return fails ? 1 : 0;
}
