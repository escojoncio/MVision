/* Lanzamisiles antiaéreo al hombro ("aa"): API propia que extiende el contrato sin tocar weapon.h.
 *
 * Pantalla integrada: pieza "screen" (uv 0..1, u→derecha, v→abajo) orientada hacia el ojo.
 * La app renderiza la escena del juego con WeaponState.aux (cámara = eje del tubo desde WP_EYE,
 * fov estrecho), llama a aa_draw_overlay() sobre el buffer RGBA y lo sube a la textura de la pieza.
 * La pantalla está activa mientras se empuña el arma (update se llama): se ve de reojo. */
#pragma once
#include "weapon.h"

extern const WeaponDef w_aa;

/* Estados del buscador (aa_lock_state). */
typedef enum {
    AA_LK_OFF,      /* sin tubo útil (gastado / ausente) */
    AA_LK_COOL,     /* enfriando el detector tras montar tubo: no adquiere */
    AA_LK_SEARCH,   /* buscando: ningún blanco en el cono */
    AA_LK_ACQ,      /* blanco en el cono, tono de adquisición creciente */
    AA_LK_LOCKED    /* bloqueo: tono continuo (WS_LOCKED) */
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
#define AA_CONE_RAD        0.0698f   /* semiángulo de adquisición: 4,0 grados */
#define AA_TRACK_MULT      1.4f      /* tolerancia de mantenimiento de un blanco ya seguido */
#define AA_LOCK_TIME       1.6f      /* s con el blanco en el cono hasta el bloqueo */
#define AA_GRACE_TIME      0.25f     /* s fuera del cono antes de perderlo (reinicia) */
#define AA_COOL_TIME       1.0f      /* s de enfriado tras acoplar un tubo */
#define AA_RANGE_MIN       15.0f
#define AA_RANGE_MAX       450.0f
#define AA_VIEW_FOV        0.2094f   /* 12 grados verticales de la pantalla */
#define AA_VIEW_W          256
#define AA_VIEW_H          192
#define AA_SLIDE_TRAVEL    0.30f     /* m de carrera de encaje del tubo */

typedef struct {
    WeaponState base;
    int lk;                   /* AALock */
    float lock_t;             /* s acumulados con el blanco en cono */
    float grace;              /* s fuera de cono */
    float cool_t;
    float beep_t;             /* temporizador del tono de adquisición */
    int tgt_valid;
    WVec3 tgt;                /* última posición mundo del blanco seguido */
    float tgt_range;
    float ndc_x, ndc_y;       /* blanco en pantalla (-1..1) */
    int tgt_on_screen;
    int guiding;              /* misil en vuelo guiado hacia tgt */
    WVec3 guide_tgt;
    int trig_armed;           /* gatillo ya apretado (1 al equipar: hay que soltarlo antes de disparar) */
    int dry_lat;              /* aviso de "sin tubos" ya emitido */
    float rel_timer;          /* temporizadores de recarga */
    float drop_v;             /* velocidad de caída del tubo gastado */
    int carry_valid;          /* el tubo nuevo viaja con la mano de apoyo */
    WVec3 ins_hand0;          /* mano en el inicio del empuje */
    float flash_t;            /* parpadeo de la pantalla */
    uint32_t seed;
} AAState;

/* Estado del buscador y progreso de adquisición (0..1). */
int aa_lock_state(const WeaponState *s);
float aa_lock_progress(const WeaponState *s);
/* Pose mundo del tubo de repuesto mientras viaja en la mano (mesh "tube" de mesh_names, origen en su
 * centro). Devuelve 0 si no hay tubo en la mano. */
int aa_spare_pose(const WeaponState *s, const WInput *in, WPose *out);
/* Zona de la espalda donde está el tubo de repuesto (mundo); radio en m. */
WVec3 aa_back_zone(const WInput *in, float *radius);
/* Dibuja retícula, caja de fijación y telemetría sobre el RGBA de la vista (w*h). */
void aa_draw_overlay(const WeaponState *s, uint32_t *rgba, int w, int h);
