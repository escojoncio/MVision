/* Sin puñetazos con el arma equipada (edición europea de MGS1.EXE).
 *
 * Decompilación (chara/snake/sna_init.c sna_8005009C, ~1157): con ○ pulsado el jugador lanza el golpe del motor salvo que la tabla de ataque del arma equipada tenga
 * ACTATTACK.punch == 0x88 («sin golpe»; el original lo usa en Stinger y PSG1). Con SOCOM, FAMAS, Nikita, C4, Claymore y granadas (tablas ACTATTACK de sna_init.c, 12 bytes cada una
 * desde NoneAttack) ○ SÍ golpea en el original. En VR ○ es el botón B y el agarre derecho que detona el C4 (vrgame.c c4_remote) es un ○ sintético: con el arma equipada lanzaban un
 * puñetazo (o el combo). Se pone punch = 0x88 en esas seis tablas, el mecanismo del propio juego: ○ queda libre para lo demás (la detonación del C4 de bakudan.c lee el mismo ○) y
 * vr_melee.c (vrm_action_input) ya rechaza el golpe con 0x88. Manos vacías (NoneAttack) y prismáticos (objeto, no arma) no se tocan.
 * El agarre / estrangulamiento (Cuadrado sin arma: pWeaponFn = sna_anim_chokethrow_begin1 solo con WP_None, sna_init.c ~573 y ~7821) ya es exclusivo de las manos vacías en el original.
 * Seguridad: cada tabla solo se toca si el resto de sus bytes es el del original (otra edición u otro dato no se escribe). Idempotente: se llama cada fotograma. */
#ifndef VR_NOPUNCH_H
#define VR_NOPUNCH_H
#include <stdint.h>
#include <string.h>

#define VRNP_TABLE   0x8009FEB8u        /* NoneAttack en la edición europea; las demás siguen cada 12 bytes en el orden de armas del juego */
#define VRNP_STRIDE  12u
#define VRNP_PUNCH   2u                 /* byte ACTATTACK.punch dentro de la tabla */
#define VRNP_NONE    0x88u
#define VRNP_N       9

/* ACTATTACK {aim, reload, punch, setup, shoot, f5, f6, f7, f8}: 0 manos, 1 SOCOM, 2 FAMAS, 3 Stinger, 4 Nikita, 5 C4, 6 Claymore, 7 granada, 8 PSG1. punch 0 = «la entrada no cambia» (manos vacías: se deja). */
static const uint8_t vrnp_tab[VRNP_N][9] = {
    {136, 136, 35, 136, 36, 37, 38, 39, 0}, {42, 51, 35, 45, 47, 0, 0, 0, 0}, {5, 52, 34, 46, 48, 0, 0, 0, 0},
    {74, 136, 136, 136, 105, 0, 0, 0, 0},   {74, 136, 34, 46, 81, 0, 0, 0, 0}, {43, 136, 35, 136, 53, 55, 49, 54, 56},
    {57, 136, 35, 136, 58, 59, 0, 0, 0},    {60, 136, 35, 136, 50, 0, 0, 0, 0}, {44, 136, 136, 136, 136, 0, 0, 0, 0},
};

/* Devuelve cuántos bytes ha escrito (0 en régimen). ram = RAM del PS1 (2 MiB). */
__attribute__((unused)) static int vrnopunch_frame(uint8_t *ram) {
    int w = 0;
    if (!ram) return 0;
    for (unsigned k = 1; k < VRNP_N; k++) {                       /* 0 = manos vacías: nunca */
        uint8_t *e = ram + ((VRNP_TABLE + k * VRNP_STRIDE) & 0x1FFFFFu);
        int same = 1;
        for (unsigned i = 0; i < 9 && same; i++) if (i != VRNP_PUNCH && e[i] != vrnp_tab[k][i]) same = 0;
        if (!same || e[VRNP_PUNCH] == VRNP_NONE) continue;
        if (e[VRNP_PUNCH] != vrnp_tab[k][VRNP_PUNCH]) continue;    // valor inesperado: no es la tabla del original
        e[VRNP_PUNCH] = VRNP_NONE; w++;
    }
    return w;
}
#endif
