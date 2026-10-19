/* Sustituto (HLE) de la función del juego que dibuja el radar del HUD (en la edición europea, 0x8003bd9c; ver vr_radar_names.json).
 * Ejecuta la original tal cual y, al volver, delimita en la OT lo que añadió con dos paquetes de marca (ver vr_radar.h, "1."):
 *
 *   antes:   ot -> H0 -> ...
 *   después: ot -> MARCA_INICIO -> P_n -> ... -> P_1 -> MARCA_FIN -> H0 -> ...
 *            (las primitivas se insertan al principio de la OT: se dibujan en orden inverso al de alta)
 *
 * Las marcas son paquetes de 1 palabra con el comando GP0 0x04 (NOP en el hardware real y en el oráculo; con
 * runtime_mark.patch la GPU avisa a vrradar_on_mark con el seq de captura). Nada del juego depende de ellas.
 * Se ejecuta en el hilo del juego, entre la construcción de la OT y su envío (gfx_list): seguro respecto a la fase del frame.
 * Se rotan 4 pares de marcas porque la OT del frame anterior puede no haberse enviado aún cuando se prepara la siguiente. */
#include <stdint.h>
#include <string.h>
#include "cpu.h"
#include "sys.h"
#include "vr_radar.h"

#define RAM_MASK 0x1FFFFFu
#define A1 c->r[5]

static uint32_t mem;            /* 4 pares x 16 bytes de memoria de kernel */
static unsigned rot;
static int enabled = 1;
static unsigned n_calls, n_marked, n_fail;

void hle_vrradar_reset(void) { mem = 0; rot = 0; }
void vrradar_hle_enable(int on) { enabled = on; }
void vrradar_hle_stats(unsigned *calls, unsigned *marked, unsigned *fail) {
    if (calls) *calls = n_calls;
    if (marked) *marked = n_marked;
    if (fail) *fail = n_fail;
}

static uint32_t rd(uint32_t a) { uint32_t v; memcpy(&v, emu_ram + (a & RAM_MASK & ~3u), 4); return v; }
static void wr(uint32_t a, uint32_t v) { memcpy(emu_ram + (a & RAM_MASK & ~3u), &v, 4); }

void hle_vrradar_draw(Cpu *c) {
    uint32_t ot = A1;                      /* a1 = ranura de OT (u_long *): se guarda, la original puede tocar a1 */
    if (!enabled || !(ot & RAM_MASK) || (ot & 3u)) { rt_orig(c, "vrradar_draw"); return; }
    if (!mem) mem = bios_kalloc(64);
    uint32_t t0 = rd(ot);
    rt_orig(c, "vrradar_draw");
    n_calls++;
    if (!mem) return;
    uint32_t t1 = rd(ot);
    uint32_t h0 = t0 & 0xFFFFFFu, h1 = t1 & 0xFFFFFFu;
    if (h1 == h0) return;                  /* el juego no dibujó nada (radar apagado, otro modo de pantalla, cámara...) */
    /* última primitiva añadida: la que enlaza con H0 */
    uint32_t node = h1, last = 0;
    for (int guard = 0; guard < 16384; guard++) {
        if (node & ~RAM_MASK) break;       /* fuera de la RAM (incluye el terminador 0xFFFFFF) */
        uint32_t nx = rd(node) & 0xFFFFFFu;
        if (nx == h0) { last = node; break; }
        node = nx;
    }
    if (!last) { n_fail++; return; }       /* cadena inesperada: no tocar nada */
    uint32_t mb = mem + (rot & 3u) * 16u, me = mb + 8u;
    rot++;
    wr(mb, (1u << 24) | h1);               /* MARCA_INICIO -> P_n */
    wr(mb + 4, 0x04000000u | VRRADAR_MARK_BEGIN);
    wr(me, (1u << 24) | h0);               /* MARCA_FIN -> H0 */
    wr(me + 4, 0x04000000u | VRRADAR_MARK_END);
    wr(last, (rd(last) & 0xFF000000u) | (me & 0xFFFFFFu));
    wr(ot, (t1 & 0xFF000000u) | (mb & 0xFFFFFFu));
    n_marked++;
}
