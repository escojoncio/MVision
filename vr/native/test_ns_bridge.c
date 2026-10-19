/* Pruebas de ns_bridge (puente productor/consumidor, interpolación). Sin datos del juego. make -f vr/native/integra.mk test | test-san | test-tsan */
#include <math.h>
#define _GNU_SOURCE
#include <pthread.h>
#include <time.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "ns_bridge.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FALLO %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

/* Contenido determinista por seq: todas las piezas llevan world.m[3] = seq y el mesh_id = seq. */
static void fill(NsScene *s, uint32_t tag, uint32_t ni, uint32_t np_per) {
    s->n_inst = ni; s->n_piece = ni * np_per;
    for (uint32_t i = 0; i < ni; i++) {
        s->inst[i].key = 0x80100000u + 0x100u * i; s->inst[i].first_piece = i * np_per; s->inst[i].n_pieces = (uint16_t)np_per;
        for (uint32_t k = 0; k < np_per; k++) {
            NsPiece *p = &s->piece[i * np_per + k];
            memset(p, 0, sizeof *p);
            p->mesh_id = tag; p->world.m[0] = p->world.m[5] = p->world.m[10] = 1.0f;
            p->world.m[3] = (float)tag; p->world.m[7] = (float)i; p->world.m[11] = (float)k;
        }
    }
}
static int check(const NsScene *s) {
    for (uint32_t j = 0; j < s->n_piece; j++) if (s->piece[j].mesh_id != s->seq || s->piece[j].world.m[3] != (float)s->seq) return 0;
    return 1;
}

static void t_basic(void) {
    NsBridgeCaps cp = {8, 64, 4, 4};
    NsBridge *b = ns_bridge_create(&cp);
    CHECK(b != NULL, "create");
    NsView v;
    CHECK(ns_bridge_acquire(b, 0, &v) == 0, "vacío");
    NsScene *s = ns_bridge_begin(b);
    CHECK(s && s->cap_inst == 8 && s->cap_piece == 64 && s->version == NS_VERSION && s->n_inst == 0, "begin");
    CHECK(ns_bridge_pending(b) && ns_bridge_current(b) == s, "pendiente");
    CHECK(ns_bridge_begin(b) == s, "begin idempotente");
    s->t_ns = 1000;
    fill(s, 1, 2, 2);
    ns_bridge_publish(b, (const NsMeshStore *)0x1);
    CHECK(!ns_bridge_pending(b), "ya no pendiente");
    CHECK(ns_bridge_acquire(b, 5000, &v) == 1 && v.fresh && v.cur && v.cur->seq == 1 && !v.prev && !v.interp && v.alpha == 1.0f, "primera");
    CHECK(v.store == (const NsMeshStore *)0x1 && v.cur_gen == 1 && v.prev_gen == 0, "store/gen");
    CHECK(ns_bridge_acquire(b, 6000, &v) == 1 && !v.fresh && v.cur->seq == 1, "repite");
    /* sustituir el buzón: dos publicaciones sin tomar */
    s = ns_bridge_begin(b); s->t_ns = 2000; fill(s, 2, 2, 2); ns_bridge_publish(b, NULL);
    s = ns_bridge_begin(b); s->t_ns = 3000; fill(s, 3, 2, 2); ns_bridge_publish(b, NULL);
    NsBridgeStats st; ns_bridge_stats(b, &st);
    CHECK(st.published == 3 && st.dropped == 1, "dropped=%u", st.dropped);
    CHECK(ns_bridge_acquire(b, 7000, &v) && v.fresh && v.cur->seq == 3 && v.prev && v.prev->seq == 1, "salta la 2: cur=%u", v.cur->seq);
    CHECK(check(v.cur) && check(v.prev), "contenido");
    /* la escena actual y la anterior no se pisan aunque el productor siga */
    for (uint32_t i = 0; i < 20; i++) { s = ns_bridge_begin(b); s->t_ns = 10000 + i; fill(s, 4 + i, 2, 2); CHECK(s != v.cur && s != v.prev, "ranura en uso"); ns_bridge_publish(b, NULL); CHECK(check(v.cur) && check(v.prev), "pisada"); }
    ns_bridge_destroy(b);
}

static void t_epoch(void) {
    NsBridge *b = ns_bridge_create(NULL);
    CHECK(ns_bridge_acked_epoch(b) == 0, "ack inicial");
    ns_bridge_ack_epoch(b, 3);
    ns_bridge_ack_epoch(b, 2);   /* no retrocede */
    CHECK(ns_bridge_acked_epoch(b) == 3, "ack monótono");
    ns_bridge_destroy(b);
}

static void t_interp(void) {
    NsBridgeCaps cp = {8, 64, 4, 4};
    NsBridge *b = ns_bridge_create(&cp);
    NsView v; NsInterp ip; NsXf x;
    const uint64_t MS = 1000000ull;
    NsScene *s = ns_bridge_begin(b); s->t_ns = 100 * MS; fill(s, 7, 3, 2);
    s->inst[0].key = 0xAAA0; s->inst[1].key = 0xBBB0; s->inst[2].key = 0xCCC0;
    memcpy(s->to_vr, (float[12]){1,0,0,0, 0,1,0,0, 0,0,1,0}, 48); s->valid = NS_SV_TOVR_OK;
    ns_bridge_publish(b, NULL);
    ns_bridge_acquire(b, 100 * MS, &v);
    s = ns_bridge_begin(b); s->t_ns = 140 * MS; fill(s, 7, 3, 2);
    /* orden distinto y una instancia nueva */
    s->inst[0].key = 0xCCC0; s->inst[1].key = 0xDDD0; s->inst[2].key = 0xAAA0;
    for (uint32_t i = 0; i < 6; i++) s->piece[i].world.m[3] = 1000.0f;   /* trasladadas 1000 respecto a la anterior (m[3]=7) -> ver abajo */
    s->piece[0].world.m[3] = 107.0f; s->piece[1].world.m[3] = 107.0f; s->piece[4].world.m[3] = 9000.0f; s->piece[5].world.m[3] = 9000.0f;
    memcpy(s->to_vr, (float[12]){1,0,0,40, 0,1,0,0, 0,0,1,0}, 48); s->valid = NS_SV_TOVR_OK;
    ns_bridge_publish(b, NULL);
    CHECK(ns_bridge_acquire(b, 140 * MS, &v) && v.interp, "continua");
    CHECK(fabsf(v.alpha - 0.0f) < 0.02f, "alpha al llegar la escena nueva = %f", v.alpha);   /* T = 140 - 40 (periodo) = 100 -> alpha 0 */
    ns_bridge_acquire(b, 160 * MS, &v);
    CHECK(fabsf(v.alpha - 0.5f) < 0.02f, "alpha mitad = %f", v.alpha);
    ns_bridge_acquire(b, 1000 * MS, &v);
    CHECK(fabsf(v.alpha - 1.25f) < 1e-4f, "alpha acotado = %f", v.alpha);
    ns_bridge_acquire(b, 160 * MS, &v);
    ns_interp_begin(b, &v, &ip);
    CHECK(ip.enabled && ip.map && ip.map[0] == 2 && ip.map[1] == -1 && ip.map[2] == 0, "mapa por key: %d %d %d", ip.map[0], ip.map[1], ip.map[2]);
    /* cur inst 0 (CCC0) emparejó con prev inst 2 (tag 7, m[3]=7): 7 -> 107 en alpha 0.5 = 57 */
    CHECK(ns_interp_piece(&ip, 0, 0, &x) == 1 && fabsf(x.m[3] - 57.0f) < 0.5f, "pieza interpolada %f", x.m[3]);
    /* instancia nueva: sin emparejar -> la de cur */
    CHECK(ns_interp_piece(&ip, 1, 0, &x) == 0 && x.m[3] == 1000.0f, "sin pareja");
    /* teletransporte: inst 2 (AAA0) pasa de 7 a 1000 */
    CHECK(ns_interp_piece(&ip, 2, 0, &x) == 0 && x.m[3] == 9000.0f, "teletransporte no se interpola");
    float tv[12]; ns_interp_to_vr(&ip, tv);
    CHECK(fabsf(tv[3] - 20.0f) < 0.6f, "to_vr interpolado %f", tv[3]);
    /* discontinuidad: época distinta */
    ns_bridge_acquire(b, 160 * MS, &v);
    s = ns_bridge_begin(b); s->t_ns = 180 * MS; s->mesh_epoch = 9; fill(s, 7, 3, 2); ns_bridge_publish(b, NULL);
    ns_bridge_acquire(b, 190 * MS, &v);
    CHECK(!v.interp && v.alpha == 1.0f, "época nueva sin interpolar");
    ns_interp_begin(b, &v, &ip);
    CHECK(!ip.enabled && ns_interp_piece(&ip, 0, 0, &x) == 0, "desactivada");
    /* hueco largo */
    ns_bridge_acquire(b, 190 * MS, &v);
    s = ns_bridge_begin(b); s->t_ns = 900 * MS; s->mesh_epoch = 9; fill(s, 7, 3, 2); ns_bridge_publish(b, NULL);
    ns_bridge_acquire(b, 900 * MS, &v);
    CHECK(!v.interp, "hueco > 400 ms");
    ns_bridge_destroy(b);
}

static void t_lerp(void) {
    /* rotación de 0 a 90 grados sobre Y a mitad = 45; escala uniforme se conserva */
    NsXf a = {{1,0,0,0, 0,1,0,0, 0,0,1,0}}, b = {{0,0,1,10, 0,1,0,0, -1,0,0,0}}, o;
    ns_xf_lerp(&a, &b, 0.5f, &o);
    float c = cosf(0.78539816f);
    CHECK(fabsf(o.m[0] - c) < 1e-4f && fabsf(o.m[2] - c) < 1e-4f && fabsf(o.m[8] + c) < 1e-4f && fabsf(o.m[3] - 5.0f) < 1e-5f, "slerp 45: %f %f", o.m[0], o.m[2]);
    float len = sqrtf(o.m[0] * o.m[0] + o.m[4] * o.m[4] + o.m[8] * o.m[8]);
    CHECK(fabsf(len - 1.0f) < 1e-4f, "longitud de columna %f", len);
    NsXf a2 = a, b2 = b;
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) { a2.m[4 * i + j] *= 0.001f; b2.m[4 * i + j] *= 0.001f; }
    ns_xf_lerp(&a2, &b2, 0.5f, &o);
    len = sqrtf(o.m[0] * o.m[0] + o.m[4] * o.m[4] + o.m[8] * o.m[8]);
    CHECK(fabsf(len - 0.001f) < 1e-6f, "escala conservada %g", len);
    /* matriz aplanada (sombra): cae a lineal, finita */
    NsXf f1 = {{500,0,0,0, 0,0,0,0, 0,0,500,0}}, f2 = {{700,0,0,0, 0,0,0,0, 0,0,300,0}};
    ns_xf_lerp(&f1, &f2, 0.5f, &o);
    CHECK(o.m[0] == 600.0f && o.m[10] == 400.0f, "aplanada lineal");
    ns_xf_lerp(&a, &b, 0.0f, &o); CHECK(memcmp(&o, &a, sizeof o) == 0, "t=0");
    ns_xf_lerp(&a, &a, 0.7f, &o); CHECK(memcmp(&o, &a, sizeof o) == 0, "iguales");
    /* camino corto: 170 grados y -170 grados */
    float th1 = 170.0f * 0.0174532925f, th2 = -170.0f * 0.0174532925f;
    NsXf r1 = {{cosf(th1),0,sinf(th1),0, 0,1,0,0, -sinf(th1),0,cosf(th1),0}}, r2 = {{cosf(th2),0,sinf(th2),0, 0,1,0,0, -sinf(th2),0,cosf(th2),0}};
    ns_xf_lerp(&r1, &r2, 0.5f, &o);
    CHECK(fabsf(o.m[0] + 1.0f) < 1e-3f, "camino corto por 180: %f", o.m[0]);
}

/* ---- hilos ---- */
static NsBridge *TB;
static _Atomic int stop_flag;
#define NPUB 30000
static void *producer(void *arg) {
    (void)arg;
    for (uint32_t i = 1; i <= NPUB; i++) {
        NsScene *s = ns_bridge_begin(TB);
        uint32_t tag = i;
        fill(s, tag, 3, 4);
        s->t_ns = ns_now_ns();
        ns_bridge_publish(TB, NULL);
        s->seq = s->seq;   /* (no se toca tras publicar: la lectura de seq no es del productor) */
        if ((i & 255) == 0) { struct timespec t = {0, 100000}; nanosleep(&t, NULL); }
    }
    atomic_store(&stop_flag, 1);
    return NULL;
}
static void t_threads(void) {
    NsBridgeCaps cp = {8, 64, 4, 4};
    TB = ns_bridge_create(&cp);
    atomic_store(&stop_flag, 0);
    pthread_t th;
    pthread_create(&th, NULL, producer, NULL);
    uint32_t last = 0, seen = 0, bad = 0, nonmono = 0;
    NsView v;
    for (;;) {
        int done = atomic_load(&stop_flag);
        if (ns_bridge_acquire(TB, ns_now_ns(), &v)) {
            if (v.cur->seq < last) nonmono++;
            if (v.fresh) { seen++; last = v.cur->seq; }
            /* consistencia: el contenido corresponde a su seq (tag == seq porque la ranura se publica en orden) y prev anterior a cur */
            if (!check(v.cur)) bad++;
            if (v.prev && (!check(v.prev) || v.prev->seq >= v.cur->seq)) bad++;
        }
        if (done) { if (!ns_bridge_acquire(TB, ns_now_ns(), &v) || v.cur->seq == NPUB) break; }
    }
    pthread_join(th, NULL);
    ns_bridge_acquire(TB, ns_now_ns(), &v);
    CHECK(v.cur->seq == NPUB, "última escena = %u", v.cur->seq);
    CHECK(bad == 0 && nonmono == 0, "escenas corruptas %u, no monótonas %u", bad, nonmono);
    NsBridgeStats st; ns_bridge_stats(TB, &st);
    CHECK(st.published == NPUB && st.acquired + st.dropped <= st.published + 1 && seen == st.acquired, "contadores pub=%u acq=%u drop=%u seen=%u", st.published, st.acquired, st.dropped, seen);
    printf("hilos: %u publicadas, %u tomadas, %u sustituidas\n", st.published, st.acquired, st.dropped);
    ns_bridge_destroy(TB);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    t_basic(); t_epoch(); t_interp(); t_lerp(); t_threads();
    printf(fails ? "ns_bridge: %d fallos\n" : "ns_bridge: todas las pruebas OK\n", fails);
    return fails != 0;
}
