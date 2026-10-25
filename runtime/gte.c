/* GTE (COP2) exacta en enteros, para validación diferencial contra emulador de referencia.
 * La GTE en float para VR irá aparte; esta es la fuente de verdad del comportamiento original.
 * Semántica según la documentación pública del hardware (MAC 44 bits, saturaciones, flags, UNR). */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "cpu.h"
#include "gpu.h"   /* GpuHpInfo (solo el tipo; la GTE no depende del rasterizador) */
#include "hps.h"

static uint32_t hp_sxyt[3];   /* etiqueta de vértice (hps.h) de SXY0..2: ver "Asociación por dirección" */
static int32_t D[32];   /* datos */
static int32_t C[32];   /* control */
static uint32_t flag;

static void hp_clear(void);
void gte_reset(void) { memset(D, 0, sizeof D); memset(C, 0, sizeof C); flag = 0; hp_clear(); }

/* --- acceso a campos --- */
#define LO16(x) ((int16_t)((x) & 0xFFFF))
#define HI16(x) ((int16_t)((uint32_t)(x) >> 16))
static int16_t VX(int v) { return LO16(D[v * 2]); }
static int16_t VY(int v) { return HI16(D[v * 2]); }
static int16_t VZ(int v) { return LO16(D[v * 2 + 1]); }
#define IR0 D[8]
#define IR(i) D[8 + (i)]
#define MAC0 D[24]
#define MAC(i) D[24 + (i)]
#define SXY(i) D[12 + (i)]
#define SZ(i) D[16 + (i)]
#define RGBC ((uint32_t)D[6])

/* Matrices 3x3 en control 0-4 (RT), 8-12 (LLM), 16-20 (LCM). */
static int16_t mat(int base, int r, int col) {
    int idx = r * 3 + col;            /* 0..8 */
    int32_t w = C[base + idx / 2];
    return (idx & 1) ? HI16(w) : LO16(w);
}
#define TR(i) C[5 + (i)]
#define BK(i) C[13 + (i)]
#define FC(i) C[21 + (i)]
#define OFX C[24]
#define OFY C[25]
#define H ((uint16_t)C[26])
#define DQA ((int16_t)C[27])
#define DQB C[28]
#define ZSF3 ((int16_t)C[29])
#define ZSF4 ((int16_t)C[30])

/* --- comprobaciones --- */
static int64_t A(int i, int64_t v) {
    if (v > 0x7FFFFFFFFFFLL) flag |= 1u << (31 - i);
    else if (v < -0x80000000000LL) flag |= 1u << (28 - i);
    return (v << 20) >> 20; /* signo a 44 bits */
}
static int32_t setmac(int i, int64_t v, int sh) {
    A(i, v);
    MAC(i) = (int32_t)(v >> sh);
    return MAC(i);
}
static int32_t lim_ir(int i, int32_t v, int lm) {
    int32_t lo = lm ? 0 : -0x8000;
    if (v < lo) { flag |= 1u << (25 - i); return lo; }
    if (v > 0x7FFF) { flag |= 1u << (25 - i); return 0x7FFF; }
    return v;
}
static void mac_to_ir(int lm) {
    for (int i = 1; i <= 3; i++) IR(i) = lim_ir(i, MAC(i), lm);
}
static void F(int64_t v) {
    if (v > 0x7FFFFFFFLL) flag |= 1u << 16;
    else if (v < -0x80000000LL) flag |= 1u << 15;
}
static int32_t lim_c(int i, int32_t v) {     /* i=0 R, 1 G, 2 B */
    if (v < 0) { flag |= 1u << (21 - i); return 0; }
    if (v > 255) { flag |= 1u << (21 - i); return 255; }
    return v;
}
static int32_t lim_d(int64_t v) {
    if (v < 0) { flag |= 1u << 18; return 0; }
    if (v > 0xFFFF) { flag |= 1u << 18; return 0xFFFF; }
    return (int32_t)v;
}

static void push_color(void) {
    uint32_t col = (uint32_t)lim_c(0, MAC(1) >> 4) | ((uint32_t)lim_c(1, MAC(2) >> 4) << 8) |
                   ((uint32_t)lim_c(2, MAC(3) >> 4) << 16) | (RGBC & 0xFF000000u);
    D[20] = D[21]; D[21] = D[22]; D[22] = (int32_t)col;
}
static void push_sz(int32_t z) { SZ(0) = SZ(1); SZ(1) = SZ(2); SZ(2) = SZ(3); SZ(3) = z; }
static void push_sxy(int32_t x, int32_t y) {
    SXY(0) = SXY(1); SXY(1) = SXY(2);
    SXY(2) = (int32_t)((uint32_t)(x & 0xFFFF) | ((uint32_t)y << 16));
    hp_sxyt[0] = hp_sxyt[1]; hp_sxyt[1] = hp_sxyt[2]; hp_sxyt[2] = 0;
}

/* --- división UNR --- */
static uint8_t unr_table[257];
static int unr_ready;
static void unr_init(void) {
    for (int i = 0; i < 257; i++) {
        int v = (0x40000 / (i + 0x100) + 1) / 2 - 0x101;
        unr_table[i] = (uint8_t)(v < 0 ? 0 : v);
    }
    unr_ready = 1;
}
static uint32_t gte_divide(uint32_t h, uint32_t sz3) {
    if (h >= sz3 * 2) { flag |= 1u << 17; return 0x1FFFF; }
    if (!unr_ready) unr_init();
    int z = __builtin_clz(sz3 & 0xFFFF) - 16;
    uint64_t n = (uint64_t)h << z;
    uint32_t d = sz3 << z;
    uint32_t u = unr_table[(d - 0x7FC0) >> 7] + 0x101;
    d = (0x2000080 - d * u) >> 8;
    d = (0x0000080 + d * u) >> 8;
    uint64_t r = (n * d + 0x8000) >> 16;
    return r > 0x1FFFF ? 0x1FFFF : (uint32_t)r;
}


/* ====================== Alta precisión (sombra en coma flotante) ======================
 * Paralelo EXACTO al camino entero: RTPS/RTPT siguen calculando MAC/IR/SZ/SXY/flags con la misma aritmética de
 * siempre (los vectores de regresión no cambian); si la alta precisión está encendida (gte_hp_enable(1)) se calcula
 * además, en double y sin redondear ni saturar IR1/IR2:
 *     P = TR + R·V / 4096          posición en espacio de VISTA (el que deje la matriz RT que cargó el juego;
 *                                   si el juego compone modelo*cámara en RT es la cámara; el "mundo" no se puede
 *                                   recuperar en general porque la composición de matrices la hace el juego)
 *     pz = P.z                      profundidad de vista real (el hardware la trunca a SZ entero 0..65535)
 *     fx = H·P.x/pz + OFX/65536     posición de pantalla sin cuantizar (el hardware: trunca IR1 y SZ y divide con UNR)
 *     fy = H·P.y/pz + OFY/65536
 * y se guarda en una tabla indexada por el SXY entero que se empujó a la FIFO (la clave que verá el GPU cuando el
 * juego copie SXY a la primitiva). Solo se registra con sf=1 (todos los usos reales de RTPS/RTPT en juegos) y si
 * el hardware no saturó (con vértices saturados/detrás de la cámara se registra solo la vista y un flag; ver más abajo).
 *
 * Tabla: asociativa por conjuntos, HP_SETS conjuntos x HP_WAYS vías, hash de (sx,sy). Clave = (sx,sy) saturados a
 * 11 bits + SZ entero (dos vértices en el mismo píxel con distinta profundidad ocupan vías distintas; el mismo
 * (sx,sy,SZ) se actualiza en sitio). Reemplazo: vía inválida/caducada, si no la más antigua (seq).
 * Vaciado por frame: gte_hp_frame() (lo llama el cierre de frame de la captura de gpu.c) incrementa la generación;
 * una entrada vale si se escribió en la generación actual o la anterior (tolera que el juego transforme la geometría
 * del frame N+1 antes del flip del N). Colisiones/ambigüedad documentadas:
 *   - misma (sx,sy) con distinto SZ: ver "Asociación por secuencia" (más abajo).
 *   - conjunto lleno (4 vías con otras claves de la misma generación): se pierde la más antigua; el vértice
 *     afectado cae a vvalid=0 (se dibuja con el entero). Se cuenta en gte_hp_stats().
 *   - coincidencia espuria: una primitiva 2D (HUD) cuyos vértices coincidan con vértices GTE del frame; el
 *     consumidor exige vvalid/fvalid en TODOS los vértices y |float-entero| acotado, por lo que es muy improbable.
 *
 * VÉRTICES SATURADOS / DESBORDADOS / DETRÁS DE LA CÁMARA: también se registran, con la posición de vista P (que es correcta
 * aunque la proyección sature) y un flag: GPU_VF_SAT si el SXY entero saturó (±0x400/0x3FF; un SXY justo en el borde también
 * se trata así porque es indistinguible), la división desbordó (H >= 2·SZ), IR1/IR2 saturaron o Pz no cabe en SZ (>65535);
 * GPU_VF_BEHIND si Pz <= 0. En SAT, fx,fy = H·Px/Pz+OFX en double sin saturar (Pz>0); en BEHIND no hay proyección (fx=fy=0).
 * Solo los flags==0 alimentan fvalid (modo plano); todos alimentan vvalid.
 *
 * ASOCIACIÓN POR SECUENCIA (gte_hp_resolve). Problema: muchos vértices saturados comparten el mismo SXY de borde, y el GPU
 * solo ve el SXY (el SZ no llega a la primitiva). Cada vértice producido lleva un `seq` creciente (orden de salida de
 * RTPS/RTPT). Estructuras: tabla asociativa de vértices normales (clave sx,sy,SZ) y un ANILLO de HP_RING vértices con flag,
 * encadenados por hash de (sx,sy) (sin desalojos por colisión; solo por antigüedad), ambos con caducidad por generación.
 * Para una primitiva de n vértices se reúnen, por vértice, los candidatos con su (sx,sy) exacto (hasta HP_MAXC, los más
 * recientes del anillo). El juego escribe los vértices de una primitiva con SXY producidos por la GTE en el mismo trabajo
 * (RTPT de 3 + RTPS, o un RTPT por triángulo), así que sus `seq` son cercanos. Heurística:
 *   1. vértices con UN solo candidato = anclas (el caso normal y la mayoría de los saturados: un SXY de borde con la otra
 *      coordenada normal casi es único);
 *   2. cada vértice ambiguo (de menos a más candidatos) toma el candidato de máxima verosimilitud conjunta con las anclas ya
 *      resueltas (al resolverse pasa a ser ancla), sin repetir un candidato que ya tomó otro vértice de la misma primitiva.
 *      Verosimilitud de un par (vértice i, vértice j): log del conteo del histograma APRENDIDO EN LÍNEA de s_j - s_i para ese
 *      par de ranuras (i,j) y ese tipo (tri/quad), con bins exactos en ±32 y por octavas más lejos, más un suavizado
 *      1/(1+|d|) (sin datos = "cuanto más cerca en la secuencia, mejor"). El histograma se alimenta de las primitivas sin
 *      ambigüedad: cada juego escribe los vértices con un patrón estable (en este: RTPT de 3 + RTPS de 1 con ranuras
 *      0,1,3,2 -> deltas (1,3,2), o su inverso, o vértices indexados de una malla, spans de decenas a miles) y el prior lo recoge;
 *   3. sin ninguna ancla (todos ambiguos, p.ej. todos en la misma esquina): se elige el par (vértice, candidato) pivote de
 *      mayor verosimilitud conjunta con el mejor candidato de cada otro vértice; después se procede como en 2.
 *   (La distancia en el espacio de vista se probó como criterio extra y se descartó: un vértice saturado está por definición
 *   lejos de los de pantalla; gte_hp_tune lo deja disponible solo para medir.)
 * Tasa de acierto medida: ver bitácora (autoevaluación gte_hp_eval: recorta artificialmente la ventana de pantalla, trata
 * como 'saturados' los vértices fuera de ella y comprueba que la asociación devuelve la misma posición de vista que la
 * búsqueda sin recorte). */
#define HP_SETS 16384
#define HP_WAYS 4
#define HP_RING 32768
#define HP_RBUCKETS 16384
#define HP_MAXC 48
#define HP_PIVC 16 /* candidatos por vértice que puntúan el pivote de una primitiva sin ancla */
#define HP_MARGIN 1.0f /* diferencia mínima de log2-verosimilitud entre el mejor y el segundo candidato para fiarse */
typedef struct {
    int16_t sx, sy;
    uint16_t sz;
    uint8_t used;
    uint32_t gen, seq;
    GpuHpInfo i;
} HpE;
static HpE hp_tab[HP_SETS][HP_WAYS];
/* Anillo encadenado: id absoluto creciente (empieza en 1); la posición es id % HP_RING; head[] guarda el id más reciente
 * de cada cubo hash y prev el anterior de la misma cadena. Un id es válido si id > n - HP_RING y su generación vive. */
typedef struct { int16_t sx, sy; uint32_t gen, prev; GpuHpInfo i; } HpRE;
typedef struct { HpRE e[HP_RING]; uint32_t head[HP_RBUCKETS]; uint32_t n; } HpRing;
static HpRing hp_ring, hp_evr; /* hp_evr: solo autoevaluación */
static int hp_on, hp_dirty, hp_ev_on, hp_ev_x = 100, hp_ev_y = 80;
static uint32_t hp_gen, hp_seq;
static GteHpStats hp_st;

/* ---- Asociación por DIRECCIÓN de memoria (hps.h; estándar de oro de PGXP) ----
 * Por valor de SXY, dos vértices saturados con el mismo SXY son indistinguibles y la elección por secuencia puede fallar (quads de suelo
 * pegados a la cámara con 2 vértices saturados). El intérprete etiqueta cada palabra SXY que el juego guarda (swc2 / mfc2+sw), la propaga
 * en lw/sw/move/lwc2/mtc2 y la GPU resuelve cada palabra de vértice de la primitiva por su dirección. Cada vértice de la GTE se copia al
 * pool (indexado por seq; tag = seq+1, 0 = ninguno) con su clave de 32 bits (el SXY empujado): la etiqueta solo vale mientras el valor que
 * hay en memoria/registro sea esa clave y el vértice no haya caducado (HP_ADDR_GEN cierres de frame) ni lo haya sobrescrito el pool. */
#define HP_POOL 32768
#define HP_ADDR_GEN 4
typedef struct { uint32_t tag, key, gen; GpuHpInfo i; } HpPoolE;
static HpPoolE hp_pool[HP_POOL];
int hps_on;
uint32_t hps_mem[HPS_WORDS];
uint32_t hps_bm[HPS_WORDS / 32 + 1];
static int hps_dirty;

#define HP_NB 87
static uint32_t hp_pair[3][4][4][HP_NB]; /* clase: 0 triángulo, 1 quad, 2 línea (no se mezclan: otro patrón de escritura) */
static void hp_clear(void) {
    memset(hp_pair, 0, sizeof hp_pair);
    if (hp_dirty) {
        memset(hp_tab, 0, sizeof hp_tab);
        memset(&hp_ring, 0, sizeof hp_ring); memset(&hp_evr, 0, sizeof hp_evr);
        hp_dirty = 0;
    }
    if (hps_dirty) { memset(hp_pool, 0, sizeof hp_pool); memset(hps_mem, 0, sizeof hps_mem); memset(hps_bm, 0, sizeof hps_bm); hps_dirty = 0; }
    memset(hp_sxyt, 0, sizeof hp_sxyt);
    hp_gen = hp_seq = 0;
    memset(&hp_st, 0, sizeof hp_st);
}
void gte_hp_enable(int on) { hp_on = on; hps_on = on; }
int gte_hp_enabled(void) { return hp_on; }
void gte_hp_frame(void) { hp_gen++; hp_st.gen_stored = 0; }
void gte_hp_stats(GteHpStats *s) { *s = hp_st; }
void gte_hp_eval(int on, int ex, int ey) { hp_ev_on = on; if (ex > 0) hp_ev_x = ex; if (ey > 0) hp_ev_y = ey; }
static inline uint32_t hp_set(int sx, int sy) {
    return (((uint32_t)sx & 0x7FF) * 0x9E3779B1u ^ ((uint32_t)sy & 0x7FF) * 0x85EBCA6Bu) >> 18; /* 14 bits */
}
static inline uint32_t hp_bucket(int sx, int sy) {
    return (((uint32_t)sx & 0xFFFF) * 0x9E3779B1u ^ ((uint32_t)sy & 0xFFFF) * 0x85EBCA6Bu) >> 18;
}
static inline int hp_live(const HpE *e) { return e->used && (uint32_t)(hp_gen - e->gen) <= 1u; }
static inline int hp_edge(int sx, int sy) { return sx <= -0x400 || sx >= 0x3FF || sy <= -0x400 || sy >= 0x3FF; }

static void hp_store(int sx, int sy, uint32_t sz, const GpuHpInfo *in) {
    HpE *set = hp_tab[hp_set(sx, sy)], *v = NULL;
    hp_dirty = 1;
    for (int w = 0; w < HP_WAYS; w++) {
        HpE *e = &set[w];
        if (hp_live(e) && e->sx == sx && e->sy == sy && e->sz == sz) { v = e; break; }
    }
    if (!v) {
        for (int w = 0; w < HP_WAYS; w++) if (!hp_live(&set[w])) { v = &set[w]; break; }
        if (!v) { /* conjunto lleno: sustituye la más antigua */
            v = &set[0];
            for (int w = 1; w < HP_WAYS; w++) if ((int32_t)(set[w].seq - v->seq) < 0) v = &set[w];
            hp_st.evict++;
        }
    }
    v->used = 1; v->sx = (int16_t)sx; v->sy = (int16_t)sy; v->sz = (uint16_t)sz;
    v->gen = hp_gen; v->seq = in->seq; v->i = *in;
    hp_st.stored++;
    if (++hp_st.gen_stored > hp_st.max_gen_stored) hp_st.max_gen_stored = hp_st.gen_stored;
}

static void hp_ring_push(HpRing *r, int sx, int sy, const GpuHpInfo *in) {
    uint32_t id = ++r->n, b = hp_bucket(sx, sy);
    if (!id) id = ++r->n; /* id 0 = cadena vacía (la vuelta de 2^32 ids no debe producirlo) */
    HpRE *e = &r->e[id % HP_RING];
    if (id > HP_RING && (uint32_t)(hp_gen - e->gen) <= 1u) hp_st.ring_evict++; /* sobrescribe un vértice aún vivo */
    e->sx = (int16_t)sx; e->sy = (int16_t)sy; e->gen = hp_gen; e->prev = r->head[b]; e->i = *in;
    r->head[b] = id;
    hp_dirty = 1;
}
/* Reúne los vértices vivos con clave (sx,sy) de la cadena, del más reciente al más antiguo. */
static int hp_ring_gather(const HpRing *r, int sx, int sy, const GpuHpInfo **c, int max) {
    int n = 0, steps = 0;
    uint32_t id = r->head[hp_bucket(sx, sy)];
    while (id && (uint32_t)(r->n - id) < HP_RING && n < max && steps++ < 512) { /* resta modular: válido tras la vuelta de ids */
        const HpRE *e = &r->e[id % HP_RING];
        if ((uint32_t)(hp_gen - e->gen) > 1u) break; /* la cadena va de más nuevo a más viejo: el resto ya caducó */
        if (e->sx == sx && e->sy == sy) c[n++] = &e->i;
        id = e->prev;
    }
    return n;
}

int gte_hp_lookup(int sx, int sy, GpuHpInfo *out) {
    HpE *set = hp_tab[hp_set(sx, sy)], *best = NULL;
    /* Un SXY en el borde del rango (-0x400 / 0x3FF) es indistinguible de un vértice saturado: no se devuelve aquí
     * (gte_hp_resolve sí los resuelve, con flag). */
    if (hp_edge(sx, sy)) { hp_st.lookups++; return 0; }
    for (int w = 0; w < HP_WAYS; w++) {
        HpE *e = &set[w];
        if (!hp_live(e) || e->sx != sx || e->sy != sy) continue;
        if (!best || (int32_t)(e->seq - best->seq) > 0) best = e;
    }
    hp_st.lookups++;
    if (!best) return 0;
    hp_st.hits++;
    *out = best->i;
    return 1;
}

/* Candidatos de un vértice: tabla normal + anillo con flag. */
static int hp_gather(int sx, int sy, const GpuHpInfo **c, int max) {
    int n = 0;
    HpE *set = hp_tab[hp_set(sx, sy)];
    for (int w = 0; w < HP_WAYS && n < max; w++) {
        HpE *e = &set[w];
        if (hp_live(e) && e->sx == sx && e->sy == sy) c[n++] = &e->i;
    }
    return n + hp_ring_gather(&hp_ring, sx, sy, c + n, max - n);
}
/* Prior aprendido de la distancia en la secuencia de la GTE entre los vértices i y j de una primitiva (tri/quad por separado).
 * Se aprende en línea de las primitivas SIN ambigüedad (todos los vértices con un único candidato): el motor de un juego
 * escribe los vértices de la primitiva con un patrón estable (p.ej. RTPT de 3 + RTPS de 1 con orden de ranura 0,1,3,2, o un
 * grupo de vértices indexados de una malla) y el histograma de s_j - s_i lo recoge (exacto en ±32, por octavas más lejos).
 * Sin datos todos los bins valen lo mismo y solo manda la anchura del bin = "cuanto más cerca en seq, mejor". */
static int hp_bin(int32_t d, float *width) {
    if (d >= -32 && d <= 32) { *width = 1.0f; return d + 32; }
    uint32_t a = (uint32_t)(d < 0 ? -(int64_t)d : d); int e = 0;
    while ((a >> (e + 1)) != 0) e++;
    if (e < 5) e = 5;
    if (e > 15) e = 15;
    *width = (float)(1u << e);
    return 65 + (d > 0 ? 0 : 11) + (e - 5);
}
/* log2 de la verosimilitud (por unidad de seq) de que los vértices i (seq si) y j (seq sj) sean de la misma primitiva */
static float hp_ll(int q, int i, int j, uint32_t si, uint32_t sj) {
    float w; int32_t d = (int32_t)(sj - si); int b = hp_bin(d, &w);
    /* suavizado: sin datos manda "cuanto más cerca en seq mejor" (1/(1+|d|)); con datos mandan los conteos */
    (void)w; /* log2 de la anchura del bin: 0 en los exactos, el exponente en los de octava (b = 65 + (d<0?11:0) + e-5) */
    float lw = b <= 64 ? 0.0f : (float)((b - 65) % 11 + 5);
    return log2f((float)hp_pair[q][i][j][b] + 0.5f / (1.0f + (float)(d < 0 ? -(int64_t)d : d))) - lw;
}
static void hp_learn(int n, const GpuHpInfo *const *cand) {
    int q = n == 4 ? 1 : n == 2 ? 2 : 0;
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++) {
            if (i == j) continue;
            float w; int b = hp_bin((int32_t)(cand[j]->seq - cand[i]->seq), &w);
            if (++hp_pair[q][i][j][b] > 0x3FFFFFFFu) for (int k = 0; k < HP_NB; k++) hp_pair[q][i][j][k] >>= 1;
        }
}
/* Coherencia espacial (§56): los vértices de una misma primitiva están cerca en el espacio de VISTA (la posición de vista es correcta aunque el SXY
 * sature; lo que está lejos en pantalla no lo está en 3D). Penalización suave en bits, 0 en el mismo punto y creciente con la distancia: desempata
 * candidatos de igual secuencia y evita elegir un vértice lejano de otro objeto cuando la secuencia no basta. Peso HP_SPATIAL_W (env MV_HPSPATIAL;
 * 0 = desactivada; def. 1,0); escala 4096 unidades de vista. */
static float hp_spatial_w = -1.0f;
static inline float hp_spatial(const GpuHpInfo *a, const GpuHpInfo *b) {
    if (hp_spatial_w < 0) { const char *e = getenv("MV_HPSPATIAL"); hp_spatial_w = e ? (float)atof(e) : 1.0f; }
    if (hp_spatial_w == 0.0f) return 0.0f;
    float dx = a->vx - b->vx, dy = a->vy - b->vy, dz = a->vz - b->vz;
    float d = sqrtf(dx * dx + dy * dy + dz * dz);
    if (!(d == d)) return 0.0f;
    return -hp_spatial_w * log2f(1.0f + d * (1.0f / 4096.0f));
}
static inline float hp_pair_score(int q, int i, const GpuHpInfo *ci, int j, const GpuHpInfo *cj) {
    return hp_ll(q, j, i, cj->seq, ci->seq) + hp_ll(q, i, j, ci->seq, cj->seq) + hp_spatial(ci, cj);
}

/* ¿El candidato ya lo tomó otro vértice de la primitiva? (dos vértices con el mismo SXY son vértices distintos del mismo SXY) */
static int hp_used(int n, const GpuHpInfo *(*c)[HP_MAXC], const int *idx, int v, const GpuHpInfo *cand) {
    for (int j = 0; j < n; j++) if (j != v && idx[j] >= 0 && c[j][idx[j]] == cand) return 1;
    return 0;
}

/* Elige un candidato por vértice (idx[i] = -1 si no hay). cnt[i] candidatos en c[i][]. Heurística documentada arriba.
 * Devuelve 1 si no hubo ambigüedad (todos los vértices con 0 o 1 candidato). */
static int hp_select(int n, const GpuHpInfo *(*c)[HP_MAXC], const int *cnt, int *idx, uint8_t *low) {
    int anch[4], na = 0, pend[4], np = 0, q = n == 4 ? 1 : n == 2 ? 2 : 0;
    for (int i = 0; i < n; i++) {
        idx[i] = -1; low[i] = 0;
        if (cnt[i] == 1) { idx[i] = 0; anch[na++] = i; }
        else if (cnt[i] > 1) pend[np++] = i;
    }
    if (!np) return 1;
    hp_st.res_amb += (uint32_t)np;
    for (int a = 1; a < np; a++) { /* menos candidatos primero (inserción) */
        int t = pend[a], b = a;
        while (b > 0 && cnt[pend[b - 1]] > cnt[t]) { pend[b] = pend[b - 1]; b--; }
        pend[b] = t;
    }
    if (!na) { /* sin anclas: pivote (vértice, candidato) de máxima verosimilitud conjunta */
        hp_st.res_pivot++;
        for (int a = 0; a < np; a++) low[pend[a]] = 1; /* sin ancla: candidatos de la misma esquina indistinguibles */
        float bests = -1e30f; int bv = -1, bk = 0;
        /* coste acotado: solo los HP_PIVC candidatos más recientes de cada vértice puntúan el pivote (4*16*3*16 pares como mucho) */
        for (int a = 0; a < np; a++) {
            int va = pend[a], ka = cnt[va] < HP_PIVC ? cnt[va] : HP_PIVC;
            for (int k = 0; k < ka; k++) {
                const GpuHpInfo *s = c[va][k]; float tot = 0;
                for (int b = 0; b < np; b++) {
                    int vb = pend[b], kb = cnt[vb] < HP_PIVC ? cnt[vb] : HP_PIVC;
                    if (vb == va) continue;
                    float m = -1e30f;
                    for (int j = 0; j < kb; j++) { if (c[vb][j] == s) continue; float d = hp_pair_score(q, vb, c[vb][j], va, s); if (d > m) m = d; }
                    tot += m;
                }
                if (tot > bests || (tot == bests && s->seq > c[bv][bk]->seq)) { bests = tot; bv = va; bk = k; }
            }
        }
        idx[bv] = bk; anch[na++] = bv;
        for (int a = 0; a < np; a++) if (pend[a] == bv) { for (int b = a; b + 1 < np; b++) pend[b] = pend[b + 1]; np--; break; }
    }
    for (int a = 0; a < np; a++) {
        int v = pend[a], bi = -1; float bc = -1e30f, b2 = -1e30f;
        for (int k = 0; k < cnt[v]; k++) {
            if (hp_used(n, c, idx, v, c[v][k])) continue;
            float sc = 0;
            for (int j = 0; j < na; j++) sc += hp_pair_score(q, v, c[v][k], anch[j], c[anch[j]][idx[anch[j]]]);
            if (bi < 0 || sc > bc || (sc == bc && c[v][k]->seq > c[v][bi]->seq)) { b2 = bc; bc = sc; bi = k; }
            else if (sc > b2) b2 = sc;
        }
        if (bi >= 0 && b2 > -1e29f && bc - b2 < HP_MARGIN) low[v] = 1; /* el segundo candidato es casi igual de verosímil */
        if (bi < 0) low[v] = 1; /* todos sus candidatos ya los tomó otro vértice (>=3 vértices con el mismo SXY): se repite el más reciente, marcado AMBIG */
        idx[v] = bi < 0 ? 0 : bi; anch[na++] = v;
    }
    return 0;
}

/* Cuadrilátero de vértices de vista coplanar (distancia del 4.º al plano de los otros tres < 1 % de la mayor arista), sin triángulos degenerados y con orden de tira
 * válido: las normales de (0,1,2) y (1,2,3) apuntan en sentidos opuestos (intercambiar v1 y v3 también es coplanar, pero cruza el cuadrilátero). */
static int hp_quad_planar(const GpuHpInfo *const p[4]) {
    float e1[3], e2[3], e3[3], f1[3], f2[3];
    const float *a = &p[0]->vx, *b = &p[1]->vx, *c = &p[2]->vx, *d = &p[3]->vx;
    for (int k = 0; k < 3; k++) { e1[k] = b[k] - a[k]; e2[k] = c[k] - a[k]; e3[k] = d[k] - a[k]; f1[k] = c[k] - b[k]; f2[k] = d[k] - b[k]; }
    float n[3] = {e1[1]*e2[2] - e1[2]*e2[1], e1[2]*e2[0] - e1[0]*e2[2], e1[0]*e2[1] - e1[1]*e2[0]};
    float m[3] = {f1[1]*f2[2] - f1[2]*f2[1], f1[2]*f2[0] - f1[0]*f2[2], f1[0]*f2[1] - f1[1]*f2[0]};
    float nn = sqrtf(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]), mn = sqrtf(m[0]*m[0] + m[1]*m[1] + m[2]*m[2]);
    float l1 = sqrtf(e1[0]*e1[0] + e1[1]*e1[1] + e1[2]*e1[2]), l2 = sqrtf(e2[0]*e2[0] + e2[1]*e2[1] + e2[2]*e2[2]), l3 = sqrtf(e3[0]*e3[0] + e3[1]*e3[1] + e3[2]*e3[2]);
    float lm = fmaxf(l1, fmaxf(l2, l3));
    if (!(nn > 1e-3f * lm * lm) || !(mn > 1e-3f * lm * lm) || !isfinite(nn + mn)) return 0;
    if (!(fabsf(n[0]*e3[0] + n[1]*e3[1] + n[2]*e3[2]) / nn < 0.01f * lm)) return 0;
    return n[0]*m[0] + n[1]*m[1] + n[2]*m[2] < 0;
}

/* Cuadrilátero con candidatos ambiguos (mismo SXY, típico de los saturados): si la elección por secuencia no es plana, se prueban las combinaciones de candidatos
 * (hasta HP_MAXC por vértice con 1-2 ambiguos; menos con más) y se toma la plana de mayor verosimilitud. Si es la única plana de verosimilitud comparable, los
 * vértices dejan de ser AMBIG. Sin combinación plana (cuadrilátero no plano, vértice no registrado) no cambia nada. */
static void hp_planar_fix(const GpuHpInfo *(*c)[HP_MAXC], const int *cnt, int *idx, uint8_t *low) {
    int pv[4], np = 0, any_low = 0;
    for (int i = 0; i < 4; i++) {
        if (cnt[i] < 1 || idx[i] < 0) return;
        if (cnt[i] > 1) { pv[np++] = i; any_low |= low[i]; }
    }
    if (!np || !any_low) return;
    const GpuHpInfo *cur[4];
    for (int i = 0; i < 4; i++) cur[i] = c[i][idx[i]];
    if (hp_quad_planar(cur)) return;
    int lim = np <= 2 ? HP_MAXC : np == 3 ? 12 : 8, K[4] = {1, 1, 1, 1}, ci[4] = {0, 0, 0, 0}, best[4] = {0, 0, 0, 0}, nbest = 0, nclose = 0;
    for (int a = 0; a < np; a++) K[pv[a]] = cnt[pv[a]] < lim ? cnt[pv[a]] : lim;
    float bests = -1e30f;
    for (;;) {
        const GpuHpInfo *t[4];
        for (int i = 0; i < 4; i++) t[i] = c[i][ci[i]];
        int dup = 0;
        for (int i = 0; i < 4 && !dup; i++) for (int j = i + 1; j < 4; j++) if (t[i] == t[j]) { dup = 1; break; }
        if (!dup && hp_quad_planar(t)) {
            float sc = 0;
            for (int i = 0; i < 4; i++) for (int j = i + 1; j < 4; j++) sc += hp_pair_score(1, i, t[i], j, t[j]);
            if (!nbest || sc > bests) { nclose = nbest && sc - bests < HP_MARGIN; memcpy(best, ci, sizeof best); bests = sc; nbest = 1; }
            else if (bests - sc < HP_MARGIN) nclose = 1;
        }
        int a = 0;   /* siguiente combinación (contador mixto sobre los vértices ambiguos) */
        for (; a < np; a++) { int v = pv[a]; if (++ci[v] < K[v]) break; ci[v] = 0; }
        if (a == np) break;
    }
    if (!nbest) return;
    for (int a = 0; a < np; a++) { idx[pv[a]] = best[pv[a]]; low[pv[a]] = (uint8_t)nclose; }
}

/* Autoevaluación (gte_hp_eval): ventana artificial |sx-OFX|<ex, |sy-OFY|<ey; fuera de ella la clave se recorta al borde
 * (como un SXY saturado). hp_evr guarda esos vértices por clave recortada; aquí se compara con la resolución sin recorte. */
static inline void hp_evkey(const GpuHpInfo *t, int sx, int sy, int *kx, int *ky) {
    int ox = (int)t->ofx, oy = (int)t->ofy, dx = sx - ox, dy = sy - oy;
    *kx = ox + (dx <= -hp_ev_x ? -hp_ev_x : dx >= hp_ev_x ? hp_ev_x : dx);
    *ky = oy + (dy <= -hp_ev_y ? -hp_ev_y : dy >= hp_ev_y ? hp_ev_y : dy);
}
static void hp_eval(int n, const int *sx, const int *sy, const GpuHpInfo *truth) {
    const GpuHpInfo *c[4][HP_MAXC]; int cnt[4], idx[4], clamped[4], any = 0, kx[4], ky[4];
    for (int i = 0; i < n; i++) {
        hp_evkey(&truth[i], sx[i], sy[i], &kx[i], &ky[i]);
        clamped[i] = kx[i] != sx[i] || ky[i] != sy[i]; any |= clamped[i];
    }
    if (!any) return;
    for (int i = 0; i < n; i++) {
        if (clamped[i]) cnt[i] = hp_ring_gather(&hp_evr, kx[i], ky[i], c[i], HP_MAXC);
        else cnt[i] = hp_gather(sx[i], sy[i], c[i], HP_MAXC);
    }
    GteHpStats keep = hp_st;
    uint8_t low[4];
    (void)hp_select(n, c, cnt, idx, low);
    hp_st.res_amb = keep.res_amb; hp_st.res_pivot = keep.res_pivot;
    int nanc = 0; for (int i = 0; i < n; i++) if (!clamped[i] && cnt[i] == 1) nanc++;
    for (int i = 0; i < n; i++) {
        if (!clamped[i]) continue;
        int ok = idx[i] >= 0 && fabs(c[i][idx[i]]->vx - truth[i].vx) < 1.0 && fabs(c[i][idx[i]]->vy - truth[i].vy) < 1.0 &&
                 fabs(c[i][idx[i]]->vz - truth[i].vz) < 1.0;
        hp_st.ev_verts++; hp_st.ev_ok += ok;
        if (!low[i]) { hp_st.ev_hi++; hp_st.ev_hi_ok += ok; }
        if (nanc) { hp_st.ev_anc++; hp_st.ev_anc_ok += ok; }
        if (cnt[i] > 1) { hp_st.ev_amb++; hp_st.ev_amb_ok += ok; }
    }
}

int gte_hp_tag_ok(uint32_t tag, uint32_t v) {
    if (!tag) return 0;
    const HpPoolE *e = &hp_pool[(tag - 1) & (HP_POOL - 1)];
    return e->tag == tag && e->key == v && (uint32_t)(hp_gen - e->gen) <= HP_ADDR_GEN;
}
uint32_t gte_hp_sxy_tag(int r, uint32_t v) {
    if (r < 12 || r > 15) return 0;
    uint32_t t = hp_sxyt[r == 15 ? 2 : r - 12];
    return gte_hp_tag_ok(t, v) ? t : 0;
}
void gte_hp_sxy_set(int r, uint32_t tag, uint32_t v) {
    if (r < 12 || r > 15) return;
    hp_sxyt[r == 15 ? 2 : r - 12] = gte_hp_tag_ok(tag, v) ? tag : 0;
}

/* Resolución de una primitiva. addr/word (opcionales; addr[i] == 0 = dirección desconocida): palabra de vértice i y dirección de la que la
 * leyó la GPU; si la sombra por dirección la reconoce, ese vértice es EXACTO (ancla única, sin AMBIG) y el resto se resuelve por valor
 * como siempre, usándolo de ancla. */
int gte_hp_resolve_a(int n, const int *sx, const int *sy, const uint32_t *addr, const uint32_t *word, GpuHpInfo *out, uint8_t *ok) {
    const GpuHpInfo *c[4][HP_MAXC]; int cnt[4] = {0, 0, 0, 0}, idx[4], got = 0;
    const GpuHpInfo *known[4] = {0, 0, 0, 0};
    if (n > 4) n = 4;
    if (n <= 0) return 0;
    hp_st.res_prims++;
    for (int i = 0; i < n; i++) {
        if (addr && addr[i] && hps_on) {
            uint32_t tag = hps_get(addr[i]);
            if (gte_hp_tag_ok(tag, word[i])) known[i] = &hp_pool[(tag - 1) & (HP_POOL - 1)].i;
        }
        if (known[i]) { c[i][0] = known[i]; cnt[i] = 1; }
        else cnt[i] = hp_gather(sx[i], sy[i], c[i], HP_MAXC);
    }
    uint8_t low[4];
    int unamb = hp_select(n, c, cnt, idx, low);
    if (unamb && n >= 2) {
        int full = 1; for (int i = 0; i < n; i++) full &= idx[i] >= 0;
        if (full) { const GpuHpInfo *pc[4]; for (int i = 0; i < n; i++) pc[i] = c[i][0]; hp_learn(n, pc); }
    }
    /* AMBIG solo si de verdad hay alternativas distintas: los candidatos casi idénticos (mismo vértice transformado en dos
     * generaciones seguidas, p.ej. cámara quieta; < 0,2 % de z de diferencia) no restan confianza. */
    for (int i = 0; i < n; i++) {
        if (!low[i] || idx[i] < 0) continue;
        const GpuHpInfo *s = c[i][idx[i]]; float tol = 1.0f + 0.002f * fabsf(s->vz); int same = 1;
        for (int k = 0; k < cnt[i] && same; k++) {
            const GpuHpInfo *o2 = c[i][k];
            same = o2 == s || (o2->flags == s->flags && fabsf(o2->vx - s->vx) <= tol && fabsf(o2->vy - s->vy) <= tol && fabsf(o2->vz - s->vz) <= tol);
        }
        if (same) low[i] = 0;
    }
    if (n == 4) hp_planar_fix(c, cnt, idx, low);
    int all_norm = 1;
    for (int i = 0; i < n; i++) {
        hp_st.lookups++;
        ok[i] = idx[i] >= 0;
        if (ok[i]) {
            out[i] = *c[i][idx[i]]; got++; hp_st.hits++; all_norm &= out[i].flags == 0; if (low[i]) out[i].flags |= GPU_VF_AMBIG;
            if (known[i]) hp_st.res_addr++; else hp_st.res_val++;
        }
        else all_norm = 0;
    }
    if (hp_ev_on && all_norm) hp_eval(n, sx, sy, out);
    return got;
}
int gte_hp_resolve(int n, const int *sx, const int *sy, GpuHpInfo *out, uint8_t *ok) { return gte_hp_resolve_a(n, sx, sy, NULL, NULL, out, ok); }

/* Registro de un vértice RTPS/RTPT tras empujar SXY (sx,sy ya saturados). */
static void hp_vertex(int v, int sf, int sx, int sy, int sat) {
    if (!hp_on) return;
    if (!sf) { hp_st.skipped++; hp_st.skip_sf++; return; }
    double P[3];
    for (int i = 0; i < 3; i++)
        P[i] = ((double)TR(i) * 4096.0 + (double)mat(0, i, 0) * VX(v) + (double)mat(0, i, 1) * VY(v) +
                (double)mat(0, i, 2) * VZ(v)) / 4096.0;
    int32_t sz = SZ(3);
    GpuHpInfo in;
    memset(&in, 0, sizeof in);
    int fl = 0;
    if (sat || hp_edge(sx, sy)) { fl |= GPU_VF_SAT; hp_st.skip_sat++; }
    if (H >= (uint32_t)sz * 2u) { fl |= GPU_VF_SAT; hp_st.skip_ovf++; }
    if (IR(1) != MAC(1) || IR(2) != MAC(2) || P[2] > 65535.0) fl |= GPU_VF_SAT; /* IR1/IR2/SZ saturados: el SXY del hardware no es la proyección */
    double h = (double)H;
    if (P[2] <= 0.0) { fl |= GPU_VF_BEHIND | GPU_VF_SAT; hp_st.skip_z++; in.fx = in.fy = 0.0f; }
    else {
        in.fx = (float)(h * P[0] / P[2] + (double)OFX / 65536.0);
        in.fy = (float)(h * P[1] / P[2] + (double)OFY / 65536.0);
    }
    in.fz = (float)P[2];
    in.vx = (float)P[0]; in.vy = (float)P[1]; in.vz = (float)P[2];
    in.h = (float)h; in.ofx = (float)((double)OFX / 65536.0); in.ofy = (float)((double)OFY / 65536.0);
    in.flags = (uint8_t)fl; in.seq = hp_seq++;
    {   /* pool por dirección: clave = el SXY que acaba de empujar push_sxy */
        HpPoolE *pe = &hp_pool[in.seq & (HP_POOL - 1)];
        pe->tag = in.seq + 1; pe->key = (uint32_t)SXY(2); pe->gen = hp_gen; pe->i = in;
        hp_sxyt[2] = pe->tag; hps_dirty = 1;
    }
    if (fl) { hp_ring_push(&hp_ring, sx, sy, &in); hp_st.flagged++; hp_st.stored++; }
    else {
        hp_store(sx, sy, (uint32_t)sz, &in);
        if (hp_ev_on) { /* pseudo-saturación: guarda por clave recortada si cae fuera de la ventana de evaluación */
            int kx, ky; hp_evkey(&in, sx, sy, &kx, &ky);
            if (kx != sx || ky != sy) hp_ring_push(&hp_evr, kx, ky, &in);
        }
    }
}

/* --- RTPS núcleo --- */
static void rtp(int v, int sf, int lm, int last) {
    int sh = sf ? 12 : 0;
    int64_t m3 = 0;
    for (int i = 1; i <= 3; i++) {
        int64_t s = (int64_t)TR(i - 1) << 12;
        s = A(i, s + (int64_t)mat(0, i - 1, 0) * VX(v));
        s = A(i, s + (int64_t)mat(0, i - 1, 1) * VY(v));
        s = A(i, s + (int64_t)mat(0, i - 1, 2) * VZ(v));
        MAC(i) = (int32_t)(s >> sh);
        if (i == 3) m3 = s;
    }
    IR(1) = lim_ir(1, MAC(1), lm);
    IR(2) = lim_ir(2, MAC(2), lm);
    /* IR3: la flag se calcula sobre MAC3>>12 aunque sf=0 */
    {
        int32_t v3 = MAC(3), lo = lm ? 0 : -0x8000;
        int32_t f3 = (int32_t)(m3 >> 12);
        if (f3 < -0x8000 || f3 > 0x7FFF) flag |= 1u << 22;
        IR(3) = v3 < lo ? lo : v3 > 0x7FFF ? 0x7FFF : v3;
    }
    push_sz(lim_d(m3 >> 12));
    uint32_t n = gte_divide(H, (uint32_t)SZ(3));
    int64_t x = (int64_t)n * IR(1) + OFX; F(x);
    int64_t y = (int64_t)n * IR(2) + OFY; F(y);
    int32_t sx = (int32_t)(x >> 16), sy = (int32_t)(y >> 16);
    int sat = 0;
    if (sx < -0x400) { sx = -0x400; flag |= 1u << 14; sat = 1; } else if (sx > 0x3FF) { sx = 0x3FF; flag |= 1u << 14; sat = 1; }
    if (sy < -0x400) { sy = -0x400; flag |= 1u << 13; sat = 1; } else if (sy > 0x3FF) { sy = 0x3FF; flag |= 1u << 13; sat = 1; }
    push_sxy(sx, sy);
    hp_vertex(v, sf, sx, sy, sat);
    if (last) {
        int64_t q = (int64_t)n * DQA + DQB; F(q);
        MAC0 = (int32_t)q;
        int32_t i0 = (int32_t)(q >> 12);
        if (i0 < 0) { i0 = 0; flag |= 1u << 12; } else if (i0 > 0x1000) { i0 = 0x1000; flag |= 1u << 12; }
        IR0 = i0;
    }
}

/* --- multiplicación matriz·vector + traslación (MVMVA y color) --- */
static void mvmva_core(int mx, const int32_t vec[3], int cv, int sh, int lm) {
    int16_t M[3][3];
    if (mx < 3) {
        static const int bases[3] = {0, 8, 16};
        for (int r = 0; r < 3; r++) for (int k = 0; k < 3; k++) M[r][k] = mat(bases[mx], r, k);
    } else { /* matriz basura del hardware */
        int16_t R = (int16_t)((RGBC & 0xFF) << 4);
        M[0][0] = (int16_t)-R; M[0][1] = R; M[0][2] = (int16_t)IR0;
        M[1][0] = M[1][1] = M[1][2] = mat(0, 0, 2);
        M[2][0] = M[2][1] = M[2][2] = mat(0, 1, 1);
    }
    int32_t T[3] = {0, 0, 0};
    if (cv == 0) for (int i = 0; i < 3; i++) T[i] = TR(i);
    else if (cv == 1) for (int i = 0; i < 3; i++) T[i] = BK(i);
    else if (cv == 2) for (int i = 0; i < 3; i++) T[i] = FC(i);
    for (int i = 1; i <= 3; i++) {
        int64_t t = (int64_t)T[i - 1] << 12;
        if (cv == 2) {
            /* fallo del hardware: la columna 0 solo afecta a las flags */
            int64_t f = A(i, t + (int64_t)M[i - 1][0] * vec[0]);
            lim_ir(i, (int32_t)(f >> sh), 0);
            t = 0;
        } else {
            t = A(i, t + (int64_t)M[i - 1][0] * vec[0]);
        }
        t = A(i, t + (int64_t)M[i - 1][1] * vec[1]);
        t = A(i, t + (int64_t)M[i - 1][2] * vec[2]);
        MAC(i) = (int32_t)(t >> sh);
    }
    mac_to_ir(lm);
}

static void vec_of(int v, int32_t out[3]) {
    if (v < 3) { out[0] = VX(v); out[1] = VY(v); out[2] = VZ(v); }
    else { out[0] = IR(1); out[1] = IR(2); out[2] = IR(3); }
}
static void ir_vec(int32_t o[3]) { o[0] = IR(1); o[1] = IR(2); o[2] = IR(3); }

static uint32_t R8(void) { return RGBC & 0xFF; }
static uint32_t G8(void) { return (RGBC >> 8) & 0xFF; }
static uint32_t B8(void) { return (RGBC >> 16) & 0xFF; }

/* Interpolación hacia el color lejano (FC) con IR0. Entrada: MAC1-3 ya cargados (sin desplazar). */
static void interp_fc(int sh, int lm) {
    int32_t m[3] = {MAC(1), MAC(2), MAC(3)};
    for (int i = 1; i <= 3; i++) {
        int64_t d = ((int64_t)FC(i - 1) << 12) - m[i - 1];
        A(i, d);
        IR(i) = lim_ir(i, (int32_t)(d >> sh), 0);
    }
    for (int i = 1; i <= 3; i++) setmac(i, (int64_t)IR(i) * IR0 + m[i - 1], sh);
    push_color();
    mac_to_ir(lm);
}

/* Luz: IR = lim(LLM·V), luego IR = lim(BK + LCM·IR). */
static void light(int v, int sh, int lm) {
    int32_t a[3]; vec_of(v, a);
    mvmva_core(1, a, 3, sh, lm);
    ir_vec(a);
    mvmva_core(2, a, 1, sh, lm);
}
static void color_mul(void) { /* MAC = [R*IR1, G*IR2, B*IR3] << 4 */
    setmac(1, ((int64_t)R8() * IR(1)) << 4, 0);
    setmac(2, ((int64_t)G8() * IR(2)) << 4, 0);
    setmac(3, ((int64_t)B8() * IR(3)) << 4, 0);
}
static void finish_shift(int sh, int lm) { /* MAC = MAC >> sh, push, IR = lim */
    for (int i = 1; i <= 3; i++) MAC(i) = MAC(i) >> sh;
    push_color();
    mac_to_ir(lm);
}

static void ncs(int v, int sh, int lm) { light(v, sh, lm); push_color(); }
static void ncc(int v, int sh, int lm) { light(v, sh, lm); color_mul(); finish_shift(sh, lm); }
static void ncd(int v, int sh, int lm) { light(v, sh, lm); color_mul(); interp_fc(sh, lm); }

void gte_cmd(Cpu *c, uint32_t cmd) {
    (void)c;
    int sf = (cmd >> 19) & 1, sh = sf ? 12 : 0, lm = (cmd >> 10) & 1;
    int mx = (cmd >> 17) & 3, v = (cmd >> 15) & 3, cv = (cmd >> 13) & 3;
    flag = 0;
    switch (cmd & 0x3F) {
    case 0x01: rtp(0, sf, lm, 1); break;                                   /* RTPS */
    case 0x30: rtp(0, sf, lm, 0); rtp(1, sf, lm, 0); rtp(2, sf, lm, 1); break; /* RTPT */
    case 0x06: { /* NCLIP */
        int64_t x0 = LO16(SXY(0)), y0 = HI16(SXY(0)), x1 = LO16(SXY(1)), y1 = HI16(SXY(1));
        int64_t x2 = LO16(SXY(2)), y2 = HI16(SXY(2));
        int64_t m = x0 * y1 + x1 * y2 + x2 * y0 - x0 * y2 - x1 * y0 - x2 * y1;
        F(m); MAC0 = (int32_t)m; break;
    }
    case 0x0C: { /* OP */
        int64_t d1 = mat(0, 0, 0), d2 = mat(0, 1, 1), d3 = mat(0, 2, 2);
        setmac(1, IR(3) * d2 - IR(2) * d3, sh);
        setmac(2, IR(1) * d3 - IR(3) * d1, sh);
        setmac(3, IR(2) * d1 - IR(1) * d2, sh);
        mac_to_ir(lm); break;
    }
    case 0x10: /* DPCS */
        setmac(1, (int64_t)R8() << 16, 0); setmac(2, (int64_t)G8() << 16, 0); setmac(3, (int64_t)B8() << 16, 0);
        interp_fc(sh, lm); break;
    case 0x2A: /* DPCT */
        for (int k = 0; k < 3; k++) {
            uint32_t col = (uint32_t)D[20];
            setmac(1, (int64_t)(col & 0xFF) << 16, 0);
            setmac(2, (int64_t)((col >> 8) & 0xFF) << 16, 0);
            setmac(3, (int64_t)((col >> 16) & 0xFF) << 16, 0);
            interp_fc(sh, lm);
        }
        break;
    case 0x11: /* INTPL */
        for (int i = 1; i <= 3; i++) setmac(i, (int64_t)IR(i) << 12, 0);
        interp_fc(sh, lm); break;
    case 0x29: color_mul(); interp_fc(sh, lm); break;                       /* DCPL */
    case 0x12: { int32_t a[3]; vec_of(v, a); mvmva_core(mx, a, cv, sh, lm); break; } /* MVMVA */
    case 0x13: ncd(0, sh, lm); break;                                       /* NCDS */
    case 0x16: ncd(0, sh, lm); ncd(1, sh, lm); ncd(2, sh, lm); break;      /* NCDT */
    case 0x1B: ncc(0, sh, lm); break;                                       /* NCCS */
    case 0x3F: ncc(0, sh, lm); ncc(1, sh, lm); ncc(2, sh, lm); break;      /* NCCT */
    case 0x1E: ncs(0, sh, lm); break;                                       /* NCS */
    case 0x20: ncs(0, sh, lm); ncs(1, sh, lm); ncs(2, sh, lm); break;      /* NCT */
    case 0x14: { /* CDP */
        int32_t a[3]; ir_vec(a); mvmva_core(2, a, 1, sh, lm);
        color_mul(); interp_fc(sh, lm); break;
    }
    case 0x1C: { /* CC */
        int32_t a[3]; ir_vec(a); mvmva_core(2, a, 1, sh, lm);
        color_mul(); finish_shift(sh, lm); break;
    }
    case 0x28: /* SQR */
        for (int i = 1; i <= 3; i++) setmac(i, (int64_t)IR(i) * IR(i), sh);
        mac_to_ir(lm); break;
    case 0x2D: { /* AVSZ3 */
        int64_t m = (int64_t)ZSF3 * (SZ(1) + SZ(2) + SZ(3)); F(m); MAC0 = (int32_t)m;
        D[7] = lim_d(m >> 12); break;
    }
    case 0x2E: { /* AVSZ4 */
        int64_t m = (int64_t)ZSF4 * (SZ(0) + SZ(1) + SZ(2) + SZ(3)); F(m); MAC0 = (int32_t)m;
        D[7] = lim_d(m >> 12); break;
    }
    case 0x3D: /* GPF */
        for (int i = 1; i <= 3; i++) setmac(i, (int64_t)IR(i) * IR0, sh);
        push_color(); mac_to_ir(lm); break;
    case 0x3E: /* GPL */
        for (int i = 1; i <= 3; i++) setmac(i, ((int64_t)MAC(i) << sh) + (int64_t)IR(i) * IR0, sh);
        push_color(); mac_to_ir(lm); break;
    default: break; /* comandos no documentados: sin efecto */
    }
    if (flag & 0x7F87E000u) flag |= 0x80000000u;
    C[31] = (int32_t)flag;
}

/* --- transferencias de registros --- */
static int32_t sat5(int32_t v) { v >>= 7; return v < 0 ? 0 : v > 0x1F ? 0x1F : v; }

uint32_t gte_mfc2(Cpu *c, int r) {
    (void)c;
    switch (r) {
    case 1: case 3: case 5: case 8: case 9: case 10: case 11: return (uint32_t)(int32_t)(int16_t)D[r];
    case 7: case 16: case 17: case 18: case 19: return (uint32_t)D[r] & 0xFFFF;
    case 15: return (uint32_t)SXY(2);
    case 28: case 29: return (uint32_t)(sat5(IR(1)) | (sat5(IR(2)) << 5) | (sat5(IR(3)) << 10));
    default: return (uint32_t)D[r];
    }
}

void gte_mtc2(Cpu *c, int r, uint32_t v) {
    (void)c;
    switch (r) {
    case 15: push_sxy(LO16(v), HI16(v)); break;
    case 12: case 13: case 14: D[r] = (int32_t)v; hp_sxyt[r - 12] = 0; break;
    case 28:
        D[28] = (int32_t)(v & 0x7FFF);
        IR(1) = (int32_t)((v & 0x1F) << 7);
        IR(2) = (int32_t)(((v >> 5) & 0x1F) << 7);
        IR(3) = (int32_t)(((v >> 10) & 0x1F) << 7);
        break;
    /* Registros de 16 bits: el hardware solo guarda 16 bits. Los guardamos ya extendidos (con signo
     * VZ0..VZ2/IR0..IR3; sin signo OTZ/SZ0..SZ3) para que los cálculos internos, que leen el registro
     * entero, no vean basura en la mitad alta cuando el juego carga una palabra de 32 bits (lwc2). */
    case 1: case 3: case 5: case 8: case 9: case 10: case 11: D[r] = (int32_t)(int16_t)v; break;
    case 7: case 16: case 17: case 18: case 19: D[r] = (int32_t)(v & 0xFFFF); break;
    case 29: case 31: break; /* solo lectura */
    case 30: {
        D[30] = (int32_t)v;
        uint32_t x = (int32_t)v < 0 ? ~v : v;
        D[31] = x ? __builtin_clz(x) : 32;
        break;
    }
    default: D[r] = (int32_t)v; break;
    }
}

uint32_t gte_cfc2(Cpu *c, int r) {
    (void)c;
    switch (r) {
    case 4: case 12: case 20: case 26: case 27: case 29: case 30: return (uint32_t)(int32_t)(int16_t)C[r];
    default: return (uint32_t)C[r];
    }
}

void gte_ctc2(Cpu *c, int r, uint32_t v) {
    (void)c;
    if (r == 31) {
        flag = v & 0x7FFFF000u;
        if (flag & 0x7F87E000u) flag |= 0x80000000u;
        C[31] = (int32_t)flag;
        return;
    }
    C[r] = (int32_t)v;
}

void gte_lwc2(Cpu *c, int r, uint32_t v) { gte_mtc2(c, r, v); }
uint32_t gte_swc2(Cpu *c, int r) { return gte_mfc2(c, r); }
