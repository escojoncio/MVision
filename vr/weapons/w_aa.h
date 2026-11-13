/* Lanzamisiles antiaéreo al hombro ("aa"): tubo de un solo uso y recarga por sustitución del tubo.
 *
 * El FIJADO DE BLANCO NO se hace aquí: lo hace el propio juego (vr/gameplay/vr_aa.h: el buscador del juego corre con la
 * vista del visor de este lanzador y el misil persigue el blanco que ese buscador deja). El arma solo refleja el estado
 * del buscador del juego (aa_feed_game_lock: lo alimenta el anfitrión una vez por VBlank) para el háptico y para el
 * estado visible (aa_lock_state / aa_lock_progress). La mira y el marco de fijación son los del HUD del juego; el tono
 * de fijación es el del juego. Sin vista auxiliar ni superposición propia.
 *
 * El visor (WP_EYE) es el origen de la vista del buscador del juego (vraa_set_scope; su dirección es el eje del tubo, no la de la cabeza); el gesto de apuntar (vraa_gesture) se mide contra la LED (aa_gesture_point). */
#pragma once
#include "weapon.h"

extern const WeaponDef w_aa;

/* Estados del visor (aa_lock_state), reflejo del buscador del juego. */
typedef enum {
    AA_LK_OFF,      /* sin tubo útil (gastado / ausente) */
    AA_LK_STBY,     /* tubo listo, visor sin levantar: el juego no tiene mira ni buscador */
    AA_LK_SEARCH,   /* mirando por el visor, ningún blanco en el cono */
    AA_LK_ACQ,      /* el juego sigue un blanco, marco cerrándose */
    AA_LK_LOCKED    /* marco cerrado (el juego lleva 12 o más fotogramas con el mismo blanco) */
} AALock;

/* Fases de la máquina de estados (WeaponState.phase). */
typedef enum {
    AA_P_BOOT,      /* primera actualización: carga el primer tubo desde el contador del juego */
    AA_P_READY,     /* tubo con misil, listo */
    AA_P_SPENT,     /* tubo disparado, aún acoplado */
    AA_P_EJECT,     /* soltando el tubo gastado (desliza atrás y cae) */
    AA_P_AWAIT,     /* sin tubo: esperando el nuevo (manual: de la espalda) */
    AA_P_CARRY,     /* tubo nuevo en la mano de apoyo, sin acoplar */
    AA_P_INSERT,    /* tubo alineado, empujándolo hacia dentro */
    AA_P_AUTO_LOAD  /* acople automático (modos simplificado/automático) */
} AAPhase;

/* Parámetros de ajuste (documentados en bitacora.md). */
#define AA_BOX_FRAMES      12        /* fotogramas con el mismo blanco hasta que el marco del juego termina de cerrarse */
#define AA_SLIDE_TRAVEL    0.30f     /* m de carrera de encaje del tubo */

/* Estado del buscador del juego (lo rellena el anfitrión desde vr_aa: VrAaLock). */
typedef struct {
    int sight;       /* el buscador del juego está activo este VBlank (visor levantado y mira del juego creada) */
    int target;      /* el juego sigue un blanco */
    int frames;      /* fotogramas seguidos con el mismo blanco */
} AAGameLock;

typedef struct {
    WeaponState base;
    int lk;                   /* AALock */
    AAGameLock gl;            /* último estado del buscador del juego */
    int trig_armed;           /* gatillo ya apretado (1 al equipar: hay que soltarlo antes de disparar) */
    int dry_lat;              /* aviso de "sin tubos" ya emitido */
    float drop_v;             /* velocidad de caída del tubo gastado */
    int carry_valid;          /* el tubo nuevo viaja con la mano de apoyo */
    WVec3 ins_hand0;          /* mano en el inicio del empuje */
    uint32_t seed;
} AAState;

/* Estado del visor y progreso de cierre del marco (0..1) según el buscador del juego. */
int aa_lock_state(const WeaponState *s);
float aa_lock_progress(const WeaponState *s);
/* El anfitrión pasa el estado del buscador del juego una vez por VBlank (antes de update). g == NULL: sin datos. */
void aa_feed_game_lock(WeaponState *s, const AAGameLock *g);
/* Pose mundo del tubo de repuesto mientras viaja en la mano (mesh "tube" de mesh_names, origen en su
 * centro). Devuelve 0 si no hay tubo en la mano. */
int aa_spare_pose(const WeaponState *s, const WInput *in, WPose *out);
/* Zona de la espalda donde está el tubo de repuesto (mundo); radio en m. */
WVec3 aa_back_zone(const WInput *in, float *radius);
/* Pose mundo del visor (WP_EYE): el punto desde el que mira el buscador del juego (para vraa_set_scope). */
WPose aa_sight_pose(const WeaponState *s, const WInput *in);
/* Punto del gesto de apuntar (marco del arma, sin giro: el eje del tubo es -Z): el centro de la pantalla LED (aa_led_face), contra el que vraa_gesture mide la distancia de la cabeza; la LED esta pegada a la cara del
 * jugador cuando mira por el visor, mientras que WP_EYE (origen de la camara del buscador) queda ~0,6 m por delante. Sin modelo enlazado (o s nulo): el centro nominal medido en el modelo real. */
#define AA_LED_NOMINAL_X (-0.070f)
#define AA_LED_NOMINAL_Y  0.172f
#define AA_LED_NOMINAL_Z (-0.044f)
WPose aa_gesture_point(const WeaponState *s);
/* Cara trasera de la mira del modelo (pieza "screen": el polígono por el que mira el ojo), en el marco del arma: face = centro y orientación (+Z hacia el ojo, sin giro) adelantada AA_FACE_LIFT hacia el ojo,
 * w x h = caja del contorno (m, ejes x derecha / y arriba), poly = contorno convexo (npoly <= AA_POLY_MAX) relativo al centro (x derecha, y arriba, m). Devuelve 0 sin modelo enlazado o sin pieza (el anfitrión usa
 * entonces el rectángulo del HUD de siempre). */
#define AA_POLY_MAX   8
#define AA_FACE_LIFT  0.002f
typedef struct { WPose face; float w, h; int npoly; float poly[AA_POLY_MAX][2]; } AaSightFace;
int aa_sight_face(const WeaponState *s, AaSightFace *out);

/* Pantalla LED del Stinger (marco del arma): sobre la cara trasera de la unidad de la mira (plano de la pieza "screen"), 4:3 (marco de sombreador incluido), girada AA_LED_YAW / AA_LED_PITCH
 * hacia el ojo de apuntar y desplazada lo justo para no atravesar ni la unidad ni el tubo (la caja de "screen" da el plano trasero y el borde superior del tubo). Rectangular (npoly = 0).
 * 0 sin modelo enlazado o sin pieza "screen". Sustituye a aa_sight_face (cuadrilatero fino y alabeado ~19 grados, ~113x30 mm) como superficie del visor. */
#define AA_LED_LIFT      0.003f   /* 3 mm hacia el ojo (a lo largo de la normal de la pantalla) */
#define AA_LED_YAW_DEG   8.2f
#define AA_LED_PITCH_DEG 8.6f
int aa_led_face(const WeaponState *s, WScreenFace *out);
