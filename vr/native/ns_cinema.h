#ifndef NS_CINEMA_H
#define NS_CINEMA_H
/* ns_cinema: cinemáticas del motor con el render nativo. La pantalla 3D de la cinemática enseña el mundo desde la CÁMARA DEL JUEGO (no desde la cabeza): se dibuja la
 * escena nativa dos veces (ojo izquierdo/derecho) en las texturas de la pantalla, con la proyección del motor (x_px = OFX + H*vx/vz, y_px = OFY + H*vy*66/64/vz).
 * Este módulo da la matriz mundo del motor -> mundo VR de esa cámara y las cámaras estéreo (réplica de XRStereo.eye en app/Sources/XRMath.swift: mantener sincronizado).
 * Convenciones: matrices de 16 floats en orden de COLUMNAS (como simd); to_vr de 12 floats fila-mayor (3x4, como NsScene.to_vr). C puro (probado en Linux). */
#include "ns_types.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Mundo del motor -> mundo VR (m) de la cámara del juego: gv * diag(1, overscan_y, 1) * (R^T (p - eye)), con R = cam->rot (vista -> mundo) y gv = VrScene.game_view
 * (vista del motor en unidades -> mundo VR en metros). 0 si los datos no son finitos o gv es singular. */
int ns_cinema_to_vr(const NsCamera *cam, const float gv[16], float out[12]);

/* (ipd 0 = pantalla de cine 3D mono: ambos ojos idénticos.) Cámara estéreo de la pantalla 3D (réplica de XRStereo.eye): vista (mundo VR -> ojo GL: x derecha, y arriba, -z delante) y tangentes (izquierda, derecha, arriba, abajo).
 * 0 si los datos no son válidos. */
int ns_cinema_eye(const float gv[16], int eye, float ipd, float conv, float tan_x, float tan_y, float view[16], float tangents[4]);

/* Tangentes base de la ventana de la pantalla del motor con la H REAL de la escena (cam->h) y su centro de proyección (cam->ofx/ofy; 0 = centro 160/120):
 * x_px = OFX + H*vx/vz  =>  izquierda = OFX/H, derecha = (320-OFX)/H, arriba = OFY/H, abajo = (240-OFY)/H. Devuelve tan_x = 160/H, tan_y = 120/H (para ns_cinema_eye) y el
 * desplazamiento del centro `shift` = {dx, -dx, dy, -dy} (tangentes) que se SUMA a las tangentes de ns_cinema_eye. 0 si H no es válida. */
int ns_cinema_window(const NsCamera *cam, float *tan_x, float *tan_y, float shift[4]);

/* Ventana estéreo de la pantalla 3D. La pantalla es un recuadro fijo (el MISMO para los dos ojos) de semiángulo horizontal atan(tan_screen) visto por el usuario a `dist` m; dentro,
 * cada ojo ve la escena con cámaras paralelas separadas `b` y ventana desplazada (ns_cinema_eye) que convergen a `conv` m. Un punto a z m de la cámara del juego se ve con un paralaje
 * angular (radianes, ángulo pequeño) de  M * b * (1/conv - 1/z),  M = tan_screen / tan_x (ampliación de la pantalla respecto al campo del motor): 0 en el sujeto, + detrás, - delante.
 * Con b = IPD real y conv fija el paralaje no tenía cota (primeros planos: varios grados cruzados; teleobjetivos: divergencia). Aquí conv = z_subj (el sujeto queda en el plano de la
 * pantalla; desconocido = NS_CINEMA_CONV_DEFAULT) y b = lo mayor que cumple: b <= ipd (nunca más separación que la real), paralaje en el infinito <= min(NS_CINEMA_PAR_POS, ipd/dist)
 * (ipd/dist: desplazamiento en la pantalla <= IPD real, los ojos no divergen) y paralaje en z_near >= -NS_CINEMA_PAR_NEG (solo si 0 < z_near < conv).
 * ipd = separación real de los ojos del usuario (m; 0 = mono: b = 0). tan_x = 160/H del motor. z_near, z_subj (m): ns_cinema_depth; <= 0 = desconocido. 0 si los datos no son válidos. */
#define NS_CINEMA_CONV_DEFAULT 3.0f
#define NS_CINEMA_PAR_POS 0.0174533f   /* 1 grado */
#define NS_CINEMA_PAR_NEG 0.0218166f   /* 1,25 grados */
typedef struct {
    float b;        /* separación de las cámaras (m; 1 unidad del motor = 1 mm) */
    float conv;     /* distancia de convergencia (m) */
    float par_max;  /* paralaje angular en el infinito (grados, +) */
    float par_min;  /* paralaje angular en z_near (grados, -; 0 si z_near no se conoce o no queda por delante del sujeto) */
} NsCinemaStereo;
int ns_cinema_stereo(float ipd, float z_near, float z_subj, float tan_x, float tan_screen, float dist, NsCinemaStereo *out);

/* Profundidades (m desde la cámara del juego, 1 unidad = 1 mm) de lo que la escena enseña: orígenes de las piezas visibles de las instancias NO de escenario (personajes, objetos)
 * que caen dentro de la ventana del motor. z_subj = media armónica ponderada (peso 1 en el centro de la ventana, 0 en sus bordes: continua mientras las piezas se mueven);
 * z_near = la menor menos 0,25 m (radio de la pieza), nunca menos de 0,1. 0 (y ambas a 0) si la cámara no es válida o no hay ninguna pieza así. */
int ns_cinema_depth(const NsScene *sc, float *z_near, float *z_subj);

/* ns_cinema_depth con suavizado temporal, una llamada por escena (el estado `f` a cero al empezar). Dentro de un plano las dos profundidades se filtran en 1/z (lineal en paralaje)
 * con constante de tiempo NS_CINEMA_TAU_S y el reloj de la escena (NsScene.t_ns; la misma escena repetida no avanza), y si deja de haber piezas a la vista se conserva lo último
 * válido: ni la convergencia ni la separación (ns_cinema_stereo) saltan cuando el sujeto sale de la ventana. Excepción: lo cercano que se ACERCA vale en el acto (la separación baja
 * hasta el límite de paralaje cruzado, nunca lo supera); al alejarse vuelve con la constante de tiempo (la separación sube despacio). En un corte de plano (primera escena, la cámara
 * salta más de NS_CINEMA_CUT_DIST unidades o gira más de NS_CINEMA_CUT_DEG grados entre escenas, como vr_cine, o el reloj retrocede) valen sin filtrar las del plano nuevo (0 si no hay).
 * Escena o cámara no válidas: olvida el estado. Devuelve 1 si las profundidades se conocen (> 0). */
#define NS_CINEMA_TAU_S 0.3f
#define NS_CINEMA_CUT_DIST 2500.0f
#define NS_CINEMA_CUT_DEG 28.0f
typedef struct {
    int have;                   /* hay escena anterior */
    uint64_t t_ns;              /* su reloj */
    float eye[3], fwd[3];       /* su cámara (posición y eje Z) */
    float inv_near, inv_subj;   /* 1/m filtradas; 0 = desconocidas */
    float z_static;             /* ENTRADA (la pone quien llama antes de cada llamada): profundidad (m) de lo más cercano que se dibuja en la ventana del motor (ns_solid_view_near: escenario y dinámicos, cara a cara), 0 = desconocida.
                                 * Acota z_near (las profundidades de ns_cinema_depth solo miran los orígenes de las piezas dinámicas) y, sin piezas a la vista, da z_near con el sujeto anterior (o NS_CINEMA_CONV_DEFAULT) */
} NsCinemaDepthFilter;
int ns_cinema_depth_filter(NsCinemaDepthFilter *f, const NsScene *sc, float *z_near, float *z_subj);

/* Proyección de Z invertida (réplica de XRMath.projection) a partir de las tangentes. */
void ns_cinema_projection(const float tangents[4], float znear, float zfar, float out[16]);

#ifdef __cplusplus
}
#endif
#endif
