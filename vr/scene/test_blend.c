/* Prueba de vr_blend.h (la misma función que usa vr_fs_scene en Metal): los 4 modos de mezcla semitransparente del original en espacio gamma. make -C vr/scene test */
#include <math.h>
#include <stdio.h>
#include "vr_blend.h"

static int fails;
#define NEAR(a, b) (fabsf((a) - (b)) < 1e-6f)
#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "FALLO %d: ", __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); fails++; } } while (0)

int main(void) {
    CHECK(NEAR(vr_blend1(1, 0.4f, 0.4f), 0.8f), "aditivo .4+.4 = .8 (en gamma; en lineal saldria mas oscuro)");
    CHECK(NEAR(vr_blend1(1, 0.7f, 0.6f), 1.0f), "aditivo satura a 1");
    CHECK(NEAR(vr_blend1(0, 0.2f, 0.6f), 0.4f), "modo 0: media");
    CHECK(NEAR(vr_blend1(0, 1.0f, 1.0f), 1.0f) && NEAR(vr_blend1(0, 0.0f, 0.0f), 0.0f), "modo 0: extremos");
    CHECK(NEAR(vr_blend1(2, 0.5f, 0.2f), 0.3f), "modo 2: B-F");
    CHECK(NEAR(vr_blend1(2, 0.2f, 0.5f), 0.0f), "modo 2: resta con suelo en 0");
    CHECK(NEAR(vr_blend1(3, 0.5f, 0.4f), 0.6f), "modo 3: B+0,25F");
    CHECK(NEAR(vr_blend1(3, 0.9f, 0.8f), 1.0f), "modo 3: satura a 1");
    fprintf(stderr, fails ? "test_blend: %d FALLOS\n" : "test_blend: OK\n", fails);
    return fails ? 1 : 0;
}
