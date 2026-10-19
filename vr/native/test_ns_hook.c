/* Pruebas de ns_player y ns_hook (núcleo, sin runtime) con una RAM falsa y un extractor sintético. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ns_hook.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FALLO %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static uint8_t *RAM;
static void w32(uint32_t a, uint32_t v) { memcpy(RAM + (a & 0x1FFFFF), &v, 4); }
static void w16(uint32_t a, int16_t v) { memcpy(RAM + (a & 0x1FFFFF), &v, 2); }
#define OBJ 0x80120000u
#define BODY 0x80110000u
static void put_part(int k, int16_t scale, int32_t tx) {   /* rotación = escala*identidad, traslación */
    uint32_t a = OBJ + 0x48 + 0x5C * k;
    for (int i = 0; i < 9; i++) w16(a + 2 * i, (i % 4 == 0) ? scale : 0);
    w32(a + 0x14, (uint32_t)tx); w32(a + 0x18, (uint32_t)(100 * k)); w32(a + 0x1C, 7);
}
static void setup_ram(void) {
    memset(RAM, 0, 0x200000);
    w32(0x800ACBB0u, BODY); w32(BODY, OBJ); w16(OBJ + 0x2E, 16);
    for (int k = 0; k < 16; k++) put_part(k, 4096, 1000 + 10 * k);
}
/* extractor: escena con 3 instancias; la 1 es el jugador (16 piezas = partes 0..15, la parte 4 con una pieza encadenada extra) y dos más. */
static int ex_calls, ex_ret;
static int extract(const uint8_t *ram, const uint8_t *scratch, NsScene *s, void *u) {
    (void)scratch; (void)u;
    ex_calls++;
    if (ex_ret) return ex_ret;
    s->valid = NS_SV_GAME | NS_SV_LISTS_OK;
    s->n_inst = 3; s->inst[0].key = 0x80130000u; s->inst[0].first_piece = 0; s->inst[0].n_pieces = 1;
    s->inst[1].key = OBJ; s->inst[1].first_piece = 1; s->inst[1].n_pieces = 17;
    s->inst[2].key = 0x80140000u; s->inst[2].first_piece = 18; s->inst[2].n_pieces = 1;
    s->n_piece = 19;
    for (int i = 0; i < 19; i++) s->piece[i].flags = 0;   /* las ranuras se reutilizan: el extractor escribe todos los campos */
    s->piece[0].world.m[0] = s->piece[0].world.m[5] = s->piece[0].world.m[10] = 1; s->piece[0].mesh_id = 1;
    int p = 1;
    for (int k = 0; k < 16; k++) {
        int16_t r[9]; int32_t t[3];
        uint32_t a = OBJ + 0x48 + 0x5C * k;
        for (int i = 0; i < 9; i++) memcpy(&r[i], ram + (a & 0x1FFFFF) + 2 * i, 2);
        for (int i = 0; i < 3; i++) memcpy(&t[i], ram + (a & 0x1FFFFF) + 0x14 + 4 * i, 4);
        ns_xf_from_engine(&s->piece[p].world, r, t); s->piece[p].mesh_id = 100 + k; p++;
        if (k == 4) { s->piece[p].world = s->piece[p - 1].world; s->piece[p].mesh_id = 500; p++; }
    }
    s->piece[18].world.m[0] = 1; s->piece[18].mesh_id = 2;
    s->to_vr[0] = 0;
    return 0;
}
static int ctx_calls; static uint32_t ctx_key; static int ctx_hide;
static void ctx(NsScene *s, NsCtxOut *o, void *u) {
    (void)u; ctx_calls++;
    s->valid |= NS_SV_TOVR_OK; s->area_id = 3;
    o->player_key = ctx_key; o->hide_head = ctx_hide; o->refresh_parts = NS_PARTS_ARMS;
}

static void t_player(void) {
    setup_ram();
    CHECK(ns_player_key(RAM) == OBJ, "clave del jugador");
    w32(BODY, 0x70000000u); CHECK(ns_player_key(RAM) == 0, "puntero inválido"); w32(BODY, OBJ);
    NsBridge *b = ns_bridge_create(NULL);
    NsScene *s = ns_bridge_begin(b);
    extract(RAM, NULL, s, NULL);
    NsPlayerMap m;
    CHECK(ns_player_bind(&m, s, RAM, OBJ) == 16, "partes enlazadas");
    CHECK(m.bound && m.inst == 1 && m.part[4].n == 2 && m.part[5].n == 1, "pieza encadenada: %d", m.part[4].n);
    CHECK(ns_player_hide_part(&m, s, NS_PART_HEAD) == 1 && (s->piece[1 + 6 + 1].flags & NS_PF_HIDDEN) && !(s->piece[1 + 5 + 1].flags & NS_PF_HIDDEN), "oculta la cabeza");
    /* la IK cambia la matriz de la parte 3 (brazo derecho antebrazo) en RAM */
    put_part(3, 2048, 5555);
    CHECK(ns_player_refresh(&m, s, RAM, NS_PARTS_ARMS) == 6, "brazos actualizados");
    CHECK(fabsf(s->piece[1 + 3].world.m[0] - 0.5f) < 1e-6f && s->piece[1 + 3].world.m[3] == 5555.0f, "pose IK en la escena");
    CHECK(fabsf(s->piece[1 + 1].world.m[0] - 1.0f) < 1e-6f, "el torso no se toca");
    /* sin coincidencia con la RAM (modelo distinto): nada enlazado */
    put_part(2, 777, 1); put_part(0, 100, 1);
    CHECK(ns_player_bind(&m, s, RAM, OBJ) < 16, "parte distinta no enlaza");
    CHECK(ns_player_bind(&m, s, RAM, 0x80199990u) == 0 && !m.bound, "jugador ausente");
    ns_bridge_destroy(b);
}

static void t_hook(void) {
    setup_ram();
    NsBridge *b = ns_bridge_create(NULL);
    ns_hook_init(b); ns_hook_reset();
    NsView v; NsHookStats st;
    ns_hook_set_frame(77);
    ns_capture(RAM, NULL);    /* sin extractor: sustituto, solo cabecera */
    ns_hook_tick(RAM);
    CHECK(ns_bridge_acquire(b, ns_now_ns(), &v) && v.cur->seq == 1 && v.cur->valid == 0 && v.cur->vblank == 77 && v.cur->game_frame == 1 && v.cur->t_ns, "sustituto");
    CHECK(!ns_hook_native_ready(ns_now_ns(), 1000000000ull), "no lista con valid=0");
    ns_hook_set_extractor(extract, NULL, (const NsMeshStore *)0x42);
    ns_hook_set_context(ctx, NULL);
    CHECK(ns_hook_has_extractor(), "extractor");
    ctx_key = OBJ; ctx_hide = 1;
    ns_capture(RAM, NULL);
    CHECK(ns_bridge_acquire(b, ns_now_ns(), &v) && v.cur->seq == 1, "no se publica hasta el tick");
    put_part(4, 1024, 4242);               /* la IK ocurre después de la captura */
    ns_hook_post_ik(RAM);
    ns_hook_tick(RAM);
    CHECK(ns_bridge_acquire(b, ns_now_ns(), &v) && v.fresh && v.cur->seq == 2 && v.store == (const NsMeshStore *)0x42, "publicada");
    CHECK(v.cur->area_id == 3 && (v.cur->valid & NS_SV_TOVR_OK) && v.cur->game_frame == 2, "contexto y contador de fotogramas");
    CHECK(v.cur->piece[1 + 6 + 1].flags & NS_PF_HIDDEN, "cabeza oculta por id de instancia");
    CHECK(v.cur->piece[1 + 4].world.m[3] == 4242.0f && fabsf(v.cur->piece[1 + 4].world.m[0] - 0.25f) < 1e-6f, "brazo con pose IK");
    CHECK(v.cur->piece[1 + 4 + 1].world.m[3] == 4242.0f, "pieza encadenada también");
    CHECK(ns_hook_native_ready(ns_now_ns(), 1000000000ull), "lista");
    CHECK(!ns_hook_native_ready(ns_now_ns() + 5000000000ull, 1000000000ull), "caduca");
    /* dos capturas antes del tick: la segunda reemplaza */
    setup_ram();
    ns_capture(RAM, NULL); ns_capture(RAM, NULL); ns_hook_tick(RAM);
    ns_hook_stats(&st);
    CHECK(st.replaced == 1 && st.published == 3 && st.captures == 4 && st.ik_refresh == 1 && st.player_bound == 3, "estadísticas c=%u p=%u r=%u", st.captures, st.published, st.replaced);
    /* extractor que falla: no se publica y el siguiente intento reutiliza la ranura */
    ex_ret = -1; ns_capture(RAM, NULL); ns_hook_tick(RAM); ex_ret = 0;
    ns_hook_stats(&st);
    CHECK(st.extract_fail == 1 && st.published == 3, "fallo del extractor");
    ns_capture(RAM, NULL); ns_hook_tick(RAM);
    ns_hook_stats(&st);
    CHECK(st.published == 4, "recupera");
    /* sin jugador */
    ctx_key = 0; ns_capture(RAM, NULL); ns_hook_tick(RAM);
    ns_bridge_acquire(b, ns_now_ns(), &v);
    CHECK(!(v.cur->piece[1 + 6 + 1].flags & NS_PF_HIDDEN), "sin jugador no se oculta nada");
    ns_hook_init(NULL); ns_capture(RAM, NULL);   /* sin puente: no hace nada */
    ns_bridge_destroy(b);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    RAM = (uint8_t *)calloc(1, 0x200000);
    t_player(); t_hook();
    free(RAM);
    printf(fails ? "ns_hook: %d fallos\n" : "ns_hook: todas las pruebas OK\n", fails);
    return fails != 0;
}
