/* ns_extract: extractor de la escena nativa (ver ns_extract.h, ns_types.h y docs/escena_nativa.md). */
#include "ns_extract.h"
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------------------------------------------- direcciones del motor */
#define A_BGCHAN     0x800B2990u   /* canal de fondo (tabla de canales, entrada 0) */
#define A_CHAN       0x800B2B7Cu   /* canal principal */
#define A_TEXTAB     0x800B30E0u   /* tabla de texturas: 512 x 12 B */
#define A_GROUP      0x800ACAF8u   /* grupo activo (máscara) */
#define A_CLOCK      0x800ACAB0u   /* reloj de búfer */
#define A_LIGHT_G    0x8009E55Cu   /* luz global: dos matrices de 32 B consecutivas */
#define A_AMBIENT_G  0x800AC530u   /* ambiente global: 3 x s16 */
#define ADDR_MASK    0x1FFFFFFFu   /* dirección física (normaliza KUSEG/KSEG0/KSEG1) */

#define MAX_QUEUE     256
#define MAX_PIECES    1024         /* tope de piezas por objeto compuesto */
#define MAX_FACES     8192         /* por modelo */
#define MAX_VERTS     256
#define MAX_NORMS     256
#define MAX_EXT_DEPTH 32           /* extensiones encadenadas por pieza: medido hasta 10 en un mapa (nx = nº de modelos del def; el bucle se detecta con `seen`) */
/* El motor retira una pieza del dibujo sin tocar su modelo poniéndole el sesgo de orden a -30000 (y un solo paquete): así oculta la cabeza del jugador cuando un objeto equipado (gafas, máscara)
 * trae la suya, pegada a la matriz de la cabeza (medido: pieza 6 del jugador con sesgo -30000 y n_paq 1 con las gafas térmicas puestas; 0 y 81 sin ellas). El extractor rederiva las caras del
 * modelo, así que sin esta regla dibujaba las dos cabezas. Solo objetos dinámicos: en el escenario un sesgo extremo es «al fondo del todo» (cielos), no una retirada. */
#define PIECE_BIAS_HIDDEN (-30000)
#define MAX_STATIC_DEFS 256
#define MEMO_N        16384u       /* potencia de 2 */
#define ARENA_BUCKETS 4096u        /* potencia de 2 */

/* flags del objeto compuesto */
#define OF_TEXTURED    0x001u
#define OF_PRECOLORED  0x002u
#define OF_TRANSLUCENT 0x004u
#define OF_LIT         0x008u
#define OF_ONE_MATRIX  0x040u   /* ONE_MATRIX_WORLD: las rutinas de pantalla del motor (0x8001C614 en juego, 0x80081200 en la etapa de la overlay de la introducción) transforman TODAS las piezas con la matriz de la raíz
                                  * (obj+0x00, ya con padre_mat copiada). En juego copian esa misma matriz a cada pieza (+0x00); en la overlay escriben ahí la raíz con la traslación RELATIVA AL OJO (raíz - ojo,
                                  * medido: pieza t = -ojo.t con raíz en el origen). Por eso el mundo de un objeto rígido es la raíz, no la matriz de la pieza. */
#define OF_GBOUND      0x020u   /* visibilidad por la caja de la definición; sus caras no se subdividen (libdg/divide.c) */
#define OF_HIDDEN      0x080u
#define OF_OWN_AMBIENT 0x100u
#define OF_ALT_PALETTE 0x200u
#define OF_ENV_REFLECT 0x400u
/* flags del modelo */
#define MF_TRANSLUCENT 0x2u
#define MF_UNLIT       0x4u
#define MF_TWO_SIDED   0x400u
#define MF_SHARED_VERTS 0x10000u   /* vértices soldados al padre; índices de 7 bits */

/* ---------------------------------------------------------------------------------------------------------------- lectura acotada de RAM */
typedef struct {
    const uint8_t *b;
    uint32_t size;
    uint32_t oob;
} Ram;

/* [a, a+n) dentro de la RAM (KUSEG/KSEG0/KSEG1, sin espejos) */
static int ok_rng(const Ram *r, uint32_t a, uint32_t n) {
    uint32_t seg = a >> 29;
    if (seg != 0 && seg != 4 && seg != 5) return 0;
    uint32_t o = a & ADDR_MASK;
    return o < r->size && n <= r->size - o;
}
static const uint8_t *at(Ram *r, uint32_t a, uint32_t n) {
    if (!ok_rng(r, a, n)) { r->oob++; return NULL; }
    return r->b + (a & ADDR_MASK);
}
static uint32_t rd32(Ram *r, uint32_t a) { const uint8_t *p = at(r, a, 4); uint32_t v = 0; if (p) memcpy(&v, p, 4); return v; }
static uint16_t rd16(Ram *r, uint32_t a) { const uint8_t *p = at(r, a, 2); uint16_t v = 0; if (p) memcpy(&v, p, 2); return v; }
static uint8_t rd8(Ram *r, uint32_t a) { const uint8_t *p = at(r, a, 1); return p ? *p : 0; }
static int16_t rs16(Ram *r, uint32_t a) { return (int16_t)rd16(r, a); }
static int32_t rs32(Ram *r, uint32_t a) { return (int32_t)rd32(r, a); }
static uint32_t norm_addr(uint32_t a) { return a & ADDR_MASK; }

/* matriz del motor (32 B): rotación s16[9] en +0, traslación s32[3] en +0x14 */
static int read_matrix(Ram *r, uint32_t a, NsXf *o) {
    if (!ok_rng(r, a, 0x20)) { r->oob++; memset(o, 0, sizeof *o); return 0; }
    int16_t m[9]; int32_t t[3];
    for (int i = 0; i < 9; i++) m[i] = rs16(r, a + 2u * (uint32_t)i);
    for (int i = 0; i < 3; i++) t[i] = rs32(r, a + 0x14u + 4u * (uint32_t)i);
    ns_xf_from_engine(o, m, t);
    return 1;
}

/* ---------------------------------------------------------------------------------------------------------------- almacén: épocas de arena */
typedef struct Node { _Atomic(struct Node *) next; NsMesh mesh; } Node;                         /* arrays a continuación */
typedef struct BNode { _Atomic(struct BNode *) next; NsBaked b; uint64_t chash; } BNode;       /* colores a continuación */
typedef struct Arena {
    uint32_t epoch;
    size_t bytes;
    uint32_t count;
    _Atomic(Node *) mesh[ARENA_BUCKETS];
    _Atomic(BNode *) baked[ARENA_BUCKETS];
} Arena;

typedef struct {
    uint32_t addr, def;       /* direcciones físicas del modelo y de su definición (0 = libre: la dirección 0 no es válida) */
    uint64_t sig, zsig, id, baked_hash;   /* sig: contenido completo (recodificar); zsig: solo estructura, sin vértices ni normales (cambio de zona) */
    uint32_t baked_ver;
    uint16_t n_faces;
    int16_t parent, bias;
    uint8_t empty;            /* el modelo no tiene caras */
} Memo;

typedef struct {
    NsVert v[MAX_VERTS];
    NsNorm n[MAX_NORMS];
    NsFace f[MAX_FACES];
    uint32_t col[MAX_FACES * 4];
    uint8_t marked[MAX_VERTS];
} Work;

struct NsMeshStore {
    _Atomic(Arena *) cur, prev;
    _Atomic uint32_t ack;
    size_t limit;
    size_t compact;           /* umbral de compactación en bytes (0 = automático: 1/4 del tope, mínimo 8 MiB) */
    uint32_t zone_gen;
    uint32_t baked_counter;   /* versiones de color únicas en toda la vida del almacén (no se reinician con la época) */
    Memo *memo;
    Work *w;                  /* temporales de decodificación, reservados una sola vez */
    uint32_t prev_defs[MAX_STATIC_DEFS];
    uint32_t n_prev_defs;
};

static uint32_t bucket_of(uint64_t id) { return (uint32_t)((id * 0x9E3779B97F4A7C15ull) >> 40) & (ARENA_BUCKETS - 1u); }

static Arena *arena_new(uint32_t epoch) {
    Arena *a = (Arena *)calloc(1, sizeof *a);
    if (a) a->epoch = epoch;
    return a;
}
static void arena_free(Arena *a) {
    if (!a) return;
    for (uint32_t i = 0; i < ARENA_BUCKETS; i++) {
        Node *n = atomic_load_explicit(&a->mesh[i], memory_order_relaxed);
        while (n) { Node *nx = atomic_load_explicit(&n->next, memory_order_relaxed); free(n); n = nx; }
        BNode *b = atomic_load_explicit(&a->baked[i], memory_order_relaxed);
        while (b) { BNode *nx = atomic_load_explicit(&b->next, memory_order_relaxed); free(b); b = nx; }
    }
    free(a);
}

NsMeshStore *ns_store_create(void) {
    NsMeshStore *s = (NsMeshStore *)calloc(1, sizeof *s);
    if (!s) return NULL;
    s->memo = (Memo *)calloc(MEMO_N, sizeof(Memo));
    s->w = (Work *)malloc(sizeof(Work));
    Arena *a = arena_new(1);
    if (!s->memo || !s->w || !a) { free(s->memo); free(s->w); arena_free(a); free(s); return NULL; }
    atomic_store(&s->cur, a); atomic_store(&s->prev, NULL);
    atomic_store(&s->ack, 0u);
    s->limit = NS_STORE_DEFAULT_LIMIT; s->zone_gen = 1;
    return s;
}
void ns_store_destroy(NsMeshStore *s) {
    if (!s) return;
    arena_free(atomic_load(&s->cur)); arena_free(atomic_load(&s->prev));
    free(s->memo); free(s->w); free(s);
}
void ns_store_set_limit(NsMeshStore *s, size_t bytes) { if (s) s->limit = bytes; }
void ns_store_set_compact(NsMeshStore *s, size_t bytes) { if (s) s->compact = bytes; }
uint32_t ns_store_zone_gen(const NsMeshStore *s) { return s ? s->zone_gen : 0; }
uint32_t ns_store_epoch(const NsMeshStore *s) { Arena *c = s ? atomic_load(&((NsMeshStore *)s)->cur) : NULL; return c ? c->epoch : 0; }
void ns_store_ack(NsMeshStore *s, uint32_t epoch) {
    if (!s) return;
    uint32_t o = atomic_load_explicit(&s->ack, memory_order_relaxed);
    while (o < epoch && !atomic_compare_exchange_weak_explicit(&s->ack, &o, epoch, memory_order_release, memory_order_relaxed)) {}
}
size_t ns_store_bytes(const NsMeshStore *s) {
    if (!s) return 0;
    Arena *c = atomic_load(&((NsMeshStore *)s)->cur), *p = atomic_load(&((NsMeshStore *)s)->prev);
    return (c ? c->bytes : 0) + (p ? p->bytes : 0);
}
uint32_t ns_store_count(const NsMeshStore *s) {
    if (!s) return 0;
    Arena *c = atomic_load(&((NsMeshStore *)s)->cur), *p = atomic_load(&((NsMeshStore *)s)->prev);
    return (c ? c->count : 0) + (p ? p->count : 0);
}
/* Libera la época anterior si el consumidor ya reconoció la actual (productor). */
static void try_free_prev(NsMeshStore *s) {
    Arena *p = atomic_load_explicit(&s->prev, memory_order_relaxed), *c = atomic_load_explicit(&s->cur, memory_order_relaxed);
    if (p && atomic_load_explicit(&s->ack, memory_order_acquire) >= c->epoch) { atomic_store_explicit(&s->prev, NULL, memory_order_release); arena_free(p); }
}
int ns_store_new_epoch(NsMeshStore *s) {
    if (!s) return 0;
    try_free_prev(s);
    if (atomic_load(&s->prev)) return 0;
    Arena *c = atomic_load(&s->cur), *n = arena_new(c->epoch + 1u);
    if (!n) return 0;
    atomic_store_explicit(&s->prev, c, memory_order_release);
    atomic_store_explicit(&s->cur, n, memory_order_release);
    return 1;
}

static const Arena *readable_prev(const NsMeshStore *s, const Arena *c) {
    Arena *p = atomic_load_explicit(&((NsMeshStore *)s)->prev, memory_order_acquire);
    (void)c;
    if (p && atomic_load_explicit(&((NsMeshStore *)s)->ack, memory_order_acquire) <= p->epoch) return p;   /* el consumidor aún puede tocar esa época */
    return NULL;
}
static const NsMesh *arena_find(const Arena *a, uint64_t id) {
    for (Node *n = atomic_load_explicit(&((Arena *)a)->mesh[bucket_of(id)], memory_order_acquire); n; n = atomic_load_explicit(&n->next, memory_order_acquire))
        if (n->mesh.id == id) return &n->mesh;
    return NULL;
}
const NsMesh *ns_store_get(const NsMeshStore *s, uint64_t id) {
    if (!s) return NULL;
    const Arena *c = atomic_load_explicit(&((NsMeshStore *)s)->cur, memory_order_acquire);
    const NsMesh *m = c ? arena_find(c, id) : NULL;
    if (m) return m;
    const Arena *p = readable_prev(s, c);
    return p ? arena_find(p, id) : NULL;
}
static const NsBaked *arena_find_baked(const Arena *a, uint64_t id, uint32_t ver) {
    for (BNode *n = atomic_load_explicit(&((Arena *)a)->baked[bucket_of(id)], memory_order_acquire); n; n = atomic_load_explicit(&n->next, memory_order_acquire))
        if (n->b.mesh_id == id && n->b.ver == ver) return &n->b;
    return NULL;
}
const NsBaked *ns_store_get_baked(const NsMeshStore *s, uint64_t mesh_id, uint32_t ver) {
    if (!s || !ver) return NULL;
    const Arena *c = atomic_load_explicit(&((NsMeshStore *)s)->cur, memory_order_acquire);
    const NsBaked *b = c ? arena_find_baked(c, mesh_id, ver) : NULL;
    if (b) return b;
    const Arena *p = readable_prev(s, c);
    return p ? arena_find_baked(p, mesh_id, ver) : NULL;
}

/* ¿cabe `n` bytes más? Si no, libera la época anterior si se puede. */
static int store_room(NsMeshStore *s, size_t n) {
    if (ns_store_bytes(s) + n <= s->limit) return 1;
    try_free_prev(s);
    return ns_store_bytes(s) + n <= s->limit;
}
/* Añade la malla temporal a la época actual. 1 = añadida, 0 = ya estaba, -1 = tope del almacén o sin memoria. */
static int store_add_mesh(NsMeshStore *s, const NsMesh *m) {
    Arena *a = atomic_load_explicit(&s->cur, memory_order_relaxed);
    if (arena_find(a, m->id)) return 0;
    size_t sv = (size_t)m->n_verts * sizeof(NsVert), sn = (size_t)m->n_norms * sizeof(NsNorm), sf = (size_t)m->n_faces * sizeof(NsFace);
    size_t off_v = (sizeof(Node) + 7u) & ~(size_t)7u, off_n = off_v + ((sv + 7u) & ~(size_t)7u), off_f = off_n + ((sn + 7u) & ~(size_t)7u), total = off_f + sf;
    if (!store_room(s, total)) return -1;
    a = atomic_load_explicit(&s->cur, memory_order_relaxed);
    uint8_t *blk = (uint8_t *)calloc(1, total);
    if (!blk) return -1;
    Node *nd = (Node *)blk;
    nd->mesh = *m;
    nd->mesh.epoch = a->epoch;
    nd->mesh.verts = (const NsVert *)(blk + off_v); nd->mesh.norms = (const NsNorm *)(blk + off_n); nd->mesh.faces = (const NsFace *)(blk + off_f);
    memcpy(blk + off_v, m->verts, sv);
    if (sn) memcpy(blk + off_n, m->norms, sn);
    memcpy(blk + off_f, m->faces, sf);
    uint32_t b = bucket_of(m->id);
    atomic_store_explicit(&nd->next, atomic_load_explicit(&a->mesh[b], memory_order_relaxed), memory_order_relaxed);
    atomic_store_explicit(&a->mesh[b], nd, memory_order_release);
    a->bytes += total; a->count++;
    return 1;
}
/* Versión de color precalculado de (mesh_id) con ese contenido: reutiliza la que coincida, si no crea una nueva. Devuelve la versión (>= 1) o 0 si no cabe. */
static uint32_t store_add_baked(NsMeshStore *s, uint64_t mesh_id, const uint32_t *rgba, uint32_t n, uint64_t chash, int *is_new) {
    Arena *a = atomic_load_explicit(&s->cur, memory_order_relaxed);
    uint32_t b = bucket_of(mesh_id);
    for (BNode *x = atomic_load_explicit(&a->baked[b], memory_order_relaxed); x; x = atomic_load_explicit(&x->next, memory_order_relaxed))
        if (x->b.mesh_id == mesh_id && x->chash == chash && x->b.n_colors == n && !memcmp(x->b.rgba, rgba, (size_t)n * 4u)) { *is_new = 0; return x->b.ver; }
    size_t off = (sizeof(BNode) + 7u) & ~(size_t)7u, total = off + (size_t)n * 4u;
    if (!store_room(s, total)) return 0;
    a = atomic_load_explicit(&s->cur, memory_order_relaxed);
    uint8_t *blk = (uint8_t *)calloc(1, total);
    if (!blk) return 0;
    BNode *nd = (BNode *)blk;
    nd->b.mesh_id = mesh_id; nd->b.ver = ++s->baked_counter; nd->b.n_colors = n; nd->b.rgba = (const uint32_t *)(blk + off); nd->chash = chash;
    memcpy(blk + off, rgba, (size_t)n * 4u);
    atomic_store_explicit(&nd->next, atomic_load_explicit(&a->baked[b], memory_order_relaxed), memory_order_relaxed);
    atomic_store_explicit(&a->baked[b], nd, memory_order_release);
    a->bytes += total;
    *is_new = 1;
    return nd->b.ver;
}

/* ---------------------------------------------------------------------------------------------------------------- memoria (dirección del modelo, de la definición) */
static Memo *memo_find(NsMeshStore *s, uint32_t addr, uint32_t def) {
    if (!addr) return NULL;   /* la dirección 0 no es de un modelo: nunca acierta */
    uint32_t h = (addr >> 2) * 2654435761u ^ (def >> 2) * 40503u;
    for (uint32_t i = 0; i < 32; i++) {
        Memo *m = &s->memo[(h + i) & (MEMO_N - 1u)];
        if (!m->addr) return NULL;
        if (m->addr == addr && m->def == def) return m;
    }
    return NULL;
}
static Memo *memo_put(NsMeshStore *s, uint32_t addr, uint32_t def) {   /* entrada (nueva o existente) o NULL si la tabla está llena en ese tramo */
    if (!addr) return NULL;
    uint32_t h = (addr >> 2) * 2654435761u ^ (def >> 2) * 40503u;
    for (uint32_t i = 0; i < 32; i++) {
        Memo *m = &s->memo[(h + i) & (MEMO_N - 1u)];
        if (!m->addr || (m->addr == addr && m->def == def)) {
            if (!m->addr) { memset(m, 0, sizeof *m); m->addr = addr; m->def = def; }
            return m;
        }
    }
    return NULL;
}
static void memo_clear(NsMeshStore *s) { memset(s->memo, 0, MEMO_N * sizeof(Memo)); }

/* Cambio de ejecutable (ns_hook_set_store_reset; hilo del productor): las direcciones memorizadas dejan de valer. Se vacía la memoria, sube zone_gen y se intenta abrir
 * una época nueva; las mallas ya publicadas NO se liberan aquí (el consumidor puede estar leyéndolas: las libera el protocolo de dos épocas). */
void ns_store_reset(NsMeshStore *s) {
    if (!s) return;
    memo_clear(s);
    s->zone_gen++;
    (void)ns_store_new_epoch(s);
}
/* 0: este es el almacén real del extractor (en la app no hay otra definición de ns_store_*; ver la prueba de enlace del banco). */
int ns_store_is_stub(void) { return 0; }
/* Mallas propias del productor (manos nuevas): se copian a la época actual. El id lo pone quien llama (ns_mesh_content_hash). 1 = añadida, 0 = ya estaba, -1 = no cabe / malla inválida. */
int ns_store_put(NsMeshStore *s, const NsMesh *m) {
    if (!s || !m || !m->verts || !m->faces || !m->n_verts || !m->n_faces || (m->n_norms && !m->norms)) return -1;
    return store_add_mesh(s, m);
}

/* ---------------------------------------------------------------------------------------------------------------- contexto */
typedef struct {
    Ram r;
    NsScene *sc;
    NsMeshStore *st;
    NsExtractStats *s;
    Work *w;
    uint32_t zone_hint;       /* firmas de modelos del escenario que no coinciden con la memoria */
} Ctx;

static const uint8_t WELD_ORDER[4] = {0, 1, 3, 2};   /* esquina -> posición en el paquete; es su propia inversa */

static uint32_t tex_find(Ctx *c, uint16_t id) {
    if (!id) return 0;
    for (uint32_t k = 0; k < 512; k++) {
        uint32_t e = A_TEXTAB + 12u * (((uint32_t)id + k) % 512u);
        uint16_t eid = rd16(&c->r, e);
        if (eid == id) return e;
        if (eid == 0) return 0;
    }
    return 0;
}

static uint64_t sample(Ram *r, uint64_t h, uint32_t ptr, uint32_t len) {
    h = ns_hash64_u32(h, ptr);
    if (len && ok_rng(r, ptr, len)) {
        uint32_t k = len < 16u ? len : 16u;
        h = ns_hash64(h, r->b + (ptr & ADDR_MASK), k);
        h = ns_hash64(h, r->b + (ptr & ADDR_MASK) + (len - k), k);
    }
    return h;
}
/* Tabla pequeña (<= SIG_FULL_BYTES): se firma ENTERA. Las mallas que el juego (o la capa VR) deforma en la RAM sin cambiar de dirección ni de tamaño (la mano del personaje
 * cuando se cierra en puño: 100-128 vértices) conservan idénticos el principio y el final de la tabla; con solo esa muestra la firma no cambiaba, la memoria devolvía la malla
 * abierta y el render nativo no veía nunca el puño. Las tablas grandes (escenario) siguen con la muestra. */
#define SIG_FULL_BYTES 2048u
static uint64_t sample_full(Ram *r, uint64_t h, uint32_t ptr, uint32_t len) {
    if (len && len <= SIG_FULL_BYTES && ok_rng(r, ptr, len)) {
        h = ns_hash64_u32(h, ptr);
        return ns_hash64(h, r->b + (ptr & ADDR_MASK), len);
    }
    return sample(r, h, ptr, len);
}
/* Firma barata del modelo: cabecera, muestra (principio y final) de cada tabla, entradas de textura de la primera y la última cara y, si tiene vértices
 * soldados, la cabecera y los índices del padre. 0 si el modelo no es legible. */
static uint64_t model_sig(Ctx *c, uint32_t m, uint32_t def) {
    Ram *r = &c->r;
    const uint8_t *hd = at(r, m, 0x58);
    if (!hd) return 0;
    uint64_t h = ns_hash64(NS_HASH64_INIT, hd, 0x58);
    int32_t nf = rs32(r, m + 4), nv = rs32(r, m + 0x34), nn = rs32(r, m + 0x40);
    if (nf < 0 || nf > MAX_FACES || nv < 0 || nv > MAX_VERTS || nn < 0 || nn > MAX_NORMS) return h;
    h = sample_full(r, h, rd32(r, m + 0x38), (uint32_t)nv * 8u);
    h = sample(r, h, rd32(r, m + 0x3C), (uint32_t)nf * 4u);
    h = sample_full(r, h, rd32(r, m + 0x44), (uint32_t)nn * 8u);
    h = sample(r, h, rd32(r, m + 0x48), (uint32_t)nf * 4u);
    h = sample(r, h, rd32(r, m + 0x4C), (uint32_t)nf * 8u);
    uint32_t tx = rd32(r, m + 0x50);
    h = sample(r, h, tx, (uint32_t)nf * 2u);
    if (nf > 0 && ok_rng(r, tx, (uint32_t)nf * 2u)) {
        uint32_t ids[2] = {rd16(r, tx), rd16(r, tx + 2u * (uint32_t)(nf - 1))};
        for (int i = 0; i < 2; i++) { uint32_t te = tex_find(c, (uint16_t)ids[i]); if (te && ok_rng(r, te, 12)) h = ns_hash64(h, r->b + (te & ADDR_MASK), 12); }
    }
    if (rd32(r, m) & MF_SHARED_VERTS) {
        int32_t par = rs32(r, m + 0x2C);
        if (par >= 0 && par < MAX_PIECES && ok_rng(r, def, 0x20)) {
            uint32_t pm = def + 0x20u + 0x58u * (uint32_t)par;
            const uint8_t *ph = at(r, pm, 0x58);
            if (ph) {
                h = ns_hash64(h, ph, 0x58);
                int32_t pnf = rs32(r, pm + 4);
                if (pnf > 0 && pnf <= MAX_FACES) h = sample(r, h, rd32(r, pm + 0x3C), (uint32_t)pnf * 4u);
            }
        }
    }
    return h;
}

/* Firma ESTRUCTURAL del modelo (cabecera, tablas de caras/índices/texturas), sin las posiciones de vértices ni las normales: un modelo del escenario cuyos vértices se
 * animan en el sitio (misma dirección, misma estructura) cambia de `model_sig` en cada fotograma pero NO es un cambio de zona. Solo esta firma dispara el cambio de zona. */
static uint64_t model_zsig(Ctx *c, uint32_t m) {
    Ram *r = &c->r;
    const uint8_t *hd = at(r, m, 0x58);
    if (!hd) return 0;
    uint64_t h = ns_hash64(NS_HASH64_INIT, hd, 0x58);
    int32_t nf = rs32(r, m + 4);
    if (nf < 0 || nf > MAX_FACES) return h;
    h = sample(r, h, rd32(r, m + 0x3C), (uint32_t)nf * 4u);
    h = sample(r, h, rd32(r, m + 0x48), (uint32_t)nf * 4u);
    h = sample(r, h, rd32(r, m + 0x50), (uint32_t)nf * 2u);
    return h;
}

/* Sesgo de profundidad derivado del flag del modelo: (4 - bits 12-13) * 250, positivo con 0x100 y negativo con 0x200 (sin ninguno: 0). */
static int16_t bias_of(uint32_t mflag) {
    if (!(mflag & 0x300u)) return 0;
    int v = (4 - (int)((mflag >> 12) & 3u)) * 250;
    return (int16_t)((mflag & 0x100u) ? v : -v);
}

/* Decodifica el modelo y lo guarda. rc: 1 = malla lista (memo rellenado), 0 = sin caras, -1 = inconsistente, -2 = tope del almacén. */
static int decode_model(Ctx *c, uint32_t m, uint32_t def, int want_baked, Memo *me, uint64_t *id_out) {
    Ram *r = &c->r;
    NsExtractStats *S = c->s;
    Work *W = c->w;
    *id_out = 0;
    if (!ok_rng(r, m, 0x58)) { r->oob++; return -1; }
    uint32_t mflag = rd32(r, m);
    int32_t nf = rs32(r, m + 4), nv = rs32(r, m + 0x34), nn = rs32(r, m + 0x40);
    uint32_t pv = rd32(r, m + 0x38), pvi = rd32(r, m + 0x3C), pn = rd32(r, m + 0x44), pni = rd32(r, m + 0x48), puv = rd32(r, m + 0x4C), ptx = rd32(r, m + 0x50);
    int32_t par = rs32(r, m + 0x2C);
    if (nf == 0) return 0;
    if (nf < 0 || nf > MAX_FACES || nv < 1 || nv > MAX_VERTS || nn < 0 || nn > MAX_NORMS) return -1;
    if (!ok_rng(r, pv, (uint32_t)nv * 8u) || !ok_rng(r, pvi, (uint32_t)nf * 4u) || !ok_rng(r, puv, (uint32_t)nf * 8u) || !ok_rng(r, ptx, (uint32_t)nf * 2u)) return -1;
    if (nn > 0 && (!ok_rng(r, pn, (uint32_t)nn * 8u) || !ok_rng(r, pni, (uint32_t)nf * 4u))) return -1;
    const int shared = (mflag & MF_SHARED_VERTS) != 0;
    const uint8_t imask = shared ? 0x7F : 0xFF;   /* sólo los modelos con vértices soldados usan 7 bits; el resto 8 */

    int16_t bb[6] = {32767, 32767, 32767, -32768, -32768, -32768};
    for (int i = 0; i < nv; i++) {
        NsVert *v = &W->v[i];
        v->x = rs16(r, pv + 8u * (uint32_t)i); v->y = rs16(r, pv + 8u * (uint32_t)i + 2); v->z = rs16(r, pv + 8u * (uint32_t)i + 4);
        v->weld = 0xFF; v->pad = 0;
        const int16_t xyz[3] = {v->x, v->y, v->z};
        for (int k = 0; k < 3; k++) {
            if (xyz[k] < bb[k]) bb[k] = xyz[k];
            if (xyz[k] > bb[3 + k]) bb[3 + k] = xyz[k];
        }
    }
    for (int i = 0; i < nn; i++) {
        NsNorm *n = &W->n[i];
        n->x = rs16(r, pn + 8u * (uint32_t)i); n->y = rs16(r, pn + 8u * (uint32_t)i + 2); n->z = rs16(r, pn + 8u * (uint32_t)i + 4); n->pad = 0;
    }
    memset(W->marked, 0, sizeof W->marked);

    for (int i = 0; i < nf; i++) {
        S->faces_seen++;
        uint8_t vi[4], ni[4];
        int bad = 0, badn = 0;
        for (int k = 0; k < 4; k++) {
            uint8_t b = rd8(r, pvi + 4u * (uint32_t)i + (uint32_t)k);
            vi[k] = (uint8_t)(b & imask);
            if (vi[k] >= nv) bad = 1;
        }
        NsFace *f = &W->f[i];
        memset(f, 0, sizeof *f);
        if (bad) { S->faces_bad_vindex++; continue; }   /* cara degenerada con v = 0: conserva la alineación con el color precalculado */
        for (int k = 0; k < 4; k++) {
            if (shared && (rd8(r, pvi + 4u * (uint32_t)i + (uint32_t)k) & 0x80)) W->marked[vi[k]] = 1;
            uint8_t b = nn > 0 ? (uint8_t)(rd8(r, pni + 4u * (uint32_t)i + (uint32_t)k) & imask) : 0;
            if (nn > 0 && b >= nn) { badn = 1; b = 0; }
            ni[k] = b;
        }
        if (badn) S->faces_bad_nindex++;   /* una vez por cara */
        for (int k = 0; k < 4; k++) { f->v[k] = vi[k]; f->n[k] = ni[k]; }
        const int tri = vi[2] == vi[3];
        if (tri) { f->flags |= NS_FF_TRI; S->faces_tri++; } else S->faces_quad++;
        {   /* repeticiones distintas de C==D: cara degenerada (se cuenta; el adaptador la descarta) */
            int rep = (vi[0] == vi[1]) || (vi[0] == vi[2]) || (vi[1] == vi[2]) || (!tri && (vi[0] == vi[3] || vi[1] == vi[3]));
            if (rep) S->faces_degenerate++;
        }
        uint16_t texid = rd16(r, ptx + 2u * (uint32_t)i);
        uint32_t te = tex_find(c, texid);
        if (te) {
            uint8_t ox = rd8(r, te + 8), oy = rd8(r, te + 9), w = rd8(r, te + 10), h = rd8(r, te + 11);
            f->tpage = rd16(r, te + 4); f->clut = rd16(r, te + 6); f->texid = texid;
            for (int k = 0; k < 4; k++) {
                uint32_t u = rd8(r, puv + 8u * (uint32_t)i + 2u * (uint32_t)k), v = rd8(r, puv + 8u * (uint32_t)i + 2u * (uint32_t)k + 1);
                uint32_t uu = ((u * ((uint32_t)w + 1u)) >> 8) + ox, vv = ((v * ((uint32_t)h + 1u)) >> 8) + oy;
                f->u[k] = (uint8_t)(uu > 255u ? 255u : uu); f->t[k] = (uint8_t)(vv > 255u ? 255u : vv);
            }
        } else {
            S->faces_no_tex++;
        }
        if (want_baked) f->flags |= NS_FF_BAKED_OK;
    }

    /* soldaduras: el desplazamiento de `pad` apunta al paquete del padre. Se resuelve a (cara, esquina) del padre y de ahí al vértice del padre. Los encadenados
     * (el padre también soldado) los resuelve el consumidor en orden de jerarquía con la posición ya soldada del padre. */
    int parent_out = -1;
    if (shared && par >= 0 && par < MAX_PIECES && ok_rng(r, def, 0x20)) {
        uint32_t pm = def + 0x20u + 0x58u * (uint32_t)par;
        if (ok_rng(r, pm, 0x58)) {
            int32_t pnf = rs32(r, pm + 4), pnv = rs32(r, pm + 0x34);
            uint32_t ppvi = rd32(r, pm + 0x3C), pflag = rd32(r, pm);
            int pok = pnf > 0 && pnf <= MAX_FACES && pnv > 0 && pnv <= MAX_VERTS && ok_rng(r, ppvi, (uint32_t)pnf * 4u);
            for (int i = 0; i < nv; i++) {
                if (!W->marked[i]) continue;
                uint16_t pad = rd16(r, pv + 8u * (uint32_t)i + 6);
                int done = 0;
                if (pok && pad != 0xFFFFu && pad >= 8) {
                    uint32_t off = (uint32_t)pad - 8u, face = off / 52u, rem = off % 52u;
                    if (rem < 48u && rem % 12u == 0 && face < (uint32_t)pnf) {
                        uint8_t b = rd8(r, ppvi + 4u * face + WELD_ORDER[rem / 12u]);
                        uint8_t pvx = (uint8_t)(b & ((pflag & MF_SHARED_VERTS) ? 0x7F : 0xFF));
                        if (pvx < pnv) { W->v[i].weld = pvx; S->welded_verts++; done = 1; }
                    }
                }
                if (!done) S->weld_bad++;
            }
            parent_out = (int)par;
        }
    } else if (shared) {
        for (int i = 0; i < nv; i++) if (W->marked[i]) S->weld_bad++;
    }

    NsMesh mesh;
    memset(&mesh, 0, sizeof mesh);
    mesh.model_flags = mflag;
    if (mflag & MF_TRANSLUCENT) mesh.flags |= NS_MF_BLEND;
    if (mflag & MF_UNLIT) mesh.flags |= NS_MF_UNLIT;
    if (mflag & MF_TWO_SIDED) mesh.flags |= NS_MF_TWOSIDED;
    if (shared) mesh.flags |= NS_MF_WELDED;
    if (want_baked) mesh.flags |= NS_MF_BAKED;
    mesh.depth_bias = bias_of(mflag);
    if (mesh.depth_bias > 0) mesh.flags |= NS_MF_BIAS_UP;
    mesh.n_verts = (uint16_t)nv; mesh.n_norms = (uint16_t)nn; mesh.n_faces = (uint16_t)nf;
    memcpy(mesh.bbox, bb, sizeof bb);
    mesh.verts = W->v; mesh.norms = W->n; mesh.faces = W->f;
    mesh.id = ns_mesh_content_hash(&mesh);
    int a = store_add_mesh(c->st, &mesh);
    if (a < 0) { S->store_full++; return -2; }
    if (a) S->mesh_new++; else S->mesh_ref++;
    *id_out = mesh.id;
    me->parent = (int16_t)parent_out; me->bias = mesh.depth_bias; me->n_faces = (uint16_t)nf;
    return 1;
}

/* ---------------------------------------------------------------------------------------------------------------- luces */
static void read_light(Ctx *c, uint32_t lp, uint32_t oflag, NsLight *L) {
    Ram *r = &c->r;
    memset(L, 0, sizeof *L);
    if (!ok_rng(r, lp, 64)) lp = A_LIGHT_G;
    for (int l = 0; l < 3; l++) for (int k = 0; k < 3; k++) {
        L->dir[l][k] = rs16(r, lp + 6u * (uint32_t)l + 2u * (uint32_t)k);
        L->col[l][k] = rs16(r, lp + 32u + 6u * (uint32_t)l + 2u * (uint32_t)k);
    }
    for (int k = 0; k < 3; k++) {
        int32_t a = (oflag & OF_OWN_AMBIENT) ? rs32(r, lp + 0x14u + 4u * (uint32_t)k) : (int32_t)rs16(r, A_AMBIENT_G + 2u * (uint32_t)k);
        L->amb[k] = (int16_t)(a < 0 ? 0 : a > 255 ? 255 : a);
    }
    int dyn = 0;
    for (int l = 1; l < 3; l++) for (int k = 0; k < 3; k++) if (L->dir[l][k]) dyn = 1;
    L->flags = (uint16_t)((dyn ? 1u : 0u) | ((oflag & OF_OWN_AMBIENT) ? 2u : 0u));
}
static int light_index(Ctx *c, const NsLight *L) {
    NsScene *sc = c->sc;
    for (uint32_t i = 0; i < sc->n_light; i++) if (!memcmp(&sc->lights[i], L, sizeof *L)) return (int)i;
    if (sc->n_light >= sc->cap_light) { sc->truncated |= NS_TR_LIGHT; return -1; }
    sc->lights[sc->n_light] = *L;
    c->s->n_lights++;
    return (int)sc->n_light++;
}

/* ---------------------------------------------------------------------------------------------------------------- piezas */
enum { RES_OK = 0, RES_HIDDEN = 1, RES_FULL = 2, RES_BAD = 3 /* modelo ilegible/roto: la geometría FALTA (no es una pieza oculta por el juego) */ };

/* Relee el color precalculado de la pieza y devuelve su versión (0 si no hay). */
static uint32_t baked_version(Ctx *c, Memo *me, uint64_t id, uint32_t rgbs) {
    Ram *r = &c->r;
    uint32_t n = me->n_faces;
    if (!n || !ok_rng(r, rgbs, n * 16u)) return 0;
    uint32_t *col = c->w->col;
    const uint8_t *p = r->b + (rgbs & ADDR_MASK);
    uint64_t h = NS_HASH64_INIT;
    for (uint32_t i = 0; i < n * 4u; i++) {
        col[i] = (uint32_t)p[4u * i] | ((uint32_t)p[4u * i + 1] << 8) | ((uint32_t)p[4u * i + 2] << 16) | 0xFF000000u;
        h = ns_hash64_u32(h, col[i]);
    }
    if (me->baked_ver && me->baked_hash == h) { c->s->baked_ref++; return me->baked_ver; }
    int is_new = 0;
    uint32_t v = store_add_baked(c->st, id, col, n * 4u, h, &is_new);
    if (!v) { c->s->store_full++; return 0; }
    if (is_new) c->s->baked_new++; else c->s->baked_ref++;
    me->baked_ver = v; me->baked_hash = h;
    return v;
}

/* Resuelve la malla de una pieza (memoria + firma + decodificación) y rellena mesh_id, parent y baked_ver. */
static int resolve_piece(Ctx *c, uint32_t model, uint32_t def, uint32_t rgbs, uint32_t oflag, int nm_base, int16_t bias_field, int check_bias, int is_static, NsPiece *p) {
    NsExtractStats *S = c->s;
    NsMeshStore *st = c->st;
    p->mesh_id = 0; p->parent = -1; p->baked_ver = 0;
    if (!model) { S->null_model++; return RES_HIDDEN; }
    uint32_t mn = norm_addr(model), dn = norm_addr(def);
    if (!ok_rng(&c->r, model, 0x58) || !mn) { c->r.oob++; S->drop_bad_model++; return RES_BAD; }
    const int want_baked = (oflag & OF_PRECOLORED) && rgbs && ok_rng(&c->r, rgbs, 16);
    uint64_t sig = model_sig(c, model, def);
    Memo *me = memo_find(st, mn, dn);
    if (me && me->sig != sig) { S->memo_stale++; if (is_static) c->zone_hint++; me = NULL; }   /* el contenido cambió en la misma dirección: recodificar */
    uint64_t id = 0;
    if (me) {
        S->mesh_ref++;
        if (me->empty) return RES_HIDDEN;
        id = me->id;
    } else {
        Memo tmp; memset(&tmp, 0, sizeof tmp);
        Memo *slot = memo_put(st, mn, dn);
        Memo *use = slot ? slot : &tmp;
        use->baked_ver = 0; use->baked_hash = 0;
        int rc = decode_model(c, model, def, want_baked, use, &id);
        if (rc == 0) { if (slot) { slot->sig = sig; slot->zsig = model_zsig(c, model); slot->empty = 1; slot->id = 0; } else S->memo_full++; if (slot) { /* queda memorizado como vacío */ } return RES_HIDDEN; }
        if (rc < 0) { if (slot) { memset(slot, 0, sizeof *slot); } if (rc == -1) S->drop_bad_model++; return rc == -2 ? RES_FULL : rc == -1 ? RES_BAD : RES_HIDDEN; }
        use->sig = sig; use->zsig = model_zsig(c, model); use->id = id; use->empty = 0;
        if (!slot) S->memo_full++;
        me = use;
    }
    p->mesh_id = id;
    if (me->parent >= 0 && me->parent < nm_base) p->parent = me->parent;
    if (check_bias && me->bias != bias_field) S->bias_mismatch++;
    if (want_baked) p->baked_ver = baked_version(c, me, id, rgbs);
    return RES_OK;
}

typedef struct { uint32_t o; uint32_t flag; uint16_t gid; int nm; uint32_t def; int is_static; } Cand;

static int shadow_like(Ctx *c, const Cand *cd) {   /* objeto de una pieza con una sola cara plana, translúcido e iluminado */
    Ram *r = &c->r;
    if (cd->nm != 1 || !(cd->flag & OF_TRANSLUCENT) || !(cd->flag & OF_LIT)) return 0;
    uint32_t m = rd32(r, cd->o + 0x48u + 0x40u);
    if (!ok_rng(r, m, 0x58) || rs32(r, m + 4) != 1) return 0;
    int32_t nv = rs32(r, m + 0x34);
    uint32_t pv = rd32(r, m + 0x38);
    if (nv < 3 || nv > 4 || !ok_rng(r, pv, (uint32_t)nv * 8u)) return 0;
    int16_t y0 = rs16(r, pv + 2);
    for (int i = 1; i < nv; i++) if (rs16(r, pv + 8u * (uint32_t)i + 2) != y0) return 0;
    return 1;
}

/* Emite una instancia. Devuelve 0 si se emitió, 1 si se descartó (capacidad o tope del almacén; sólo dinámicas). */
static int emit_object(Ctx *c, const Cand *cd, uint32_t piece_cap) {
    Ram *r = &c->r;
    NsScene *sc = c->sc;
    NsExtractStats *S = c->s;
    const uint32_t o = cd->o, flag = cd->flag;
    const int nm = cd->nm;
    if (sc->n_inst >= sc->cap_inst) { sc->truncated |= NS_TR_INST; if (cd->is_static) sc->valid |= NS_SV_MAP_INCOMPLETE; sc->n_dropped_inst++; sc->n_dropped_piece += (uint32_t)nm; return 1; }
    NsInstance *in = &sc->inst[sc->n_inst];
    const uint32_t first = sc->n_piece, light_before = sc->n_light;
    memset(in, 0, sizeof *in);
    in->key = 0x80000000u | norm_addr(o); in->first_piece = first; in->zone_mask = cd->gid;
    in->layer = rs16(r, o + 0x30); in->vis_state = rs16(r, o + 0x32); in->light = -1;
    if (flag & OF_LIT) in->flags |= NS_IF_LIT;
    if (flag & OF_OWN_AMBIENT) in->flags |= NS_IF_OWN_AMBIENT;
    if (flag & OF_TRANSLUCENT) in->flags |= NS_IF_BLEND;
    if (cd->is_static) in->flags |= NS_IF_STATIC;
    if (flag & OF_ALT_PALETTE) in->flags |= NS_IF_THERMAL_OK;
    if (flag & OF_ONE_MATRIX) in->flags |= NS_IF_RIGID;
    if (flag & OF_GBOUND) in->flags |= NS_IF_GBOUND;
    if (flag & OF_ENV_REFLECT) in->flags |= NS_IF_ENVREFL;
    if (flag & OF_LIT) { NsLight L; read_light(c, rd32(r, o + 0x34), flag, &L); in->light = (int16_t)light_index(c, &L); }

    int dropped = 0, full = 0;
    /* piezas dibujables, en orden (los índices de pieza que usan los padres) */
    for (int k = 0; k < nm && !dropped; k++) {
        uint32_t p = o + 0x48u + 0x5Cu * (uint32_t)k;
        if (sc->n_piece >= piece_cap) { dropped = 1; break; }
        NsPiece *pc = &sc->piece[sc->n_piece];
        memset(pc, 0, sizeof *pc);
        read_matrix(r, (flag & OF_ONE_MATRIX) ? o : p, &pc->world);   /* M_RIGIDO: la matriz de la raíz (docs §3.3; la pieza guarda un residuo, ver OF_ONE_MATRIX) */
        int rr = resolve_piece(c, rd32(r, p + 0x40), cd->def, rd32(r, p + 0x44), flag, nm, rs16(r, p + 0x50), 1, cd->is_static, pc);
        if (rr == RES_FULL) full = 1;
        if (rr != RES_OK) { pc->flags |= NS_PF_HIDDEN; S->n_hidden_pieces++; }
        else if (!cd->is_static && rs16(r, p + 0x50) <= PIECE_BIAS_HIDDEN) { pc->flags |= NS_PF_HIDDEN; S->n_hidden_pieces++; }   /* pieza retirada por el motor (ver PIECE_BIAS_HIDDEN) */
        if (rr == RES_BAD && cd->is_static) sc->valid |= NS_SV_MAP_INCOMPLETE;   /* geometría del escenario ausente: la escena no es apta (se pinta por GPU) */
        if (rr == RES_BAD && !cd->is_static) S->bad_dyn_pieces++;   /* geometría dinámica ausente: la escena sigue siendo apta, pero se cuenta (registro) */
        sc->n_piece++; S->n_pieces++;
    }
    /* extensiones: otra malla que comparte la matriz de la pieza que la enlaza; viven en la parte «extra» del array (índice >= nm) */
    uint32_t nx = MAX_PIECES;
    if (ok_rng(r, cd->def, 8)) { int32_t v = rs32(r, cd->def + 4); if (v > 0 && v < MAX_PIECES) nx = (uint32_t)v; }
    for (int k = 0; k < nm && !dropped; k++) {
        uint32_t p = o + 0x48u + 0x5Cu * (uint32_t)k;
        uint32_t e = rd32(r, p + 0x48);
        if (!e) continue;
        NsXf w; read_matrix(r, (flag & OF_ONE_MATRIX) ? o : p, &w);
        const uint32_t base = norm_addr(o) + 0x48u;
        uint32_t seen[MAX_EXT_DEPTH + 1];
        for (int depth = 0; e && depth < MAX_EXT_DEPTH; depth++) {
            uint32_t en = norm_addr(e);
            uint32_t idx = en >= base ? (en - base) / 0x5Cu : 0;
            int bad = en < base || (en - base) % 0x5Cu != 0 || idx < (uint32_t)nm || idx >= nx || !ok_rng(r, e, 0x5C);
            for (int j = 0; j < depth && !bad; j++) if (seen[j] == en) bad = 1;
            if (bad) { S->ext_bad++; break; }
            seen[depth] = en;
            if (sc->n_piece >= piece_cap) { dropped = 1; break; }
            NsPiece *pc = &sc->piece[sc->n_piece];
            memset(pc, 0, sizeof *pc);
            pc->world = w;
            int rr = resolve_piece(c, rd32(r, e + 0x40), cd->def, rd32(r, e + 0x44), flag, nm, rs16(r, e + 0x50), 1, cd->is_static, pc);
            if (rr == RES_FULL) full = 1;
            if (rr != RES_OK) { pc->flags |= NS_PF_HIDDEN; S->n_hidden_pieces++; }
            else if (!cd->is_static && rs16(r, e + 0x50) <= PIECE_BIAS_HIDDEN) { pc->flags |= NS_PF_HIDDEN; S->n_hidden_pieces++; }
            if (rr == RES_BAD && cd->is_static) sc->valid |= NS_SV_MAP_INCOMPLETE;   /* geometría del escenario ausente: la escena no es apta (se pinta por GPU) */
            if (rr == RES_BAD && !cd->is_static) S->bad_dyn_pieces++;
            sc->n_piece++; S->n_pieces++; S->n_ext_pieces++;
            e = rd32(r, e + 0x48);
            if (e && depth == MAX_EXT_DEPTH - 1) S->ext_bad++;   /* cadena demasiado profunda */
        }
    }
    if (dropped || (full && !cd->is_static)) {   /* las dinámicas se descartan enteras; el escenario nunca */
        if (cd->is_static) {
            sc->truncated |= NS_TR_PIECE; sc->valid |= NS_SV_MAP_INCOMPLETE; sc->n_dropped_piece++;
        } else {
            if (dropped) sc->truncated |= NS_TR_PIECE;
            if (full) sc->valid |= NS_SV_STORE_FULL;
            S->n_pieces -= sc->n_piece - first;
            sc->n_piece = first; sc->n_light = light_before;
            sc->n_dropped_inst++; sc->n_dropped_piece += (uint32_t)nm;
            return 1;
        }
    }
    if (full && cd->is_static) { sc->valid |= NS_SV_MAP_INCOMPLETE | NS_SV_STORE_FULL; }
    in->n_pieces = (uint16_t)(sc->n_piece - first);
    sc->n_inst++;
    S->n_inst++;
    if (cd->is_static) S->n_static++;
    return 0;
}

/* ---------------------------------------------------------------------------------------------------------------- escena */
static int scene_alloc(NsScene *sc, uint32_t ci, uint32_t cp, uint32_t cl, uint32_t ce) {
    memset(sc, 0, sizeof *sc);
    sc->size = (uint32_t)sizeof *sc; sc->version = NS_VERSION;
    sc->cap_inst = ci; sc->cap_piece = cp; sc->cap_light = cl; sc->cap_effect = ce;
    sc->inst = (NsInstance *)calloc(ci ? ci : 1, sizeof(NsInstance));
    sc->piece = (NsPiece *)calloc(cp ? cp : 1, sizeof(NsPiece));
    sc->lights = (NsLight *)calloc(cl ? cl : 1, sizeof(NsLight));
    sc->effects = (NsEffect *)calloc(ce ? ce : 1, sizeof(NsEffect));
    if (!sc->inst || !sc->piece || !sc->lights || !sc->effects) { ns_scene_free(sc); return -1; }
    return 0;
}
int ns_scene_alloc_exact(NsScene *sc, uint32_t ci, uint32_t cp, uint32_t cl, uint32_t ce) { return scene_alloc(sc, ci, cp, cl, ce); }
int ns_scene_alloc(NsScene *sc, uint32_t ci, uint32_t cp, uint32_t cl, uint32_t ce) {
    return scene_alloc(sc, ci < NS_MIN_INST ? NS_MIN_INST : ci, cp < NS_MIN_PIECE ? NS_MIN_PIECE : cp, cl < NS_MIN_LIGHT ? NS_MIN_LIGHT : cl, ce < NS_MIN_EFFECT ? NS_MIN_EFFECT : ce);
}
void ns_scene_free(NsScene *sc) {
    free(sc->inst); free(sc->piece); free(sc->lights); free(sc->effects);
    memset(sc, 0, sizeof *sc);
}

int ns_extract(NsScene *sc, NsMeshStore *st, const uint8_t *ram, size_t ram_size, const uint8_t *scratch, size_t scratch_size, NsExtractStats *stats) {
    return ns_extract_ex(sc, st, ram, ram_size, scratch, scratch_size, NULL, stats);
}

static int zone_changed(NsMeshStore *st, const uint32_t *defs, uint32_t n) {
    int changed = 0;
    if (st->n_prev_defs && n) {
        int overlap = 0;
        for (uint32_t i = 0; i < n && !overlap; i++) for (uint32_t j = 0; j < st->n_prev_defs; j++) if (defs[i] == st->prev_defs[j]) { overlap = 1; break; }
        changed = !overlap;
    }
    st->n_prev_defs = n;
    if (n) memcpy(st->prev_defs, defs, n * sizeof *defs);
    return changed;
}
static void do_zone_change(NsMeshStore *st, NsExtractStats *S) {
    st->zone_gen++;
    S->zone_changes++;
    memo_clear(st);
    if (ns_store_new_epoch(st)) S->epoch_opened = 1;
}


/* Factor vertical (66/64 con la cámara del propio juego, 1 con la de la capa VR) con el que el motor compuso las matrices de PANTALLA (pieza +0x20) que alimentan la GTE, y por tanto las posiciones de vista de la
 * lista de la GPU. Se mide en la RAM: para las primeras piezas de los objetos de la cola, razón entre la norma de la fila Y de la matriz de pantalla y la de (ojo_inv * mundo), con la misma matriz de mundo que
 * usa el extractor. Solo vale la muestra si las filas X y Z coinciden con las de ojo_inv * mundo (si no, esa pieza no se compuso así). Mediana; 0 = sin muestras válidas (desconocido). */
float ns_extract_screen_ys(const uint8_t *ram, size_t ram_size) {
    if (!ram) return 0.0f;
    Ram r; memset(&r, 0, sizeof r);
    r.b = ram; r.size = ram_size > NS_RAM_SIZE ? NS_RAM_SIZE : (uint32_t)ram_size;
    if (!ok_rng(&r, A_CHAN, 0x60)) return 0.0f;
    float E[9];
    for (int i = 0; i < 9; i++) E[i] = (float)rs16(&r, A_CHAN + 0x10u + 2u * (uint32_t)i) * (1.0f / 4096.0f);
    int n = rs16(&r, A_CHAN + 0x56);
    uint32_t q = rd32(&r, A_CHAN + 0x58);
    if (n <= 0) return 0.0f;
    if (n > MAX_QUEUE) n = MAX_QUEUE;
    if (!ok_rng(&r, q, 4u * (uint32_t)n)) return 0.0f;
    float v[64]; int nv = 0;
    for (int i = 0; i < n && nv < 64; i++) {
        uint32_t o = rd32(&r, q + 4u * (uint32_t)i);
        if (!o || !ok_rng(&r, o, 0x48)) continue;
        uint32_t flag = rd32(&r, o + 0x28);
        int nm = rs16(&r, o + 0x2E);
        if (nm <= 0 || nm > MAX_PIECES || !ok_rng(&r, o + 0x48u, 0x5Cu * (uint32_t)nm)) continue;
        for (int k = 0; k < nm && k < 4 && nv < 64; k++) {
            uint32_t p = o + 0x48u + 0x5Cu * (uint32_t)k;
            NsXf w, pw;
            read_matrix(&r, (flag & OF_ONE_MATRIX) ? o : p, &w);
            read_matrix(&r, p + 0x20u, &pw);
            float c[3][3], pr[3][3];
            for (int a = 0; a < 3; a++) for (int b = 0; b < 3; b++) {
                c[a][b] = E[3 * a] * w.m[b] + E[3 * a + 1] * w.m[4 + b] + E[3 * a + 2] * w.m[8 + b];
                pr[a][b] = pw.m[4 * a + b];
            }
            float nc[3], np[3];
            for (int a = 0; a < 3; a++) {
                nc[a] = sqrtf(c[a][0] * c[a][0] + c[a][1] * c[a][1] + c[a][2] * c[a][2]);
                np[a] = sqrtf(pr[a][0] * pr[a][0] + pr[a][1] * pr[a][1] + pr[a][2] * pr[a][2]);
            }
            if (nc[1] < 0.25f || np[1] < 0.25f || nc[0] < 0.25f || nc[2] < 0.25f) continue;
            if (fabsf(np[0] / nc[0] - 1.0f) > 0.01f || fabsf(np[2] / nc[2] - 1.0f) > 0.01f) continue;
            float ys = np[1] / nc[1];
            if (ys < 0.9f || ys > 1.2f) continue;
            v[nv++] = ys;
        }
    }
    if (!nv) return 0.0f;
    for (int i = 1; i < nv; i++) { float x = v[i]; int j = i - 1; while (j >= 0 && v[j] > x) { v[j + 1] = v[j]; j--; } v[j + 1] = x; }
    return v[nv / 2];
}

int ns_extract_ex(NsScene *sc, NsMeshStore *st, const uint8_t *ram, size_t ram_size, const uint8_t *scratch, size_t scratch_size, const NsExtractOpts *opt, NsExtractStats *stats) {
    (void)scratch; (void)scratch_size;
    NsExtractStats dummy;
    NsExtractStats *S = stats ? stats : &dummy;
    memset(S, 0, sizeof *S);
    sc->size = (uint32_t)sizeof *sc; sc->version = NS_VERSION;
    sc->seq++;
    sc->n_inst = sc->n_piece = sc->n_light = sc->n_effect = 0;
    sc->valid = 0; sc->truncated = 0; sc->n_dropped_inst = sc->n_dropped_piece = sc->n_dropped_effect = sc->n_shadow_omitted = 0;
    sc->units_per_m = 1000.0f;
    memset(sc->to_vr, 0, sizeof sc->to_vr);
    sc->to_vr[0] = sc->to_vr[5] = sc->to_vr[10] = 1.0f / sc->units_per_m;   /* escala nominal; el origen y el espejo los fija la capa VR (docs §2) */
    memset(&sc->cam, 0, sizeof sc->cam); memset(&sc->fog, 0, sizeof sc->fog);
    sc->bg_rgb[0] = sc->bg_rgb[1] = sc->bg_rgb[2] = 0;
    memset(&sc->global_light, 0, sizeof sc->global_light);
    memset(&sc->screen, 0, sizeof sc->screen);
    if (!ram || !st) return -1;
    sc->mesh_epoch = ns_store_epoch(st); sc->zone_gen = st->zone_gen;

    Ctx c;
    memset(&c, 0, sizeof c);
    c.r.b = ram; c.r.size = ram_size > NS_RAM_SIZE ? NS_RAM_SIZE : (uint32_t)ram_size;
    c.sc = sc; c.st = st; c.s = S; c.w = st->w;
    Ram *r = &c.r;
    int ret = -1;
    if (!ok_rng(r, A_CHAN, 0x60)) goto done;
    {
        /* cámara del juego: ojo (vista->mundo) en +0x30, H en +0x50 */
        NsXf eye;
        if (read_matrix(r, A_CHAN + 0x30u, &eye)) {
            for (int i = 0; i < 3; i++) {
                sc->cam.rot[3 * i + 0] = eye.m[4 * i + 0]; sc->cam.rot[3 * i + 1] = eye.m[4 * i + 1]; sc->cam.rot[3 * i + 2] = eye.m[4 * i + 2];
                sc->cam.eye[i] = eye.m[4 * i + 3];
            }
        }
        sc->cam.h = (float)rs16(r, A_CHAN + 0x50);
        sc->cam.overscan_y = NS_OVERSCAN_Y;
        sc->cam.vr_active = 0;
        sc->clock = rd32(r, A_CLOCK) & 1u;
        uint32_t gmask = rd32(r, A_GROUP);
        sc->group_mask = gmask;
        {   /* color de fondo: orden de relleno del entorno de dibujo del canal de fondo (código 0x60 + RGB) */
            uint32_t w = rd32(r, A_BGCHAN + 0x88u);
            if ((w >> 24) == 0x60u) { sc->bg_rgb[0] = (float)(w & 255u); sc->bg_rgb[1] = (float)((w >> 8) & 255u); sc->bg_rgb[2] = (float)((w >> 16) & 255u); }
        }
        NsLight gl; read_light(&c, A_LIGHT_G, 0, &gl);
        sc->global_light = gl;

        int n = rs16(r, A_CHAN + 0x56);
        uint32_t q = rd32(r, A_CHAN + 0x58);
        if (n < 0) { n = 0; S->q_clamped = 1; }
        if (n > MAX_QUEUE) { n = MAX_QUEUE; S->q_clamped = 1; }
        if (n > 0 && !ok_rng(r, q, 4u * (uint32_t)n)) { S->drop_badptr++; n = 0; }
        S->queue_n = (uint32_t)n;
        ret = 0;

        /* clasificación de la cola */
        Cand cand[MAX_QUEUE];
        uint32_t ncand = 0;
        uint32_t seen[MAX_QUEUE], nseen = 0;
        uint32_t sdefs[MAX_STATIC_DEFS], nsdefs = 0;
        for (int i = 0; i < n; i++) {
            uint32_t o = rd32(r, q + 4u * (uint32_t)i);
            if (!o) { S->drop_null++; continue; }
            if (!ok_rng(r, o, 0x48)) { S->drop_badptr++; continue; }
            uint32_t on = norm_addr(o);
            int dup = 0;
            for (uint32_t k = 0; k < nseen; k++) if (seen[k] == on) { dup = 1; break; }
            if (dup) { S->drop_dup++; continue; }
            seen[nseen++] = on;
            Cand cd;
            cd.o = o; cd.flag = rd32(r, o + 0x28); cd.gid = rd16(r, o + 0x2C); cd.nm = rs16(r, o + 0x2E); cd.def = rd32(r, o + 0x24);
            cd.is_static = (cd.flag & OF_PRECOLORED) && !(cd.flag & OF_LIT);
            if ((cd.flag & OF_HIDDEN) && !(opt && opt->player_key && norm_addr(opt->player_key) == on)) { S->drop_hidden_flag++; continue; }   /* el jugador oculto por el motor sí se extrae */
            if (cd.gid != 0 && !(cd.gid & gmask)) {   /* grupo de mapa no activo: los objetos dinámicos no se dibujan (libdg/bound.c); el escenario (mapas cargados del stage) se extrae igual, marcado con su grupo (NsInstance.zone_mask) */
                if (!cd.is_static) { S->drop_group++; continue; }
                S->static_off_group++;
            }
            if (cd.nm <= 0 || cd.nm > MAX_PIECES || !ok_rng(r, o + 0x48u, 0x5Cu * (uint32_t)cd.nm)) { S->drop_badobj++; continue; }
            int is_shadow = 0;
            if (opt && opt->shadow_objs) for (uint32_t k = 0; k < opt->n_shadow && !is_shadow; k++) is_shadow = norm_addr(opt->shadow_objs[k]) == on;
            if (!is_shadow && opt && opt->detect_shadows && !cd.is_static) is_shadow = shadow_like(&c, &cd);
            if (is_shadow) { sc->n_shadow_omitted++; S->shadow_omitted++; continue; }
            if (cd.is_static) {
                uint32_t dn = norm_addr(cd.def), k;
                for (k = 0; k < nsdefs; k++) if (sdefs[k] == dn) break;
                if (k == nsdefs && nsdefs < MAX_STATIC_DEFS) sdefs[nsdefs++] = dn;
            }
            cand[ncand++] = cd;
        }

        /* cambio de zona: sustitución total de las definiciones del escenario o firmas distintas en modelos conocidos del escenario */
        {
            int changed = zone_changed(st, sdefs, nsdefs);
            for (uint32_t i = 0; i < ncand && !changed; i++) {
                if (!cand[i].is_static) continue;
                for (int k = 0; k < cand[i].nm && !changed; k++) {
                    uint32_t m = rd32(r, cand[i].o + 0x48u + 0x5Cu * (uint32_t)k + 0x40u);
                    Memo *me = memo_find(st, norm_addr(m), norm_addr(cand[i].def));
                    if (me && !me->empty && me->zsig != model_zsig(&c, m)) changed = 1;
                }
            }
            if (changed) do_zone_change(st, S);
            else {   /* compactación: los modelos con vértices animados generan una malla nueva por fotograma; al pasar de 1/4 del tope (mín. 8 MiB) se abre época y se re-extrae solo lo que se usa */
                size_t th = st->compact;
                if (!th) { th = st->limit / 4u; if (th < ((size_t)8u << 20)) th = (size_t)8u << 20; }
                if (ns_store_bytes(st) > th && ns_store_new_epoch(st)) { memo_clear(st); S->epoch_opened = 1; S->compactions++; }
            }
            sc->mesh_epoch = ns_store_epoch(st); sc->zone_gen = st->zone_gen;
        }

        uint32_t piece_cap = sc->cap_piece;
        if (opt && opt->max_pieces && opt->max_pieces < piece_cap) piece_cap = opt->max_pieces;
        for (int pass = 0; pass < 2; pass++)   /* el escenario primero, luego el resto por orden de cola */
            for (uint32_t i = 0; i < ncand; i++) if ((pass == 0) == (cand[i].is_static != 0)) emit_object(&c, &cand[i], piece_cap);
        sc->valid |= (sc->n_inst ? NS_SV_GAME : 0u) | ((sc->truncated || sc->n_dropped_inst || sc->n_dropped_piece) ? 0u : NS_SV_LISTS_OK);
        if (S->n_static) sc->valid |= NS_SV_MAP_OK;
    }
done:
    S->oob_reads = r->oob;
    return ret;
}
