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
#include "vr_hit.h"
#include "rtpatch.h"

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
    uint32_t a0 = A0, a3 = A3;
    if (r == VRFIRE_SUBST) { vrfire_store_mat(emu_ram, zbuf + Z_BULLET, &m); A0 = zbuf + Z_BULLET; }
    /* El tipo 1 de la decompilación (bullet.c NewBullet: alcance 10000, 5000 por tick; pistola y fusil de asalto) traza la colisión del juego solo hasta 10000 unidades, menos que el alcance del tiro VR (SOCOM 25 m, FAMAS 30 m, resto 15 m; vrhit_weapon_range_m): lo que hubiera
     * más allá no llegaba a producir impacto. El tipo 2 (el del PSG1: alcance 100000, misma velocidad) traza hasta 100000 con la misma colisión (HZD_CHK_ALL), así que
     * el juego mismo da el punto, la chispa y el sonido en la pared del láser; vrhit_bullet recorta lo que pase del alcance del láser. */
    const int ext = kind == 0 && r == VRFIRE_SUBST && A3 == 1u;
    if (ext) { A3 = 2u; vrhit_next_bullet_extended(); }
    rt_orig(c, orig);
    A3 = a3;
    if (kind == 0 && r == VRFIRE_SUBST) vrhit_bullet(emu_ram, V0, NULL);     /* bala apuntada con el arma VR: cápsulas, zonas y línea de tiro (vr_hit.h) */
    vrfire_probe(kind, r, V0, &m);
    A0 = a0;     /* a0 es de quien llama (convenio o32: el llamado puede cambiarlo; se restaura por si el llamador lo reutiliza) */
}

void hle_vrfire_bullet(Cpu *c) {
    if ((A1 & 0xFFu) != PLAYER_SIDE) {       /* bala de un enemigo: misma regla de línea de tiro contra la caja del jugador; ni su puntería ni su decisión de disparar se tocan */
        rt_orig(c, "vrfire_bullet");
        vrhit_bullet(emu_ram, V0, NULL);
        return;
    }
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
    if (A2 != 0) {      /* adherida a un blanco móvil (enemigo): el actor sigue la matriz del blanco (puntero que guarda), así que no se le puede dar la de la zona temporal; la petición VR se consume */
        VrFireMat d;
        if (zone()) vrfire_take_projectile(2, &d);
        rt_orig(c, "vrfire_charge");
        return;
    }
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
    int16_t p[3] = {0, 0, 0}, v[3] = {0, 0, 0};
    VrFireMat m = {{0}, {0}};
    int thrown = 0;
    uint32_t a0 = A0, a1 = A1;
    int16_t op[3], ov[3];     /* argumentos originales del juego (para el diagnóstico) */
    for (int i = 0; i < 3; i++) {
        memcpy(&op[i], emu_ram + ((a0 + 2u * (uint32_t)i) & RAM_MASK), 2);
        memcpy(&ov[i], emu_ram + ((a1 + 2u * (uint32_t)i) & RAM_MASK), 2);
    }
    if (zone() && vrfire_take_throw(p, v)) {
        for (int i = 0; i < 3; i++) {
            *(int16_t *)(emu_ram + ((zbuf + Z_POS + 2u * (uint32_t)i) & RAM_MASK)) = p[i];
            *(int16_t *)(emu_ram + ((zbuf + Z_VEL + 2u * (uint32_t)i) & RAM_MASK)) = v[i];
        }
        A0 = zbuf + Z_POS; A1 = zbuf + Z_VEL;
        thrown = 1;
        for (int i = 0; i < 3; i++) { m.t[i] = p[i]; m.m[i] = v[i]; }
    }
    vrfire_log_grenade(op, ov, thrown, p, v);
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

/* ---- C4 directo (vr_fire.h): crear la carga sin la animación de Snake ----
 * hle_vrfire_c4_pump la llama el gancho de la construcción de la cámara (vr_camera_hle.c hle_vrcam_view, lienzo principal: la cámara es un actor y corre en la fase de actores, nunca en la de dibujado) UNA vez por
 * llamada; sin petición pendiente no hace nada. Crea el actor con el código original de NewBakudan (0x8006B69C, el mismo que hle_vrfire_charge sustituye, aquí sin sustituir) como lo llama weapon/bomb.c:
 * NewBakudan(world, NULL, 0, side, GM_BombSeg). La matriz va en la zona propia; el quinto argumento (GM_BombSeg: segmento/suelo al que se pega la carga, solo para que el juego la destruya con una pared rompible
 * o la mueva con un ascensor) va en la pila, 16 bytes sobre un marco nuevo bajo sp, y se pasa 0 (no hay forma de leer GM_BombSeg sin su dirección en la edición europea). Los registros que el llamante espera
 * (a0-a3, v0, v1, sp, ra) se restauran: la HLE sustituida (vrcam_view) sigue con sus argumentos intactos. */
#define SE_PLAY_POS 0x800335a0u         /* GM_SeSet(pos, id) de la edición europea; la misma dirección, longitud y huella que vr/ai/vrai.c (SE_PLAY_NW / SE_PLAY_HASH) */
#define SE_PLAY_NW 67u
#define SE_PLAY_HASH 0xcd503c2b4fcaf495ull
static uint32_t c4_create(void *user, const VrFireMat *m) {
    Cpu *c = user;
    if (!zone()) return 0;
    vrfire_store_mat(emu_ram, zbuf + Z_BULLET, m);
    const uint32_t sp = c->r[29], nsp = sp - 32u, zero = 0;
    memcpy(emu_ram + ((nsp + 16u) & RAM_MASK), &zero, 4);
    c->r[29] = nsp; A0 = zbuf + Z_BULLET; A1 = 0; A2 = 0; A3 = PLAYER_SIDE;
    rt_orig(c, "vrfire_charge");
    const uint32_t res = V0;
    c->r[29] = sp;
    return res;
}
static void c4_sound(void *user, uint32_t pos, int id) {
    Cpu *c = user;
    if (rt_code_hash(SE_PLAY_POS, SE_PLAY_NW) != SE_PLAY_HASH) return;     /* otra edición / otro código: sin sonido antes que llamar a ciegas */
    A0 = pos; A1 = (uint32_t)id;
    rt_call(c, SE_PLAY_POS);
}
void hle_vrfire_c4_pump(Cpu *c) {
    if (!vrfire_c4_pending()) return;
    const uint32_t a0 = A0, a1 = A1, a2 = A2, a3 = A3, v0 = V0, v1 = c->r[3], sp = c->r[29], ra = c->r[31];
    const VrFireC4Ops ops = {c4_create, c4_sound, c};
    vrfire_c4_pump(emu_ram, &ops);
    A0 = a0; A1 = a1; A2 = a2; A3 = a3; V0 = v0; c->r[3] = v1; c->r[29] = sp; c->r[31] = ra;
}
