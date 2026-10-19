/* Lanzador de misiles guiados al hombro ("guided"): API propia (no toca weapon.h).
 *
 * Flujo: apoyo al hombro -> ojo en la mirilla -> gatillo -> vista del proyectil (WAuxView con
 * use_game_camera=1) -> guiado inclinando el lanzador -> impacto/sin combustible -> estática -> normal.
 * Todo el estado es explícito en GuidedState (testeable sin renderer). */
#pragma once
#include "weapon.h"

extern const WeaponDef w_guided;

/* Piezas lógicas (índice = índice en parts = índice en mesh_names). Los modelos se enlazan en ejecución
 * con weapon_bind() (meshes en el orden de mesh_names); sin binding no se dibuja nada. La mesh de GP_SCREEN
 * debe tener uv 0..1. Este arma no tiene estado global: todo vive en GuidedState. */
enum { GP_TUBE, GP_GRIP, GP_TRIGGER, GP_SIGHT, GP_SCREEN, GP_SCREEN_FRAME, GP_LID, GP_ROUND, GP_COUNT };

/* Fases principales. */
enum { G_IDLE = 0, G_FLIGHT, G_STATIC };
/* Etapas de recarga automática/simplificada. */
enum { GR_NONE = 0, GR_WAIT, GR_OPEN, GR_INSERT, GR_CLOSE };

/* Parámetros ajustables (unidades: metros, segundos, radianes). */
#define GD_SHOULDER_R      0.16f   /* radio de detección del apoyo al hombro */
#define GD_EYE_IN          0.10f   /* ojo a la mirilla: entra el visor grande */
#define GD_EYE_OUT         0.14f   /* ... y sale (histéresis) */
#define GD_STEER_DEAD      0.0524f /* zona muerta (3 grados) */
#define GD_STEER_FULL      0.3840f /* inclinación para guiado máximo (22 grados) */
#define GD_STEER_TAU       0.10f   /* constante de suavizado (s) */
#define GD_FUEL_TIME       14.0f   /* s de combustible */
#define GD_STATIC_TIME     0.80f   /* s de estática al terminar el vuelo */
#define GD_CREATE_GRACE    1.50f   /* s que se espera a que el juego cree el misil antes de darlo por no creado */
#define GD_VIEW_W          320
#define GD_VIEW_H          240

typedef struct {
    WeaponState base;
    int   shouldered;           /* tubo apoyado al hombro */
    int   eye_near;             /* ojo pegado a la mirilla (con histéresis) */
    float view_blend;           /* 0 = pantallita del arma, 1 = visor grande ante el ojo */
    /* vuelo */
    WQuat q0;                   /* orientación de referencia (mano del arma) */
    WVec3 launch_pos; float launch_ground_y;
    float flight_t, fuel;
    int   seen;                 /* el juego ha dado alguna vez pose del proyectil */
    WVec3 proj_pos, proj_prev; int proj_valid;
    float dist, alt, speed;     /* telemetría (metros, m/s) */
    float steer_raw[2], steer[2]; /* yaw, pitch en -1..1 (bruto / suavizado enviado al juego) */
    float stick[2];             /* entrada alternativa (-1..1) */
    float noise;                /* 0..1 nivel de ruido de vídeo */
    float static_t; int ended_by; /* ended_by: 1 impacto, 2 sin combustible, 3 el juego no creó el misil */
    float world_scale;          /* unidades del juego -> metros para la telemetría (1 por defecto) */
    uint32_t frame;             /* contador para el ruido */
    /* recarga */
    int   hand_missile;         /* misil tomado de la espalda en la mano libre */
    float ins;                  /* 0..1 progreso de inserción por detrás */
    float lid_target;
    int   auto_stage; float auto_t;
    float prev_grip1, prev_trig; uint32_t prev_btn0;   /* prev_trig = 1 al equipar: hay que soltar el gatillo */
    float deny_t;
    uint32_t seed;
} GuidedState;

/* Entrada alternativa de guiado (el contrato WInput no trae ejes de stick). */
void guided_set_stick(WeaponState *s, float x, float y);
/* Panel virtual del visor: devuelve el factor 0..1 (0 = solo la pantallita) y rellena la pose
 * (mundo; cara visible = +Z local) y el tamaño en metros. Devuelve 0 si no hay vuelo. */
float guided_view_panel(const WeaponState *s, const WInput *in, WPose *out, float *w_m, float *h_m);
/* Misil en la mano libre (sacado de la espalda): 1 si hay que dibujarlo, con su pose de mundo.
 * Usar la mesh "round" (ranura GP_ROUND). */
int guided_hand_missile_pose(const WeaponState *s, const WInput *in, WPose *out);
/* 1 si la pieza WCH_ROUND debe dibujarse (la pieza se aleja a -400 m cuando no hay misil). */
int guided_round_visible(const WeaponState *s);
/* Dibuja el overlay del visor sobre la textura de la vista (bytes R,G,B,A en memoria).
 * En vuelo se superpone a la imagen del juego (tinte monocromo, ruido, scanlines, retícula,
 * telemetría, marco); en estática o en reposo (fase IDLE) pinta la pantalla completa. */
void guided_draw_overlay(const WeaponState *s, uint32_t *rgba, int w, int h);
