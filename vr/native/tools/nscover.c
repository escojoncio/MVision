/* nscover: cobertura de la lista de la GPU de un volcado por la escena nativa extraída de su RAM (mismo código que el registro de la app: ns_cover.c).
 * Uso: nscover DIR_VOLCADO [DIR_VOLCADO_DE_LA_LISTA]   (el segundo, opcional, aporta list.bin si el primero no la trae, p. ej. la pareja _stage/_frame).
 * Imprime el resumen y los peores grupos. NO ejecuta el juego. */
#include "../ns_cover.h"
#include "../ns_dump.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "uso: nscover DIR [DIR_LISTA]\n"); return 2; }
    NsDump *d = ns_dump_open(argv[1]);
    if (!d) { fprintf(stderr, "no se pudo abrir %s\n", argv[1]); return 1; }
    NsDump *dl = argc > 2 ? ns_dump_open(argv[2]) : d;
    if (!dl || !dl->has_list) { fprintf(stderr, "sin list.bin\n"); return 1; }
    NsCover *c = ns_cover_create();
    NsCoverResult r;
    NsCoverCfg cfg;
    ns_cover_cfg_default(&cfg);
    float lm[9], le[3];   /* cámara con la que se emitió la lista, si el volcado la trae */
    if (!getenv("NSCOVER_SIN_CAM_LISTA") && !getenv("NSCOVER_RAMCAM") && ns_dump_meta_floats(dl, "rec_M", lm, 9) == 9 && ns_dump_meta_floats(dl, "rec_eye", le, 3) == 3) { cfg.list_M = lm; cfg.list_eye = le; }
    if (getenv("NSCOVER_MAXDET")) cfg.max_detail = (uint32_t)strtoul(getenv("NSCOVER_MAXDET"), NULL, 10);   /* triángulos sin pareja clasificados a fondo (defecto 512) */
    if (getenv("NSCOVER_YS")) cfg.ys = (float)atof(getenv("NSCOVER_YS"));
    const char *dp = getenv("NSCOVER_DETALLE");   /* fichero de detalle: una línea por triángulo sin pareja */
    if (dp) cfg.dbg = fopen(dp, "w");
    int rc = ns_cover_run(c, d->ram, d->scratch, dl->list.ops, dl->list.nops, &cfg, &r);
    if (cfg.dbg) fclose((FILE *)cfg.dbg);
    if (rc) { fprintf(stderr, "fallo %d\n", rc); return 1; }
    char buf[8192];
    ns_cover_format(&r, (uint32_t)ns_dump_meta_num(dl, "game_frame", 0), buf, sizeof buf);
    fputs(buf, stdout);
    ns_cover_destroy(c);
    return 0;
}
