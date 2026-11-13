/* Modelos REALES de la granada, el bote (aturdidora y señuelo) y el C4 del archivo de stages del usuario (marco del motor, sin rotar). Se omite sin datos del juego.
 * Fija lo que usa el código: el C4 (c4_bomb, 0xf83d) tiene su cara adhesiva en z = -79 y la antena hasta z = 170 (vrgame.c place_c4: C4_LIFT_M 0,079 y eje Z del modelo = normal de la superficie);
 * la granada de mano (grenade, 0x3b88) es alargada en X; el bote (can_gren, 0x7a64), que usan la aturdidora y el señuelo, es alargado en Z y distinto de la granada.
 *   ./test_throw_place_models [archivo_de_stages]   o STAGE_FILE; por defecto /tmp/gamedata/STAGE.DIR y gamedata/stages.bin */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "wload.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FALLO l.%d: %s ", __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(int argc, char **argv) {
    const char *cands[4] = { argc > 1 ? argv[1] : NULL, getenv("STAGE_FILE"), "/tmp/gamedata/STAGE.DIR", "gamedata/stages.bin" };
    FILE *f = NULL;
    for (int i = 0; i < 4 && !f; i++) if (cands[i]) f = fopen(cands[i], "rb");
    if (!f) { printf("test_throw_place_models: sin archivo de stages: omitido\n"); return 0; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = (uint8_t *)malloc((size_t)n);
    if (!d || fread(d, 1, (size_t)n, f) != (size_t)n) { printf("lectura fallida\n"); return 1; }
    fclose(f);
    WLoadOpts o; wload_default_opts(&o);
    o.frame = W_FRAME_ENGINE; o.scale = 1.0f; o.resident = "init";
    WLoaded c4, gr, can;
    CHECK(wload_model(d, (size_t)n, "init", 0xf83d, &o, &c4) == 0, "c4_bomb");
    CHECK(wload_model(d, (size_t)n, "init", 0x3b88, &o, &gr) == 0, "grenade");
    CHECK(wload_model(d, (size_t)n, "init", 0x7a64, &o, &can) == 0, "can_gren");
    if (fails) { printf("%d comprobaciones, %d fallos\n", checks, fails); return 1; }
    CHECK(fabsf(c4.bmin.z - -79.0f) < 1.5f && fabsf(c4.bmax.z - 170.0f) < 1.5f, "C4: z de %.0f a %.0f (cara adhesiva en -79, antena hasta 170)", c4.bmin.z, c4.bmax.z);
    CHECK(c4.bmax.x - c4.bmin.x < 110.0f && c4.bmax.y - c4.bmin.y < 110.0f && c4.bmax.z - c4.bmin.z > 240.0f, "C4: base ~100x100, eje largo = Z del modelo");
    CHECK(fabsf(c4.bmin.z) / 1000.0f > 0.07f && fabsf(c4.bmin.z) / 1000.0f < 0.085f, "elevación sobre la superficie de 7,9 cm = distancia de la cara adhesiva al origen");
    CHECK((gr.bmax.x - gr.bmin.x) > (gr.bmax.y - gr.bmin.y) && (gr.bmax.x - gr.bmin.x) > 100.0f && (gr.bmax.x - gr.bmin.x) < 130.0f, "granada: ~12 cm, larga en X (%.0f)", gr.bmax.x - gr.bmin.x);
    CHECK((can.bmax.z - can.bmin.z) > 240.0f && (can.bmax.x - can.bmin.x) < 110.0f, "bote: ~25 cm, largo en Z");
    CHECK((can.bmax.z - can.bmin.z) > 1.5f * (gr.bmax.z - gr.bmin.z), "el bote (aturdidora, señuelo) no es la granada");
    CHECK(gr.ntex_missing == 0 && can.ntex_missing == 0 && c4.ntex_missing == 0, "texturas completas (sin ausentes: granada %u, bote %u, C4 %u)", gr.ntex_missing, can.ntex_missing, c4.ntex_missing);
    printf("test_throw_place_models: %d comprobaciones, %d fallos\n", checks, fails);
    return fails ? 1 : 0;
}
