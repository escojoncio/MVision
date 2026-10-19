/* Sustitutos débiles de las HLE de las capas de cámara y cuerpo VR (vr/camera/vr_camera_hle.c define las reales) y del radar
 * (vr/radar/vr_radar_hle.c).
 * hle_table.c (generada también de vr/camera/hle_vrcam.txt) los referencia; las herramientas y pruebas que enlazan los .c de runtime/
 * sin la capa de cámara siguen enlazando, y si alguien los enlazara por dirección se comportarían como el código original. */
#include "cpu.h"

#define WEAK_ORIG(sym, name) __attribute__((weak)) void sym(Cpu *c) { rt_orig(c, name); }
WEAK_ORIG(hle_vrcam_view, "vrcam_view")
WEAK_ORIG(hle_vrcam_padorg, "vrcam_padorg")
WEAK_ORIG(hle_vrcam_trans, "vrcam_trans")
WEAK_ORIG(hle_vrcam_draw, "vrcam_draw")
WEAK_ORIG(hle_vrcam_overscan, "vrcam_overscan")
WEAK_ORIG(hle_vrcam_pack, "vrcam_pack")
WEAK_ORIG(hle_vrbody_sort, "vrbody_sort")
WEAK_ORIG(hle_vrradar_draw, "vrradar_draw")
