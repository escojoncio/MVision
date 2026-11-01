/* Procedencia de las primitivas de la GPU: tabla de ámbitos, sombra de escrituras por palabra de RAM y pila sombra del intérprete (ver prov.h). */
#include <string.h>
#include "cpu.h"
#include "prov.h"

int rt_prov_on;
uint16_t rt_prov_cur;
uint16_t rt_prov_mem[0x80000];
ProvEntry rt_prov_tab[PROV_MAX];
unsigned rt_prov_n;
unsigned long rt_prov_full, rt_prov_ovf;

#define HB 4096                         /* cubetas de la tabla hash (potencia de 2 > PROV_MAX) */
static uint16_t htab[HB];

typedef struct { uint32_t ret, sp; uint16_t old; } Frame;
typedef struct { uint16_t cur; unsigned n; Frame f[PROV_STK]; } Ctx;
static Ctx ctxs[PROV_THREADS];
static Ctx *ctx = &ctxs[0];             /* pila sombra del hilo en ejecución (hilo 0 al arrancar) */

static inline uint32_t norm(uint32_t a) { return (a & 0x1FFFFFFFu) | 0x80000000u; }
/* [a, a+n) dentro de la RAM de usuario (segmentos 0/4/5, fuera de los primeros 64 KiB). */
static int ram_ok(uint32_t a, uint32_t n) {
    uint32_t seg = a >> 29, p = a & 0x1FFFFFFFu;
    return (seg == 0 || seg == 4 || seg == 5) && p >= 0x10000u && p < EMU_RAM_SIZE && n <= EMU_RAM_SIZE - p;
}
static uint32_t rd32(uint32_t a) { uint32_t v; memcpy(&v, &emu_ram[a & (EMU_RAM_SIZE - 1)], 4); return v; }

static void read_name(uint32_t node, char out[20]) {
    out[0] = 0;
    uint32_t p = rd32(node + 20);
    int n = 0;
    for (; n < 19; n++) {
        if (!ram_ok(p + (uint32_t)n, 1)) { out[0] = 0; return; }
        uint8_t ch = emu_ram[(p + (uint32_t)n) & (EMU_RAM_SIZE - 1)];
        if (ch == 0) break;
        if (ch < 0x20 || ch > 0x7E) { out[0] = 0; return; }
        out[n] = (char)ch;
    }
    out[n] = 0;
}

/* Un id por CLAVE (rutina, ¿es actor?, nombre del actor), no por instancia: las instancias de un mismo tipo de actor comparten id y la tabla
 * queda acotada por el número de tipos. actor_node guarda el primer nodo visto (informativo). */
static uint16_t intern(uint32_t fn, uint32_t node) {
    char name[20];
    if (node) read_name(node, name); else name[0] = 0;
    uint32_t hn = 2166136261u;
    for (const char *p = name; *p; p++) hn = (hn ^ (uint8_t)*p) * 16777619u;
    unsigned h = ((fn >> 2) * 2654435761u ^ hn ^ (node ? 0x9E3779B9u : 0)) & (HB - 1);
    while (htab[h]) {
        const ProvEntry *e = &rt_prov_tab[htab[h]];
        if (e->fn == fn && (e->actor_node != 0) == (node != 0) && !strcmp(e->name, name)) return e->id;
        h = (h + 1) & (HB - 1);
    }
    if (rt_prov_n >= PROV_MAX - 1) { rt_prov_full++; return 0; }
    ProvEntry *e = &rt_prov_tab[++rt_prov_n];
    e->id = (uint16_t)rt_prov_n; e->fn = fn; e->actor_node = node;
    memcpy(e->name, name, sizeof name);
    htab[h] = e->id;
    return e->id;
}

void rt_prov_set(int on) {
    on = on != 0;
    if (on == rt_prov_on) return;
    rt_prov_on = on;
    rt_prov_cur = 0;
    ctx->n = 0;
}

void rt_prov_reset(void) {
    memset(rt_prov_mem, 0, sizeof rt_prov_mem);
    memset(rt_prov_tab, 0, sizeof rt_prov_tab);
    memset(htab, 0, sizeof htab);
    memset(ctxs, 0, sizeof ctxs);
    ctx = &ctxs[0];
    rt_prov_n = 0; rt_prov_full = rt_prov_ovf = 0; rt_prov_cur = 0;
}

uint16_t rt_prov_enter(uint32_t tgt, uint32_t a0) {
    uint16_t old = rt_prov_cur;
    uint32_t fn = norm(tgt), node = 0;
    if (!(a0 & 3) && ram_ok(a0, 32)) {
        uint32_t act = rd32(a0 + 8);
        if (act && norm(act) == fn) node = norm(a0);
    }
    rt_prov_cur = intern(fn, node);
    return old;
}

void rt_prov_push(uint32_t tgt, uint32_t a0, uint32_t ret, uint32_t sp) {
    if (ctx->n >= PROV_STK) { rt_prov_ovf++; return; }
    Frame *f = &ctx->f[ctx->n++];
    f->ret = ret; f->sp = sp;
    f->old = rt_prov_enter(tgt, a0);
}

void rt_prov_pop(uint32_t sp, uint32_t target) {
    for (unsigned i = ctx->n; i-- > 0;)
        if (ctx->f[i].ret == target && ctx->f[i].sp == sp) { rt_prov_cur = ctx->f[i].old; ctx->n = i; return; }
}

void rt_prov_unwind(uint32_t sp) {
    while (ctx->n && ctx->f[ctx->n - 1].sp <= sp) rt_prov_cur = ctx->f[--ctx->n].old;
}

void rt_prov_thread_init(unsigned t) {
    if (t < PROV_THREADS) { ctxs[t].cur = 0; ctxs[t].n = 0; }
}

void rt_prov_switch(unsigned from, unsigned to) {
    if (from < PROV_THREADS) ctxs[from].cur = rt_prov_cur;
    if (to < PROV_THREADS) { ctx = &ctxs[to]; rt_prov_cur = ctx->cur; }
}
