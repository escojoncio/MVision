/* Pruebas de vr_nopunch.h: tablas ACTATTACK con el golpe retirado solo en armas, con comprobación del original. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vr_nopunch.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FALLO %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint8_t *mkram(void) {
    uint8_t *r = calloc(1, 0x200000);
    for (unsigned k = 0; k < VRNP_N; k++) memcpy(r + ((VRNP_TABLE + k * VRNP_STRIDE) & 0x1FFFFFu), vrnp_tab[k], 9);
    return r;
}
static uint8_t punch(const uint8_t *r, unsigned k) { return r[((VRNP_TABLE + k * VRNP_STRIDE) & 0x1FFFFFu) + VRNP_PUNCH]; }

int main(void) {
    uint8_t *r = mkram();
    CHECK(vrnopunch_frame(r) == 6, "seis armas con golpe en el original");
    for (unsigned k = 1; k < VRNP_N; k++) CHECK(punch(r, k) == 0x88, "arma %u sin golpe (%u)", k, punch(r, k));
    CHECK(punch(r, 0) == 35, "manos vacias intactas (%u)", punch(r, 0));
    CHECK(vrnopunch_frame(r) == 0, "idempotente");
    CHECK(r[((VRNP_TABLE + 5 * VRNP_STRIDE) & 0x1FFFFFu)] == 43 && r[((VRNP_TABLE + 5 * VRNP_STRIDE) & 0x1FFFFFu) + 8] == 56, "el resto de la tabla del C4 no cambia");
    free(r);

    r = mkram(); r[((VRNP_TABLE + 1 * VRNP_STRIDE) & 0x1FFFFFu)] ^= 1;     /* otra edicion / datos distintos: esa tabla no se toca */
    CHECK(vrnopunch_frame(r) == 5 && punch(r, 1) == 35, "tabla distinta: se respeta");
    free(r);
    r = mkram(); r[((VRNP_TABLE + 2 * VRNP_STRIDE) & 0x1FFFFFu) + VRNP_PUNCH] = 77;
    CHECK(vrnopunch_frame(r) == 5 && punch(r, 2) == 77, "punch inesperado: no se toca");
    free(r);
    memset((r = calloc(1, 0x200000)), 0, 1);
    CHECK(vrnopunch_frame(r) == 0, "RAM sin tablas: nada");
    free(r);
    CHECK(vrnopunch_frame(NULL) == 0, "ram nula");
    printf("test_vr_nopunch: %d comprobaciones, %d fallos\n", checks, fails);
    return fails != 0;
}
