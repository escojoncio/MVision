/* vr_activity: la SIMULACIÓN de enemigos y sensores no debe depender de la cámara original.
 *
 * En VR el jugador mira lejos y en cualquier dirección; el motor, en cambio, decide algunas cosas según lo que
 * ve la cámara cenital original (ver "Mecanismos" abajo). Esta unidad:
 *  - clasifica los actores de la lista del puente en SOLDADOS y SENSORES (cámaras de vigilancia, focos, torretas
 *    con arma...) con CONMUTADORES SEPARADOS (original / VR) y radios independientes: el comportamiento VR de los
 *    sensores se decide aparte del de los soldados;
 *  - decide, para un punto o un objeto 3D, si está "activo para la VR" (dentro del radio de actividad, por
 *    defecto el tamaño de la zona, y con línea de visión posible);
 *  - ofrece los sustitutos nativos que se registran con rt_patch (vr_activity_rt.h) y una auditoría que explica
 *    por qué un actor lejano NO estaría activo.
 * No escribe en la RAM del juego: solo lee (como el puente) y entrega decisiones a los parches. C99/gnu11, sin
 * dependencias del runtime (el pegamento con el runtime está en vr_activity_rt.{h,c}).
 *
 * ---------------------------------------------------------------------------------------------------------
 * Mecanismos del motor (edición EU; direcciones del EXE principal salvo "ov" = código de overlay de zona)
 * ---------------------------------------------------------------------------------------------------------
 * Resultado del análisis: NADA se congela, se desactiva ni se anima a menor frecuencia por estar fuera de
 * cámara, de pantalla o a distancia. Los actores corren todos, todos los fotogramas, y no hay LOD de animación.
 * Lo que sí depende de la cámara/mapa:
 *  M1  Planificador de actores (9 listas @0x800ADDA8): se ejecutan todos los de cada lista salvo que
 *      (pausa_de_la_lista & nivel_de_pausa_global) != 0 (bit0 comunicador/película, bit1 pausa, bit2 menú).
 *      Sin distancia ni cámara. Los soldados están en la lista 4 (nivel normal).
 *  M2  Animación: cada actor llama a la función de "actualizar objeto" (EU 0x80035968, que llama a la de
 *      reproducir movimiento 0x800356c8) cada fotograma, sin condición de dibujo. Las matrices de articulaciones
 *      las calcula el paso de pantalla del canal 3D para TODA la cola del canal, sin mirar la bandera de
 *      invisible ni clip_state. No hay LOD de animación. => la animación NO depende de que el objeto se dibuje.
 *  M3  Paso de límites del canal 3D (0x80018c8c): por bloque de objeto 3D: si flag&0x80 (invisible) -> clip_state=0;
 *      si id de grupo!=0 y (id de grupo & grupo_actual @0x800ACAF8)==0 -> 0; si flag&0x20 (caja global) -> prueba de caja
 *      contra la pantalla ORIGINAL (320x224 del canal 1) -> 0/1/2; si no 2. Se guarda en clip_state (s16
 *      @+0x32 del bloque) en la instrucción 0x80018fc4 (sh $7,0x32($4)) y decide si se construyen paquetes de
 *      polígonos (los libera tras 8 fotogramas). Bloque: +0x14 world.t (posición mundo), +0x24 def, +0x28 flag,
 *      +0x2c id de grupo, +0x2e nº de modelos, +0x30 canal, +0x32 clip_state, +0x48 objs[] (0x5C B cada uno).
 *  M4  El juego LEE clip_state en lógica, no solo en dibujo. Lectores en overlays de enemigos (cada zona tiene su
 *      propia copia en direcciones distintas, p. ej. 's00a' 0x800c72a4 / 0x800cd2a0 / 0x800d18bc / 0x800d5760;
 *      la tabla por overlay sale de buscar `lh/lhu rt,0x32(rs)`):
 *       a) soldado "ir a una zona" (ov 0x800cd2a0, rama clip_state==0 && bit 0x100 del estado de acción): si la
 *          cámara no lo ve, en vez de andar se TELETRANSPORTA por las zonas de ruta (función de "salir de
 *          pantalla por zonas", EU 0x8005dce0, que usa la proyección a pantalla 0x8001c30c para decidir qué
 *          zona queda fuera de pantalla).
 *       b) soldado en marcha (ov 0x800c72a4): clip_state==0 -> gira de golpe a la dirección de marcha;
 *          !=0 -> giro suave. Solo estética del giro.
 *       c) cámara de vigilancia (ov): clip_state!=0 -> actualiza la matriz de luz del modelo (solo iluminación).
 *       d) objetos sueltos: si clip_state==0 y se agotó su tiempo -> desaparecen; roedores; cuerpo del jugador.
 *      Con la cámara original lo habitual es clip_state==0 para todo lo lejano (comprobado en el muelle:
 *      soldados a 10000-20000 unidades con clip_state 0).
 *  M5  Proyección de puntos a pantalla (0x8001c30c, llamadores EU: 0x8005dde8 la función de M4a, 0x80078124 un
 *      efecto, y la rama M4a de cada overlay, p. ej. 's00a' 0x800cd32c): proyecta con la matriz del canal 1
 *      (@0x800B2B8C) y marca `pad`=1 en cada SVECTOR si cae en la pantalla original. La variante de un punto
 *      (0x8001c564; llamadores 0x8002d814 = orden de guion "¿en pantalla?" y 0x800659b4 = cámara de fotos) debe
 *      seguir siendo ORIGINAL: son reglas del juego ligadas a la cámara que sí se usa (foto) o a guiones.
 *  M6  Mapas/zonas: cada actor tiene un bit de mapa; el conjunto de mapas del jugador (0x800ACB9C) es el OR de
 *      los mapas "usados" (altas/bajas desde el guion de la zona) y es también el grupo de dibujo. Un actor en un
 *      mapa NO usado sigue corriendo pero: no ve ni oye al jugador (visión, ruido, cámaras, focos), su modelo no
 *      se dibuja y no ejecuta efectos. Es lógica de guion (puertas, plantas), no de cámara: este módulo NO la
 *      modifica (ver pendientes).
 *  M7  Reparto en el tiempo: la visión de cada soldado solo se evalúa cuando (contador % intervalo) == su
 *      índice (ov, 's00a' 0x800c9e68 y similares): escalonamiento por soldado, no por cámara; es dominio de
 *      vr_perception.
 *  M8  Banderas globales: la bandera 0x200 del estado global (0x800AC570) apaga a todos los soldados
 *      (guiones); fundido de salida individual. Independientes de la cámara.
 *  M9  Cámaras de vigilancia/focos/torretas (SENSORES): se ejecutan cada fotograma; barren sin condición de
 *      cámara; su detección solo mira el mapa del jugador, el objeto de camuflaje y la geometría. Lo único ligado
 *      al dibujo es la matriz de luz (M4c). No se congelan fuera de la cámara original.
 *
 * Conclusión de diseño: para que la simulación no dependa de la cámara original basta con
 *   (1) que clip_state NO sea 0 para soldados/sensores activos dentro del radio (gancho a mitad de función en
 *       0x80018fc4), y
 *   (2) que la proyección a pantalla M5 considere "visible" todo punto activo (parche de 0x8001c30c),
 * con conmutadores separados soldados / sensores. Todo lo demás ya corre a frecuencia completa.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "bridge.h"

typedef enum {
    VRA_CLASS_SOLDIER = 0,   /* soldados de infantería (y NPC que usan su IA) */
    VRA_CLASS_SENSOR  = 1,   /* cámaras de vigilancia, focos, torretas/cámaras con arma */
    VRA_CLASS_COUNT   = 2,
    VRA_CLASS_OTHER   = 255  /* cualquier otro actor: nunca se toca */
} VraClass;

typedef enum { VRA_ORIGINAL = 0, VRA_VR = 1 } VraMode;

#define VRA_MAX_TRACKED 96
#define VRA_MAX_OBJS    4      /* objeto 3D distintos por actor (cuerpo, arma...) */
#define VRA_WARMUP      3      /* fotogramas válidos seguidos antes de activar (histéresis del puente) */

/* Razones de inactividad (VraAudit.why); 0 = activo. */
enum {
    VRA_WHY_NOT_VR        = 1u << 0,  /* el conmutador de su clase está en ORIGINAL */
    VRA_WHY_OUT_OF_RANGE  = 1u << 1,  /* fuera del radio de actividad */
    VRA_WHY_NO_LOS        = 1u << 2,  /* la función de línea de visión lo descarta */
    VRA_WHY_NOT_IN_GROUP  = 1u << 3,  /* su modelo no está en el grupo de mapas del jugador (M6) */
    VRA_WHY_INVISIBLE     = 1u << 4,  /* bandera de invisible (lo decide el juego: fundido, muerte...) */
    VRA_WHY_CLIP_ZERO    = 1u << 5,  /* en rango y en grupo pero clip_state==0: el hook no actuó (BUG) */
    VRA_WHY_PAUSED        = 1u << 6,  /* el juego está pausado/detenido (M1) */
    VRA_WHY_ENEMY_OFF     = 1u << 7,  /* soldados apagados por el estado global (M8) */
    VRA_WHY_NO_OBJS       = 1u << 8   /* no se localizó su objeto 3D */
};

/* Línea de visión posible entre el observador y el punto (coordenadas del motor). 1 = posible. NULL = siempre.
 * Debe ser barata y conservadora (devolver 1 ante la duda). */
typedef int (*VraLosFn)(void *user, const int32_t from[3], const int32_t to[3]);

typedef struct VraConfig {
    VraMode mode[VRA_CLASS_COUNT];     /* conmutadores independientes; defecto VRA_VR para ambos */
    int32_t radius[VRA_CLASS_COUNT];   /* radio de actividad (unidades del motor); 0 = automático (zona) */
    int32_t max_dy[VRA_CLASS_COUNT];   /* diferencia de altura máxima (0 = sin límite) */
    VraLosFn los;
    void *los_user;
} VraConfig;

typedef struct VraTracked {
    uint32_t actor;                    /* dirección del actor (BridgeActor.addr) */
    uint8_t cls;                       /* VraClass */
    uint8_t level;                     /* lista del actor */
    uint8_t n_objs;
    uint8_t in_range;                  /* dentro del radio de SU clase (con LOS) */
    uint32_t objs[VRA_MAX_OBJS];       /* objeto 3D localizados */
    int32_t pos[3];                    /* posición mundo del primer objeto 3D */
    int32_t dist;                      /* distancia 3D al observador */
    int32_t prev_pos[3];               /* posición del fotograma anterior (actividad) */
    uint32_t moved;                    /* nº de fotogramas con cambio de posición (medidor) */
    uint32_t frames;                   /* fotogramas observados */
} VraTracked;

typedef struct VraAudit {
    uint32_t actor;
    uint8_t cls, level;
    int32_t dist;
    int16_t clip_state;                /* clip_state del primer objeto 3D (o -1) */
    uint32_t why;                      /* VRA_WHY_* (0 = activo) */
} VraAudit;

typedef struct VrActivity {
    VraConfig cfg;
    int active;                        /* hay partida válida y pasó el calentamiento */
    int warm;
    int32_t viewer[3];                 /* observador (mundo) usado este fotograma */
    int viewer_forced;                 /* vra_set_viewer lo fijó el anfitrión */
    int32_t forced_viewer[3];
    int32_t zone_diag;                 /* diagonal de la zona (radio automático) */
    uint32_t cur_group;                /* grupo de dibujo actual */
    uint32_t game_status;
    int32_t pause_level;
    int n_tracked;
    VraTracked tracked[VRA_MAX_TRACKED];
    /* conjunto objs->índice de tracked para búsquedas desde los parches (hash abierto, 512) */
    uint32_t set_key[512];
    uint8_t set_val[512];
    uint32_t frame;
    /* estadísticas del último fotograma */
    int n_class[VRA_CLASS_COUNT];
    int n_active[VRA_CLASS_COUNT];
} VrActivity;

/* ---- configuración ---- */
void vra_init(VrActivity *a);   /* VR para soldados y sensores, radio automático, sin LOS */
void vra_set_mode(VrActivity *a, VraClass cls, VraMode m);
VraMode vra_get_mode(const VrActivity *a, VraClass cls);
void vra_set_radius(VrActivity *a, VraClass cls, int32_t r);       /* 0 = zona */
void vra_set_max_dy(VrActivity *a, VraClass cls, int32_t dy);
void vra_set_los(VrActivity *a, VraLosFn fn, void *user);
void vra_set_viewer(VrActivity *a, const int32_t pos[3]);          /* NULL = posición del jugador del puente */

/* ---- por fotograma (gancho de VBlank, CPU parada; después de bridge_read) ----
 * Calcula observador, radio automático (diagonal de la caja de las zonas de ruta de los mapas usados),
 * clasifica los actores del puente, localiza sus objeto 3D por validación estructural y evalúa el rango.
 * Devuelve el nº de actores seguidos. Con partida no válida (título, carga) todo queda inactivo. */
int vra_frame(VrActivity *a, const uint8_t *ram, const BridgeState *st);

/* ---- decisiones puras (usables desde parches y pruebas) ---- */
int32_t vra_radius(const VrActivity *a, VraClass cls);              /* radio efectivo */
int vra_point_active(const VrActivity *a, VraClass cls, const int32_t p[3]);   /* modo VR + activo + rango + LOS */
/* Valor de clip_state que debe quedar guardado para el objeto 3D `objs` dado lo que calculó el motor
 * (`mode_in`), su flag, grupo y el grupo actual. Devuelve mode_in sin cambios si no es un actor seguido, el
 * conmutador de su clase está en ORIGINAL, el objeto es invisible o está fuera del grupo, o ya era != 0. */
int vra_clip_state(const VrActivity *a, uint32_t objs, uint32_t flag, uint32_t group, int mode_in);
VraClass vra_class_of_objs(const VrActivity *a, uint32_t objs);
/* Marca `pad`=1 en cada punto (SVECTOR de 8 B: x,y,z,pad) que debe contar como "en pantalla" aunque la
 * cámara original no lo vea. `pts` apunta a RAM del juego. Solo clase SOLDADO (único llamador de la proyección M5
 * en lógica). Devuelve cuántos puntos cambió de 0 a 1. */
int vra_pointcheck_fixup(const VrActivity *a, uint8_t *ram, uint32_t pts, int n);

/* ---- diagnóstico ---- */
int vra_audit(const VrActivity *a, const uint8_t *ram, VraAudit *out, int max);
VraClass vra_classify_name(const char *name, int level);           /* por hash de la cadena de depuración */
uint32_t vra_name_hash(const char *s);                              /* FNV-1a 32 */
VraClass vra_classify_hash(uint32_t h, int level);
/* Añade (o sustituye) una cadena de depuración de actor a una clase en las listas [lmin,lmax]; máx. 16. 0 = ok. */
int vra_register_name(const char *name, VraClass cls, int lmin, int lmax);
/* Caja de zonas de los mapas usados (EU): devuelve la diagonal 3D en unidades del motor o 0 si no se pudo. */
int32_t vra_zone_diag(const uint8_t *ram, int32_t *box /* NULL o int32[6]: minx,miny,minz,maxx,maxy,maxz */);

/* ---- direcciones EU (RAM de la plataforma original, KSEG0) ---- */
#define VRA_A_CUR_GROUP   0x800ACAF8u  /* grupo de dibujo actual (lw -0x3508 sobre 0x800B0000 en el paso M3) */
#define VRA_A_MAPS        0x800B8A90u  /* MAP[16] de 20 B: index,name u16,used u16,haz,lit,zone (hallado por escaneo) */
#define VRA_MAP_STRIDE    20u
#define VRA_ZONE_SZ 24u
