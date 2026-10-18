/* bridge_addrs: direcciones (RAM de PS1, KSEG0) del estado del juego en el EXE principal de la edición europea.
 * Solo lectura. Todas son direcciones del EXE principal (no de overlays), válidas mientras el juego corre.
 * Cómo se halló cada una y su confianza: bridge_addrs.c (tabla `bridge_addr_table`).
 *
 * Convenciones: SVECTOR = {s16 x, s16 y, s16 z, s16 pad}; ángulos 4096 = vuelta completa; MATRIX = {s16 m[3][3],
 * s16 pad, s32 t[3]} (32 B, m en coma fija 1.12: 4096 = 1,0). Eje Y: el de la PS1 (el valor crece hacia abajo en
 * el motor; la capa VR convierte). Las estructuras por actor se leen con los offsets de abajo.
 */
#pragma once
#include <stdint.h>

#define BRIDGE_RAM_SIZE 0x200000u
/* Valor del campo de identificador (u16) del bloque de control del jugador (hash fijo del actor del jugador). */
#define BRIDGE_PLAYER_HASH 0x21CAu

/* ---- Escalares (sección de datos pequeños) ---- */
#define BA_GAME_STATUS      0x800AC570u /* u32  banderas globales del juego (BRIDGE_GS_*) */
#define BA_STAGE_CODE       0x800AC568u /* char[8] código de zona actual, NUL-terminado (p. ej. "q00a") */
#define BA_PLAYER_CTRL      0x800ACB84u /* u32  puntero al bloque de control (posición/rotación) del jugador */
#define BA_NOISE_POS        0x800ACB88u /* SVECTOR posición del último ruido */
#define BA_ALERT_MODE       0x800ACB90u /* s32  0 normal, 1 interferencia, 2 evasión, 3 alerta */
#define BA_PLAYER_MAP       0x800ACB9Cu /* s32  máscara de mapa(s) donde está el jugador */
#define BA_PLAYER_POS       0x800ACBA0u /* SVECTOR posición del jugador (la que usa el resto del juego) */
#define BA_ALERT_LEVEL      0x800ACBA8u /* s32  cuenta atrás de la evasión/alerta (fotogramas) */
#define BA_PLAYER_BODY      0x800ACBB0u /* u32  puntero al objeto animado (modelo) del jugador */
#define BA_NOISE_POWER      0x800ACBB4u /* s32  potencia del ruido */
#define BA_DISABLE_ITEM     0x800ACBB8u /* u32  máscara de objetos bloqueados */
#define BA_MAGAZINE_MAX     0x800ACBBCu /* s16  capacidad del cargador del arma equipada */
#define BA_NOISE_LEN        0x800ACBC0u /* s32  duración restante del ruido */
#define BA_O2               0x800ACBC4u /* s16  oxígeno (1024 = lleno) */
#define BA_PLAYER_ACTION    0x800ACBD0u /* s32  acción/animación actual del jugador */
#define BA_PLAYER_STATUS    0x800ACBE0u /* u32  banderas de estado del jugador (BRIDGE_PS_*) */
#define BA_MAGAZINE         0x800ACB7Cu /* s16  munición en el cargador del arma equipada */
#define BA_ALERT_MAX        0x800ACB70u /* s32  máximo de alerta acumulado en el fotograma */
#define BA_DISABLE_WEAPON   0x800ACB74u /* u32  máscara de armas bloqueadas */
#define BA_AREA_ID          0x800ACB50u /* s16  id (hash) de la zona actual */
#define BA_PAUSE_LEVEL      0x800ACAB8u /* s32  bit0 detenido (comunicador/película), bit1 pausa, bit2 menú, bit3 error de lectura */
#define BA_N_CONTROLS       0x800ACB44u /* s32  nº de bloques de control activos */
#define BA_N_TARGETS        0x800ACBF8u /* s32  nº de blancos usados (tope del array) */

/* ---- Estructuras/tablas ---- */
#define BA_VARS             0x800B5F28u /* s16[96] variables de partida (ver BRIDGE_V* abajo) */
#define BA_AREA_HISTORY     0x800B69E0u /* s16[8] ids de las últimas zonas (0 = la actual) */
#define BA_CONTROL_LIST     0x800B6860u /* u32[96] punteros a bloques de control (0 = jugador) */
#define BA_TARGET_ARRAY     0x800B7660u /* 64 x 72 B blancos (BRIDGE_T*) */
#define BA_CAMERA           0x800B8968u /* cámara del juego (BRIDGE_CAM_*) */
#define BA_VIEW_CHANNEL     0x800B2B7Cu /* canal 3D principal del motor (BRIDGE_CH_*) */
#define BA_ACTOR_LISTS      0x800ADDA8u /* 9 listas x 68 B (nodo inicio, nodo fin, pausa, muerte) */
#define BA_MENU_WORK        0x800BE4E0u /* bloque del gestor de menús (estado en +0x2A) */

/* Índices de las variables de partida (s16 en BA_VARS) */
enum {
    BRIDGE_V_DIFFICULTY = 1, BRIDGE_V_DISC = 4,
    BRIDGE_V_POS_X = 8, BRIDGE_V_POS_Y = 9, BRIDGE_V_POS_Z = 10,
    BRIDGE_V_LIFE = 11, BRIDGE_V_LIFE_MAX = 12,
    BRIDGE_V_WEAPON = 14, BRIDGE_V_ITEM = 15, BRIDGE_V_STANCE = 16,
    BRIDGE_V_WEAPON_AMMO = 17,  /* 10 entradas (-1 = no tiene) */
    BRIDGE_V_WEAPON_MAX = 27,   /* 10 entradas */
    BRIDGE_V_ITEM_COUNT = 37,   /* 24 entradas (-1 = no tiene; consumibles: unidades) */
    BRIDGE_V_ITEM_MAX = 61,     /* 3 entradas: consumibles 13, 14, 15 */
    BRIDGE_V_PLAY_HOURS = 91, BRIDGE_V_PLAY_SECS = 92,
    BRIDGE_V_FIRST_PERSON = 95
};
#define BRIDGE_N_WEAPONS 10
#define BRIDGE_N_ITEMS   24

/* Bloque de control (CONTROL): offsets */
#define BC_MOV      0x00 /* SVECTOR posición */
#define BC_ROT      0x08 /* SVECTOR rotación (4096 = vuelta) */
#define BC_MAP      0x2C /* u32 puntero al mapa (su primer s32 es el índice/bit de mapa) */
#define BC_NAME     0x30 /* u16 identificador (hash) del actor */
#define BC_HEIGHT   0x32 /* s16 altura sobre la posición */
#define BC_RADAR    0x3A /* u16 atributos de radar: bit0 visible, bit2 cono, bit3 todo el mapa, bits 12-15 estado */
#define BC_CONE     0x3C /* {u16 dir, u16 dis, u16 range, u16 r} cono de visión mostrado en el radar */
#define BC_STEP     0x44 /* SVECTOR desplazamiento del fotograma */
#define BC_SIZE     0x80 /* tamaño mínimo que se exige leer */

/* Blanco (TARGET) de 72 B: offsets */
#define BT_SIZE     72
#define BT_CLASS    0x00 /* u16 bits: 1 disponible, 2 captura, 4 daño, 8 empuje, 16 búsqueda, 32 caído, 64 C4 */
#define BT_SIDE     0x02 /* u16 0 ninguno, 1 jugador, 2 enemigo, 3 ambos */
#define BT_MAP      0x04 /* u16 máscara de mapa */
#define BT_DAMAGED  0x06 /* u16 */
#define BT_CENTER   0x08 /* SVECTOR centro */
#define BT_BOX      0x10 /* SVECTOR semitamaño de la caja */
#define BT_VITAL    0x26 /* s16 vida restante del blanco */
#define BT_DAMAGE   0x28 /* s16 daño acumulado en el fotograma */
#define BT_FAINT    0x2A /* s16 contador de aturdimiento/desmayo */
#define BT_WEAPON   0x44 /* s32 arma que lo golpeó por última vez (-1 ninguna) */

/* Cámara del juego: offsets */
#define BCAM_POS    0x00 /* SVECTOR posición */
#define BCAM_TARGET 0x08 /* SVECTOR objetivo */
#define BCAM_ROT    0x10 /* SVECTOR rotación */
#define BCAM_FLAG   0x18 /* u32 */
#define BCAM_TRACK  0x1C /* s32 distancia de seguimiento */
#define BCAM_ZOOM   0x20 /* s16 zoom (320 = normal) */
#define BCAM_FIRST  0x22 /* s16 1 = primera persona */
#define BCAM_TYPE   0x28 /* s16 modo */

/* Canal 3D del motor: offsets desde BA_VIEW_CHANNEL */
#define BCH_EYE_INV 0x10 /* MATRIX mundo->vista (la que usa la GTE) */
#define BCH_EYE     0x30 /* MATRIX vista->mundo: columnas = ejes de la cámara, t = posición del ojo */
#define BCH_SCREEN  0x50 /* s16 distancia de pantalla (focal en píxeles) */

/* Banderas de estado global (BA_GAME_STATUS) */
enum {
    BRIDGE_GS_CHAFF = 0x1, BRIDGE_GS_STUN = 0x2, BRIDGE_GS_NVG = 0x4, BRIDGE_GS_THERMAL = 0x8,
    BRIDGE_GS_BEHIND_CAMERA = 0x10, BRIDGE_GS_VOX_STREAM = 0x20, BRIDGE_GS_CUT_IN = 0x40,
    BRIDGE_GS_ENEMY_OFF = 0x200, BRIDGE_GS_TAKING_PHOTO = 0x400, BRIDGE_GS_CAMERA_ACTIVE = 0x800,
    BRIDGE_GS_RADIO_OFF = 0x2000, BRIDGE_GS_PAUSE_OFF = 0x4000,
    BRIDGE_GS_LIFE_OFF = 0x20000, BRIDGE_GS_EQUIP_OFF = 0x40000, BRIDGE_GS_MENU_DISABLE = 0x80000,
    BRIDGE_GS_RADAR_OFF = 0x400000, BRIDGE_GS_GAME_OVER = 0x4000000,
    BRIDGE_GS_PADMASK = 0x8000000, BRIDGE_GS_PADRELEASE = 0x10000000,
    BRIDGE_GS_PADDEMO = 0x40000000, BRIDGE_GS_DEMO = 0x80000000u
};

/* Banderas de estado del jugador (BA_PLAYER_STATUS) */
enum {
    BRIDGE_PS_WATCH = 0x1,        /* primera persona */
    BRIDGE_PS_INTRUDE = 0x2,
    BRIDGE_PS_ACT_ONLY = 0x4,
    BRIDGE_PS_WATCH_PEEK = 0x8,   /* primera persona asomándose */
    BRIDGE_PS_MOVE = 0x10,
    BRIDGE_PS_SQUAT = 0x20,       /* agachado */
    BRIDGE_PS_GROUND = 0x40,      /* tumbado/arrastrándose */
    BRIDGE_PS_NOT_PLAYABLE = 0x80,
    BRIDGE_PS_DAMAGED = 0x100,
    BRIDGE_PS_DOWNED = 0x200,
    BRIDGE_PS_STILL_WATCH = 0x400,
    BRIDGE_PS_ATTACK = 0x800,
    BRIDGE_PS_BOX = 0x1000,       /* dentro de la caja */
    BRIDGE_PS_GAME_OVER = 0x2000,
    BRIDGE_PS_INVISIBLE = 0x4000,
    BRIDGE_PS_MENU_DISABLE = 0x8000,
    BRIDGE_PS_WALL = 0x10000,     /* pegado a la pared */
    BRIDGE_PS_KNOCK = 0x20000,    /* golpeando la pared */
    BRIDGE_PS_MOVE_WATCH = 0x40000,
    BRIDGE_PS_IN_WATER = 0x1000000,
    BRIDGE_PS_HIDDEN = 0x2000000
};

/* Tabla documentada (bridge_addrs.c) */
typedef struct {
    const char *id;       /* identificador neutro */
    uint32_t addr;
    uint16_t size;        /* bytes */
    uint8_t conf;         /* 3 alta (validada dinámicamente y por código), 2 media, 1 baja */
    const char *tipo;
    const char *metodo;   /* cómo se halló */
} BridgeAddr;
extern const BridgeAddr bridge_addr_table[];
extern const int bridge_addr_count;
