/* Enlace de funciones HLE por firmas, en ejecución (ver hlebind.h).
 *
 * FICHERO DE FIRMAS (formato "sigs", JSON; subconjunto del que produce tools/sig/sigs.py make):
 *   [ {"name": "VSync", "addr": "800996d0", "words": 132,
 *      "prefix": "<sha1 hex, 40>", "full": "<sha1 hex, 40>", "role": "hle"|"anchor"}, ... ]
 *   - addr: dirección de la función en el ejecutable de REFERENCIA (hexadecimal, sin 0x). Solo se usa para
 *     ordenar y para medir distancias entre funciones vecinas; no es una dirección del juego del usuario.
 *   - words: longitud en palabras; prefix: sha1 de las 8 primeras palabras ENMASCARADAS; full: sha1 de las
 *     `words` palabras enmascaradas. Palabras en little-endian. Enmascarado (hle_sig_mask): se ponen a 0
 *     los 16 bits bajos de las instrucciones lui/addiu/addi/ori/lb/lh/lwl/lw/lbu/lhu/lwr/sb/sh/swl/sw/swr/
 *     lwc2/swc2 y los 26 bits bajos de j/jal. Nunca se guardan bytes de código.
 *   - fd / pd (opcionales, por defecto 1): cuántas funciones de la referencia COMPLETA comparten el hash `full` / `prefix`
 *     de esta (las calcula hlesigs.py, que ve toda la lista; el subconjunto del fichero no basta para saberlo). Una
 *     firma con fd>1 (gemelas: mismo código enmascarado) no se empareja por hash exacto, y con pd>1 no por prefijo:
 *     un candidato único en el EXE no demuestra nada si hay varias funciones de la librería indistinguibles.
 *   - role (opcional, informativo): "hle" = función a sustituir; "anchor" = vecina de contexto. El runtime
 *     decide qué es HLE por el NOMBRE (hle_table, generada de runtime/hle*.txt + setjmp); el resto de
 *     entradas solo sirven de anclas. Admite el sigs_ref.json completo, pero se genera solo el subconjunto
 *     necesario con tools/sig/hlesigs.py (HLE + 4 vecinas por lado) para que el fichero sea pequeño.
 *
 * EMPAREJAMIENTO (port a C del `match` de tools/sig/sigs.py y del mínimo de align.py):
 *   1. exacta: la ventana de `words` palabras enmascaradas del EXE con el mismo prefix y full, única;
 *   2. prefijo: prefix único entre las posiciones no usadas por otra firma;
 *   3. delta: dos anclas vecinas (ya halladas) con el mismo desplazamiento ref->EXE y la posición resultante
 *      es inicio de función (aproximación: destino de jal o instrucción tras `jr ra`+delay slot);
 *   4. orden: entre dos anclas hay el mismo nº de funciones en la referencia que inicios en el EXE;
 *   5. delta de la ancla previa/siguiente si cae en un inicio de función.
 * VERIFICACIÓN: los emparejamientos que no son por hash exacto (2-5) exigen además que el candidato tenga forma de función
 * del tamaño esperado (shape_ok: cabe en el texto, un `jr ra`/`j` entre sus 6 últimas palabras y, o ningún inicio de función
 * conocido en su interior, o un inicio justo al final de la ventana). Dos firmas que acaban en la misma dirección se descartan ambas (una de las dos está mal).
 * Si una HLE no aparece se avisa y se deja su código original (interpretado): preferible a enlazar la función equivocada. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu.h"
#include "hlebind.h"
#include "rtpatch.h"
#include "disc_img.h"
#include "sys.h"

#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "hlebind.c asume host little-endian"
#endif

/* ------------------------------------------------------------------ SHA-1 */
typedef struct { uint32_t h[5]; uint8_t buf[64]; uint64_t len; unsigned n; } Sha1;
#define ROL(x, k) (((x) << (k)) | ((x) >> (32 - (k))))
static void sha1_block(uint32_t h[5], const uint8_t *p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; i++) w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
        uint32_t t = ROL(a, 5) + f + e + k + w[i];
        e = d; d = c; c = ROL(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}
static void sha1(const void *data, size_t n, uint8_t out[20]) {
    uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    const uint8_t *p = data;
    uint8_t tail[128];
    size_t full = n & ~(size_t)63;
    for (size_t i = 0; i < full; i += 64) sha1_block(h, p + i);
    size_t r = n - full;
    memcpy(tail, p + full, r);
    tail[r++] = 0x80;
    size_t tl = r <= 56 ? 64 : 128;
    memset(tail + r, 0, tl - r);
    uint64_t bits = (uint64_t)n * 8;
    for (int i = 0; i < 8; i++) tail[tl - 1 - i] = (uint8_t)(bits >> (8 * i));
    sha1_block(h, tail);
    if (tl == 128) sha1_block(h, tail + 64);
    for (int i = 0; i < 5; i++) { out[4 * i] = (uint8_t)(h[i] >> 24); out[4 * i + 1] = (uint8_t)(h[i] >> 16); out[4 * i + 2] = (uint8_t)(h[i] >> 8); out[4 * i + 3] = (uint8_t)h[i]; }
}
void hle_sig_hash(const uint32_t *w, unsigned n, uint8_t out[20]) { sha1(w, (size_t)n * 4, out); }

/* Enmascarado idéntico a tools/sig/sigs.py (MASK_IMM_OPS). */
uint32_t hle_sig_mask(uint32_t w) {
    uint32_t op = w >> 26;
    if (op == 2 || op == 3) return w & 0xFC000000u;
    static const uint64_t imm_ops = (1ull << 0x08) | (1ull << 0x09) | (1ull << 0x0D) | (1ull << 0x0F) | (1ull << 0x20) | (1ull << 0x21) |
        (1ull << 0x22) | (1ull << 0x23) | (1ull << 0x24) | (1ull << 0x25) | (1ull << 0x26) | (1ull << 0x28) | (1ull << 0x29) | (1ull << 0x2A) |
        (1ull << 0x2B) | (1ull << 0x2E) | (1ull << 0x32) | (1ull << 0x3A);
    if ((imm_ops >> op) & 1) return w & 0xFFFF0000u;
    return w;
}

/* ------------------------------------------------------------------ JSON mínimo */
typedef struct { char name[64]; uint32_t ref, words, fd, pd; uint8_t pre[20], full[20]; int hle, found, how; uint32_t pos; } Sig;
enum { H_NONE, H_EXACT, H_PREFIX, H_DELTA, H_ORDEN, H_DPREV, H_DNEXT };
static const char *how_name[] = {"-", "exacta", "prefijo", "delta", "orden", "delta_prev", "delta_next"};

typedef struct { const char *p, *e; } Cur;
static void ws(Cur *c) { while (c->p < c->e && isspace((unsigned char)*c->p)) c->p++; }
static int jstr(Cur *c, char *out, size_t cap) {
    ws(c);
    if (c->p >= c->e || *c->p != '"') return -1;
    c->p++;
    size_t n = 0;
    while (c->p < c->e && *c->p != '"') {
        if (*c->p == '\\' && c->p + 1 < c->e) c->p++;
        if (n + 1 < cap) out[n++] = *c->p;
        c->p++;
    }
    if (c->p >= c->e) return -1;
    c->p++;
    out[n] = 0;
    return (int)n;
}
static int hex20(const char *s, uint8_t out[20]) {
    if (strlen(s) != 40) return -1;
    for (int i = 0; i < 20; i++) {
        unsigned v = 0;
        for (int k = 0; k < 2; k++) {
            char ch = s[2 * i + k];
            v <<= 4;
            if (ch >= '0' && ch <= '9') v |= (unsigned)(ch - '0');
            else if (ch >= 'a' && ch <= 'f') v |= (unsigned)(ch - 'a' + 10);
            else if (ch >= 'A' && ch <= 'F') v |= (unsigned)(ch - 'A' + 10);
            else return -1;
        }
        out[i] = (uint8_t)v;
    }
    return 0;
}

static int is_hle_name(const char *n) {
    for (unsigned i = 0; i < hle_table_count; i++) if (!strcmp(hle_table[i].name, n)) return 1;
    return 0;
}

static int cmp_sig(const void *a, const void *b) {
    uint32_t x = ((const Sig *)a)->ref, y = ((const Sig *)b)->ref;
    return x < y ? -1 : x > y;
}

/* Devuelve el nº de firmas válidas (>=0) o -1 si el JSON no es una lista de objetos. */
static int parse_sigs(const char *json, size_t len, Sig **out) {
    Cur c = {json, json + len};
    ws(&c);
    if (c.p >= c.e || *c.p++ != '[') return -1;
    Sig *v = NULL;
    size_t n = 0, cap = 0;
    for (;;) {
        ws(&c);
        if (c.p < c.e && *c.p == ']') break;
        if (c.p >= c.e || *c.p++ != '{') { free(v); return -1; }
        Sig s;
        memset(&s, 0, sizeof s);
        unsigned have = 0;
        for (;;) {
            ws(&c);
            if (c.p < c.e && *c.p == '}') { c.p++; break; }
            char key[32], val[96];
            if (jstr(&c, key, sizeof key) < 0) { free(v); return -1; }
            ws(&c);
            if (c.p >= c.e || *c.p++ != ':') { free(v); return -1; }
            ws(&c);
            if (c.p < c.e && *c.p == '"') {
                if (jstr(&c, val, sizeof val) < 0) { free(v); return -1; }
                if (!strcmp(key, "name")) { snprintf(s.name, sizeof s.name, "%s", val); have |= 1; }
                else if (!strcmp(key, "addr")) { s.ref = (uint32_t)strtoul(val, NULL, 16); have |= 2; }
                else if (!strcmp(key, "prefix")) { if (hex20(val, s.pre) == 0) have |= 8; }
                else if (!strcmp(key, "full")) { if (hex20(val, s.full) == 0) have |= 16; }
            } else {
                const char *b = c.p;
                while (c.p < c.e && *c.p != ',' && *c.p != '}' && !isspace((unsigned char)*c.p)) c.p++;
                if (!strcmp(key, "words")) { s.words = (uint32_t)strtoul(b, NULL, 10); have |= 4; }
                else if (!strcmp(key, "fd")) s.fd = (uint32_t)strtoul(b, NULL, 10);
                else if (!strcmp(key, "pd")) s.pd = (uint32_t)strtoul(b, NULL, 10);
            }
            ws(&c);
            if (c.p < c.e && *c.p == ',') c.p++;
        }
        if (!s.fd) s.fd = 1;
        if (!s.pd) s.pd = 1;
        if (have == 31 && s.words >= 8) {
            if (n == cap) { cap = cap ? cap * 2 : 256; v = realloc(v, cap * sizeof *v); if (!v) return -1; }
            v[n++] = s;
        }
        ws(&c);
        if (c.p < c.e && *c.p == ',') c.p++;
    }
    qsort(v, n, sizeof *v, cmp_sig);
    for (size_t i = 0; i < n; i++) v[i].hle = is_hle_name(v[i].name);
    *out = v;
    return (int)n;
}

/* ------------------------------------------------------------------ registro por dirección */
#define MAX_BOUND 128
static struct { uint32_t addr; FuncPtr fn; const char *name; } bound[MAX_BOUND];
static unsigned nbound, nmissing;
uint8_t hle_bind_bits[PSX_RAM_SIZE / 32];

/* longjmp plano: en el intérprete no hay marcos C que reanudar; basta restaurar los registros del buffer y
 * devolver a la ra restaurada (el intérprete continúa en ra tras el HLE). */
static void flat_longjmp(Cpu *c) {
    static const int regs[] = {31, 28, 29, 30, 16, 17, 18, 19, 20, 21, 22, 23};
    static const int offs[] = {0, 44, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40};
    uint32_t buf = c->r[4], val = c->r[5];
    for (int k = 0; k < 12; k++) c->r[regs[k]] = mem_r32(c, buf + (uint32_t)offs[k]);
    c->r[2] = val;
}

static void bound_clear(void) {
    memset(hle_bind_bits, 0, sizeof hle_bind_bits);
    nbound = nmissing = 0;
}

static void bound_add(const char *name, uint32_t addr, FuncPtr fn) {
    if (nbound >= MAX_BOUND) return;
    for (unsigned i = 0; i < nbound; i++)
        if (bound[i].addr == addr) { fprintf(stderr, "[hle] aviso: %s y %s caen en 0x%08x; se conserva la primera\n", bound[i].name, name, addr); return; }
    unsigned k = nbound++;
    while (k > 0 && bound[k - 1].addr > addr) { bound[k] = bound[k - 1]; k--; }
    bound[k].addr = addr; bound[k].fn = fn; bound[k].name = name;
    uint32_t w = (addr & (PSX_RAM_SIZE - 1)) >> 2;
    hle_bind_bits[w >> 3] |= (uint8_t)(1u << (w & 7));
}

FuncPtr hle_bind_lookup(uint32_t a) {
    a = (a & 0x1FFFFFFFu) | 0x80000000u;
    unsigned lo = 0, hi = nbound;
    while (lo < hi) { unsigned m = (lo + hi) / 2; if (bound[m].addr < a) lo = m + 1; else hi = m; }
    return lo < nbound && bound[lo].addr == a ? bound[lo].fn : NULL;
}
const char *hle_bind_name(uint32_t a) {
    a = (a & 0x1FFFFFFFu) | 0x80000000u;
    for (unsigned i = 0; i < nbound; i++) if (bound[i].addr == a) return bound[i].name;
    return NULL;
}
uint32_t hle_bind_addr(const char *name) {
    for (unsigned i = 0; i < nbound; i++) if (!strcmp(bound[i].name, name)) return bound[i].addr;
    return 0;
}
unsigned hle_bind_missing(void) { return nmissing; }

/* HLE de dirección fija (no se buscan por firma): hle_bind las omite sin avisar y se registran aparte con hle_bind_fixed. */
#define MAX_FIXED 32
static const char *fixed_decl[MAX_FIXED];
static unsigned nfixed_decl;
void hle_bind_declare_fixed(const char *name) {
    for (unsigned i = 0; i < nfixed_decl; i++) if (!strcmp(fixed_decl[i], name)) return;
    if (nfixed_decl < MAX_FIXED) fixed_decl[nfixed_decl++] = name;
}
static int is_fixed_decl(const char *name) {
    for (unsigned i = 0; i < nfixed_decl; i++) if (!strcmp(fixed_decl[i], name)) return 1;
    return 0;
}
int hle_bind_fixed(const char *name, uint32_t addr) {
    if ((addr & 3) || (addr & 0x1FFFFFFFu) < 0x10000u || (addr & 0x1FFFFFFFu) >= PSX_RAM_SIZE) return -1;
    if (rt_patch_is(addr)) return -1;     /* ya hay una sustitución en ejecución (rtpatch.c): el intérprete la preferiría y la HLE quedaría muda */
    for (unsigned t = 0; t < hle_table_count; t++) {
        if (strcmp(hle_table[t].name, name)) continue;
        unsigned before = nbound;
        bound_add(hle_table[t].name, (addr & 0x1FFFFFFFu) | 0x80000000u, hle_table[t].fn);
        return nbound > before ? 0 : -1;
    }
    return -1;
}

void interp_call_raw(Cpu *c, uint32_t entry);
int hle_bind_orig(Cpu *c, const char *name) {
    uint32_t a = hle_bind_addr(name);
    if (!a) return 0;
    interp_call_raw(c, a);
    return 1;
}

/* ------------------------------------------------------------------ emparejamiento */
static uint32_t *mw;        /* palabras enmascaradas del texto */
static unsigned nw;
static uint64_t *hkey;      /* tabla hash de prefijos: clave = 8 primeros bytes del sha1 */
static uint32_t *hpos;      /* posición+1 (0 = libre) */
static unsigned hmask;

static inline unsigned hslot(uint64_t k) { return (unsigned)((k * 0x9E3779B97F4A7C15ull) >> 40) & hmask; }

static uint64_t key_of(const uint8_t d[20]) { uint64_t k; memcpy(&k, d, 8); return k; }

/* Posiciones cuyo prefijo coincide con pre (se verifica el sha1 completo). Devuelve el nº (máx cap). */
static unsigned cands(const uint8_t pre[20], uint32_t *out, unsigned cap) {
    uint64_t k = key_of(pre);
    unsigned n = 0;
    for (unsigned s = hslot(k); hpos[s]; s = (s + 1) & hmask) {
        if (hkey[s] != k) continue;
        uint32_t p = hpos[s] - 1;
        uint8_t d[20];
        hle_sig_hash(mw + p, 8, d);
        if (!memcmp(d, pre, 20) && n < cap) out[n++] = p;
    }
    return n;
}

/* Inicios de función (port reducido de find_funcs de tools/recomp/recomp.py): destinos de jal, el punto de
 * entrada del EXE, punteros a función en datos y punteros construidos con lui+addiu/ori que apuntan justo tras
 * el fin de otra función (jr ra / j + slot) o a un prólogo `addiu sp,sp,-X`. Las anclas halladas se añaden
 * después. Sirve para validar los emparejamientos por distancia y orden. */
static uint8_t *starts;
static int after_end(uint32_t base, const uint32_t *raw, uint32_t v) {
    uint32_t i = (v - base) >> 2;
    uint32_t p = i >= 2 ? raw[i - 2] : 0, w = raw[i];
    return p == 0x03E00008u || (p >> 26) == 2 || ((w >> 16) == 0x27BDu && (w & 0x8000u));
}
static void build_starts(uint32_t base, const uint32_t *raw, uint32_t entry) {
    starts = calloc(nw + 1, 1);
    if (!starts) return;
    uint32_t end = base + nw * 4u;
    if (entry >= base && entry < end) starts[(entry - base) >> 2] = 1;
    for (unsigned i = 0; i < nw; i++) {
        uint32_t w = raw[i];
        if ((w >> 26) == 3) {
            uint32_t t = ((w & 0x3FFFFFFu) << 2) | (base & 0xF0000000u);
            if (t >= base && t < end) starts[(t - base) >> 2] = 1;
        }
        if (w >= base && w < end && !(w & 3) && after_end(base, raw, w)) starts[(w - base) >> 2] = 1;
    }
    uint32_t hv[32], ha[32];
    unsigned char hs[32] = {0};
    int64_t clear_after = -1;
    for (unsigned i = 0; i < nw; i++) {
        uint32_t w = raw[i], op = w >> 26, a = base + 4 * i;
        unsigned rs = (w >> 21) & 31;
        if (op == 15) { unsigned rt = (w >> 16) & 31; hv[rt] = (w & 0xFFFFu) << 16; ha[rt] = a; hs[rt] = 1; }
        else if ((op == 9 || op == 13) && hs[rs] && a - ha[rs] <= 32) {
            uint32_t v = hv[rs] + (op == 9 ? (uint32_t)(int32_t)(int16_t)(w & 0xFFFFu) : (w & 0xFFFFu));
            if (v >= base && v < end && !(v & 3) && after_end(base, raw, v)) starts[(v - base) >> 2] = 1;
        }
        if (clear_after == (int64_t)a) { memset(hs, 0, sizeof hs); clear_after = -1; }
        if (op == 2 || op == 3 || (op == 0 && ((w & 63) == 8 || (w & 63) == 9))) clear_after = (int64_t)a + 4;
    }
}

static void found(Sig *s, uint32_t pos, int how) { s->found = 1; s->pos = pos; s->how = how; }

static const uint32_t *rawp;   /* texto sin enmascarar (vive durante hle_bind) */

/* ¿Tiene el candidato forma de función de `words` palabras? (ver VERIFICACIÓN en la cabecera) */
static int shape_ok(uint32_t pos, uint32_t words) {
    if (!rawp || !starts || (uint64_t)pos + words > nw) return 0;
    int tail = 0;
    for (uint32_t k = 1; k <= 6 && k <= words; k++) {
        uint32_t w = rawp[pos + words - k];
        if (w == 0x03E00008u || (w >> 26) == 2) tail = 1;
    }
    if (!tail) return 0;
    /* Fin coherente: ningún inicio de función en su interior (las últimas palabras pueden pertenecer ya a la siguiente:
     * el tamaño de la referencia incluye relleno) o, si los hay (entradas internas, falsos inicios), un inicio justo al
     * final de la ventana. */
    int inside = 0, next_ok = 0;
    for (uint32_t q = pos + 1; q + 4 < pos + words; q++) inside += starts[q];
    for (uint32_t q = pos + words - 4; q <= pos + words + 8 && q < nw; q++) next_ok |= starts[q];
    if (inside && !next_ok) return 0;
    return 1;
}

/* Dos firmas halladas en la misma dirección: ninguna es fiable. */
static void drop_dups(Sig *sg, unsigned n) {
    for (unsigned i = 0; i < n; i++) {
        if (!sg[i].found) continue;
        for (unsigned j = i + 1; j < n; j++)
            if (sg[j].found && sg[j].pos == sg[i].pos) {
                fprintf(stderr, "[hle] aviso: '%s' y '%s' se emparejan en la misma dirección; se descartan\n", sg[i].name, sg[j].name);
                sg[j].found = 0; sg[i].found = 0;
                break;
            }
    }
}

static void match_all(Sig *sg, unsigned n, uint32_t base) {
    enum { CAP = 4096 };
    uint32_t *cl = malloc(CAP * sizeof *cl);
    uint8_t *used = calloc(nw + 1, 1);
    if (!cl || !used) { free(cl); free(used); return; }
    /* 1. exacta */
    for (unsigned i = 0; i < n; i++) {
        Sig *s = &sg[i];
        unsigned nc = cands(s->pre, cl, CAP), ok = 0;
        uint32_t last = 0;
        for (unsigned k = 0; k < nc; k++) {
            if ((uint64_t)cl[k] + s->words > nw) continue;
            uint8_t d[20];
            hle_sig_hash(mw + cl[k], s->words, d);
            if (!memcmp(d, s->full, 20)) { ok++; last = cl[k]; }
        }
        if (ok == 1 && s->fd == 1) { found(s, last, H_EXACT); used[last] = 1; }
    }
    /* 2. prefijo único entre las posiciones libres */
    for (unsigned i = 0; i < n; i++) {
        Sig *s = &sg[i];
        if (s->found) continue;
        unsigned nc = cands(s->pre, cl, CAP), ok = 0;
        uint32_t last = 0;
        for (unsigned k = 0; k < nc; k++) if (!used[cl[k]]) { ok++; last = cl[k]; }
        if (ok == 1 && s->pd == 1 && s->fd == 1 && shape_ok(last, s->words)) { found(s, last, H_PREFIX); used[last] = 1; }
    }
    drop_dups(sg, n);
    if (starts) for (unsigned i = 0; i < n; i++) if (sg[i].found) starts[sg[i].pos] = 1;
    /* 3-5. por vecindad con las anclas halladas (solo para las HLE) */
    unsigned na = 0;
    uint32_t *ar = malloc((n + 1) * sizeof *ar);       /* índices de firma ancla, orden de ref */
    if (ar) for (unsigned i = 0; i < n; i++) if (sg[i].found) ar[na++] = i;
    for (unsigned i = 0; ar && i < n; i++) {
        Sig *s = &sg[i];
        if (!s->hle || s->found || !starts) continue;
        unsigned k = 0;
        while (k < na && sg[ar[k]].ref < s->ref) k++;
        if (k == 0 || k == na) continue;
        const Sig *p1 = &sg[ar[k - 1]], *p2 = &sg[ar[k]];
        /* desplazamiento en bytes: destino EXE - ref (ambos con base de sus ejecutables) */
        int64_t t1 = (int64_t)base + 4 * (int64_t)p1->pos, t2 = (int64_t)base + 4 * (int64_t)p2->pos;
        int64_t d1 = t1 - (int64_t)p1->ref, e1 = t2 - (int64_t)p2->ref;
        int64_t c1 = (int64_t)s->ref + d1, c2 = (int64_t)s->ref + e1;
#define IS_START(t) ((t) >= (int64_t)base && (t) < (int64_t)base + 4 * (int64_t)nw && !((t) & 3) && starts[((t) - (int64_t)base) >> 2])
#define CAND(t) ((uint32_t)(((t) - (int64_t)base) >> 2))
        if (d1 == e1 && IS_START(c1) && shape_ok(CAND(c1), s->words)) { found(s, CAND(c1), H_DELTA); continue; }
        /* orden: nº de firmas entre las anclas (referencia) = nº de inicios entre sus destinos (EXE) */
        unsigned nref = 0, idx = 0, nst = 0, hit = 0xFFFFFFFFu;
        for (unsigned j = 0; j < n; j++) {
            if (sg[j].ref <= p1->ref || sg[j].ref >= p2->ref) continue;
            if (&sg[j] == s) idx = nref;
            nref++;
        }
        for (uint32_t q = p1->pos + 1; q < p2->pos; q++)
            if (starts[q]) { if (nst == idx) hit = q; nst++; }
        if (nref == nst && hit != 0xFFFFFFFFu && shape_ok(hit, s->words)) { found(s, hit, H_ORDEN); continue; }
        if (IS_START(c1) && shape_ok(CAND(c1), s->words)) found(s, CAND(c1), H_DPREV);
        else if (IS_START(c2) && shape_ok(CAND(c2), s->words)) found(s, CAND(c2), H_DNEXT);
    }
    drop_dups(sg, n);
    free(ar); free(cl); free(used);
}

int hle_bind(const char *json, size_t len, uint32_t base, uint32_t size, uint32_t entry) {
    bound_clear();
    Sig *sg = NULL;
    int n = parse_sigs(json, len, &sg);
    if (n < 0) { fprintf(stderr, "[hle] fichero de firmas ilegible\n"); return -1; }
    if (((base & 0x1FFFFF) + size) > PSX_RAM_SIZE) { free(sg); return -1; }
    nw = size / 4;
    unsigned tsz = 1024;
    while (tsz < 2u * nw) tsz <<= 1;
    mw = malloc((nw + 1) * sizeof *mw);
    uint32_t *raw = malloc((nw + 1) * sizeof *raw);
    hkey = calloc(tsz, sizeof *hkey);
    hpos = calloc(tsz, sizeof *hpos);
    hmask = tsz - 1;
    if (!mw || !raw || !hkey || !hpos) { free(sg); free(mw); free(raw); free(hkey); free(hpos); return -1; }
    memcpy(raw, &psx_ram[base & (PSX_RAM_SIZE - 1)], (size_t)nw * 4);
    rawp = raw;
    for (unsigned i = 0; i < nw; i++) mw[i] = hle_sig_mask(raw[i]);
    for (unsigned i = 0; i + 8 <= nw; i++) {
        uint8_t d[20];
        hle_sig_hash(mw + i, 8, d);
        uint64_t k = key_of(d);
        unsigned s = hslot(k);
        while (hpos[s]) s = (s + 1) & hmask;
        hkey[s] = k; hpos[s] = i + 1;
    }
    build_starts(base, raw, entry);
    match_all(sg, (unsigned)n, base);
    int verbose = getenv("MV_HLELOG") != NULL, bound_n = 0, essential_missing = 0;
    unsigned searched = 0;
    for (unsigned t = 0; t < hle_table_count; t++) {
        const char *nm = hle_table[t].name;
        const Sig *hit = NULL;
        if (is_fixed_decl(nm)) continue;   /* dirección fija: la registra hle_bind_fixed */
        searched++;
        for (int i = 0; i < n; i++) if (sg[i].hle && !strcmp(sg[i].name, nm)) { hit = &sg[i]; break; }
        if (!hit) { fprintf(stderr, "[hle] aviso: '%s' no está en el fichero de firmas; se conserva el código original\n", nm); nmissing++; continue; }
        if (!hit->found) {
            fprintf(stderr, "[hle] aviso: '%s' no se encuentra en el ejecutable; se conserva el código original\n", nm);
            nmissing++;
            /* Entrada del mando: sin estas dos el juego correría el código original contra hardware inexistente y los
             * controles no responderían sin ningún error visible. Se falla en voz alta (la firma está, el ejecutable no casa). */
            if (pad_hle_required(hle_table[t].fn)) {
                fprintf(stderr, "[hle] ERROR: HLE de entrada '%s' sin enlazar: el mando no funcionaría\n", nm);
                essential_missing++;
            }
            continue;
        }
        uint32_t addr = base + 4 * hit->pos;
        FuncPtr fn = strcmp(nm, "longjmp") ? hle_table[t].fn : flat_longjmp;
        unsigned before = nbound;
        bound_add(nm, addr, fn);
        if (nbound > before) bound_n++;
        if (verbose) fprintf(stderr, "[hle] %-18s 0x%08x %s\n", nm, addr, how_name[hit->how]);
    }
    free(sg); free(mw); free(raw); free(hkey); free(hpos); free(starts);
    rawp = NULL; mw = NULL; hkey = NULL; hpos = NULL; starts = NULL;
    fprintf(stderr, "[hle] %d/%u funciones HLE enlazadas por firmas\n", bound_n, searched);
    return essential_missing ? -1 : bound_n;
}

int hle_bind_file(const char *path, uint32_t base, uint32_t size, uint32_t entry) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "[hle] no se puede abrir el fichero de firmas '%s'\n", mv_base(path)); return -1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > (64l << 20)) { fclose(f); return -1; }
    char *buf = malloc((size_t)n + 1);
    if (!buf) { fclose(f); return -1; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    int r = hle_bind(buf, got, base, size, entry);
    free(buf);
    return r;
}
