/* Puente C -> Swift del banco del renderer nativo (bench/native/main.swift): puente de escena, empaquetado, referencia por CPU y escena sintética. */
#include "ns_bridge.h"
#include "ns_gpu.h"
#include "ns_screen.h"
#include "ns_present.h"
#include "ns_cinema.h"
#include "ns_solid.h"    /* ns_solid_view_near (cota del paralaje de la pantalla 3D); bench/c/fill_stub.c: 0 = sin cota */
#include "ns_cpuref.h"
#include "native_synth.h"
#include "ns_fill_glue.h"   /* el renderer ya no recoge el relleno; bench/c/fill_stub.c sigue dando sus símbolos al banco */
/* NativeRenderer.ingest pide al anfitrión la pose del cuerpo del jugador por fotograma (vr/host/vrhost.h, que este banco no incluye); bench/c/fill_stub.c: 0 = la de la escena */
int32_t mv_vrhost_body_piece(const void *sc, uint32_t inst, uint32_t k, void *out_xf);
