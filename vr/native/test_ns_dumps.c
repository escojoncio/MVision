/* Banco sin conexión sobre volcados NSD1 reales (el volcado lleva datos del juego: NO se sube al repositorio). Uso: test_ns_dumps <carpeta con volcados> (o MV_DUMPS=carpeta).
 * Sin carpeta: se salta (código 0). Por cada volcado ejecuta el enganche real (ns_hook + extractor del enganche ns_extract_glue + puente) de dos formas:
 *   envoltura: ns_capture (la etapa «pantalla» original, entrada 0 de la tabla = 0x8001D324)
 *   respaldo:  ns_capture_backup_due/ns_capture_backup (entrada 0 apuntando a una copia en overlay: las introducciones)
 * y compara con el comportamiento anterior (extractor sin la clave del jugador: el objeto del jugador oculto por el motor se descartaba => jugador sin enlazar => escena no apta).
 * Contexto simulado (no hay cámara VR fuera del visor): TOVR_OK solo si el volcado trae partida con cámara VR activa (game=1, rec_active=1), sin IK (refresh_parts = 0).
 * Comprobaciones: volcado con partida y cámara VR => la escena de la envoltura es apta (ns_scene_native_ok) con el jugador enlazado; todo volcado => la captura por respaldo da instancias > 0
 * con partida/listas/escenario; escena de introducción (game=0) => apta para el cine (GAME|LISTS_OK|MAP_OK, sin MAP_INCOMPLETE ni STORE_FULL). */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "ns_dump.h"
#include "ns_extract.h"
#include "ns_hook.h"

void ns_extract_register(void);

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FALLO %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static const uint8_t *g_ram;
static int g_tovr;
static void ctx(NsScene *sc, NsCtxOut *o, void *u) {
    (void)u;
    if (!g_tovr) return;                       /* sin cámara VR (cinemática, introducción): sin mapa VR ni jugador, como ns_ctx de vrhost */
    sc->valid |= NS_SV_TOVR_OK;
    sc->to_vr[0] = sc->to_vr[5] = sc->to_vr[10] = 0.001f;
    o->player_key = ns_player_key(g_ram);
    o->hide_head = 1;
    o->refresh_parts = 0;
}

typedef struct { uint32_t valid, n_inst, n_static, n_piece, bound, ok, cinema_ok; } Res;

static Res run_mode(NsBridge *b, const NsDump *d, int backup) {
    Res r; memset(&r, 0, sizeof r);
    NsHookStats s0, s1;
    ns_hook_stats(&s0);
    if (backup) { if (ns_capture_backup_due()) ns_capture_backup(d->ram, d->scratch); }
    else ns_capture(d->ram, d->scratch);
    ns_hook_tick(d->ram);
    ns_hook_stats(&s1);
    NsView v;
    if (!ns_bridge_acquire(b, ns_now_ns(), &v) || !v.cur) return r;
    const uint32_t cin = NS_SV_GAME | NS_SV_LISTS_OK | NS_SV_MAP_OK;
    r.valid = v.cur->valid; r.n_inst = v.cur->n_inst; r.n_piece = v.cur->n_piece;
    for (uint32_t i = 0; i < v.cur->n_inst; i++) r.n_static += (v.cur->inst[i].flags & NS_IF_STATIC) != 0;
    r.bound = s1.player_bound - s0.player_bound;
    r.ok = (uint32_t)ns_scene_native_ok(r.valid);
    r.cinema_ok = (r.valid & cin) == cin && !(r.valid & (NS_SV_MAP_INCOMPLETE | NS_SV_STORE_FULL));
    CHECK(s1.cap_wrap - s0.cap_wrap == (backup ? 0u : 1u) && s1.cap_backup - s0.cap_backup == (backup ? 1u : 0u), "captura por el canal equivocado (envoltura +%u respaldo +%u)", s1.cap_wrap - s0.cap_wrap, s1.cap_backup - s0.cap_backup);
    return r;
}

/* comportamiento anterior: extracción sin la clave del jugador y aptitud con PLAYER_OK exigido */
static void old_behavior(const NsDump *d, uint32_t *inst, uint32_t *hidden, uint32_t *bound, int *old_ok) {
    NsScene sc; memset(&sc, 0, sizeof sc);
    ns_scene_alloc(&sc, 0, 0, 0, 0);
    NsMeshStore *st = ns_store_create();
    NsExtractOpts o; memset(&o, 0, sizeof o);
    NsExtractStats es;
    ns_extract_ex(&sc, st, d->ram, NS_RAM_SIZE, d->scratch, 1024u, &o, &es);
    NsPlayerMap m;
    uint32_t key = ns_player_key(d->ram);
    *inst = sc.n_inst; *hidden = es.drop_hidden_flag;
    *bound = key ? (uint32_t)(ns_player_bind(&m, &sc, d->ram, key) > 0) : 0;
    *old_ok = *bound && (sc.valid & (NS_SV_GAME | NS_SV_LISTS_OK | NS_SV_MAP_OK)) == (NS_SV_GAME | NS_SV_LISTS_OK | NS_SV_MAP_OK);
    ns_scene_free(&sc);
    ns_store_destroy(st);
}

static int cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

int main(int argc, char **argv) {
    const char *root = argc > 1 ? argv[1] : getenv("MV_DUMPS");
    if (!root || !*root) { printf("test_ns_dumps: SALTADO (sin carpeta de volcados: argumento o MV_DUMPS)\n"); return 0; }
    DIR *dir = opendir(root);
    if (!dir) { printf("test_ns_dumps: no se puede abrir %s\n", root); return 1; }
    char *names[256]; int n = 0;
    for (struct dirent *e; (e = readdir(dir)) && n < 256;) {
        if (e->d_name[0] == '.') continue;
        char p[1024]; struct stat sb;
        snprintf(p, sizeof p, "%s/%s/ram.bin", root, e->d_name);
        if (stat(p, &sb) == 0) names[n++] = strdup(e->d_name);
    }
    closedir(dir);
    qsort(names, (size_t)n, sizeof *names, cmp);
    NsBridge *b = ns_bridge_create(NULL);
    ns_hook_init(b);
    ns_extract_register();
    ns_hook_set_context(ctx, NULL);
    int ndumps = 0;
    for (int i = 0; i < n; i++) {
        char p[1024];
        snprintf(p, sizeof p, "%s/%s", root, names[i]);
        NsDump *d = ns_dump_open(p);
        if (!d) { printf("%-34s no se puede abrir\n", names[i]); free(names[i]); continue; }
        ndumps++;
        int game = (int)ns_dump_meta_num(d, "game", 0), rec = (int)ns_dump_meta_num(d, "rec_active", 0);
        g_ram = d->ram; g_tovr = game && rec;
        uint32_t e0 = ns_r32(d, 0x8009E534u);
        uint32_t oi, oh, ob; int ook;
        old_behavior(d, &oi, &oh, &ob, &ook);
        ns_hook_reset();
        Res w = run_mode(b, d, 0);
        ns_hook_reset();
        Res k = run_mode(b, d, 1);
        printf("%-34s juego=%d vr=%d entrada0=%08x | antes: inst=%u ocultos=%u jugador=%u apta=%d | envoltura: inst=%u estat=%u piezas=%u valid=0x%02x jugador=%u apta=%u | respaldo: inst=%u estat=%u valid=0x%02x jugador=%u apta=%u cine=%u\n",
               names[i], game, rec, e0, oi, oh, ob, ook, w.n_inst, w.n_static, w.n_piece, w.valid, w.bound, w.ok, k.n_inst, k.n_static, k.valid, k.bound, k.ok, k.cinema_ok);
        if (g_tovr) {
            CHECK(w.ok, "%s: escena de la envoltura no apta (valid=0x%x)", names[i], w.valid);
            CHECK(w.bound == 1 && (w.valid & NS_SV_PLAYER_OK) && (w.valid & NS_SV_PLAYER_PRESENT), "%s: jugador sin enlazar (enlazados=%u valid=0x%x)", names[i], w.bound, w.valid);
            CHECK(k.ok && k.bound == 1, "%s: el respaldo da una escena distinta (apta=%u jugador=%u)", names[i], k.ok, k.bound);
        }
        CHECK(k.n_inst > 0 && k.n_static > 0 && k.cinema_ok, "%s: respaldo sin instancias/escenario (inst=%u estat=%u valid=0x%x)", names[i], k.n_inst, k.n_static, k.valid);
        CHECK(w.n_inst == k.n_inst && w.n_piece > 0, "%s: envoltura y respaldo difieren (inst %u / %u)", names[i], w.n_inst, k.n_inst);
        ns_dump_close(d);
        free(names[i]);
    }
    CHECK(ndumps > 0, "ningún volcado en %s", root);
    ns_bridge_destroy(b);
    printf(fails ? "test_ns_dumps: %d fallos\n" : "test_ns_dumps: todas las comprobaciones OK\n", fails);
    return fails != 0;
}
