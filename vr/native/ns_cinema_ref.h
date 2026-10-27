#ifndef NS_CINEMA_REF_H
#define NS_CINEMA_REF_H
/* ns_cinema_ref: herramienta/validador de ESCRITORIO (no entra en la app) que reproduce en C, para triángulos del rasterizador de referencia, la cadena de la pantalla 3D de
 * las cinemáticas tal como la hace la app: ns_cinema_to_vr (NativeRenderer) -> vista de ojo (XRStereo.eye, stereoCamera de RenderRG) -> XRMath.projection -> píxel.
 * Sirve para comparar la imagen de esa cadena con la cámara directa del motor (sin pasar por game_view ni por la vista GL) y con el fotograma real. */
#include "ns_adapt.h"
#include "ns_cpuref.h"
#include "ns_types.h"
#ifdef __cplusplus
extern "C" {
#endif

#define NS_CINE_NGV 4

/* Cámara directa del motor (mundo -> vista, con el 66/64 en Y): e = A (p - eye) con A = diag(1, ys, 1) R^T; t = -A eye. */
void ns_cine_engine_view(const NsCamera *cam, float A[9], float t[3]);
/* game_view (columna-mayor) de prueba k: 0 = diag(k,-k,-k) y (0,1.5,0); 1 = k R con la traslación de la cámara (como vr_scene sin mapa de usuario); 2 = giro Y de 90 grados,
 * traslación cualquiera; 3 = vista con el eje Z espejado (zs = -1 como vr_scene con mirror_z). k = 0.001 m por unidad. */
void ns_cine_test_gv(const NsCamera *cam, int k, float gv[16]);
/* Cadena de la app para el ojo `eye`: transforma los triángulos (mundo del motor) con to_vr, la vista del ojo y deja `c` listo para ns_render_view (e = X der, Y abajo, Z delante en
 * metros del ojo, fx/fy/cx/cy desde las tangentes de ns_cinema_window + ns_cinema_eye + el desplazamiento del centro). Comprueba además (abort silencioso = devuelve 0) que la
 * proyección GL (ns_cinema_projection) y el pinhole dan el mismo píxel. `ctx_*` = fracciones (tx, ty) no usadas: reservadas (0). */
int ns_cine_app_chain(const NsCamera *cam, const float gv[16], int eye, float ipd, float conv, float unused_a, float unused_b, NsRTri *tris, size_t n, int w, int h, NsRCam *c);

/* Descarte por detrás de la app (ns_gpu_tri_back) sobre triángulos del adaptador SIN cull_eye (src_out): quita los de modelos de una sola cara (NS_MF_TWOSIDED ausente en NsAdaptSrc.mflags)
 * vistos por detrás desde `eye`. El 2.º triángulo de un cuadrilátero del adaptador es (B,D,C); el de la GPU, (B,C,D): se invierte. Compacta tris y src en sitio y devuelve el nuevo número. */
size_t ns_cine_cull_back(NsRTri *tris, struct NsAdaptSrc *src, size_t n, const float eye[3]);
/* Igual con la política del sombreador (NSG_CULL_ALL = la de arriba; NSG_CULL_SEMI = solo mallas semitransparentes: la superficie del agua vista desde abajo con la cabeza sumergida). */
size_t ns_cine_cull_back_mode(NsRTri *tris, struct NsAdaptSrc *src, size_t n, const float eye[3], int mode);

typedef struct { double block_corr; double com_a[2], com_b[2]; double com_dist; } NsCineScore;
/* Compara dos RGB8 w x h: correlación media por bloques de 16x16 de la luminancia (1 = igual, ~0 = no relacionadas, <0 = invertidas) y centro de masas de la luminancia (0..1)
 * de cada una y su distancia. */
void ns_cine_score(const uint8_t *a, const uint8_t *b, int w, int h, NsCineScore *out);

#ifdef __cplusplus
}
#endif
#endif
