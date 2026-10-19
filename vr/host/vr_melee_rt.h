/* Unión de vr_melee con el runtime (sustitución de función por dirección con guarda de hash, ver runtime/rtpatch.h). */
#pragma once
#include "vr_melee.h"

int  vrmelee_rt_install(void);          /* 0 = ok; <0 = no se pudo (nada queda parcheado) */
void vrmelee_rt_uninstall(void);
int  vrmelee_rt_installed(void);
Vrm *vrmelee_instance(void);
void vrmelee_rt_frame(const VrmFrame *f);   /* una vez por VBlank (hilo de emulación) antes del fotograma del juego */
