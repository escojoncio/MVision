/* Sustitutos (HLE) de las funciones del motor que crean el proyectil del jugador y de una etapa de dibujado, para
 * el disparo VR (ver vr_fire.h). Se enlazan por dirección fija con los nombres de hle_vrfire.txt y las direcciones
 * de vr_fire_names.json (patrón de vr/camera/vr_camera_hle.c). Cada sustituto delega en el código original con
 * rt_orig() y solo cambia los ARGUMENTOS (matriz de origen/orientación, posición y velocidad de la granada) o
 * suprime la llamada (disparo de desarmado). Se ejecutan en el hilo del juego.
 *
 *   vrfire_bullet     (matriz*, bando, color, tipo)          crea una bala instantánea (pistola, fusil, francotirador)
 *   vrfire_flash_a    (matriz*, matriz*, modo)               resplandor del disparo de la pistola
 *   vrfire_flash_b    (matriz*)                              resplandor del disparo del fusil
 *   vrfire_rmissile   (matriz*, bando)                       misil guiado
 *   vrfire_amissile   (matriz*, bando)                       misil antiaéreo
 *   vrfire_mine       (matriz*, suelo*)                      mina direccional
 *   vrfire_charge     (matriz*, pos*, adherida, bando, seg)  carga adhesiva
 *   vrfire_grenade    (pos*, velocidad*, mecha, tipo, modelo) granada de mano
 *   vrfire_hide       (canal, índice)                        etapa de recorte por objeto; después oculta el arma del juego
 *   vrfire_aalock     (actor)                                paso del buscador de blancos del lanzador antiaéreo (vr_aa.h)
 *   vrfire_aaview     ()                                     carga la matriz del canal 3D principal en la GTE (lo llama el buscador)
 */
#include <stdint.h>
#include <string.h>
#include "cpu.h"
#include "sys.h"
#include "vr_fire.h"
#include "vr_aa.h"

#define RAM_MASK 0x1FFFFFu
#define A0 c->r[4]
#define A1 c->r[5]
#define A2 c->r[6]
#define A3 c->r[7]
#define V0 c->r[2]
#define PLAYER_SIDE 1u

/* Zona propia (kernel, RAM baja libre): 2 matrices de 32 B + 2 vectores de 8 B. Si el runtime se reinicia, llamar a
 * hle_vrfire_reset(). */
static uint32_t zbuf;
static int aa_pending, aa_swapped;       /* buscador antiaéreo (más abajo) */
void hle_vrfire_reset(void) { zbuf = 0; aa_pending = 0; aa_swapped = 0; }
#define Z_BULLET 0
#define Z_FLASH  32
#define Z_POS    64
#define Z_VEL    72
static int zone(void) { if (!zbuf) zbuf = bios_kalloc(96); return zbuf != 0; }

/* Núcleo común de los que reciben (matriz*, ...): kind 0 bala, 1 misil, 2 mina/carga. */
static void with_matrix(Cpu *c, const char *orig, int kind) {
    VrFireMat m = {{0}, {0}};
    int r = zone() ? vrfire_take_projectile(kind, &m) : VRFIRE_PASS;
    if (r == VRFIRE_SKIP) { V0 = 0; return; }
    uint32_t a0 = A0;
    if (r == VRFIRE_SUBST) { vrfire_store_mat(emu_ram, zbuf + Z_BULLET, &m); A0 = zbuf + Z_BULLET; }
    rt_orig(c, orig);
    vrfire_probe(kind, r, V0, &m);
    A0 = a0;     /* a0 es de quien llama (convenio o32: el llamado puede cambiarlo; se restaura por si el llamador lo reutiliza) */
}

void hle_vrfire_bullet(Cpu *c) {
    if ((A1 & 0xFFu) != PLAYER_SIDE) { rt_orig(c, "vrfire_bullet"); return; }
    with_matrix(c, "vrfire_bullet", 0);
}
void hle_vrfire_rmissile(Cpu *c) {
    if ((A1 & 0xFFu) != PLAYER_SIDE) { rt_orig(c, "vrfire_rmissile"); return; }
    with_matrix(c, "vrfire_rmissile", 1);
}
void hle_vrfire_amissile(Cpu *c) {
    if ((A1 & 0xFFu) != PLAYER_SIDE) { rt_orig(c, "vrfire_amissile"); return; }
    with_matrix(c, "vrfire_amissile", 1);
}
void hle_vrfire_mine(Cpu *c) { with_matrix(c, "vrfire_mine", 2); }
void hle_vrfire_charge(Cpu *c) {
    if ((A3 & 0xFFu) != PLAYER_SIDE) { rt_orig(c, "vrfire_charge"); return; }
    with_matrix(c, "vrfire_charge", 2);
}

/* Resplandor: solo si el proyectil anterior fue sustituido en este mismo disparo (vrfire_take_flash lo sabe). */
static void flash(Cpu *c, const char *orig, int style) {
    VrFireMat m;
    uint32_t a0 = A0, a1 = A1;
    if (zone() && vrfire_take_flash(style, &m)) {
        vrfire_store_mat(emu_ram, zbuf + Z_FLASH, &m);
        A0 = zbuf + Z_FLASH;
        if (style == 0) A1 = zbuf + Z_FLASH;
    }
    rt_orig(c, orig);
    A0 = a0; A1 = a1;
}
void hle_vrfire_flash_a(Cpu *c) { flash(c, "vrfire_flash_a", 0); }
void hle_vrfire_flash_b(Cpu *c) { flash(c, "vrfire_flash_b", 1); }

void hle_vrfire_grenade(Cpu *c) {
    int16_t p[3], v[3];
    VrFireMat m = {{0}, {0}};
    int thrown = 0;
    uint32_t a0 = A0, a1 = A1;
    if (zone() && vrfire_take_throw(p, v)) {
        for (int i = 0; i < 3; i++) {
            *(int16_t *)(emu_ram + ((zbuf + Z_POS + 2u * (uint32_t)i) & RAM_MASK)) = p[i];
            *(int16_t *)(emu_ram + ((zbuf + Z_VEL + 2u * (uint32_t)i) & RAM_MASK)) = v[i];
        }
        A0 = zbuf + Z_POS; A1 = zbuf + Z_VEL;
        thrown = 1;
        for (int i = 0; i < 3; i++) { m.t[i] = p[i]; m.m[i] = v[i]; }
    }
    rt_orig(c, "vrfire_grenade");
    vrfire_probe(3, thrown, V0, &m);
    A0 = a0; A1 = a1;
}

/* Etapa de recorte por objeto: tras ella el arma del juego ya tiene su límite de pantalla; se pone a 0. */
void hle_vrfire_hide(Cpu *c) {
    rt_orig(c, "vrfire_hide");
    if (vrfire_hide_enabled()) vrfire_hide_weapon(emu_ram);
}

/* ---- buscador de blancos del lanzador antiaéreo (vr_aa.h) ----
 * El actor del buscador llama UNA vez a aaview (0x800711ac: H de la GTE, rotación y traslación del canal 3D principal) y con esa
 * GTE proyecta los blancos; el canal solo lo lee aaview (comprobado en el código del paso). Por eso la sustitución se hace
 * DENTRO de aaview y solo mientras dura esa llamada: se escribe en el canal la matriz del visor VR y la H del juego, se llama al
 * original (la GTE queda cargada con la vista del visor) y se restauran al instante las dos zonas de memoria del canal. Así el
 * canal nunca queda sustituido fuera de esa llamada (ni si el paso cambia de hilo o cede). Al terminar el paso, SI aaview corrió
 * con el visor, se recarga la GTE con la vista del juego (el estado que dejaría el original: rotación, traslación y H); si el
 * original no llegó a llamar a aaview (se destruye a sí mismo), la GTE no se toca. */
#define CH_MAIN   0x800B2B7Cu
#define CH_VIEW   (CH_MAIN + 0x10u)     /* MAT16 mundo->vista (32 B) */
#define CH_SCREEN (CH_MAIN + 0x50u)     /* s16 distancia de pantalla */

/* aa_pending: dentro de un paso con visor, el siguiente aaview usa aa_view. aa_swapped: aaview corrió con el visor en este paso. */
static VrFireMat aa_view;

void hle_vrfire_aaview(Cpu *c) {
    if (!aa_pending) { rt_orig(c, "vrfire_aaview"); return; }
    aa_pending = 0;                      /* una sola vez por paso */
    uint8_t save[32];
    int16_t focal = 0;
    memcpy(save, emu_ram + (CH_VIEW & RAM_MASK), sizeof save);
    memcpy(&focal, emu_ram + (CH_SCREEN & RAM_MASK), 2);
    int16_t h16 = (int16_t)vraa_view_focal(focal);
    vrfire_store_mat(emu_ram, CH_VIEW, &aa_view);
    memcpy(emu_ram + (CH_SCREEN & RAM_MASK), &h16, 2);
    vraa_note_h((float)h16);
    rt_orig(c, "vrfire_aaview");
    memcpy(emu_ram + (CH_VIEW & RAM_MASK), save, sizeof save);
    memcpy(emu_ram + (CH_SCREEN & RAM_MASK), &focal, 2);
    aa_swapped = 1;
}

void hle_vrfire_aalock(Cpu *c) {
    uint32_t actor = A0;
    aa_swapped = 0;
    aa_pending = vraa_take_view(&aa_view);
    rt_orig(c, "vrfire_aalock");
    aa_pending = 0;
    int sw = aa_swapped;
    aa_swapped = 0;
    if (sw) {
        uint32_t keep[6] = {c->r[2], c->r[3], c->r[4], c->r[5], c->r[6], c->r[7]};
        uint32_t ra = c->r[31];
        rt_orig(c, "vrfire_aaview");     /* GTE otra vez con la vista del juego (aa_pending ya es 0) */
        for (int i = 0; i < 6; i++) c->r[2 + i] = keep[i];
        c->r[31] = ra;
    }
    vraa_after_step(emu_ram, actor, sw);
}
