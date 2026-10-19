/* Pruebas del enlace de HLE por firmas (hlebind.c) y de su integración con el intérprete (interp.c):
 * hash/enmascarado, emparejamiento exacto/delta/ausente sobre un texto sintético, HLE por dirección en
 * llamada (jal), salto (j) y puntero (jalr), código original interpretado (rt_orig), setjmp/longjmp planos.
 * Sin datos del juego: el "EXE" se construye en RAM y las firmas se calculan aquí con las mismas funciones. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu.h"
#include "hlebind.h"
#include "sys.h"

const FuncEntry rt_func_table[] = {{0, 0}};
const unsigned rt_func_count = 0;
const FuncName rt_func_names[] = {{0xFFFFFFFFu, ""}};
const unsigned rt_func_name_count = 0;
const uint32_t rt_entry = 0;

static int fails;
#define EQ(a, b) do { unsigned long long _a = (a), _b = (b); \
    if (_a != _b) { printf("FALLO %s:%d %s = 0x%llx, esperado 0x%llx\n", __FILE__, __LINE__, #a, _a, _b); fails++; } } while (0)

#define R_(op, s, t, d, sh, f) (((uint32_t)(op) << 26) | ((uint32_t)(s) << 21) | ((uint32_t)(t) << 16) | ((uint32_t)(d) << 11) | ((uint32_t)(sh) << 6) | (f))
#define I_(op, s, t, i) (((uint32_t)(op) << 26) | ((uint32_t)(s) << 21) | ((uint32_t)(t) << 16) | ((uint32_t)(i) & 0xFFFF))
#define J_(op, a) (((uint32_t)(op) << 26) | (((a) >> 2) & 0x3FFFFFF))
#define NOP 0
#define JR_RA 0x03E00008u
#define ADDIU(t, s, i) I_(9, s, t, i)
#define ADDU(d, s, t) R_(0, s, t, d, 0, 33)
#define LUI(t, i) I_(15, 0, t, i)
#define ORI(t, s, i) I_(13, s, t, i)
#define BNE(s, t, off) I_(5, s, t, off)
#define SW(t, s, i) I_(43, s, t, i)
#define JAL(a) J_(3, a)
#define J(a) J_(2, a)
#define JALR(d, s) R_(0, s, 0, d, 0, 9)
#define BREAK 0x0000000Du
enum { V0 = 2, A0 = 4, A1 = 5, T0 = 8, T1 = 9, S0 = 16, SP = 29, RA = 31 };

/* ---------------------------------------------------------------- texto sintético */
#define BASE 0x80010000u
static uint32_t at;                                   /* dirección de escritura */
static void emit(uint32_t w) { mem_w32(NULL, at, w); at += 4; }

/* Función de n palabras (n>=8): prólogo, cuerpo de ALU que depende de seed/variant, jr ra + nop.
 * variant cambia solo una palabra de ALU (no enmascarada) para romper el hash completo. */
static void func(unsigned n, unsigned seed, unsigned variant, uint32_t jal) {
    emit(ADDIU(SP, SP, -16));
    for (unsigned i = 1; i < n - 2; i++) if (jal && i == 5) emit(JAL(jal)); else emit(ADDU(1 + (seed + i) % 20, 1 + (seed * 3 + i) % 20, 1 + (seed + 7 * i + (i == 3 ? variant : 0)) % 20));
    emit(JR_RA);
    emit(NOP);
}

static char json[8192];
static unsigned sig_fd;                               /* si != 0, la firma lleva "fd": sig_fd */
static size_t jn;
static int first_sig = 1;
/* Añade la firma de la función de n palabras que empieza en `addr` del texto, con la dirección de referencia ref. */
static void sig(const char *name, uint32_t ref, uint32_t addr, unsigned n) {
    uint32_t w[256], m[256];
    for (unsigned i = 0; i < n; i++) { w[i] = mem_r32(NULL, addr + 4 * i); m[i] = hle_sig_mask(w[i]); }
    uint8_t p[20], f[20];
    hle_sig_hash(m, 8, p);
    hle_sig_hash(m, n, f);
    char ph[41], fh[41];
    for (int i = 0; i < 20; i++) { sprintf(ph + 2 * i, "%02x", p[i]); sprintf(fh + 2 * i, "%02x", f[i]); }
    jn += (size_t)sprintf(json + jn, "%s{\"name\":\"%s\",\"addr\":\"%08x\",\"words\":%u,\"prefix\":\"%s\",\"full\":\"%s\"", first_sig ? "" : ",\n", name, ref, n, ph, fh);
    if (sig_fd) jn += (size_t)sprintf(json + jn, ",\"fd\":%u", sig_fd);
    jn += (size_t)sprintf(json + jn, "}");
    first_sig = 0;
}

static void t_hash(void) {
    uint32_t w[20];
    for (unsigned i = 0; i < 20; i++) w[i] = i;
    uint8_t d[20];
    char h[41];
    hle_sig_hash(w, 8, d);
    for (int i = 0; i < 20; i++) sprintf(h + 2 * i, "%02x", d[i]);
    EQ(strcmp(h, "643f7c1d25adafa2f5ed135d8331618a14714ed2"), 0);
    hle_sig_hash(w, 20, d);
    for (int i = 0; i < 20; i++) sprintf(h + 2 * i, "%02x", d[i]);
    EQ(strcmp(h, "64cd8cdd5ae0d692a498167ce14bc003cf2bda96"), 0);
    hle_sig_hash(w, 0, d);
    for (int i = 0; i < 20; i++) sprintf(h + 2 * i, "%02x", d[i]);
    EQ(strcmp(h, "da39a3ee5e6b4b0d3255bfef95601890afd80709"), 0);
    /* enmascarado: lui/addiu/lw/jal pierden inmediatos; addu no */
    EQ(hle_sig_mask(LUI(T0, 0x1234)), LUI(T0, 0));
    EQ(hle_sig_mask(ADDIU(T0, T1, 0x7FFF)), ADDIU(T0, T1, 0));
    EQ(hle_sig_mask(I_(35, T1, T0, 0x40)), I_(35, T1, T0, 0));
    EQ(hle_sig_mask(JAL(0x80012340)), JAL(0));
    EQ(hle_sig_mask(J(0x80012340)), J(0));
    EQ(hle_sig_mask(BNE(T0, T1, 5)), BNE(T0, T1, 5));
    EQ(hle_sig_mask(ADDU(1, 2, 3)), ADDU(1, 2, 3));
}

/* Disposición del texto (palabras): A 12 | gfx_sync 14 | B 12 | frame_wait 20 | C 12 | frame_cb 10 (original) |
 * setjmp 10 | longjmp 10 | D 12. */
static uint32_t a_fw, a_gs, a_fcb, a_jsave, a_jrest, a_end;

static void build_text(void) {
    at = BASE;
    func(12, 1, 0, BASE + 48);           /* A (llama a gfx_sync: la hace inicio de función) */
    a_gs = at; func(14, 2, 0, 0);
    func(12, 3, 0, 0);                   /* B */
    a_fw = at; func(20, 4, 0, 0);
    func(12, 5, 0, 0);                   /* C */
    a_fcb = at;                      /* original: v0 = 77 */
    emit(ADDIU(V0, 0, 77)); emit(JR_RA); emit(NOP);
    for (int i = 0; i < 7; i++) emit(ADDU(1, 1 + i, 2));
    a_jsave = at; emit(BREAK); for (int i = 0; i < 9; i++) emit(ADDU(2, 3 + i, 4));
    a_jrest = at; emit(BREAK); for (int i = 0; i < 9; i++) emit(ADDU(3, 4 + i, 5));
    func(12, 6, 0, 0);                   /* D */
    a_end = at;
}

static void build_sigs(void) {
    /* La referencia tiene los mismos tamaños; gfx_sync difiere en una palabra no enmascarada (sin match exacto
     * ni por prefijo: la palabra cambiada está dentro de las 8 primeras) y se hallará por distancia a las anclas. */
    const uint32_t d = 0x40000;
    jn = 0; first_sig = 1;
    jn += (size_t)sprintf(json + jn, "[\n");
    sig("anchor_a", BASE + d, BASE, 12);
    /* firma de referencia de gfx_sync: copia con la palabra 3 distinta */
    uint32_t keep[14], p = a_gs;
    for (unsigned i = 0; i < 14; i++) keep[i] = mem_r32(NULL, p + 4 * i);
    mem_w32(NULL, p + 12, ADDU(9, 9, 9));
    sig("gfx_sync", a_gs + d, a_gs, 14);
    for (unsigned i = 0; i < 14; i++) mem_w32(NULL, p + 4 * i, keep[i]);
    sig("anchor_b", BASE + 26 * 4 + d, BASE + 26 * 4, 12);
    sig("frame_wait", a_fw + d, a_fw, 20);
    sig("anchor_c", a_fw + 80 + d, a_fw + 80, 12);
    sig("frame_cb", a_fcb + d, a_fcb, 10);
    sig("jmp_save", a_jsave + d, a_jsave, 10);
    sig("jmp_restore", a_jrest + d, a_jrest, 10);
    /* disc_init: firma que no existe en el texto */
    jn += (size_t)sprintf(json + jn, ",\n{\"name\":\"disc_init\",\"addr\":\"%08x\",\"words\":9,\"prefix\":\"%040x\",\"full\":\"%040x\"}", BASE + 0x70000, 1, 2);
    jn += (size_t)sprintf(json + jn, "\n]\n");
}

static void reset(Cpu *c) {
    memset(c, 0, sizeof *c);
    c->r[SP] = 0x801FFF00u;
}

static void t_bind(void) {
    build_text();
    build_sigs();
    int n = hle_bind(json, jn, BASE, a_end - BASE, BASE);
    EQ(n, 5);                                  /* frame_wait, gfx_sync (por distancia), frame_cb, setjmp, longjmp */
    EQ(hle_bind_missing(), hle_table_count - 5);
    EQ(hle_bind_addr("frame_wait"), a_fw);
    EQ(hle_bind_addr("gfx_sync"), a_gs);
    EQ(hle_bind_addr("jmp_save"), a_jsave);
    EQ(hle_bind_addr("disc_init"), 0);            /* firma ausente */
    EQ(hle_bind_addr("anchor_a"), 0);          /* las anclas no se enlazan */
    EQ(strcmp(hle_bind_name(a_fw), "frame_wait"), 0);
}


/* Falsos positivos: una función del juego con el mismo prólogo que la HLE pero sin forma de función del tamaño esperado
 * no se enlaza por prefijo; con forma correcta sí; y una firma con gemelas (fd>1) no se enlaza por hash único. */
static void t_decoy(void) {
    const uint32_t b2 = 0x80100000u;
    int n;
    /* texto: ancla (12) | decoy frame_wait (20 palabras, sin jr ra) */
    at = b2;
    func(12, 1, 0, 0);
    uint32_t dec = at;
    emit(ADDIU(SP, SP, -16));
    for (unsigned i = 1; i < 20; i++) emit(ADDU(1 + (4 + i) % 20, 1 + (4 * 3 + i) % 20, 1 + (4 + 7 * i) % 20));
    uint32_t end = at;
    /* la referencia tiene el mismo prólogo y un cuerpo distinto en la palabra 12 (sin match exacto) */
    uint32_t tmp = mem_r32(NULL, dec + 48);
    mem_w32(NULL, dec + 48, ADDU(9, 9, 9));
    jn = 0; first_sig = 1;
    jn += (size_t)sprintf(json + jn, "[\n");
    sig("frame_wait", dec + 0x40000, dec, 20);
    jn += (size_t)sprintf(json + jn, "\n]\n");
    mem_w32(NULL, dec + 48, tmp);
    n = hle_bind(json, jn, b2, end - b2, b2);
    EQ(n, 0);                                          /* prefijo único pero sin cola de función: descartado */
    EQ(hle_bind_addr("frame_wait"), 0);
    /* mismo texto con cola correcta (jr ra + nop): el prefijo único sí enlaza */
    mem_w32(NULL, dec + 4 * 18, JR_RA); mem_w32(NULL, dec + 4 * 19, NOP);
    tmp = mem_r32(NULL, dec + 48);
    mem_w32(NULL, dec + 48, ADDU(9, 9, 9));
    jn = 0; first_sig = 1;
    jn += (size_t)sprintf(json + jn, "[\n");
    sig("frame_wait", dec + 0x40000, dec, 20);
    jn += (size_t)sprintf(json + jn, "\n]\n");
    mem_w32(NULL, dec + 48, tmp);
    n = hle_bind(json, jn, b2, end - b2, b2);
    EQ(n, 1);
    EQ(hle_bind_addr("frame_wait"), dec);
    /* con gemelas en la referencia completa (fd=2) el hash único tampoco basta; sin ancla no hay otra vía: no se enlaza */
    sig_fd = 2;
    jn = 0; first_sig = 1;
    jn += (size_t)sprintf(json + jn, "[\n");
    sig("frame_wait", dec + 0x40000, dec, 20);
    jn += (size_t)sprintf(json + jn, "\n]\n");
    sig_fd = 0;
    n = hle_bind(json, jn, b2, end - b2, b2);
    EQ(n, 0);
    EQ(hle_bind_addr("frame_wait"), 0);
}

/* Código de invitado fuera del texto. */
static uint32_t gat;
static void gemit(uint32_t w) { mem_w32(NULL, gat, w); gat += 4; }

static void t_hle_dispatch(Cpu *c) {
    /* frame_cb (HLE) devuelve el valor anterior del callback y guarda a0; el cuerpo original (v0=77) no corre. */
    EQ(hle_bind_lookup(a_fcb) != NULL, 1);
    EQ(hle_bind_addr("frame_cb"), a_fcb);
    EQ(hle_bind_is(a_fcb), 1);
    EQ(hle_bind_is(a_fw + 4), 0);
    /* llamada: jal HLE, dos veces */
    gat = 0x80020000;
    gemit(ADDIU(SP, SP, -16)); gemit(SW(RA, SP, 0));
    gemit(ADDIU(A0, 0, 0x1234)); gemit(JAL(a_fcb)); gemit(NOP);
    gemit(ADDU(S0, V0, 0));                     /* s0 = valor anterior (0) */
    gemit(ADDIU(A0, 0, 0)); gemit(JAL(a_fcb)); gemit(NOP);
    gemit(ADDU(T1, V0, 0));                     /* t1 = 0x1234 */
    gemit(0x8FBF0000u); gemit(ADDIU(SP, SP, 16)); gemit(JR_RA); gemit(NOP);
    reset(c);
    c->r[RA] = 0xDEAD0000u;
    rt_call(c, 0x80020000);
    EQ(c->r[S0], 0);
    EQ(c->r[T1], 0x1234);
    EQ(c->r[RA], 0xDEAD0000u);
    /* salto en cola (j) a la HLE: vuelve a ra del llamador */
    gat = 0x80020100;
    gemit(ADDIU(A0, 0, 0x4321)); gemit(J(a_fcb)); gemit(NOP);
    reset(c);
    c->r[RA] = 0xDEAD0000u;
    rt_call(c, 0x80020100);
    EQ(c->r[V0], 0);                            /* anterior era 0 (se puso a 0) */
    c->r[RA] = 0xDEAD0000u; c->r[A0] = 0;
    rt_call(c, a_fcb);                      /* desde el host: también HLE */
    EQ(c->r[V0], 0x4321);
    /* puntero a función (jalr) */
    gat = 0x80020200;
    gemit(ADDIU(SP, SP, -16)); gemit(SW(RA, SP, 0));
    gemit(LUI(T0, a_fcb >> 16)); gemit(ORI(T0, T0, a_fcb & 0xFFFF));
    gemit(JALR(RA, T0)); gemit(ADDIU(A0, 0, 7));
    gemit(0x8FBF0000u); gemit(ADDIU(SP, SP, 16)); gemit(JR_RA); gemit(NOP);
    reset(c);
    c->r[RA] = 0xDEAD0000u;
    rt_call(c, 0x80020200);
    EQ(c->r[V0], 0);
    /* el original, interpretado */
    reset(c);
    c->r[RA] = 0xDEAD0000u;
    EQ(hle_bind_orig(c, "frame_cb"), 1);
    EQ(c->r[V0], 77);
    EQ(c->r[RA], 0xDEAD0000u);
    EQ(rt_orig(c, "frame_cb"), 1);
    EQ(rt_orig(c, "NoExiste"), 0);
    EQ(hle_bind_orig(c, "NoExiste"), 0);
    irq_set_vsync_callback(0);
}

/* setjmp/longjmp planos: setjmp devuelve 0, longjmp reanuda tras el setjmp con v0 = val y los registros guardados. */
static void t_setjmp(Cpu *c) {
    gat = 0x80020300;
    gemit(ADDIU(SP, SP, -16)); gemit(SW(RA, SP, 0));
    gemit(LUI(A0, 0x8004));                      /* buf = 0x80040000 */
    gemit(JAL(a_jsave)); gemit(ADDIU(S0, 0, 0x1234));   /* slot: s0 = 0x1234 antes de guardar */
    gemit(BNE(V0, 0, 7)); gemit(NOP);            /* v0 != 0 -> después */
    gemit(ADDIU(T0, T0, 1));                     /* primera pasada */
    gemit(ADDIU(S0, 0, 0x9999));
    gemit(LUI(A0, 0x8004)); gemit(JAL(a_jrest)); gemit(ADDIU(A1, 0, 5));
    gemit(NOP);                                  /* no se ejecuta */
    gemit(0x8FBF0000u); gemit(ADDIU(SP, SP, 16)); gemit(JR_RA); gemit(NOP);
    reset(c);
    c->r[RA] = 0xDEAD0000u;
    rt_call(c, 0x80020300);
    EQ(c->r[T0], 1);                             /* la primera pasada se ejecutó una sola vez */
    EQ(c->r[V0], 5);
    EQ(c->r[S0], 0x1234);                        /* restaurado por longjmp */
    EQ(c->r[SP], 0x801FFF00u);
    EQ(c->r[RA], 0xDEAD0000u);
}

/* HLE de dirección fija (capa de cámara VR): hle_bind las omite sin contarlas como faltantes y hle_bind_fixed las registra. */
static void t_fixed(Cpu *c) {
    build_text();
    build_sigs();
    hle_bind_declare_fixed("vrcam_view");
    hle_bind_declare_fixed("vrcam_view");      /* idempotente */
    int n = hle_bind(json, jn, BASE, a_end - BASE, BASE);
    EQ(n, 5);
    EQ(hle_bind_missing(), hle_table_count - 5 - 1);   /* vrcam_view no se busca por firma */
    EQ(hle_bind_fixed("vrcam_view", a_fw + 4 * 3), 0);
    EQ(hle_bind_addr("vrcam_view"), a_fw + 4 * 3);
    EQ(hle_bind_lookup(a_fw + 4 * 3) != NULL, 1);
    EQ(hle_bind_is(a_fw + 4 * 3), 1);
    EQ(hle_bind_fixed("vrcam_view", a_fw + 4 * 3), -1);   /* ya ocupada */
    EQ(hle_bind_fixed("no_existe", a_fw + 8), -1);        /* nombre fuera de hle_table */
    EQ(hle_bind_fixed("vrcam_trans", a_fw + 2), -1);      /* no alineada */
    EQ(hle_bind_fixed("vrcam_trans", 0x80000000u), -1);      /* fuera de la RAM útil */
    EQ(hle_bind_fixed("vrcam_trans", 0x80200000u), -1);
    /* el sustituto débil delega en el original: hle_bind_orig ejecuta el código interpretado de esa dirección */
    reset(c);
    c->r[31] = 0xFFFFFF00u;
    EQ(hle_bind_orig(c, "vrcam_view"), 1);
    /* un hle_bind nuevo borra los enlaces anteriores, también los fijos */
    n = hle_bind(json, jn, BASE, a_end - BASE, BASE);
    EQ(hle_bind_addr("vrcam_view"), 0);
}

int main(void) {
    Cpu c;
    t_hash();
    t_bind();
    t_decoy();
    t_bind();
    t_hle_dispatch(&c);
    t_setjmp(&c);
    t_fixed(&c);
    printf(fails ? "%d fallos\n" : "enlace HLE por firmas: todas las pruebas OK\n", fails);
    return fails != 0;
}
