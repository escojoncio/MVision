/* vr_activity: ver vr_activity.h (mecanismos, API). Solo lectura de la RAM; sin dependencias del runtime. */
#include "vr_activity.h"
#include <string.h>

/* ---------- lectura de RAM segura (2 MB, espejos enmascarados) ---------- */
#define RAMSZ 0x200000u
static int ok_addr(uint32_t a, uint32_t len) {
    uint32_t o = a & 0x1FFFFFFFu;
    return (a & 0xC0000000u) == 0x80000000u && o >= 0x10000u && (uint64_t)o + len <= RAMSZ;   /* KSEG0/KSEG1 */
}
static uint32_t r32(const uint8_t *ram, uint32_t a) {
    uint32_t o = a & (RAMSZ - 1u), v;
    memcpy(&v, ram + (o & ~3u), 4);
    return v;
}
static int16_t r16(const uint8_t *ram, uint32_t a) {
    uint32_t o = a & (RAMSZ - 1u);
    uint16_t v;
    memcpy(&v, ram + (o & ~1u), 2);
    return (int16_t)v;
}
/* El juego manda y no se toca nada si: pausa/película/comunicador (M1) o, para soldados, enemigos apagados (M8). */
static int suspended(const VrActivity *a, VraClass cls) {
    return (a->pause_level & 3) || (cls == VRA_CLASS_SOLDIER && (a->game_status & 0x200u));
}
static int32_t abs32(int32_t v) { return v < 0 ? -v : v; }
/* raíz cuadrada entera (sin libm) */
static int32_t isqrt64(uint64_t v) {
    uint64_t r = 0, bit = 1ull << 62;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; } else r >>= 1;
        bit >>= 2;
    }
    return r > 0x7fffffffu ? 0x7fffffff : (int32_t)r;
}

/* ---------- clasificación por hash de la cadena de depuración del actor ----------
 * FNV-1a 32 bits. Sin nombres en claro: la tabla se generó a partir de las cadenas del propio EXE.
 * min_level/max_level restringen la lista de actores (un actor de otra clase comparte nombre con el de la
 * cámara de juego, que vive en la lista 2). */
static const struct { uint32_t h; uint8_t cls, lmin, lmax; } NAMES[] = {
    {0x7f1a6140u, VRA_CLASS_SOLDIER, 3, 6}, {0x28477fd8u, VRA_CLASS_SOLDIER, 3, 6},
    {0x2e42ca30u, VRA_CLASS_SOLDIER, 3, 6}, {0x42affe38u, VRA_CLASS_SOLDIER, 3, 6},
    {0x9662c384u, VRA_CLASS_SOLDIER, 3, 6}, {0xebd3e279u, VRA_CLASS_SOLDIER, 3, 6},
    {0x848a7679u, VRA_CLASS_SENSOR, 4, 6},  {0x909c58f7u, VRA_CLASS_SENSOR, 3, 6},
    {0xac19cfa5u, VRA_CLASS_SENSOR, 3, 6},  {0xa01f9914u, VRA_CLASS_SENSOR, 3, 6},
};

uint32_t vra_name_hash(const char *s) {
    uint32_t h = 0x811c9dc5u;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 0x01000193u;
    return h;
}
/* Entradas añadidas por el anfitrión (zonas con actores no previstos) o por las pruebas. */
static struct { uint32_t h; uint8_t cls, lmin, lmax; } EXTRA[16];
static int n_extra;
int vra_register_name(const char *name, VraClass cls, int lmin, int lmax) {
    if (!name || !name[0] || (unsigned)cls >= VRA_CLASS_COUNT || n_extra >= 16 || lmin < 0 || lmax > 8 || lmin > lmax) return -1;
    uint32_t h = vra_name_hash(name);
    for (int i = 0; i < n_extra; i++) if (EXTRA[i].h == h) { EXTRA[i].cls = (uint8_t)cls; EXTRA[i].lmin = (uint8_t)lmin; EXTRA[i].lmax = (uint8_t)lmax; return 0; }
    EXTRA[n_extra].h = h; EXTRA[n_extra].cls = (uint8_t)cls; EXTRA[n_extra].lmin = (uint8_t)lmin; EXTRA[n_extra].lmax = (uint8_t)lmax;
    n_extra++;
    return 0;
}
VraClass vra_classify_hash(uint32_t h, int level) {
    for (int i = 0; i < n_extra; i++)
        if (EXTRA[i].h == h) return level >= EXTRA[i].lmin && level <= EXTRA[i].lmax ? (VraClass)EXTRA[i].cls : VRA_CLASS_OTHER;
    for (size_t i = 0; i < sizeof NAMES / sizeof *NAMES; i++)
        if (NAMES[i].h == h) return level >= NAMES[i].lmin && level <= NAMES[i].lmax ? (VraClass)NAMES[i].cls : VRA_CLASS_OTHER;
    return VRA_CLASS_OTHER;
}
VraClass vra_classify_name(const char *name, int level) {
    if (!name || !name[0]) return VRA_CLASS_OTHER;
    return vra_classify_hash(vra_name_hash(name), level);
}

/* ---------- configuración ---------- */
void vra_init(VrActivity *a) {
    memset(a, 0, sizeof *a);
    for (int c = 0; c < VRA_CLASS_COUNT; c++) a->cfg.mode[c] = VRA_VR;
}
void vra_set_mode(VrActivity *a, VraClass cls, VraMode m) {
    if ((unsigned)cls < VRA_CLASS_COUNT) a->cfg.mode[cls] = m == VRA_VR ? VRA_VR : VRA_ORIGINAL;
}
VraMode vra_get_mode(const VrActivity *a, VraClass cls) {
    return (unsigned)cls < VRA_CLASS_COUNT ? a->cfg.mode[cls] : VRA_ORIGINAL;
}
void vra_set_radius(VrActivity *a, VraClass cls, int32_t r) {
    if ((unsigned)cls < VRA_CLASS_COUNT) a->cfg.radius[cls] = r < 0 ? 0 : r;
}
void vra_set_max_dy(VrActivity *a, VraClass cls, int32_t dy) {
    if ((unsigned)cls < VRA_CLASS_COUNT) a->cfg.max_dy[cls] = dy < 0 ? 0 : dy;
}
void vra_set_los(VrActivity *a, VraLosFn fn, void *user) { a->cfg.los = fn; a->cfg.los_user = user; }
void vra_set_viewer(VrActivity *a, const int32_t pos[3]) {
    if (pos) { memcpy(a->forced_viewer, pos, sizeof a->forced_viewer); a->viewer_forced = 1; }
    else a->viewer_forced = 0;
}

/* ---------- zona ---------- */
/* Recorre los mapas "usados" (MAP[16]); de cada uno el manejador de peligros -> definición -> zonas de ruta.
 * Todo validado: punteros en RAM, nº de zonas razonable, bit de mapa == 1<<i. */
int32_t vra_zone_diag(const uint8_t *ram, int32_t *box) {
    int32_t mn[3] = {0x7fffffff, 0x7fffffff, 0x7fffffff}, mx[3] = {-0x7fffffff, -0x7fffffff, -0x7fffffff};
    int any = 0;
    for (unsigned i = 0; i < 16; i++) {
        uint32_t m = VRA_A_MAPS + i * VRA_MAP_STRIDE;
        if (!ok_addr(m, VRA_MAP_STRIDE)) break;
        if (r32(ram, m) != (1u << i)) continue;
        if (!(uint16_t)r16(ram, m + 6)) continue;                    /* used */
        uint32_t haz = r32(ram, m + 8);
        if (!ok_addr(haz, 4) || (haz & 3)) continue;
        uint32_t def = r32(ram, haz);
        if (!ok_addr(def, 24) || (def & 3)) continue;
        int nz = r16(ram, def + 12);
        uint32_t zp = r32(ram, def + 20);
        if (nz < 1 || nz > 255 || !ok_addr(zp, (uint32_t)nz * VRA_ZONE_SZ) || (zp & 3)) continue;
        for (int z = 0; z < nz; z++) {
            uint32_t q = zp + (uint32_t)z * VRA_ZONE_SZ;
            int32_t x = r16(ram, q), zz = r16(ram, q + 2), y = r16(ram, q + 4), w = r16(ram, q + 6), h = r16(ram, q + 8);
            w = abs32(w); h = abs32(h);
            if (x - w < mn[0]) mn[0] = x - w;
            if (x + w > mx[0]) mx[0] = x + w;
            if (zz - h < mn[2]) mn[2] = zz - h;
            if (zz + h > mx[2]) mx[2] = zz + h;
            if (y < mn[1]) mn[1] = y;
            if (y > mx[1]) mx[1] = y;
            any = 1;
        }
    }
    if (!any) return 0;
    if (box) { box[0] = mn[0]; box[1] = mn[1]; box[2] = mn[2]; box[3] = mx[0]; box[4] = mx[1]; box[5] = mx[2]; }
    int64_t dx = (int64_t)mx[0] - mn[0], dy = (int64_t)mx[1] - mn[1], dz = (int64_t)mx[2] - mn[2];
    return isqrt64((uint64_t)(dx * dx + dy * dy + dz * dz));
}

/* ---------- radios y rango ---------- */
int32_t vra_radius(const VrActivity *a, VraClass cls) {
    if ((unsigned)cls >= VRA_CLASS_COUNT) return 0;
    int32_t r = a->cfg.radius[cls] > 0 ? a->cfg.radius[cls] : a->zone_diag;
    return r;
}
static int in_range(const VrActivity *a, VraClass cls, const int32_t p[3], int32_t *dist, int stay) {
    int64_t dx = (int64_t)p[0] - a->viewer[0], dy = (int64_t)p[1] - a->viewer[1], dz = (int64_t)p[2] - a->viewer[2];
    int64_t d2 = dx * dx + dy * dy + dz * dz;
    int64_t r = vra_radius(a, cls);
    if (stay) r += r / 16;                       /* histéresis: quien ya estaba activo sale un 6 % más lejos */
    if (dist) *dist = isqrt64((uint64_t)d2);
    if (r <= 0 || d2 > r * r) return 0;
    if (a->cfg.max_dy[cls] > 0 && (dy < 0 ? -dy : dy) > a->cfg.max_dy[cls]) return 0;
    if (a->cfg.los && !a->cfg.los(a->cfg.los_user, a->viewer, p)) return 0;
    return 1;
}
int vra_point_active(const VrActivity *a, VraClass cls, const int32_t p[3]) {
    if (!a->active || (unsigned)cls >= VRA_CLASS_COUNT || a->cfg.mode[cls] != VRA_VR) return 0;
    return in_range(a, cls, p, NULL, 0);
}

/* ---------- conjunto objs -> clase (hash abierto) ---------- */
static unsigned hslot(uint32_t k) { return (k >> 2) * 2654435761u >> 23; }
static void set_clear(VrActivity *a) { memset(a->set_key, 0, sizeof a->set_key); memset(a->set_val, 0, sizeof a->set_val); }
static void set_add(VrActivity *a, uint32_t objs, uint8_t cls) {
    unsigned s = hslot(objs);
    for (unsigned i = 0; i < 512; i++, s = (s + 1) & 511) {
        if (a->set_key[s] == objs) return;
        if (!a->set_key[s]) { a->set_key[s] = objs; a->set_val[s] = (uint8_t)(((cls & 0x7f) + 1) | (cls & 0x80)); return; }
    }
}
/* v = (clase+1) | 0x80 si está en rango; 0 si no es un objeto seguido */
static int set_get(const VrActivity *a, uint32_t objs) {
    if (!objs) return 0;
    unsigned s = hslot(objs);
    for (unsigned i = 0; i < 512; i++, s = (s + 1) & 511) {
        if (a->set_key[s] == objs) return a->set_val[s];
        if (!a->set_key[s]) break;
    }
    return 0;
}
VraClass vra_class_of_objs(const VrActivity *a, uint32_t objs) {
    int v = set_get(a, objs);
    return v ? (VraClass)((v & 0x7f) - 1) : VRA_CLASS_OTHER;
}

/* Posición mundo de un objeto 3D (world.t en +0x14). Los parches la leen de RAM; aquí la copia de este
 * fotograma (la posición cambia dentro del fotograma, pero el radio es de miles de unidades). */
int vra_bound_mode(const VrActivity *a, uint32_t objs, uint32_t flag, uint32_t group, int mode_in) {
    if (!a->active || mode_in != 0) return mode_in;
    VraClass cls = vra_class_of_objs(a, objs);
    if (cls == VRA_CLASS_OTHER || a->cfg.mode[cls] != VRA_VR || suspended(a, cls)) return mode_in;
    if (flag & 0x80u) return mode_in;                                   /* invisible: lo decide el juego */
    if (group != 0 && !(group & a->cur_group)) return mode_in;          /* otro grupo de mapas (M6) */
    return (set_get(a, objs) & 0x80) ? 2 : mode_in;
}

/* ---------- validación estructural de un bloque de objeto 3D ---------- */
static int plausible_ptr(uint32_t p) { return (p & 0xE0000000u) == 0x80000000u && !(p & 3) && ok_addr(p, 0x90); }
static int is_objs(const uint8_t *ram, uint32_t p) {
    if (!plausible_ptr(p)) return 0;
    uint32_t def = r32(ram, p + 0x24);
    int n = r16(ram, p + 0x2e);
    if (!plausible_ptr(def) || n < 1 || n > 32) return 0;
    int canal = r16(ram, p + 0x30), bm = r16(ram, p + 0x32);
    if (canal < 0 || canal > 2 || bm < 0 || bm > 7) return 0;
    if (r32(ram, p + 0x28) > 0x3FFFu) return 0;
    int dn = (int)r32(ram, def);
    if (dn < 1 || dn > 32) return 0;
    return r32(ram, p + 0x48 + 0x40) == def + 0x20u;     /* objs[0].model == &def->models[0] */
}
static int find_objs(const uint8_t *ram, uint32_t actor, uint32_t span, uint32_t *out, int max) {
    int n = 0;
    if (span > 0x2400u) span = 0x2400u;
    for (uint32_t o = 0; o + 4 <= span && n < max; o += 4) {
        uint32_t p = r32(ram, actor + o);
        if (p == actor || !is_objs(ram, p)) continue;
        int dup = 0;
        for (int i = 0; i < n; i++) if (out[i] == p) dup = 1;
        if (!dup) out[n++] = p;
    }
    return n;
}

/* ---------- fotograma ---------- */

int vra_frame(VrActivity *a, const uint8_t *ram, const BridgeState *st) {
    a->frame++;
    memset(a->n_class, 0, sizeof a->n_class);
    memset(a->n_active, 0, sizeof a->n_active);
    int good = ram && st && (st->valid & BRIDGE_OK_ALL_CORE) == BRIDGE_OK_ALL_CORE && st->has_player;
    if (!good) {
        a->active = 0; a->warm = 0; a->n_tracked = 0;
        set_clear(a);
        return 0;
    }
    if (a->warm < VRA_WARMUP) a->warm++;
    a->active = a->warm >= VRA_WARMUP;
    a->game_status = st->game_status;
    a->pause_level = st->pause_level;
    a->cur_group = r32(ram, VRA_A_CUR_GROUP);
    if (a->viewer_forced) memcpy(a->viewer, a->forced_viewer, sizeof a->viewer);
    else { a->viewer[0] = st->player_pos.x; a->viewer[1] = st->player_pos.y; a->viewer[2] = st->player_pos.z; }
    a->zone_diag = vra_zone_diag(ram, NULL);

    /* conservar la actividad acumulada de los actores que siguen */
    VraTracked old[VRA_MAX_TRACKED];
    int nold = a->n_tracked;
    memcpy(old, a->tracked, sizeof(VraTracked) * (size_t)nold);
    a->n_tracked = 0;
    set_clear(a);

    int n_act = st->n_actors < 0 ? 0 : st->n_actors > BRIDGE_MAX_ACTORS ? BRIDGE_MAX_ACTORS : st->n_actors;
    for (int i = 0; i < n_act && a->n_tracked < VRA_MAX_TRACKED; i++) {
        const BridgeActor *ba = &st->actors[i];
        char nm[sizeof ba->name + 1];
        memcpy(nm, ba->name, sizeof ba->name); nm[sizeof ba->name] = 0;       /* sin fiarse del terminador */
        VraClass cls = vra_classify_name(nm, ba->level);
        if (cls == VRA_CLASS_OTHER) continue;
        /* Span hasta el siguiente actor de mayor dirección (los actores se asignan de la pila del juego). */
        uint32_t next = 0xFFFFFFFFu;
        for (int j = 0; j < n_act; j++)
            if (st->actors[j].addr > ba->addr && st->actors[j].addr < next) next = st->actors[j].addr;
        uint32_t span = next == 0xFFFFFFFFu ? 0x2000u : next - ba->addr;
        VraTracked *t = &a->tracked[a->n_tracked];
        memset(t, 0, sizeof *t);
        t->actor = ba->addr;
        t->cls = (uint8_t)cls;
        t->level = ba->level;
        t->n_objs = (uint8_t)find_objs(ram, ba->addr, span, t->objs, VRA_MAX_OBJS);
        int was_in = 0;
        for (int k = 0; k < nold; k++)
            if (old[k].actor == t->actor) { was_in = old[k].in_range; t->moved = old[k].moved; t->frames = old[k].frames; memcpy(t->prev_pos, old[k].pos, sizeof t->pos); break; }
        a->n_class[cls]++;
        if (t->n_objs) {
            for (int k = 0; k < 3; k++) t->pos[k] = (int32_t)r32(ram, t->objs[0] + 0x14 + 4u * (unsigned)k);
            if (t->frames && memcmp(t->pos, t->prev_pos, sizeof t->pos)) t->moved++;
            int ir = in_range(a, cls, t->pos, &t->dist, was_in);                  /* la distancia se calcula también en ORIGINAL */
            t->in_range = (uint8_t)(a->cfg.mode[cls] == VRA_VR && ir);
            for (int k = 0; k < t->n_objs; k++) set_add(a, t->objs[k], (uint8_t)(t->cls | (t->in_range ? 0x80 : 0)));
            if (t->in_range) a->n_active[cls]++;
        }
        t->frames++;
        a->n_tracked++;
    }
    return a->n_tracked;
}

/* ---------- parches ---------- */
int vra_pointcheck_fixup(const VrActivity *a, uint8_t *ram, uint32_t pts, int n) {
    if (!a->active || a->cfg.mode[VRA_CLASS_SOLDIER] != VRA_VR || suspended(a, VRA_CLASS_SOLDIER) || n <= 0 || n > 256 || (pts & 1)) return 0;
    int changed = 0;
    for (int i = 0; i < n; i++) {
        uint32_t q = pts + 8u * (uint32_t)i;
        if (!ok_addr(q, 8)) break;
        uint32_t o = q & (RAMSZ - 1u);
        int16_t pad;
        memcpy(&pad, ram + o + 6, 2);
        if (pad == 1) continue;
        int32_t p[3] = {r16(ram, q), r16(ram, q + 2), r16(ram, q + 4)};
        if (vra_point_active(a, VRA_CLASS_SOLDIER, p)) { pad = 1; memcpy(ram + o + 6, &pad, 2); changed++; }
    }
    return changed;
}

/* ---------- auditoría ---------- */
int vra_audit(const VrActivity *a, const uint8_t *ram, VraAudit *out, int max) {
    int n = 0;
    for (int i = 0; i < a->n_tracked && n < max; i++) {
        const VraTracked *t = &a->tracked[i];
        VraAudit *o = &out[n++];
        memset(o, 0, sizeof *o);
        o->actor = t->actor; o->cls = t->cls; o->level = t->level; o->dist = t->dist; o->bound_mode = -1;
        if (a->cfg.mode[t->cls] != VRA_VR) o->why |= VRA_WHY_NOT_VR;
        if (a->pause_level & 3) o->why |= VRA_WHY_PAUSED;   /* igual que suspended() */
        if (t->cls == VRA_CLASS_SOLDIER && (a->game_status & 0x200u)) o->why |= VRA_WHY_ENEMY_OFF;
        if (!t->n_objs) { o->why |= VRA_WHY_NO_OBJS; continue; }
        uint32_t ob = t->objs[0], flag = r32(ram, ob + 0x28);
        uint32_t group = (uint16_t)r16(ram, ob + 0x2c);
        o->bound_mode = r16(ram, ob + 0x32);
        int32_t r = vra_radius(a, (VraClass)t->cls);
        int32_t d = t->dist;
        if (r <= 0 || d > r) o->why |= VRA_WHY_OUT_OF_RANGE;
        else if (!t->in_range && a->cfg.mode[t->cls] == VRA_VR) o->why |= VRA_WHY_NO_LOS;
        if (flag & 0x80u) o->why |= VRA_WHY_INVISIBLE;
        if (group != 0 && !(group & a->cur_group)) o->why |= VRA_WHY_NOT_IN_GROUP;
        if (!(o->why & (VRA_WHY_INVISIBLE | VRA_WHY_NOT_IN_GROUP | VRA_WHY_OUT_OF_RANGE | VRA_WHY_NO_LOS | VRA_WHY_NOT_VR)) && o->bound_mode == 0)
            o->why |= VRA_WHY_BOUND_ZERO;
    }
    return n;
}
