/* cutprev: renderiza un modelo ORIGINAL ya cortado por wcut (con su textura) a PNG, con canales de pieza (cargador, corredera...)
 * y corte en seccion opcional. Lee los datos locales del usuario (archivo de stages); nada de ello viaja con el repo.
 *
 *   cutprev <archivo_stages> <stage> <id hex> -o fich.png [--view side|left|top|under|front|rear|three|three2]
 *           [--ch NOMBRE=valor ...]   canal tras el arranque: SLIDE BOLT TRIGGER HAMMER MAG LID SAFETY SCREEN ROUND CUSTOM0 CUSTOM1
 *                                      (m; MAG = metros de extraccion a lo largo del eje de la pieza)
 *           [--center x,y,z] (mm, marco del modelo)  [--radius R] (m)  [--size WxH]  [--ss N]  [--gain G] (luz de la textura)
 *           [--cutx mm]  quita los triangulos con el centroide en x > mm y dibuja las dos caras (seccion: ver el interior)
 *           [--hide pieza,pieza]  no dibuja esas piezas   [--only pieza,pieza]  solo esas
 *   Con --list imprime las piezas (triangulos, caja) y sale.
 * Compilar:  make -C vr/weapons cutprev */
#include "wprev.h"
#include "../../vr/weapons/wcut.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *chnames[] = {"NONE", "SLIDE", "BOLT", "TRIGGER", "HAMMER", "MAG", "LID", "SAFETY", "SCREEN", "ROUND", "CUSTOM0", "CUSTOM1"};

static uint8_t *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long l = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(l > 0 ? (size_t)l : 1);
    if (!d || fread(d, 1, (size_t)l, f) != (size_t)l) { free(d); fclose(f); return NULL; }
    fclose(f); *n = (size_t)l; return d;
}
static int in_list(const char *list, const char *name) {
    if (!list) return 0;
    char b[256]; strncpy(b, list, 255); b[255] = 0;
    for (char *t = strtok(b, ","); t; t = strtok(NULL, ",")) if (!strcmp(t, name)) return 1;
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "uso: cutprev <stages> <stage> <id hex> -o f.png [opciones] (ver cabecera de cutprev.c)\n"); return 1; }
    const char *out = "out.png", *view = "side", *hide = NULL, *only = NULL;
    int W = 800, H = 600, ss = 2, list = 0, nch = 0, section = 0; float radius = 0.12f, gain = 4.0f, cutx = 1e9f;
    WVec3 ctr = wv(0, 0.02f, -0.07f);
    const char *chs[16];
    for (int i = 4; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--view") && i + 1 < argc) view = argv[++i];
        else if (!strcmp(argv[i], "--ch") && i + 1 < argc && nch < 16) chs[nch++] = argv[++i];
        else if (!strcmp(argv[i], "--center") && i + 1 < argc) { float x, y, z; if (sscanf(argv[++i], "%f,%f,%f", &x, &y, &z) == 3) ctr = wv(x * 1e-3f, y * 1e-3f, z * 1e-3f); }
        else if (!strcmp(argv[i], "--radius") && i + 1 < argc) radius = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &W, &H);
        else if (!strcmp(argv[i], "--ss") && i + 1 < argc) ss = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--gain") && i + 1 < argc) gain = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--cutx") && i + 1 < argc) { cutx = (float)atof(argv[++i]) * 1e-3f; section = 1; }
        else if (!strcmp(argv[i], "--hide") && i + 1 < argc) hide = argv[++i];
        else if (!strcmp(argv[i], "--only") && i + 1 < argc) only = argv[++i];
        else if (!strcmp(argv[i], "--list")) list = 1;
        else { fprintf(stderr, "opcion desconocida: %s\n", argv[i]); return 1; }
    }
    size_t n = 0; uint8_t *d = slurp(argv[1], &n);
    if (!d) { fprintf(stderr, "no se puede leer %s\n", argv[1]); return 1; }
    uint16_t id = (uint16_t)strtoul(argv[3], NULL, 16);
    WLoadOpts o; wload_default_opts(&o); o.resident = argv[2];
    WLoaded m; int r = wload_model(d, n, argv[2], id, &o, &m);
    if (r) { fprintf(stderr, "wload_model=%d\n", r); return 1; }
    const WCutModel *cm = wcut_find(id);
    if (!cm) { fprintf(stderr, "sin tabla de corte para %04x\n", id); return 1; }
    WCutSet cs; r = wcut_run(&m, cm, &cs);
    if (r) { fprintf(stderr, "wcut_run=%d\n", r); return 1; }
    if (list) {
        for (int i = 0; i < cs.npiece; i++) if (cs.piece[i].ni)
            printf("%-10s tris %4u  x %.1f..%.1f  y %.1f..%.1f  z %.1f..%.1f mm\n", cs.piece[i].name, cs.piece[i].ni / 3,
                   cs.piece[i].bmin.x * 1e3, cs.piece[i].bmax.x * 1e3, cs.piece[i].bmin.y * 1e3, cs.piece[i].bmax.y * 1e3, cs.piece[i].bmin.z * 1e3, cs.piece[i].bmax.z * 1e3);
        return 0;
    }
    const WeaponDef *def = NULL;
    for (int i = 0; cm->weapon && i < w_registry_count; i++) if (w_registry[i] && !strcmp(w_registry[i]->id, cm->weapon)) def = w_registry[i];
    if (!def) { fprintf(stderr, "arma '%s' no registrada\n", cm->weapon ? cm->weapon : "(ninguna)"); return 1; }
    /* filtros: piezas ocultas y seccion (copia de las mallas) */
    WMesh *ms = calloc((size_t)cs.nmeshes, sizeof(WMesh));
    for (int i = 0; i < cs.nmeshes; i++) {
        ms[i] = cs.meshes[i];
        const char *nm = cs.piece[i].name;
        if ((hide && in_list(hide, nm)) || (only && !in_list(only, nm))) { ms[i].nv = ms[i].ni = 0; continue; }
        WVertex *v = malloc(sizeof(WVertex) * (cs.meshes[i].nv + 1)); uint16_t *ix = malloc(2 * (size_t)(cs.meshes[i].ni + 1)); uint32_t k = 0;
        for (uint32_t t = 0; t + 2 < cs.meshes[i].ni; t += 3) {
            if (cutx < 1e8f) {   /* x del centroide en el marco del modelo = vertice + pivote */
                float cx = 0; for (int q = 0; q < 3; q++) cx += cs.meshes[i].v[cs.meshes[i].idx[t + q]].pos[0] + cs.piece[i].pivot.x;
                if (cx / 3 > cutx) continue;
            }
            for (int q = 0; q < 3; q++) { v[k] = cs.meshes[i].v[cs.meshes[i].idx[t + q]]; ix[k] = (uint16_t)k; k++; }
        }
        ms[i].v = v; ms[i].idx = ix; ms[i].nv = ms[i].ni = k;
    }
    WeaponState *s = calloc(1, def->state_size);
    s->def = def; def->init(s);
    weapon_bind(s, ms, cs.nmeshes, cs.parts, cs.nparts);
    for (int i = 0; i < nch; i++) {
        char *e = strchr(chs[i], '='); if (!e) continue;
        for (int c = 0; c < WCH_COUNT; c++) if (strlen(chnames[c]) == (size_t)(e - chs[i]) && !strncmp(chs[i], chnames[c], e - chs[i])) s->channel[c] = (float)atof(e + 1);
    }
    wprev_set_twosided(section);
    wprev_set_winding(1);   /* wcut entrega caras antihorarias hacia fuera */
    wprev_set_atlas(cs.atlas, (int)cs.atlas_w, (int)cs.atlas_h, gain);
    for (int i = 0; i < cs.nmeshes; i++) ms[i].texture = 1;
    WPose ww = {{0, 0, 0}, {0, 0, 0, 1}};
    float fov = 28.0f, dist = radius / tanf(fov * 0.5f * 3.14159265f / 180.0f);
    WVec3 R = wv(1, 0, 0), U = wv(0, 1, 0), F = wv(0, 0, -1), dir, up = U;
    if (!strcmp(view, "side")) dir = R; else if (!strcmp(view, "left")) dir = wv_scale(R, -1);
    else if (!strcmp(view, "top")) { dir = U; up = F; } else if (!strcmp(view, "under")) { dir = wv_scale(U, -1); up = F; }
    else if (!strcmp(view, "front")) dir = F; else if (!strcmp(view, "rear")) dir = wv_scale(F, -1);
    else if (!strcmp(view, "three")) dir = wv_norm(wv_add(wv_add(wv_scale(R, 0.7f), wv_scale(F, 0.55f)), wv_scale(U, 0.45f)));
    else if (!strcmp(view, "three2")) dir = wv_norm(wv_add(wv_add(wv_scale(R, 0.7f), wv_scale(F, -0.55f)), wv_scale(U, 0.45f)));
    else if (!strcmp(view, "three3")) dir = wv_norm(wv_add(wv_add(wv_scale(R, 0.6f), wv_scale(F, -0.5f)), wv_scale(U, -0.55f)));
    else if (!strcmp(view, "top3")) dir = wv_norm(wv_add(wv_add(wv_scale(R, 0.5f), wv_scale(F, 0.35f)), wv_scale(U, 0.9f)));
    else { fprintf(stderr, "vista desconocida\n"); return 1; }
    WCam cam = {wv_add(ctr, wv_scale(dir, dist)), ctr, up, fov};
    WImage img = wimg_new(W, H);
    wprev_render(s, ww, &cam, &img, ss);
    if (wimg_write_png(&img, out)) { fprintf(stderr, "no se pudo escribir %s\n", out); return 1; }
    return 0;
}
