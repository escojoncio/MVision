/* Lanzamisiles antiaéreo al hombro ("aa"): tubo de un solo uso y recarga por sustitución del tubo.
 *
 * El FIJADO DE BLANCO NO se hace aquí: lo hace el propio juego (vr/gameplay/vr_aa.h: el buscador del juego corre con la
 * vista del visor de este lanzador y el misil persigue el blanco que ese buscador deja). El arma solo refleja el estado
 * del buscador del juego (aa_feed_game_lock: lo alimenta el anfitrión una vez por VBlank) para el háptico y para el
 * estado visible (aa_lock_state / aa_lock_progress). La mira y el marco de fijación son los del HUD del juego; el tono
 * de fijación es el del juego. Sin vista auxiliar ni superposición propia.
 *
 * El visor (WP_EYE) es el punto desde el que se mira: el anfitrión lo pasa a vr_aa (vraa_gesture / vraa_set_scope). */
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
/* Pose mundo del visor (WP_EYE): el punto desde el que mira el buscador del juego (para vraa_gesture/vraa_set_scope). */
WPose aa_sight_pose(const WeaponState *s, const WInput *in);
