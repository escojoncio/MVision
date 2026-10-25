/* Puente C -> Swift: API del host del runtime, entrada VR, jugabilidad VR (manos, arma, radar) y renderer de escena. */
#include "host.h"
#include "vr_input.h"
#include "vrhost.h"
#include "vrgame.h"
#include "vr_view.h"    /* políticas puras de la vista (vrhost_stereo2d_inside) */
#include "ns_bridge.h"
#include "vr_hands_frame.h"
#include "vr_scope.h"
#include "RenderRGBridge.h"
#include "ns_gpu.h"      /* renderer nativo: empaquetado para la GPU (estructuras compartidas con el shader) */
#include "ns_screen.h"   /* efectos de pantalla completa nativos (agua, anillo, fundidos, modos de visión) */
#include "ns_present.h"  /* qué se enseña cuando el nativo no está listo: repetir el último válido o fundir a negro */
#include "ns_fill_glue.h" /* relleno por la verdad del motor: efectos de lo que la lista de la GPU dibuja y el nativo no tiene */
#include "ns_cinema.h"   /* cinemáticas del motor con el render nativo desde la cámara del juego */
