/* Herramienta de escritorio: genera la escena sintética, la publica por el puente y escribe la referencia de CPU (cubemap en cruz + panorama). */
#include "native_synth.h"
#include "ns_bridge.h"
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    int n = argc > 2 ? atoi(argv[2]) : 256;
    char p[1024];
    snprintf(p, sizeof p, "%s/scene.nsy", dir);
    if (ns_synth_write(p)) { fprintf(stderr, "no se pudo escribir %s\n", p); return 1; }
    int r = ns_synth_load(p);
    if (r) { fprintf(stderr, "ns_synth_load: %d\n", r); return 1; }
    NsRBuf faces[6]; NsRStats st;
    long nt = ns_synth_ref_cube(n, 0.05f, faces, &st);
    if (nt < 0) { fprintf(stderr, "ref: %ld\n", nt); return 1; }
    printf("tris=%ld dibujados=%zu px_opacos=%zu px_semi=%zu huecos=%.5f ms=%.1f\n", nt, st.tris_drawn, st.px_opaque, st.px_semi, st.hole_frac, st.ms);
    uint8_t *rgb = malloc((size_t)4 * n * 3 * n * 3);
    ns_cube_cross_rgb8(faces, 0xFF00FF, 1, rgb);
    snprintf(p, sizeof p, "%s/ref_cross.png", dir);
    ns_write_png(p, 4 * n, 3 * n, rgb);
    return 0;
}
