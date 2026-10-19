#pragma once
/* Adaptador de ns_fx al contrato del renderer nativo (ns_types.h): NsFxScene.items -> NsEffect. Los efectos de pantalla completa (NsFxScreen) no
 * tienen sitio en NsScene: el consumidor los lee de NsFxScene.screen. Las sombras de personaje (NSFX_F_FROM_OBJ) salen aquí como QUAD con
 * NsFxScene.shadow_objs: el extractor de mallas debe OMITIR esos objetos o la sombra se dibujaría dos veces. */
#include "ns_fx.h"
#include "ns_types.h"

/* include_culled: 0 omite los elementos que el motor no emitió en ese fotograma (NSFX_F_ENGINE_CULLED); 1 los entrega (el renderer decide por su cámara).
 * Devuelve el nº de efectos escritos (<= cap). `truncated` (opcional) se pone a 1 si no cupieron todos. */
uint32_t ns_fx_export(const NsFxScene *sc, int include_culled, NsEffect *out, uint32_t cap, int *truncated);
