/* Sustitutos débiles de las HLE de la capa de disparo VR (vr/gameplay/vr_fire_hle.c define las reales).
 * hle_table.c (generada también de vr/gameplay/hle_vrfire.txt) los referencia; las herramientas y pruebas que enlazan
 * los .c de runtime/ sin la capa de disparo siguen enlazando, y si alguien los enlazara por dirección se comportarían
 * como el código original. */
#include "cpu.h"

#define WEAK_ORIG(sym, name) __attribute__((weak)) void sym(Cpu *c) { rt_orig(c, name); }
WEAK_ORIG(hle_vrfire_bullet, "vrfire_bullet")
WEAK_ORIG(hle_vrfire_flash_a, "vrfire_flash_a")
WEAK_ORIG(hle_vrfire_flash_b, "vrfire_flash_b")
WEAK_ORIG(hle_vrfire_rmissile, "vrfire_rmissile")
WEAK_ORIG(hle_vrfire_amissile, "vrfire_amissile")
WEAK_ORIG(hle_vrfire_mine, "vrfire_mine")
WEAK_ORIG(hle_vrfire_charge, "vrfire_charge")
WEAK_ORIG(hle_vrfire_grenade, "vrfire_grenade")
WEAK_ORIG(hle_vrfire_hide, "vrfire_hide")
