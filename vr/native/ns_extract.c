/* ns_extract: extractor de la escena nativa (ver ns_extract.h y docs/escena_nativa.md). */
#include "ns_extract.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------------------------------------------- direcciones del motor */
#define A_CHAN       0x800B2B7Cu   /* canal principal */
#define A_TEXTAB     0x800B30E0u   /* tabla de texturas: 512 x 12 B */
#define A_GROUP      0x800ACAF8u   /* grupo activo (máscara) */
#define A_CLOCK      0x800ACAB0u   /* reloj de búfer */
#define A_LIGHT_G    0x8009E55Cu   /* luz global: dos matrices de 32 B consecutivas */
#define A_AMBIENT_G  0x800AC530u   /* ambiente global: 3 x s16 */

#define MAX_QUEUE    256
#define MAX_PIECES   1024          /* por objeto compuesto (el mayor observado: 181) */
#define MAX_FACES    8192          /* por modelo */
#define MAX_VERTS    256
#define MAX_NORMS    256
#define MAX_EXT_DEPTH 4

/* flags del objeto compuesto */
#define OF_TEXTURED 0x001u
#define OF_PRECOLORED 0x002u
#define OF_TRANSLUCENT 0x004u
#define OF_LIT 0x008u
#define OF_ONE_MATRIX 0x040u
#define OF_HIDDEN 0x080u
#define OF_OWN_AMBIENT 0x100u
#define OF_ALT_PALETTE 0x200u
#define OF_ENV_REFLECT 0x400u
/* flags del modelo */
#define MF_TRANSLUCENT 0x2u
#define MF_UNLIT 0x4u
#define MF_TWO_SIDED 0x400u
#define MF_SHARED_VERTS 0x10000u

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
    uint32_t o = a & 0x1FFFFFFFu;
    return o < r->size && n <= r->size - o;
}
static const uint8_t *at(Ram *r, uint32_t a, uint32_t n) {
    if (!ok_rng(r, a, n)) { r->oob++; return NULL; }
    return r->b + (a & 0x1FFFFFFFu);
}
static uint32_t rd32(Ram *r, uint32_t a) { const uint8_t *p = at(r, a, 4); uint32_t v = 0; if (p) memcpy(&v, p, 4); return v; }
static uint16_t rd16(Ram *r, uint32_t a) { const uint8_t *p = at(r, a, 2); uint16_t v = 0; if (p) memcpy(&v, p, 2); return v; }
static uint8_t rd8(Ram *r, uint32_t a) { const uint8_t *p = at(r, a, 1); return p ? *p : 0; }
static int16_t rs16(Ram *r, uint32_t a) { return (int16_t)rd16(r, a); }
static int32_t rs32(Ram *r, uint32_t a) { return (int32_t)rd32(r, a); }

/* matriz del motor (32 B): rotación s16[9] en +0, traslación s32[3] en +0x14 */
static int read_matrix(Ram *r, uint32_t a, NsXf *o) {
    if (!ok_rng(r, a, 0x20)) { r->oob++; return 0; }
    int16_t m[9]; int32_t t[3];
    for (int i = 0; i < 9; i++) m[i] = rs16(r, a + 2u * (uint32_t)i);
    for (int i = 0; i < 3; i++) t[i] = rs32(r, a + 0x14u + 4u * (uint32_t)i);
    ns_xf_from_engine(o, m, t);
    return 1;
}

/* ---------------------------------------------------------------------------------------------------------------- almacén de mallas */
typedef struct Node {
    _Atomic(struct Node *) next;
    NsMesh mesh;           /* los arrays van a continuación en el mismo bloque */
} Node;

#define STORE_BUCKETS 16384u
typedef struct { uint32_t addr; uint64_t id; int16_t parent; } Memo;
#define MEMO_SIZE 32768u
struct NsMeshStore {
    Memo *memo;            /* dirección del modelo -> malla ya decodificada en esta generación (solo productor) */
    _Atomic(Node *) *bucket;
    _Atomic uint32_t epoch;
    uint32_t count;
};

static uint32_t id_bucket(uint64_t id) { return (uint32_t)((id * 0x9E3779B97F4A7C15ull) >> 40) & (STORE_BUCKETS - 1u); }

NsMeshStore *ns_store_create(void) {
    NsMeshStore *s = (NsMeshStore *)calloc(1, sizeof *s);
    if (!s) return NULL;
    s->bucket = (_Atomic(Node *) *)calloc(STORE_BUCKETS, sizeof(_Atomic(Node *)));
    s->memo = (Memo *)calloc(MEMO_SIZE, sizeof(Memo));
    if (!s->bucket || !s->memo) { free(s->bucket); free(s->memo); free(s); return NULL; }
    atomic_store(&s->epoch, 1u);
    return s;
}
static void store_free_nodes(NsMeshStore *s) {
    for (uint32_t i = 0; i < STORE_BUCKETS; i++) {
        Node *n = atomic_load_explicit(&s->bucket[i], memory_order_relaxed);
        while (n) { Node *nx = atomic_load_explicit(&n->next, memory_order_relaxed); free(n); n = nx; }
        atomic_store_explicit(&s->bucket[i], NULL, memory_order_relaxed);
    }
    s->count = 0;
    memset(s->memo, 0, MEMO_SIZE * sizeof(Memo));
}
void ns_store_destroy(NsMeshStore *s) {
    if (!s) return;
    store_free_nodes(s);
    free(s->bucket);
    free(s->memo);
    free(s);
}
void ns_store_clear(NsMeshStore *s) { if (!s) return; store_free_nodes(s); atomic_fetch_add(&s->epoch, 1u); }
uint32_t ns_store_count(const NsMeshStore *s) { return s ? s->count : 0; }
uint32_t ns_store_epoch(const NsMeshStore *s) { return s ? atomic_load(&((NsMeshStore *)s)->epoch) : 0; }
const NsMesh *ns_store_get(const NsMeshStore *s, uint64_t id) {
    if (!s) return NULL;
    for (Node *n = atomic_load_explicit(&s->bucket[id_bucket(id)], memory_order_acquire); n; n = atomic_load_explicit(&n->next, memory_order_acquire))
        if (n->mesh.id == id) return &n->mesh;
    return NULL;
}
static const Memo *memo_find(const NsMeshStore *s, uint32_t addr) {
    for (uint32_t i = 0, h = (addr >> 2) * 2654435761u; i < 64; i++) {
        const Memo *m = &s->memo[(h + i) & (MEMO_SIZE - 1u)];
        if (m->addr == addr) return m;
        if (!m->addr) return NULL;
    }
    return NULL;
}
static void memo_put(NsMeshStore *s, uint32_t addr, uint64_t id, int parent) {
    for (uint32_t i = 0, h = (addr >> 2) * 2654435761u; i < 64; i++) {
        Memo *m = &s->memo[(h + i) & (MEMO_SIZE - 1u)];
        if (!m->addr || m->addr == addr) { m->addr = addr; m->id = id; m->parent = (int16_t)parent; return; }
    }
}
/* Copia la malla temporal al almacén (si no está ya). Devuelve 1 si se añadió, 0 si ya estaba, -1 sin memoria. */
static int store_add(NsMeshStore *s, const NsMesh *m, const NsVert *v, const NsNorm *n, const NsFace *f, const uint32_t *paint) {
    if (ns_store_get(s, m->id)) return 0;
    size_t sv = (size_t)m->n_verts * sizeof(NsVert), sn = (size_t)m->n_norms * sizeof(NsNorm), sf = (size_t)m->n_faces * sizeof(NsFace);
    size_t sp = paint ? (size_t)m->n_faces * 4 * sizeof(uint32_t) : 0;
    size_t off_v = (sizeof(Node) + 7u) & ~(size_t)7u, off_n = off_v + ((sv + 7u) & ~(size_t)7u), off_f = off_n + ((sn + 7u) & ~(size_t)7u),
           off_p = off_f + ((sf + 7u) & ~(size_t)7u), total = off_p + sp;
    uint8_t *blk = (uint8_t *)calloc(1, total ? total : 1);
    if (!blk) return -1;
    Node *nd = (Node *)blk;
    nd->mesh = *m;
    nd->mesh.verts = sv ? (const NsVert *)(blk + off_v) : NULL;
    nd->mesh.norms = sn ? (const NsNorm *)(blk + off_n) : NULL;
    nd->mesh.faces = sf ? (const NsFace *)(blk + off_f) : NULL;
    nd->mesh.paint = paint ? (const uint32_t *)(blk + off_p) : NULL;
    if (sv) memcpy(blk + off_v, v, sv);
    if (sn) memcpy(blk + off_n, n, sn);
    if (sf) memcpy(blk + off_f, f, sf);
    if (sp) memcpy(blk + off_p, paint, sp);
    uint32_t b = id_bucket(m->id);
    atomic_store_explicit(&nd->next, atomic_load_explicit(&s->bucket[b], memory_order_relaxed), memory_order_relaxed);
    atomic_store_explicit(&s->bucket[b], nd, memory_order_release);
    s->count++;
    return 1;
}

/* ---------------------------------------------------------------------------------------------------------------- escena */
int ns_scene_alloc(NsScene *sc, uint32_t ci, uint32_t cp, uint32_t cl, uint32_t ce) {
    memset(sc, 0, sizeof *sc);
    sc->version = NS_VERSION;
    sc->cap_inst = ci; sc->cap_piece = cp; sc->cap_light = cl; sc->cap_effect = ce;
    sc->inst = (NsInstance *)calloc(ci ? ci : 1, sizeof(NsInstance));
    sc->piece = (NsPiece *)calloc(cp ? cp : 1, sizeof(NsPiece));
    sc->lights = (NsLight *)calloc(cl ? cl : 1, sizeof(NsLight));
    sc->effects = (NsEffect *)calloc(ce ? ce : 1, sizeof(NsEffect));
    if (!sc->inst || !sc->piece || !sc->lights || !sc->effects) { ns_scene_free(sc); return -1; }
    return 0;
}
void ns_scene_free(NsScene *sc) {
    free(sc->inst); free(sc->piece); free(sc->lights); free(sc->effects);
    memset(sc, 0, sizeof *sc);
}

/* ---------------------------------------------------------------------------------------------------------------- contexto de extracción */
typedef struct {
    Ram r;
    NsScene *sc;
    NsMeshStore *st;
    NsExtractStats *s;
    NsVert *tv; NsNorm *tn; NsFace *tf; uint32_t *tp;   /* temporales de decodificación (capacidad máxima) */
    uint8_t *marked;                                    /* vértices referenciados con la marca de soldadura */
} Ctx;

static const uint8_t WELD_ORDER[4] = {0, 1, 3, 2};   /* esquina -> posición en el paquete; es su propia inversa */

/* Busca la entrada de textura de un id (sondeo lineal desde id % 512, acotado). Devuelve la dirección de la entrada o 0. */
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

/* Decodifica el modelo `m` de la definición `def` con la tabla de colores `rgbs` (0 si no hay) y lo guarda en el almacén.
 * Devuelve 1 (malla lista, *id y *parent rellenos), 0 (modelo vacío, sin caras) o -1 (modelo inconsistente). */
static int decode_model(Ctx *c, uint32_t m, uint32_t def, uint32_t rgbs, int16_t depth_bias, uint32_t iflags, uint64_t *id, int *parent) {
    Ram *r = &c->r;
    NsExtractStats *S = c->s;
    *id = 0; *parent = -1;
    {   /* el hash se calcula una vez por (dirección, generación del almacén): ns_store_clear al cambiar de zona */
        const Memo *mm = memo_find(c->st, m);
        if (mm) { *id = mm->id; *parent = mm->parent; S->mesh_ref++; return 1; }
    }
    if (!ok_rng(r, m, 0x58)) { r->oob++; return -1; }
    uint32_t mflag = rd32(r, m);
    int32_t nf = rs32(r, m + 4), nv = rs32(r, m + 0x34), nn = rs32(r, m + 0x40);
    uint32_t pv = rd32(r, m + 0x38), pvi = rd32(r, m + 0x3C), pn = rd32(r, m + 0x44), pni = rd32(r, m + 0x48), puv = rd32(r, m + 0x4C), ptx = rd32(r, m + 0x50);
    int32_t par = rs32(r, m + 0x2C);
    if (nf == 0) return 0;
    if (nf < 0 || nf > MAX_FACES || nv < 1 || nv > MAX_VERTS || nn < 0 || nn > MAX_NORMS) return -1;
    if (!ok_rng(r, pv, (uint32_t)nv * 8u) || !ok_rng(r, pvi, (uint32_t)nf * 4u) || !ok_rng(r, puv, (uint32_t)nf * 8u) || !ok_rng(r, ptx, (uint32_t)nf * 2u)) return -1;
    if (nn > 0 && (!ok_rng(r, pn, (uint32_t)nn * 8u) || !ok_rng(r, pni, (uint32_t)nf * 4u))) return -1;
    const int indirect = (mflag & MF_SHARED_VERTS) != 0;
    const uint8_t imask = indirect ? 0x7F : 0xFF;
    const int use_paint = (iflags & OF_PRECOLORED) && rgbs && ok_rng(r, rgbs, (uint32_t)nf * 16u);

    /* vértices y normales */
    int16_t bb[6] = {32767, 32767, 32767, -32768, -32768, -32768};
    for (int i = 0; i < nv; i++) {
        NsVert *v = &c->tv[i];
        v->x = rs16(r, pv + 8u * (uint32_t)i); v->y = rs16(r, pv + 8u * (uint32_t)i + 2); v->z = rs16(r, pv + 8u * (uint32_t)i + 4);
        v->weld = 0xFF; v->pad = 0;
        const int16_t xyz[3] = {v->x, v->y, v->z};
        for (int k = 0; k < 3; k++) {
            if (xyz[k] < bb[k]) bb[k] = xyz[k];
            if (xyz[k] > bb[3 + k]) bb[3 + k] = xyz[k];
        }
    }
    for (int i = 0; i < nn; i++) {
        NsNorm *n = &c->tn[i];
        n->x = rs16(r, pn + 8u * (uint32_t)i); n->y = rs16(r, pn + 8u * (uint32_t)i + 2); n->z = rs16(r, pn + 8u * (uint32_t)i + 4); n->pad = 0;
    }
    memset(c->marked, 0, MAX_VERTS);

    /* caras */
    int kept = 0;
    for (int i = 0; i < nf; i++) {
        S->faces_seen++;
        uint8_t vi[4], ni[4];
        int bad = 0;
        for (int k = 0; k < 4; k++) {
            uint8_t b = rd8(r, pvi + 4u * (uint32_t)i + (uint32_t)k);
            if (indirect && (b & 0x80)) c->marked[b & 0x7F] = 1;
            vi[k] = b & imask;
            if (vi[k] >= nv) bad = 1;
        }
        if (bad) { S->faces_bad_vindex++; continue; }
        for (int k = 0; k < 4; k++) {
            uint8_t b = nn > 0 ? (uint8_t)(rd8(r, pni + 4u * (uint32_t)i + (uint32_t)k) & imask) : 0;
            if (nn > 0 && b >= nn) { S->faces_bad_nindex++; b = 0; }
            ni[k] = b;
        }
        NsFace *f = &c->tf[kept];
        memset(f, 0, sizeof *f);
        /* motor: v0=A(byte0), v1=B(byte1), v2=D(byte3), v3=C(byte2): el orden de las matrices es el de los bytes, A,B,C,D en perímetro */
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
        if (use_paint) {
            f->flags |= NS_FF_PAINT_OK; S->faces_paint++;
            for (int k = 0; k < 4; k++) {
                uint32_t a = rgbs + 16u * (uint32_t)i + 4u * (uint32_t)k;
                c->tp[(size_t)kept * 4 + (size_t)k] = (uint32_t)rd8(r, a) | ((uint32_t)rd8(r, a + 1) << 8) | ((uint32_t)rd8(r, a + 2) << 16) | 0xFF000000u;
            }
        }
        kept++;
    }
    S->faces_kept += (uint32_t)kept;
    if (kept == 0) return 0;

    /* soldaduras: el desplazamiento de `pad` apunta al paquete del padre (docs §3.5) */
    if (indirect && par >= 0 && par < MAX_PIECES && ok_rng(r, def, 0x20)) {
        uint32_t pm = def + 0x20u + 0x58u * (uint32_t)par;
        if (ok_rng(r, pm, 0x58)) {
            int32_t pnf = rs32(r, pm + 4), pnv = rs32(r, pm + 0x34);
            uint32_t ppvi = rd32(r, pm + 0x3C), pflag = rd32(r, pm);
            int pok = pnf > 0 && pnf <= MAX_FACES && pnv > 0 && pnv <= MAX_VERTS && ok_rng(r, ppvi, (uint32_t)pnf * 4u);
            for (int i = 0; i < nv; i++) {
                if (!c->marked[i]) continue;
                uint16_t pad = rd16(r, pv + 8u * (uint32_t)i + 6);
                int done = 0;
                if (pok && pad != 0xFFFFu && pad >= 8) {
                    uint32_t off = (uint32_t)pad - 8u, face = off / 52u, rem = off % 52u;
                    if (rem < 48u && face < (uint32_t)pnf) {
                        uint32_t slot = rem / 12u;
                        uint8_t b = rd8(r, ppvi + 4u * face + WELD_ORDER[slot]);
                        uint8_t pvx = (uint8_t)(b & ((pflag & MF_SHARED_VERTS) ? 0x7F : 0xFF));
                        if (pvx < pnv) { c->tv[i].weld = pvx; S->welded_verts++; done = 1; }
                    }
                }
                if (!done) S->weld_bad++;
            }
            *parent = (int)par;
        }
    } else if (indirect) {
        for (int i = 0; i < nv; i++) if (c->marked[i]) S->weld_bad++;
    }

    NsMesh mesh;
    memset(&mesh, 0, sizeof mesh);
    mesh.model_flags = mflag;
    if (mflag & MF_TRANSLUCENT) mesh.flags |= NS_MF_TRANS;
    if (mflag & MF_UNLIT) mesh.flags |= NS_MF_UNLIT;
    if (mflag & MF_TWO_SIDED) mesh.flags |= NS_MF_BOTHFACE;
    if (indirect) mesh.flags |= NS_MF_INDIRECT;
    if (use_paint) mesh.flags |= NS_MF_PAINT;
    if (depth_bias > 0) mesh.flags |= NS_MF_RAISE_UP;
    mesh.n_verts = (uint16_t)nv; mesh.n_norms = (uint16_t)nn; mesh.n_faces = (uint16_t)kept; mesh.raise = depth_bias;
    memcpy(mesh.bbox, bb, sizeof bb);
    uint32_t h = NS_HASH_INIT;
    uint32_t hd[6] = {mesh.model_flags, mesh.flags, mesh.n_verts, mesh.n_norms, mesh.n_faces, (uint32_t)(int32_t)depth_bias};
    h = ns_hash32(h, hd, sizeof hd);
    h = ns_hash32(h, bb, sizeof bb);
    h = ns_hash32(h, c->tv, (size_t)nv * sizeof(NsVert));
    if (nn) h = ns_hash32(h, c->tn, (size_t)nn * sizeof(NsNorm));
    h = ns_hash32(h, c->tf, (size_t)kept * sizeof(NsFace));
    mesh.hash = h;   /* el color precalculado NO entra en el hash (el contrato lo excluye); los rellenos van a cero (memset) */
    mesh.id = NS_MESH_ID(def, h);
    int a = store_add(c->st, &mesh, c->tv, c->tn, c->tf, use_paint ? c->tp : NULL);
    if (a < 0) return -1;
    memo_put(c->st, m, mesh.id, *parent);
    if (a) S->mesh_new++; else S->mesh_ref++;
    *id = mesh.id;
    return 1;
}

/* ---------------------------------------------------------------------------------------------------------------- luces */
static int read_light(Ctx *c, uint32_t lp, uint32_t oflag, NsLight *L) {
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
    return 0;
}
static int grow_lights(NsScene *sc, uint32_t need);
static int light_index(Ctx *c, const NsLight *L) {
    NsScene *sc = c->sc;
    for (uint32_t i = 0; i < sc->n_light; i++) if (!memcmp(&sc->lights[i], L, sizeof *L)) return (int)i;
    if (grow_lights(sc, sc->n_light + 1u)) { sc->truncated |= 4; return -1; }
    sc->lights[sc->n_light] = *L;
    c->s->n_lights++;
    return (int)sc->n_light++;
}

/* ---------------------------------------------------------------------------------------------------------------- piezas */
/* Emite una pieza con malla (model/rgbs/depth_bias propios) y la matriz de mundo `world`. */
/* Crecimiento de las tablas de la escena: nunca se trunca por capacidad (sobre todo el mapa); sólo si falla la memoria. */
static int grow_pieces(NsScene *sc, uint32_t need) {
    if (need <= sc->cap_piece) return 0;
    uint32_t nc = sc->cap_piece ? sc->cap_piece : 256u;
    while (nc < need) nc *= 2u;
    NsPiece *p = (NsPiece *)realloc(sc->piece, (size_t)nc * sizeof *p);
    if (!p) return -1;
    sc->piece = p; sc->cap_piece = nc;
    return 0;
}
static int grow_insts(NsScene *sc, uint32_t need) {
    if (need <= sc->cap_inst) return 0;
    uint32_t nc = sc->cap_inst ? sc->cap_inst : 64u;
    while (nc < need) nc *= 2u;
    NsInstance *p = (NsInstance *)realloc(sc->inst, (size_t)nc * sizeof *p);
    if (!p) return -1;
    sc->inst = p; sc->cap_inst = nc;
    return 0;
}
static int grow_lights(NsScene *sc, uint32_t need) {
    if (need <= sc->cap_light) return 0;
    uint32_t nc = sc->cap_light ? sc->cap_light : 64u;
    while (nc < need) nc *= 2u;
    NsLight *p = (NsLight *)realloc(sc->lights, (size_t)nc * sizeof *p);
    if (!p) return -1;
    sc->lights = p; sc->cap_light = nc;
    return 0;
}
static int emit_piece(Ctx *c, uint32_t model, uint32_t def, uint32_t rgbs, int16_t depth_bias, uint32_t oflag, const NsXf *world, int nm_base) {
    NsScene *sc = c->sc;
    if (grow_pieces(sc, sc->n_piece + 1u)) { sc->truncated |= 2; return -1; }
    NsPiece *p = &sc->piece[sc->n_piece++];
    memset(p, 0, sizeof *p);
    p->world = *world;
    p->parent = -1;
    uint64_t id; int parent;
    int rc = decode_model(c, model, def, rgbs, depth_bias, oflag, &id, &parent);
    if (rc > 0) {
        p->mesh_id = id;
        if (parent >= 0 && parent < nm_base) p->parent = (int16_t)parent;
    } else {
        p->flags |= NS_PF_HIDDEN;
        c->s->n_hidden_pieces++;
        if (rc < 0) c->s->drop_bad_model++;
    }
    c->s->n_pieces++;
    return 0;
}

int ns_extract(NsScene *sc, NsMeshStore *st, const uint8_t *ram, size_t ram_size, const uint8_t *scratch, size_t scratch_size, NsExtractStats *stats) {
    return ns_extract_ex(sc, st, ram, ram_size, scratch, scratch_size, NULL, stats);
}

int ns_extract_ex(NsScene *sc, NsMeshStore *st, const uint8_t *ram, size_t ram_size, const uint8_t *scratch, size_t scratch_size, const NsExtractOpts *opt, NsExtractStats *stats) {
    (void)scratch; (void)scratch_size;
    NsExtractStats dummy;
    NsExtractStats *S = stats ? stats : &dummy;
    memset(S, 0, sizeof *S);
    uint32_t seq = sc->seq + 1;
    sc->n_inst = sc->n_piece = sc->n_light = sc->n_effect = 0;
    sc->seq = seq; sc->valid = 0; sc->truncated = 0; sc->version = NS_VERSION; sc->units_per_m = 1000.0f;
    sc->mesh_epoch = ns_store_epoch(st);
    memset(sc->to_vr, 0, sizeof sc->to_vr);
    sc->to_vr[0] = sc->to_vr[5] = sc->to_vr[10] = 0.001f;   /* escala nominal; el origen y el espejo los fija la capa VR (docs §2) */
    memset(&sc->cam, 0, sizeof sc->cam); memset(&sc->fog, 0, sizeof sc->fog);
    sc->bg_rgb[0] = sc->bg_rgb[1] = sc->bg_rgb[2] = 0;
    memset(&sc->global_light, 0, sizeof sc->global_light);

    Ctx c;
    memset(&c, 0, sizeof c);
    c.r.b = ram; c.r.size = ram_size > NS_RAM_SIZE ? NS_RAM_SIZE : (uint32_t)ram_size;
    c.sc = sc; c.st = st; c.s = S;
    if (!ram || !st) return -1;
    c.tv = (NsVert *)malloc(MAX_VERTS * sizeof(NsVert));
    c.tn = (NsNorm *)malloc(MAX_NORMS * sizeof(NsNorm));
    c.tf = (NsFace *)malloc((size_t)MAX_FACES * sizeof(NsFace));
    c.tp = (uint32_t *)malloc((size_t)MAX_FACES * 4 * sizeof(uint32_t));
    c.marked = (uint8_t *)malloc(MAX_VERTS);
    if (!c.tv || !c.tn || !c.tf || !c.tp || !c.marked) { free(c.tv); free(c.tn); free(c.tf); free(c.tp); free(c.marked); return -1; }
    Ram *r = &c.r;
    int ret = -1;

    if (!ok_rng(r, A_CHAN, 0x60)) goto done;
    {
        /* cámara del juego: eye (vista->mundo) en +0x30, H en +0x50 */
        NsXf eye;
        if (read_matrix(r, A_CHAN + 0x30u, &eye)) {
            for (int i = 0; i < 3; i++) {
                sc->cam.rot[3 * i + 0] = eye.m[4 * i + 0]; sc->cam.rot[3 * i + 1] = eye.m[4 * i + 1]; sc->cam.rot[3 * i + 2] = eye.m[4 * i + 2];
                sc->cam.eye[i] = eye.m[4 * i + 3];
            }
        }
        sc->cam.h = (float)rs16(r, A_CHAN + 0x50);
        sc->cam.overscan_y = 66.0f / 64.0f;
        sc->cam.vr_active = 0;
        sc->clock = rd32(r, A_CLOCK) & 1u;
        uint32_t gmask = rd32(r, A_GROUP);
        sc->group_mask = gmask;
        NsLight gl; read_light(&c, A_LIGHT_G, 0, &gl);
        sc->global_light = gl;

        int n = rs16(r, A_CHAN + 0x56);
        uint32_t q = rd32(r, A_CHAN + 0x58);
        if (n < 0) { n = 0; S->q_clamped = 1; }
        if (n > MAX_QUEUE) { n = MAX_QUEUE; S->q_clamped = 1; }
        if (n > 0 && !ok_rng(r, q, 4u * (uint32_t)n)) { S->drop_badptr++; n = 0; }
        S->queue_n = (uint32_t)n;
        ret = 0;
        uint32_t seen[MAX_QUEUE];
        uint32_t nseen = 0;
        for (int i = 0; i < n; i++) {
            uint32_t o = rd32(r, q + 4u * (uint32_t)i);
            if (!o) { S->drop_null++; continue; }
            if (!ok_rng(r, o, 0x48)) { S->drop_badptr++; continue; }
            int dup = 0;
            for (uint32_t k = 0; k < nseen; k++) if (seen[k] == o) { dup = 1; break; }
            if (dup) { S->drop_dup++; continue; }
            seen[nseen++] = o;
            uint32_t flag = rd32(r, o + 0x28);
            uint16_t gid = rd16(r, o + 0x2C);
            if (opt && opt->skip_objs) {   /* objetos que ya pinta otro módulo (p. ej. las sombras de personaje como cuadriláteros) */
                int skip = 0;
                for (uint32_t k = 0; k < opt->n_skip && !skip; k++) skip = opt->skip_objs[k] == o;
                if (skip) { S->drop_skipped++; continue; }
            }
            if (flag & OF_HIDDEN) { S->drop_invisible++; continue; }
            if (gid != 0 && !(gid & gmask)) { S->drop_group++; continue; }
            int nm = rs16(r, o + 0x2E);
            uint32_t def = rd32(r, o + 0x24);
            if (nm <= 0 || nm > MAX_PIECES || !ok_rng(r, o + 0x48u, 0x5Cu * (uint32_t)nm)) { S->drop_badobj++; continue; }
            if (grow_insts(sc, sc->n_inst + 1u)) { sc->truncated |= 1; break; }
            if (grow_pieces(sc, sc->n_piece + (uint32_t)nm)) { sc->truncated |= 2; break; }

            NsInstance *in = &sc->inst[sc->n_inst];
            memset(in, 0, sizeof *in);
            in->key = o; in->first_piece = sc->n_piece; in->group = gid;
            in->chanl = rs16(r, o + 0x30); in->bound_mode = rs16(r, o + 0x32); in->light = -1;
            if (flag & OF_LIT) in->flags |= NS_IF_SHADE;
            if (flag & OF_OWN_AMBIENT) in->flags |= NS_IF_AMBIENT;
            if (flag & OF_TRANSLUCENT) in->flags |= NS_IF_TRANS;
            if ((flag & OF_PRECOLORED) && !(flag & OF_LIT)) in->flags |= NS_IF_STATIC;
            if (flag & OF_ALT_PALETTE) in->flags |= NS_IF_THERMAL;
            if (flag & OF_ONE_MATRIX) in->flags |= NS_IF_ONEPIECE;
            if (flag & OF_ENV_REFLECT) in->flags |= NS_IF_ENVMAP;
            if (flag & OF_LIT) { NsLight L; read_light(&c, rd32(r, o + 0x34), flag, &L); in->light = (int16_t)light_index(&c, &L); }

            /* piezas dibujables */
            for (int k = 0; k < nm; k++) {
                uint32_t p = o + 0x48u + 0x5Cu * (uint32_t)k;
                NsXf w; memset(&w, 0, sizeof w);
                read_matrix(r, p, &w);
                emit_piece(&c, rd32(r, p + 0x40), def, rd32(r, p + 0x44), rs16(r, p + 0x50), flag, &w, nm);
            }
            /* extensiones: otra malla que comparte la matriz de la pieza que la enlaza (cadena acotada); la matriz de la extensión no se calcula */
            for (int k = 0; k < nm; k++) {
                uint32_t p = o + 0x48u + 0x5Cu * (uint32_t)k;
                NsXf w; memset(&w, 0, sizeof w);
                read_matrix(r, p, &w);
                uint32_t e = rd32(r, p + 0x48);
                for (int depth = 0; e && depth < MAX_EXT_DEPTH; depth++) {
                    uint32_t base = o + 0x48u;
                    if (e < base || (e - base) % 0x5Cu != 0 || (e - base) / 0x5Cu >= (uint32_t)MAX_PIECES || !ok_rng(r, e, 0x5C)) { S->ext_bad++; break; }
                    if (e == p) { S->ext_bad++; break; }   /* se apunta a sí misma */
                    if (grow_pieces(sc, sc->n_piece + 1u)) { sc->truncated |= 2; break; }
                    emit_piece(&c, rd32(r, e + 0x40), def, rd32(r, e + 0x44), rs16(r, e + 0x50), flag, &w, nm);
                    S->n_ext_pieces++;
                    uint32_t e2 = rd32(r, e + 0x48);
                    if (e2 == p || e2 == e) { S->ext_bad++; break; }
                    e = e2;
                    if (depth == MAX_EXT_DEPTH - 1 && e) S->ext_bad++;
                }
            }
            in->n_pieces = (uint16_t)(sc->n_piece - in->first_piece);
            sc->n_inst++;
            S->n_inst++;
        }
        sc->valid = (sc->n_inst ? NS_SV_GAME : 0u) | (sc->truncated ? 0u : NS_SV_LISTS_OK);
        for (uint32_t i = 0; i < sc->n_inst; i++) if (sc->inst[i].flags & NS_IF_STATIC) { sc->valid |= NS_SV_MAP_OK; break; }
    }
done:
    S->oob_reads = r->oob;
    S->truncated = sc->truncated;
    free(c.tv); free(c.tn); free(c.tf); free(c.tp); free(c.marked);
    return ret;
}
