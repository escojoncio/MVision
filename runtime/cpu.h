#pragma once
#include <stdint.h>

typedef struct Cpu {
    uint32_t r[32];
    uint32_t hi, lo;
} Cpu;

typedef void (*FuncPtr)(Cpu *c);
typedef struct { uint32_t addr; FuncPtr fn; } FuncEntry;
typedef struct { uint32_t addr; const char *name; } FuncName;

/* Generado por tools/recomp/recomp.py (dispatch.c). Tabla ordenada por dirección. */
extern const FuncEntry rt_func_table[];
extern const unsigned rt_func_count;
extern const FuncName rt_func_names[];
extern const unsigned rt_func_name_count;
#ifdef MV_NOGEN
extern uint32_t rt_entry;   /* modo sin código generado: lo fija el cargador con el pc de la cabecera del EXE */
#else
extern const uint32_t rt_entry;
#endif
/* Código original de las funciones sustituidas por HLE (nombre -> orig_f_XXXXXXXX), para que el HLE
 * delegue en él cuando necesita sus efectos sobre el estado interno de la librería. */
typedef struct { const char *name; FuncPtr fn; } HleOrig;
extern const HleOrig rt_hle_orig[];
extern const unsigned rt_hle_orig_count;
int rt_orig(Cpu *c, const char *name);   /* ejecuta el original; 0 si no existe */

/* ---- Intérprete (interp.c) ----
 * interp_call ejecuta desde `entry` hasta que el código salta a la ra de entrada (activación plana, ver interp.c);
 * interp_call_raw lo hace ignorando la HLE enlazada en `entry` (código original). Las escrituras en RAM con
 * bloques decodificados los invalidan: rt_codemap marca granos de 256 B con código. */
void interp_call(Cpu *c, uint32_t entry);
void interp_call_raw(Cpu *c, uint32_t entry);
/* Últimas direcciones de bloque ejecutadas por el intérprete (orden cronológico, hasta 64; diagnóstico). Devuelve cuántas. */
int interp_recent_blocks(uint32_t *out, int max);
void interp_flush_all(void);
extern uint8_t rt_codemap[0x200000u >> 8];
void rt_code_hit(uint32_t ram_offset);                   /* escritura en un grano marcado (mem.c, interp.c) */
void rt_code_invalidate(uint32_t addr, uint32_t len);    /* escritura masiva (cargas de disco, memcpy de BIOS) */

void rt_dump_trace(void);   /* últimas funciones ejecutadas (anillo de 64; vacío sin -DRT_TRACE) */
/* Traza opcional de entrada a funciones (compilar con -DRT_TRACE). */
#ifdef RT_TRACE
void rt_trace_enter(Cpu *c, uint32_t addr);
#define RT_ENTER(a) rt_trace_enter(c, (a))
#else
#define RT_ENTER(a) ((void)0)
#endif

/* ---- Sondeo de interrupciones en puntos seguros ----
 * El recompilador emite RT_POLL(c) en las aristas hacia atrás (bucles) y el intérprete en sus saltos
 * hacia atrás. Cada sondeo "gasta" ciclos virtuales: rt_budget cuenta sondeos hasta el próximo evento
 * del reloj (VBlank, temporizadores). Al agotarse se llama a rt_poll, que avanza el reloj, genera los
 * eventos de hardware vencidos y, si SR.IEc && SR.IM2 && (I_STAT & I_MASK), entrega la excepción HLE
 * (irq.c). Cualquier fuente que levante una IRQ pone rt_budget a 0 para que se atienda en el siguiente
 * sondeo. Los registros MIPS viven siempre en *c, así que cualquier punto de sondeo es consistente. */
extern int32_t rt_budget;
void rt_poll(Cpu *c);
#define RT_POLL(c) do { if (__builtin_expect(--rt_budget < 0, 0)) rt_poll(c); } while (0)
extern uint32_t rt_irq_enabled;          /* (SR.IEc && SR.IM2) del hilo en ejecución */
uint32_t rt_sr_get(void);                /* bits de interrupción de SR (IEc=1, IM2=0x400) del hilo actual */
void rt_sr_set(uint32_t v);              /* idem, sin entregar IRQ (lo usan fibras y excepciones) */
void rt_irq_check(Cpu *c);               /* entrega la excepción ahora si procede (tras habilitar IEc) */

/* ---- Memoria PS1: RAM 2 MB (espejos KUSEG/KSEG0/KSEG1 x4), scratchpad 1 KB, E/S. ---- */
#define PSX_RAM_SIZE   0x200000u
#define PSX_SCRATCH    0x1F800000u
#define PSX_IO         0x1F801000u
extern uint8_t psx_ram[PSX_RAM_SIZE];
extern uint8_t psx_scratch[0x400];

uint8_t  mem_r8(Cpu *c, uint32_t a);
uint16_t mem_r16(Cpu *c, uint32_t a);
uint32_t mem_r32(Cpu *c, uint32_t a);
void mem_w8(Cpu *c, uint32_t a, uint8_t v);
void mem_w16(Cpu *c, uint32_t a, uint16_t v);
void mem_w32(Cpu *c, uint32_t a, uint32_t v);
uint32_t mem_lwl(Cpu *c, uint32_t a, uint32_t cur);
uint32_t mem_lwr(Cpu *c, uint32_t a, uint32_t cur);
void mem_swl(Cpu *c, uint32_t a, uint32_t v);
void mem_swr(Cpu *c, uint32_t a, uint32_t v);
/* Puntero host a RAM/scratchpad (NULL si no es memoria directa). */
void *mem_ptr(uint32_t a);

/* Ganchos de E/S (registros hardware 0x1F801000-0x1F802FFF). Por defecto: registro plano + log. */
uint32_t io_read(uint32_t a, int size);
void io_write(uint32_t a, uint32_t v, int size);

/* ---- Control de flujo fuera de lo conocido estáticamente. ---- */
FuncPtr rt_lookup(uint32_t addr);
void rt_call(Cpu *c, uint32_t target);
void rt_jump(Cpu *c, uint32_t target);
void rt_fallthrough(Cpu *c, uint32_t target);
void rt_unimpl(Cpu *c, uint32_t pc, uint32_t insn);
void rt_syscall(Cpu *c, uint32_t pc);
void rt_break(Cpu *c, uint32_t pc);
void rt_div(Cpu *c, uint32_t a, uint32_t b);
void rt_divu(Cpu *c, uint32_t a, uint32_t b);
/* ---- Overlays recompiladas (tools/recomp/recomp.py --ov-batch; índice generado ov_index.c) ----
 * Todas las overlays se cargan en la misma base. Cada una trae su tabla de funciones y el hash FNV-1a 64
 * de sus bytes de código [base+off, base+off+len). El runtime registra la tabla cuya firma coincide con
 * la RAM (rt.c, rt_ov_*). Sin índice enlazado (rt_ov_count = 0) todo sigue en el intérprete. */
typedef struct { const char *name; uint32_t base, off, len, w0; uint64_t hash; const FuncEntry *table; unsigned count; } OvDesc;
extern const OvDesc rt_ov_index[];
extern const unsigned rt_ov_count;
/* Aviso de que se escribieron bytes de RAM de código potencial (cargas de disco: CdGetSector): invalida la overlay. */
void rt_ov_written(uint32_t addr, uint32_t len);

/* Registro de tablas extra (overlays cargados en RAM). */
void rt_register_table(const FuncEntry *t, unsigned n);
void rt_clear_tables(void);
const char *rt_name(uint32_t addr);
_Noreturn void rt_fatal(Cpu *c, const char *fmt, ...);

/* BIOS HLE (vectores A0/B0/C0, función en t1). */
void bios_hle(Cpu *c, uint32_t vec);

/* ---- COP0 / GTE ---- */
uint32_t cop0_read(Cpu *c, int r);
void cop0_write(Cpu *c, int r, uint32_t v);
void cop0_rfe(Cpu *c);
void gte_cmd(Cpu *c, uint32_t cmd);
uint32_t gte_mfc2(Cpu *c, int r);
uint32_t gte_cfc2(Cpu *c, int r);
void gte_mtc2(Cpu *c, int r, uint32_t v);
void gte_ctc2(Cpu *c, int r, uint32_t v);
void gte_lwc2(Cpu *c, int r, uint32_t v);
uint32_t gte_swc2(Cpu *c, int r);
void gte_reset(void);
