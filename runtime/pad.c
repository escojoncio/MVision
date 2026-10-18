/* Mando: estado inyectado por el host y escrito cada frame en los búferes de recepción del juego.
 * Sustituye a la librería de mando de acceso directo (PadInitDirect/PadGetState/PadInfoAct/
 * PadSetAct...) y al InitPAD/StartPAD de la BIOS. Formato de búfer PS1:
 *   [0] 0x00 = datos válidos, 0xFF = sin mando
 *   [1] tipo<<4 | medias palabras (0x41 digital, 0x73 analógico)
 *   [2..3] botones activos a nivel bajo, [4..7] RX RY LX LY. */
#include <string.h>
#include "cpu.h"
#include "sys.h"

typedef struct {
    int type;
    uint16_t buttons;
    uint8_t ax[4];          /* rx, ry, lx, ly */
    uint32_t act_buf;       /* búfer de actuadores del juego (PadSetAct) */
    uint8_t align[6];
    uint8_t small, large;
} Pad;

static Pad pads[2] = {
    {.type = PAD_ANALOG, .ax = {0x80, 0x80, 0x80, 0x80}, .align = {0, 1, 0xFF, 0xFF, 0xFF, 0xFF}},
    {.type = PAD_NONE, .ax = {0x80, 0x80, 0x80, 0x80}, .align = {0, 1, 0xFF, 0xFF, 0xFF, 0xFF}},
};
/* Búferes de recepción: modo directo (PadInitDirect) o BIOS (InitPAD). */
static uint32_t rx_buf[2], rx_len[2];
static int started;

void pad_set_type(int port, int type) { if (port >= 0 && port < 2) pads[port].type = type; }

void pad_set_state(int port, uint16_t buttons, uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry) {
    if (port < 0 || port > 1) return;
    Pad *p = &pads[port];
    p->buttons = buttons;
    p->ax[0] = rx; p->ax[1] = ry; p->ax[2] = lx; p->ax[3] = ly;
}

void pad_get_rumble(int port, uint8_t *small, uint8_t *large) {
    if (port < 0 || port > 1) { *small = *large = 0; return; }
    *small = pads[port].small; *large = pads[port].large;
}

static void fill(Cpu *c, int port) {
    uint32_t b = rx_buf[port];
    if (!b) return;
    Pad *p = &pads[port];
    if (p->type == PAD_NONE) { mem_w8(c, b, 0xFF); mem_w8(c, b + 1, 0xFF); return; }
    uint16_t raw = (uint16_t)~p->buttons;
    mem_w8(c, b, 0x00);
    if (p->type == PAD_DIGITAL) {
        raw |= 0x0006;                    /* L3/R3 no existen en el digital */
        mem_w8(c, b + 1, 0x41);
        mem_w8(c, b + 2, (uint8_t)raw); mem_w8(c, b + 3, (uint8_t)(raw >> 8));
        return;
    }
    mem_w8(c, b + 1, 0x73);
    mem_w8(c, b + 2, (uint8_t)raw); mem_w8(c, b + 3, (uint8_t)(raw >> 8));
    if (!rx_len[port] || rx_len[port] >= 8)
        for (int i = 0; i < 4; i++) mem_w8(c, b + 4 + (uint32_t)i, p->ax[i]);
}

/* Lee los valores de vibración que el juego dejó en su búfer de actuadores. */
static void read_act(Cpu *c, int port) {
    Pad *p = &pads[port];
    if (!p->act_buf || p->type != PAD_ANALOG) { p->small = p->large = 0; return; }
    p->small = mem_r8(c, p->act_buf + 0) ? 1 : 0;
    p->large = mem_r8(c, p->act_buf + 1);
}

void pad_frame(Cpu *c) {
    if (!started) return;
    for (int i = 0; i < 2; i++) { fill(c, i); read_act(c, i); }
}

static int port_of(uint32_t p) { return (p >> 4) & 1; }

/* ---------- BIOS B0:12/13/14 ---------- */
void pad_bios_init(Cpu *c, uint32_t b1, uint32_t s1, uint32_t b2, uint32_t s2) {
    rx_buf[0] = b1; rx_len[0] = s1; rx_buf[1] = b2; rx_len[1] = s2;
    for (int i = 0; i < 2; i++) if (rx_buf[i]) { mem_w8(c, rx_buf[i], 0xFF); mem_w8(c, rx_buf[i] + 1, 0xFF); }
}
void pad_bios_start(Cpu *c) { started = 1; pad_frame(c); }
void pad_bios_stop(void) { started = 0; }

/* ---------- librería de acceso directo ---------- */
void hle_PadInitDirect(Cpu *c) {
    pad_bios_init(c, c->r[4], 0, c->r[5], 0);
    c->r[2] = 1;
}
void hle_PadStartCom(Cpu *c) { started = 1; pad_frame(c); c->r[2] = 1; }
void hle_PadStopCom(Cpu *c) { started = 0; c->r[2] = 1; }

/* Estados: 0 sin mando, 2 mando simple estable, 6 mando con actuadores estable. */
void hle_PadGetState(Cpu *c) {
    Pad *p = &pads[port_of(c->r[4])];
    c->r[2] = p->type == PAD_NONE ? 0 : p->type == PAD_DIGITAL ? 2 : 6;
}

/* PadInfoMode(port, term, offs): term 1 = id actual, 2 = número de modos, 3/4 = lista. */
void hle_PadInfoMode(Cpu *c) {
    Pad *p = &pads[port_of(c->r[4])];
    int32_t term = (int32_t)c->r[5], offs = (int32_t)c->r[6];
    uint32_t v = 0;
    if (p->type == PAD_NONE) { c->r[2] = 0; return; }
    switch (term) {
    case 1: v = p->type == PAD_ANALOG ? 7 : 4; break;
    case 2: v = p->type == PAD_ANALOG ? 1 : 0; break;      /* modo actual (0 digital, 1 analógico) */
    case 3: v = p->type == PAD_ANALOG ? 2 : 0; break;      /* número de modos */
    case 4: v = offs == 0 ? 4 : offs == 1 ? 7 : 0; break;
    }
    c->r[2] = v;
}

/* PadInfoAct(port, act, term): act -1 = número de actuadores.
 * Actuador 0: motor pequeño (todo/nada); 1: motor grande (0-255). */
void hle_PadInfoAct(Cpu *c) {
    Pad *p = &pads[port_of(c->r[4])];
    int32_t act = (int32_t)c->r[5], term = (int32_t)c->r[6];
    if (p->type != PAD_ANALOG) { c->r[2] = 0; return; }
    if (act < 0) { c->r[2] = 2; return; }
    static const uint8_t info[2][4] = {{1, 2, 0, 10}, {1, 1, 1, 20}};
    if (act > 1 || term < 1 || term > 4) { c->r[2] = 0; return; }
    c->r[2] = info[act][term - 1];
}

void hle_PadInfoComb(Cpu *c) { c->r[2] = 0; }

void hle_PadSetActAlign(Cpu *c) {
    Pad *p = &pads[port_of(c->r[4])];
    for (int i = 0; i < 6; i++) p->align[i] = mem_r8(c, c->r[5] + (uint32_t)i);
    c->r[2] = p->type == PAD_ANALOG ? 1 : 0;
}

void hle_PadSetAct(Cpu *c) {
    pads[port_of(c->r[4])].act_buf = c->r[5];
    c->r[2] = 1;
}

void hle_PadSetMainMode(Cpu *c) {
    Pad *p = &pads[port_of(c->r[4])];
    if (p->type != PAD_NONE) p->type = c->r[5] ? PAD_ANALOG : PAD_DIGITAL;
    c->r[2] = 1;
}

void hle_PadChkMtap(Cpu *c) { c->r[2] = 0; }
void hle_PadEnableCom(Cpu *c) { c->r[2] = 1; }
/* Envoltorios de librería sobre la BIOS (InitPAD/StartPAD/StopPAD). */
void hle_InitPAD(Cpu *c) { pad_bios_init(c, c->r[4], c->r[5], c->r[6], c->r[7]); c->r[2] = 1; }
void hle_StartPAD(Cpu *c) { pad_bios_start(c); c->r[2] = 1; }
void hle_StopPAD(Cpu *c) { pad_bios_stop(); c->r[2] = 1; }
