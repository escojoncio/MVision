/* Pruebas de ns_wphide (ocultar el arma propia del juego en la escena nativa) con RAM y escena SINTÉTICAS. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ns_wphide.h"

static int checks, fails;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FALLO %s:%d: %s ", __FILE__, __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define CTRL   0x80100000u                 /* bloque de control del jugador (BA_PLAYER_CTRL apunta aquí); bloque del jugador = CTRL - 0x20 */
#define WACT   0x80110000u                 /* actor del arma */
#define WOBJS  0x80120000u                 /* conjunto de piezas del arma */
#define POBJS  0x80130000u                 /* conjunto de piezas del jugador (otra instancia) */
static uint8_t ram[0x200000];
static void w32(uint32_t a, uint32_t v) { memcpy(ram + (a & 0x1FFFFFu), &v, 4); }
static void w16(uint32_t a, int16_t v) { memcpy(ram + (a & 0x1FFFFFu), &v, 2); }

static void build_ram(int nm) {
    memset(ram, 0, sizeof ram);
    w32(NS_WPHIDE_PLAYER_CTRL, CTRL);
    w32(CTRL - NS_WPHIDE_FROM_CTRL + NS_WPHIDE_WEAPON_ACTOR, WACT);
    w32(WACT + NS_WPHIDE_WA_OBJS, WOBJS);
    w16(WOBJS + 0x2E, (int16_t)nm);
    w16(POBJS + 0x2E, 16);
}

/* escena: instancia 0 = jugador (3 piezas), 1 = arma (2 piezas), 2 = escenario (1 pieza) */
static NsInstance inst[3];
static NsPiece piece[6];
static NsScene sc;
static void build_scene(uint32_t weapon_key) {
    memset(inst, 0, sizeof inst); memset(piece, 0, sizeof piece); memset(&sc, 0, sizeof sc);
    inst[0].key = 0x80000000u | (POBJS & 0x1FFFFFFFu); inst[0].first_piece = 0; inst[0].n_pieces = 3;
    inst[1].key = weapon_key;                          inst[1].first_piece = 3; inst[1].n_pieces = 2;
    inst[2].key = 0x80000000u | 0x00150000u;           inst[2].first_piece = 5; inst[2].n_pieces = 1;
    sc.inst = inst; sc.n_inst = 3; sc.piece = piece; sc.n_piece = 6;
}
static int hidden(int i) { return (piece[i].flags & NS_PF_HIDDEN) != 0; }
static int any_hidden(void) { for (int i = 0; i < 6; i++) if (hidden(i)) return 1; return 0; }
static int snapshot_same(const NsPiece *s) { return memcmp(s, piece, sizeof piece) == 0; }

int main(void) {
    const uint32_t wkey = 0x80000000u | (WOBJS & 0x1FFFFFFFu);

    build_ram(2); build_scene(wkey);
    CHECK(ns_wphide_apply(&sc, ram, 1) == 2, "devuelve las 2 piezas del arma");
    CHECK(!hidden(0) && !hidden(1) && !hidden(2) && hidden(3) && hidden(4) && !hidden(5), "solo la instancia del arma queda oculta");
    CHECK(ns_wphide_apply(&sc, ram, 1) == 2 && hidden(3) && hidden(4) && !hidden(0), "idempotente");

    build_ram(2); build_scene(wkey);
    piece[3].flags = 0x10;
    ns_wphide_apply(&sc, ram, 1);
    CHECK(piece[3].flags == (0x10 | NS_PF_HIDDEN), "conserva el resto de banderas");

    build_ram(2); build_scene(wkey);
    CHECK(ns_wphide_apply(&sc, ram, 0) == 0 && !any_hidden(), "on = 0: nada");

    build_ram(2); build_scene(0x80000000u | 0x00140000u);
    CHECK(ns_wphide_apply(&sc, ram, 1) == 0 && !any_hidden(), "arma que no está en la escena: nada");

    build_ram(2); build_scene(wkey); sc.n_piece = 4;   /* la instancia del arma se sale de la lista de piezas */
    CHECK(ns_wphide_apply(&sc, ram, 1) == 0 && !hidden(3), "instancia fuera de rango: no se toca");

    build_ram(2); build_scene(wkey); sc.n_piece = 0; sc.piece = NULL;
    CHECK(ns_wphide_apply(&sc, ram, 1) == 0, "escena sin piezas");
    CHECK(ns_wphide_apply(NULL, ram, 1) == 0 && ns_wphide_apply(&sc, NULL, 1) == 0, "argumentos nulos");

    /* punteros inválidos: ninguna pieza se escribe (se compara la escena entera con la copia previa) */
    NsPiece snap[6];
    build_ram(2); build_scene(wkey); memcpy(snap, piece, sizeof snap); w32(NS_WPHIDE_PLAYER_CTRL, 0);
    CHECK(ns_wphide_apply(&sc, ram, 1) == 0 && snapshot_same(snap), "sin jugador");
    build_ram(2); build_scene(wkey); w32(NS_WPHIDE_PLAYER_CTRL, 0x801FFFF0u);
    CHECK(ns_wphide_apply(&sc, ram, 1) == 0 && !any_hidden(), "control al final de la RAM");
    build_ram(2); build_scene(wkey); w32(NS_WPHIDE_PLAYER_CTRL, 0xA0100000u);
    CHECK(ns_wphide_apply(&sc, ram, 1) == 0 && !any_hidden(), "control en KSEG1");
    build_ram(2); build_scene(wkey); w32(CTRL - NS_WPHIDE_FROM_CTRL + NS_WPHIDE_WEAPON_ACTOR, 0);
    CHECK(ns_wphide_apply(&sc, ram, 1) == 0 && !any_hidden(), "sin arma equipada");
    build_ram(2); build_scene(wkey); w32(CTRL - NS_WPHIDE_FROM_CTRL + NS_WPHIDE_WEAPON_ACTOR, 0x801FFFF0u);
    CHECK(ns_wphide_apply(&sc, ram, 1) == 0 && !any_hidden(), "actor al final de la RAM");
    build_ram(2); build_scene(wkey); w32(CTRL - NS_WPHIDE_FROM_CTRL + NS_WPHIDE_WEAPON_ACTOR, 0x80200000u + 0x110000u);
    CHECK(ns_wphide_apply(&sc, ram, 1) == 0 && !any_hidden(), "actor en el espejo por encima de 2 MB");
    build_ram(2); build_scene(wkey); w32(WACT + NS_WPHIDE_WA_OBJS, 0);
    CHECK(ns_wphide_apply(&sc, ram, 1) == 0 && !any_hidden(), "actor sin conjunto de piezas");
    build_ram(2); build_scene(wkey); w32(WACT + NS_WPHIDE_WA_OBJS, 0x801FFFF0u);
    CHECK(ns_wphide_apply(&sc, ram, 1) == 0 && !any_hidden(), "conjunto de piezas al final de la RAM");
    build_ram(0); build_scene(wkey);
    CHECK(ns_wphide_apply(&sc, ram, 1) == 0 && !any_hidden(), "0 modelos");
    build_ram(200); build_scene(wkey);
    CHECK(ns_wphide_apply(&sc, ram, 1) == 0 && !any_hidden(), "nº de modelos absurdo");
    build_ram(-3); build_scene(wkey);
    CHECK(ns_wphide_apply(&sc, ram, 1) == 0 && !any_hidden(), "nº de modelos negativo");

    /* granada lanzada: su actor (objeto embebido en actor + 0x20, otra instancia) se sigue dibujando mientras el arma propia se oculta */
    build_ram(2); build_scene(wkey);
    inst[2].key = 0x80000000u | ((0x80140000u + 0x20u) & 0x1FFFFFFFu);
    CHECK(ns_wphide_apply(&sc, ram, 1) == 2 && hidden(3) && hidden(4) && !hidden(5) && !hidden(0), "la granada lanzada no se oculta con el arma");
    /* puntero del arma que quedó apuntando al actor de la granada (memoria reciclada): +0x20 es la posición del proyectil, no un puntero a piezas -> no oculta nada */
    build_ram(2); build_scene(0x80000000u | ((0x80140000u + 0x20u) & 0x1FFFFFFFu));
    w32(CTRL - NS_WPHIDE_FROM_CTRL + NS_WPHIDE_WEAPON_ACTOR, 0x80140000u);
    w16(0x80140000u + 0x20u, 100); w16(0x80140000u + 0x22u, 200); w16(0x80140000u + 0x24u, 300);
    CHECK(ns_wphide_apply(&sc, ram, 1) == 0 && !any_hidden(), "actor de arma reciclado como granada: no oculta el proyectil");

    printf("%d comprobaciones, %d fallos\n", checks, fails);
    return fails ? 1 : 0;
}
