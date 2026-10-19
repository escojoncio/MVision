/* Puente C -> Swift del renderer VR de la escena (RenderRG.swift). Añadir con #include "RenderRGBridge.h" al
 * Bridging-Header.h. Necesita en HEADER_SEARCH_PATHS: runtime (gpu.h), vr/scene (vr_scene.h) y render/metal (vr_types.h). */
#pragma once
#include "gpu.h"
#include "vr_scene.h"
#include "vr_split.h"
#include "vr_types.h"

/* Escena VR del siguiente frame del juego (la implementa el integrador en runtime/host.c). Devuelve != 0 y rellena
 * `out` si hay una escena NUEVA desde la última llamada; 0 si no (el renderer repite la última). Los punteros de `out`
 * (v, b, src) deben seguir válidos hasta la siguiente llamada. No debe saltarse listas de captura (sus eventos de VRAM
 * se aplican en orden): si se acumulan varias, entregarlas una por llamada. */
extern int mv_host_scene(VrScene *out);
