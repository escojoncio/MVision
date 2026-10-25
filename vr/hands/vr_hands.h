/* vr_hands: del estado del arma (WeaponState + piezas animadas) y de la pose de la mano en el espacio VR a la lista
 * de mallas que dibuja el renderer (RenderWeapon.swift) fuera de la captura de GPU del juego.
 * C99 sin dependencias de plataforma (usa vr/weapons: wcore). No copia geometría: apunta a las mallas del modelo
 * enlazado (weapon_bind) y al atlas que entrega wcut/wload.
 *
 * Uso:
 *   VrHandsConfig c; vrhands_default_config(&c);
 *   VrHandsRig *r = vrhands_create(&c);
 *   // tras wload_model + wcut_run + weapon_bind(state, cs.meshes, cs.nmeshes, cs.parts, cs.nparts):
 *   vrhands_bind(r, state, cs.atlas, cs.atlas_w, cs.atlas_h);
 *   // cada frame, después de update() del arma:
 *   VrHandsFrame f; if (vrhands_build(r, state, &hand0, &f) > 0) vrhands_post(&f);   // hilo de simulación
 *   // render: mv_host_hands(&f) (implementación débil en vr_hands.c: entrega lo último publicado)
 *
 * ESPACIO: metros, Y arriba, -Z delante, el mismo de la cabeza/WInput. Las poses de mano y WHeld.world deben estar
 * en ese espacio (el que usan también las matrices `views` del renderer).
 *
 * AGARRE (mano -> arma). El arma se coloca en mundo como en wcore_weapon_world:
 *       W = mano * R_x(grip_pitch) * recoil_offset * inv(grip)
 * donde `grip` es la pose del punto de empuñadura en el espacio del modelo. Se obtiene así (VRH_GRIP_AUTO):
 *   1. si def->points[WP_GRIP] NO es la identidad, se usa ése (lo fija el autor del arma);
 *   2. si es la identidad (valor por defecto = origen del modelo, que en las pistolas queda 34 mm detrás de la
 *      culata y a la altura del lomo del cargador: la mano flotaría fuera del mango), se DERIVA del propio modelo:
 *      centro de la caja envolvente de los vértices de la pieza raíz ("frame") que están por debajo de la cara inferior
 *      de la corredera ("slide") y detrás del gatillo ("trigger": z mayor que su vértice más trasero). Es el mango
 *      sin guardamonte. Orientación = identidad (los ejes del arma coinciden con los de la mano: -Z = boca);
 *      `grip_pitch_deg` permite inclinar el arma respecto a la mano cuando se conozca el marco real de la mano.
 *   3. si el modelo no tiene esas tres piezas, se usa el de def.
 * AVISO: la lógica del arma (vr/weapons) calcula sus puntos de interacción (pozo, rack, sujeción) con
 * wcore_weapon_world, que usa def->points[WP_GRIP]. Para que dibujo y lógica coincidan hay que fijar en el arma el
 * mismo agarre: vrhands_derive_grip() lo calcula (se puede volcar al arrancar y copiar a w_pist.c).
 *
 * ESCALA: las mallas de wcut/wload salen YA en metros (wload scale = 0.001 m por unidad del motor). `scale` multiplica
 * los vértices si se cargó un modelo en unidades del motor (1 unidad = 1 mm -> scale = 0.001); por defecto 1. */
#pragma once
#include "vr_hands_frame.h"
#include "../weapons/wcore.h"

#define VRH_MAX_MESHES 32     /* piezas + sueltos por frame */
#define VRH_MAX_BOUND 8       /* armas enlazadas a la vez */

typedef enum {
    VRH_GRIP_AUTO = 0,        /* ver arriba */
    VRH_GRIP_DEF,             /* siempre def->points[WP_GRIP] (coherente con la lógica del arma) */
    VRH_GRIP_DERIVED,         /* siempre derivado del modelo (cae a def si no se puede) */
    VRH_GRIP_MANUAL           /* grip_manual */
} VrHandsGripMode;

typedef struct {
    float scale;              /* unidades de vértice -> metros (1 = ya en metros) */
    VrHandsGripMode grip_mode;
    WPose grip_manual;
    float grip_pitch_deg;     /* giro extra del arma sobre el eje X de la mano (+ = boca arriba) */
    float cull_dist;          /* m: piezas a más de esto del arma se omiten (cargador "apartado" 50 m, etc.) */
    float light_dir[3];       /* hacia la luz, en mundo */
    float ambient, diffuse;
} VrHandsConfig;

typedef struct VrHandsRig VrHandsRig;

void vrhands_default_config(VrHandsConfig *c);
VrHandsRig *vrhands_create(const VrHandsConfig *c);   /* NULL = valores por defecto */
void vrhands_destroy(VrHandsRig *r);

/* Enlaza un arma (su binding ya hecho con weapon_bind) y su atlas RGBA8 (puede ser NULL = sin textura). El atlas no se
 * copia: debe vivir mientras esté enlazada (es el de WCutSet). Re-enlazar el mismo `s` cambia los ids (la geometría
 * se vuelve a subir). 0 = bien; <0 error (sin hueco, argumentos). */
int  vrhands_bind(VrHandsRig *r, const WeaponState *s, const uint8_t *atlas, uint32_t w, uint32_t h);
void vrhands_unbind(VrHandsRig *r, const WeaponState *s);

/* Agarre derivado del modelo (ver arriba) en el espacio del modelo. 0 = bien; -1 faltan piezas o geometría. */
int  vrhands_derive_grip(const WeaponState *s, WPose *out);
/* Agarre efectivo según el modo. */
WPose vrhands_grip(const VrHandsRig *r, const WeaponState *s);
/* Pose del arma en mundo para una mano dada (igual que wcore_weapon_world pero con el agarre efectivo). */
WPose vrhands_weapon_world(const VrHandsRig *r, const WeaponState *s, const VrHandsPose *hand);

/* Construye la lista de mallas del frame. Devuelve su nº (0 = nada que dibujar: mano sin seguimiento, arma sin enlazar o
 * sin geometría; `out` queda válido con nmeshes = 0). Los arrays viven en un anillo de 3 frames dentro de `r`. */
int  vrhands_build(VrHandsRig *r, const WeaponState *s, const VrHandsPose *weapon_hand, VrHandsFrame *out);

/* Buzón entre el hilo de simulación y el de render (triple búfer con candado, copia las listas). vrhands_post publica;
 * la implementación débil de mv_host_hands() entrega lo último publicado (0 si nunca se publicó o si lo último fue vacío)
 * en un búfer que no se toca hasta la siguiente llamada. vrhands_post_clear() publica "nada". */
void vrhands_post(const VrHandsFrame *f);
void vrhands_post_clear(void);
/* Lo último publicado, en un búfer que no se toca hasta la siguiente llamada (mismas reglas que mv_host_hands, que es su envoltorio
 * débil; marca el frame como "en uso" hasta mv_host_hands_done). 1 si hay algo que dibujar. */
int vrhands_fetch(VrHandsFrame *out);
/* Liberar un arma enlazada (vértices, índices, atlas): los punteros de un frame publicado apuntan a ESOS datos y el renderer
 * los lee en su hilo al subir una malla/textura nueva. Orden seguro: vrhands_post_clear(); esperar a que
 * vrhands_render_busy() sea 0 (el renderer llama a mv_host_hands_done() al terminar de usar el frame); vrhands_unbind();
 * liberar el modelo. Sin mv_host_hands_done() (renderer antiguo) busy no vuelve a 0 hasta la siguiente lectura. */
int vrhands_render_busy(void);
/* Fin de la lectura del frame entregado por vrhands_fetch (lo que hace la implementación débil de mv_host_hands_done; un integrador que defina la suya la llama). */
void vrhands_release(void);

/* Conversión de la pose de mano a/desde WPose (útil para el integrador). */
WPose vrhands_pose_to_w(const VrHandsPose *p);
