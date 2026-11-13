/* vr_hit: volúmenes de impacto por pieza de los enemigos y resolución de impactos, compartido por las balas (vr_fire_hle.c) y los puños (vr/host/vrbodyg.c).
 * C puro sobre la RAM del juego (edición europea); se prueba con RAM sintética y con volcados reales (test_fire.c).
 *
 * Lo que hace el juego (medido en el desensamblado; direcciones en vr_hit.c): la bala es un actor que avanza 5000 unidades por tick y en cada tick prueba el segmento
 * recorrido contra UNA caja alineada por blanco (tabla de 64 blancos de 72 B; soldado: semiejes 300,1000,300 centrados en la raíz); al entrar en una caja mueve su propio
 * blanco (caja de 100) al punto de entrada y la rutina de daño resta su daño (64) a la vida de los blancos dañables que solapan. Alcance de la pistola: 10000 unidades.
 * Antes, al crearla, traza el recorrido contra las paredes y suelos de colisión del mapa del tirador y guarda dónde acaba.
 *
 * Lo que hace este módulo:
 *  - Cápsulas por pieza: de las matrices local->mundo de las piezas del objeto articulado del enemigo (objeto emparejado con el blanco por la posición de su raíz) y de la
 *    caja local de cada modelo. Zonas: cabeza = la pieza hoja más alta en la pose de reposo (suma de las traslaciones de articulación; NO es un índice fijo), cuerpo = sus
 *    antepasadas (cuello, torso, pelvis), extremidad = el resto. Los blancos sin objeto articulado (cámaras, objetos) se quedan con su caja.
 *  - Bala del jugador (vrhit_bullet, tras crear el actor): rayo desde el origen de la bala contra
 *    las cápsulas (o cajas) y contra la geometría real (paredes y suelos de colisión del juego, sólidos de la escena y lo que encontró el trazado del propio juego: puertas). Si da en un blanco antes que en la geometría y dentro
 *    del alcance (>= VRHIT_RANGE_MIN), la bala se recoloca DENTRO de la caja de ese blanco: el juego lo sella en su siguiente tick con sus propias rutinas (daño, reacción,
 *    sonido). Si el rayo cruza cajas sin dar en nada (o hay geometría antes), la bala se lleva a su punto final: no sella ninguna caja. Si no cruza ninguna, no se toca.
 *    Daño: cabeza de un soldado normal (dañable, bando enemigo, caja 300x1000x300, vida <= 3 balas) = su vida (muere de un tiro); todo lo demás (cuerpo, extremidad, jefes y
 *    otros humanoides) = el del juego (64: 2-3 tiros en el soldado).
 *  - Bala de un enemigo: no se recoloca ni se anula. Solo se amplía su recorrido hasta VRHIT_RANGE_MIN cuando su rayo cruza la caja del jugador más allá del alcance del
 *    juego, su trazado no dio en nada y la geometría del anfitrión está despejada hasta la caja; sella o no el tick del propio juego.
 *  - Puños (vrhit_sweep): esfera del puño barrida contra las cápsulas. */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VRHIT_MAX_T 24
#define VRHIT_MAX_CAP 24
#define VRHIT_RANGE_MIN 16000.0f     /* alcance mínimo (unidades del motor) con el que se resuelve una bala: el láser del arma mide 15 m (15780 unidades a 1052 por metro) */
#define VRHIT_AIM_LIFE 120           /* VBlanks de vida de una puntería anotada en el gatillo (la bala del juego sale ~20 después; hasta 60 con la petición en cola) */

enum { VRHIT_Z_NONE = 0, VRHIT_Z_HEAD, VRHIT_Z_BODY, VRHIT_Z_LIMB, VRHIT_Z_BOX, VRHIT_Z_WALL, VRHIT_Z_MISS };
/* Texto del registro: cabeza, cuerpo, extremidad, caja, pared, fallo. */
const char *vrhit_zone_name(int zone);

typedef struct VrHitCap { float a[3], b[3], r; int zone; } VrHitCap;      /* segmento a-b y radio (mundo del motor, unidades) */
typedef struct VrHitTarget {
    uint32_t addr;                       /* dirección del blanco en la tabla del juego */
    int index;                           /* índice en la tabla */
    uint16_t cls, side;
    int vital;                           /* vida (blanco + 0x26) */
    float c[3], h[3];                    /* caja del juego: centro y semiejes */
    uint32_t object;                     /* objeto articulado emparejado (0 = ninguno: vale la caja) */
    int head;                            /* índice de la pieza de la cabeza (-1 sin objeto) */
    int ncap;                            /* cápsulas (una por pieza; 0 = sin objeto o piezas no fiables) */
    VrHitCap cap[VRHIT_MAX_CAP];
} VrHitTarget;

/* Blancos del mapa activo con (clase & need_cls) != 0 y bando distinto de skip_side. caps = 1: empareja el objeto articulado y construye las cápsulas. Devuelve cuántos. */
int vrhit_scan(const uint8_t *ram, unsigned skip_side, unsigned need_cls, int caps, VrHitTarget *out, int max);
/* Primer blanco que toca el rayo o + t*d (d se normaliza), t en [0, max]: cápsulas si las tiene, si no su caja. Devuelve el índice en t[] (-1 ninguno), la distancia y la zona. */
int vrhit_ray(const VrHitTarget *t, int n, const float o[3], const float d[3], float max, float *dist, int *zone);
/* Esfera de radio r barrida de a hasta b contra las cápsulas (solo blancos con cápsulas). Devuelve el índice (-1 sin contacto), la zona y el centro de la esfera en el contacto. */
int vrhit_sweep(const VrHitTarget *t, int n, const float a[3], const float b[3], float r, int *zone, float pt[3]);
/* Punto de la caja del blanco (encogida 2 unidades) más cercano a p. */
void vrhit_clamp_box(const VrHitTarget *t, const float p[3], float out[3]);
/* Rayo contra los suelos de colisión del juego (cuadriláteros de los grupos activos del mapa del jugador): distancia en (1, max] o -1. */
float vrhit_floor_ray(const uint8_t *ram, const float o[3], const float d[3], float max);

/* Geometría y cámaras que aporta el anfitrión (vr/host). Sin registrar (o con NULL) vrhit_bullet y vrhit_aim no hacen nada. */
typedef struct VrHitWorld {
    float units_per_m;                   /* solo para el registro; 0 = el valor real que da vrhit_set_range cada VBlank (calibrado), o 1000 si aún no llegó */
    /* primer impacto del rayo con las paredes de colisión del juego (HZD): distancia en (0, max] o -1. NULL = ninguna (los suelos de colisión se prueban siempre) */
    float (*wall)(const uint8_t *ram, const float o[3], const float d[3], float max);
    /* rayo contra las cámaras de vigilancia vivas: 1 si da en alguna, con la distancia y la dirección de su blanco. NULL = ninguna */
    int (*camera)(uint8_t *ram, const float o[3], const float d[3], float max, float *t, uint32_t *target);
    /* impacto de una bala del jugador en una cámara (blanco que devolvió `camera`) que el juego no va a dañar por sí mismo (clase sin el bit de daño, o fuera de su prueba): el anfitrión le cuenta el impacto. NULL = nada */
    void (*camera_hit)(uint8_t *ram, uint32_t target, const int16_t pt[3]);
    /* primer impacto del rayo con la geometría de RENDER de la escena (cristales con huecos de textura, rejillas, marcos): NO bloquea balas (no es colisión del juego: el trazado del propio juego ya
     * recoge puertas y demás colisión dinámica), solo se cita en el registro [impacto] como `escena=` cuando habría cortado antes. Distancia en (0, max] o -1. NULL = ninguna */
    float (*solid)(const uint8_t *ram, const float o[3], const float d[3], float max);
} VrHitWorld;
void vrhit_set_world(const VrHitWorld *w);

enum { VRHIT_ACT_NONE = 0,     /* la bala no se tocó (no cruza ninguna caja) */
       VRHIT_ACT_SEAL = 1,     /* recolocada dentro de la caja del blanco: el juego lo sella */
       VRHIT_ACT_VOID = 2,     /* llevada a su punto final: no sella nada */
       VRHIT_ACT_RANGE = 3 };  /* bala enemiga: solo se amplió su alcance; sella o no el tick del juego */
typedef struct VrHitShot {
    int side;                  /* bando de la bala (1 = jugador) */
    int target;                /* índice del blanco en la tabla del juego (-1 ninguno) */
    int zone;                  /* VRHIT_Z_* */
    int action;                /* VRHIT_ACT_* */
    int damage;                /* daño con el que queda la bala */
    int vital;                 /* vida del blanco antes del impacto (-1 sin blanco) */
    float dist;                /* unidades del motor hasta el impacto (blanco o pared); el alcance si no hay nada */
} VrHitShot;
/* Resuelve una bala recién creada (actor devuelto por el creador del juego, 0x800779a8). Escribe una línea [impacto] por bala del jugador (y, como observación, por bala
 * enemiga cuyo rayo cruce la caja del jugador). Devuelve 1 si la resolvió (out relleno), 0 si no hay mundo registrado o el actor no es válido. */
int vrhit_bullet(uint8_t *ram, uint32_t actor, VrHitShot *out);
/* Instante del gatillo (el juego crea la bala ~0,4 s después): anota a qué blanco y zona apunta el rayo boca -> delante en ese instante. La bala que salga con ese mismo
 * rayo impacta en ese blanco aunque se haya movido entretanto. */
void vrhit_aim(const uint8_t *ram, const float o[3], const float d[3]);
/* Distancia (unidades del motor) hasta el primer enemigo (cápsulas, o caja sin ellas) que toca el rayo o + t*d en [0, max]; -1 ninguno. No mira paredes (las corta quien llama) ni anota nada: es
 * lo que usa el láser de la pistola para acabar donde impactaría la bala (vrhit_bullet usa los mismos volúmenes). */
float vrhit_probe(const uint8_t *ram, const float o[3], const float d[3], float max);
/* Una vez por VBlank: caduca las punterías anotadas. */
void vrhit_frame(void);

/* ---- láser = bala (una sola prueba de colisión y un solo alcance) ----
 * El alcance del tiro del jugador es el del láser: `units` (unidades del motor; el anfitrión da WLASER_MAX_M * unidades_por_metro cada VBlank; `upm` solo para el registro). Por defecto VRHIT_RANGE_MIN.
 * vrhit_bullet resuelve la bala del jugador con ese alcance y vrhit_shot_dist (láser) con el mismo criterio que vrhit_bullet (resolve): primer blanco (cápsulas/caja/cámara), pared HZD (la misma
 * llamada que usa la bala) y suelos de colisión; NO la geometría de render (no bloquea balas, ver VrHitWorld.solid). */
void vrhit_set_range(float units, float upm);
/* Distancia (unidades) al primer impacto del tiro o + t*d (d se normaliza) con alcance `max`; -1 si no hay impacto en el alcance. Sin anotar nada. */
float vrhit_shot_dist(const uint8_t *ram, const float o[3], const float d[3], float max);
/* La capa VR amplió el alcance de la siguiente bala del jugador que resuelva vrhit_bullet (el sustituto la crea con tipo 2: el juego traza hasta 100000 unidades, ver vr_fire_hle.c): su alcance
 * es el del tiro (vrhit_set_range) y no el que figura en el actor; el tramo que sobra se recorta. Se consume con esa bala. */
void vrhit_next_bullet_extended(void);
/* Anota el rayo del láser (boca y dirección) y dónde acaba, para el registro `[disparo] laser=(x,y,z) bala_impacto=(x,y,z) desvio=` que escribe vrhit_bullet con la bala que salga por ese rayo
 * (mismo origen y dirección que en vrhit_aim; hasta VRHIT_AIM_LIFE VBlanks). */
void vrhit_note_laser(const uint8_t *ram, const float o[3], const float d[3]);

#ifdef __cplusplus
}
#endif
