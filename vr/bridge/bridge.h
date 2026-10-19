/* bridge: puente de lectura de la RAM emulada (2 MB, original) -> estado del juego para la capa VR.
 * SOLO LECTURA: nunca escribe en la RAM. C99, sin dependencias de plataforma ni del runtime.
 *
 * Uso: `bridge_read(emu_ram, &st)` UNA vez por fotograma, SOLO desde el gancho de fotograma/VBlank del host
 * (el que el runtime llama entre dos fotogramas del juego, con la CPU emulada detenida), y nunca desde otro hilo
 * mientras la CPU corre: la lectura NO es atómica y a mitad de actualización del juego se verían listas, posiciones
 * y matrices de fotogramas distintos. Desde ese gancho todos los campos son del mismo fotograma.
 * `st.valid` indica qué grupos son fiables; un grupo no válido está a cero. Criterio conservador para mover la
 * cámara/dibujar: exigir `valid & BRIDGE_OK_ALL_CORE` == ALL_CORE y `has_player`; en título, cargas y cambios de
 * zona (varios fotogramas) las variables de partida conservan valores iniciales que "parecen" válidos, y lo que
 * lo delata es `has_player == 0` (no hay bloque de control del jugador); `in_control` ya lo incluye. Al pasar de
 * no-válido a válido conviene esperar unos fotogramas antes de fiarse (histéresis) y no suavizar nunca a través
 * de un fotograma no válido.
 * Los valores están en las unidades del motor (enteros): posición en unidades de mundo del motor, ángulos con
 * 4096 = vuelta completa (s16 crudo del juego: puede ser negativo o >= 4096, usar bridge_deg_norm); ver
 * bridge_addrs.h. Funciones auxiliares de conversión al final.
 *
 * Lectura robusta: toda dirección se enmascara a 2 MB, los punteros se validan (alineación, rango y estructura
 * esperada) y los recorridos de listas tienen tope y detección de bucles; un volcado corrupto no puede hacer que
 * se lea fuera de `ram` ni que el recorrido no termine.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "bridge_addrs.h"

/* Grupos de campos válidos (BridgeState.valid) */
enum {
    BRIDGE_OK_VARS       = 1u << 0,  /* vida, equipo, inventario, posición de partida */
    BRIDGE_OK_PLAYER_POS = 1u << 1,  /* player_pos */
    BRIDGE_OK_PLAYER_CTL = 1u << 2,  /* bloque de control del jugador: ctl_pos, rot, altura */
    BRIDGE_OK_PLAYER_ST  = 1u << 3,  /* player_status, action, stance, derivados */
    BRIDGE_OK_LIFE       = 1u << 4,  /* life, life_max coherentes */
    BRIDGE_OK_EQUIP      = 1u << 5,  /* weapon, item, ammo/max por arma, items */
    BRIDGE_OK_EYE        = 1u << 6,  /* matriz de vista del motor (eye_*) */
    BRIDGE_OK_CAMERA     = 1u << 7,  /* cámara del juego (cam_*) */
    BRIDGE_OK_ACTORS     = 1u << 8,  /* lista de actores */
    BRIDGE_OK_CONTROLS   = 1u << 9,  /* lista de controles (posiciones de personajes) */
    BRIDGE_OK_ENEMIES    = 1u << 10, /* enemies[] (derivado de controles + blancos) */
    BRIDGE_OK_TARGETS    = 1u << 11, /* lista de blancos */
    BRIDGE_OK_ALERT      = 1u << 12, /* alert_* */
    BRIDGE_OK_AREA       = 1u << 13, /* stage_code, area_id */
    BRIDGE_OK_MODE       = 1u << 14, /* game_status, pause_level, menu_state y banderas derivadas */
    BRIDGE_OK_ALL_CORE   =  0x7FFFu
};

#define BRIDGE_MAX_ACTORS   384
#define BRIDGE_MAX_CONTROLS 96
#define BRIDGE_MAX_TARGETS  64
#define BRIDGE_MAX_ENEMIES  96
#define BRIDGE_N_ACTOR_LISTS 9

typedef struct { int16_t x, y, z; } BVec3;

typedef struct {
    uint32_t addr;       /* dirección del nodo */
    uint8_t level;       /* lista (0..8) */
    uint32_t act, die;   /* funciones de actualización y de cierre (direcciones de código) */
    uint32_t name_ptr;   /* puntero a la cadena de depuración del actor (0 si no válido) */
    char name[20];       /* copia (máx. 19 car. imprimibles) de esa cadena; vacío si no válida */
    int32_t runtime, count;
} BridgeActor;

typedef struct {
    uint32_t addr;       /* dirección del bloque de control */
    uint16_t name_id;    /* hash del actor */
    BVec3 pos, rot;      /* rot: 4096 = vuelta */
    int16_t height;
    uint32_t map_bits;   /* bit(s) de mapa del control (0 si no se pudo leer) */
    uint16_t radar;      /* atributos de radar */
    uint8_t alert_state; /* radar>>12: 0 normal, 1 sospecha/alerta, 2 cámara de vigilancia */
    uint16_t cone_dir, cone_dis, cone_range, cone_r;
    uint8_t is_player;
    int16_t actor;       /* índice en actors[] del actor dueño del control (actor + 0x20), -1 si ninguno */
} BridgeControl;

typedef struct {
    uint32_t addr;
    uint16_t cls, side, map, damaged;
    BVec3 center, half_size;
    int16_t vital, damage, faint;
    int32_t weapon;
} BridgeTarget;

/* Enemigo derivado: control visible en el radar que no es el jugador; se le asocia (si hay) el blanco enemigo
 * más cercano de su mismo mapa (distancia horizontal <= 700). Confianza media: la asociación es heurística. */
typedef struct {
    int16_t control;     /* índice en controls[] */
    int16_t target;      /* índice en targets[] o -1 */
    BVec3 pos, rot;
    uint8_t alert_state; /* 0 normal, 1 sospecha/alerta, 2 cámara */
    uint8_t has_cone;
    uint16_t cone_dir, cone_dis, cone_range;
    int16_t vital, faint;/* del blanco asociado (0 si no hay) */
    uint8_t down;        /* blanco caído */
} BridgeEnemy;

typedef struct BridgeState {
    uint32_t valid;      /* BRIDGE_OK_* */

    /* --- jugador --- */
    BVec3 player_pos;    /* BA_PLAYER_POS */
    BVec3 ctl_pos, ctl_rot; /* del bloque de control; yaw = ctl_rot.y */
    int16_t ctl_height;
    uint32_t ctl_addr;
    int32_t player_map;
    int32_t player_action;
    uint32_t player_status;
    int8_t stance;       /* 0 de pie, 1 agachado, 2 tumbado, 3 intruso */
    uint8_t crouching, prone, on_wall, knocking, first_person, in_box, moving, hidden, damaged, in_water;
    int16_t life, life_max, o2;

    /* --- equipo --- */
    int16_t weapon;      /* -1 ninguna, 0..9 */
    int16_t item;        /* -1 ninguno, 0..23 */
    int16_t weapon_ammo[BRIDGE_N_WEAPONS];  /* -1 no la tiene; si la tiene, unidades (sin el bit de bloqueo) */
    int16_t weapon_max[BRIDGE_N_WEAPONS];
    int16_t item_count[BRIDGE_N_ITEMS];     /* -1 no lo tiene; si lo tiene, valor (sin el bit de bloqueo) */
    uint16_t weapon_disabled;               /* bit i: arma i bloqueada por el juego (bit 0x8000 de su entrada) */
    uint32_t item_disabled;                 /* bit i: objeto i bloqueado */
    int16_t item_max[3];
    int16_t magazine, magazine_max;
    uint32_t disable_weapon, disable_item;

    /* --- cámara --- */
    int32_t eye_pos[3];          /* posición del ojo (mundo) */
    int16_t eye_axes[3][3];      /* [fila][col]; columnas = ejes x,y,z de la cámara, 4096 = 1,0 */
    int16_t eye_focal;           /* distancia de pantalla en píxeles */
    int32_t view_t[3];           /* traslación de la matriz mundo->vista */
    BVec3 cam_pos, cam_target, cam_rot;
    int16_t cam_zoom, cam_first_person, cam_type;
    int32_t cam_track;

    /* --- listas --- */
    int n_actors, n_controls, n_targets, n_enemies;
    BridgeActor actors[BRIDGE_MAX_ACTORS];
    BridgeControl controls[BRIDGE_MAX_CONTROLS];   /* [0] = jugador si is_player */
    BridgeTarget targets[BRIDGE_MAX_TARGETS];
    BridgeEnemy enemies[BRIDGE_MAX_ENEMIES];

    /* --- alerta global --- */
    int32_t alert_mode;  /* 0 normal, 1 interferencia, 2 evasión, 3 alerta */
    int32_t alert_level, alert_max, noise_power, noise_len;
    BVec3 noise_pos;

    /* --- zona --- */
    char stage_code[9];  /* código de zona, p. ej. de 4 caracteres */
    int16_t area_id;
    int16_t area_history[8];

    /* --- modo --- */
    uint32_t game_status;
    int32_t pause_level;
    uint8_t menu_state;  /* bit0 menú derecho (objetos), bit1 izquierdo (armas), bit2 comunicador */
    uint8_t cinematic;   /* escena/cinemática (demo del motor o escena con control retirado) */
    uint8_t comms;       /* comunicador abierto */
    uint8_t paused;      /* pausa del juego */
    uint8_t menu_open;   /* menú de armas/objetos abierto */
    uint8_t stopped;     /* juego detenido (comunicador, película, pantalla de resultado...) */
    uint8_t in_control;  /* el jugador controla a su personaje ahora (ni escena, ni comunicador, ni pausa, ni menú) */
    uint8_t on_screen_text; /* hay voz/subtítulos en curso (flujo de voz) */
    uint8_t has_player;  /* hay jugador en escena: PLAYER_POS+CTL+ST válidos (falso en título/carga/cambio de zona) */
} BridgeState;

/* Lee el estado completo. `ram` apunta a 2 MB (BRIDGE_RAM_SIZE) en el orden de la RAM de la consola (el byte
 * de dirección 0x80000000 es ram[0]). Devuelve st->valid. `st` se pone a cero antes. NULL -> 0. */
uint32_t bridge_read(const uint8_t *ram, BridgeState *out);

/* Lectura suelta (validada) para herramientas; devuelven 0 y no tocan *out si la dirección no cae en RAM. */
int bridge_peek32(const uint8_t *ram, uint32_t addr, uint32_t *out);
int bridge_peek16(const uint8_t *ram, uint32_t addr, int16_t *out);
int bridge_addr_ok(uint32_t addr, uint32_t len);

/* Conversiones: ángulo del motor (4096 = vuelta) a grados / radianes. */
static inline float bridge_deg(int16_t a) { return (float)a * (360.0f / 4096.0f); }
/* Igual, normalizado a [0, 360) (el motor acumula ángulos sin envolver; admite también u16 convertido a s16). */
static inline float bridge_deg_norm(int16_t a) { return (float)((uint32_t)(uint16_t)a & 4095u) * (360.0f / 4096.0f); }
static inline float bridge_rad(int16_t a) { return (float)a * (6.28318530718f / 4096.0f); }

/* Descripción corta del estado (para volcados), escribe en buf; devuelve buf. */
const char *bridge_mode_name(const BridgeState *s);
