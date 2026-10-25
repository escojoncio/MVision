#pragma once
/* vr_radar: el radar del HUD convertido en un dispositivo de muñeca (antebrazo izquierdo).
 * C99, sin dependencias de plataforma (usa solo las estructuras de runtime/gpu.h y vr/scene/vr_scene.h).
 *
 * ---------------------------------------------------------------------------------------------------------
 * 1. Cómo se distinguen las primitivas del radar (marcas en la lista de dibujo)
 *   La función del juego que dibuja el radar (vr_radar_hle.c, `vrradar_draw`) NO envía nada a la GPU: solo enlaza primitivas
 *   en una OT, que se dibuja después (gfx_list). Por eso no vale anotar el rango de seq de captura a la entrada/salida de la
 *   función (como hace vr/camera con el envío de la lista). El sustituto HLE hace lo equivalente EN LA OT: ejecuta la función
 *   original y, al volver, inserta un paquete de marca (comando GP0 0x04, NOP real) justo antes de la primera primitiva que
 *   añadió y otro justo después de la última. La GPU (con runtime_mark.patch) llama a `vrradar_on_mark(id, seq)` cuando
 *   ejecuta cada marca: el rango [seq_inicio, seq_fin) contiene EXACTAMENTE las primitivas que dibujó la función del radar
 *   (marco negro, fondo, paredes, puntos, conos, cabecera ALERTA/EVASIÓN/INTERFERENCIA, contador, consola...), en el mismo
 *   orden de dibujo. Si el juego no dibuja el radar (apagado, cámara activa, escena, demo) no hay marcas y el HUD no
 *   cambia; si lo interfiere (modo de interferencia/evasión/alerta) lo que se ve es el dibujo de ese modo, tal cual.
 *
 * 2. Separación
 *   vrradar_split(dl, &s): s.hud = la lista SIN las primitivas del radar (se pasa a vr_scene_build en lugar de dl; conserva todos
 *   los eventos de VRAM y el orden); s.radar = solo las primitivas del radar + todos los eventos. La ventana del radar
 *   (s.rect, coordenadas de VRAM como los vértices 2D de vr_scene) sale del centro del entorno de dibujo del radar.
 *   Contrato con vr/scene: NINGUNO nuevo (vr_scene_build recibe s.hud). Si algún día se prefiere una clase kind 4, basta con
 *   marcar los mismos ops con vrradar_seq_is_radar(op->seq).
 *
 * 3. Conos ampliados (percepción VR)
 *   La visión ampliada de los soldados solo existe durante la llamada a la función de visión; el juego dibuja su cono original.
 *   vrradar_build() añade, bajo cada cono de soldado que el juego dibuja, una corona tenue (del alcance original a
 *   range_mult x alcance) con el mismo trazado: se escala cada cono ORIGINAL (vértices del polígono que emitió el juego) respecto a
 *   su origen, es decir con exactamente la transformación mundo->radar del juego (centro = jugador, escala y giro del mapa, longitud
 *   del cono incluida), sin reconstruirla. Color = el del cono (celeste normal, rojo alerta), relleno aditivo tenue, borde punteado;
 *   la periferia (fracción del semiángulo > periph_frac) más tenue y con punteado más ralo ("solo cuenta si te mueves").
 *   Las cámaras de vigilancia (cono amarillo) y el cono del propio jugador no se amplían. Con perception.enabled == 0 o
 *   range_mult <= 1 (preset ORIGINAL) no se dibuja nada extra. Desaparece solo cuando el juego oculta o interfiere el radar
 *   (no hay conos de soldado en el dibujo).
 *
 * 4. Anclaje al antebrazo (vrradar_wrist_quad)
 *   El cuerpo (vr/body) debe exponer la pose del antebrazo izquierdo tras la IK:
 *       int vrbody_forearm_pose(int side, VrRadarArm *out);   // side 0 = izq, 1 = der; devuelve 1 si es válida; espacio VR en metros
 *   con elbow = centro del codo, wrist = centro de la muñeca, dorsal = dirección unitaria (aprox. perpendicular al hueso) hacia
 *   el dorso del antebrazo/mano (la cara "superior/exterior": la que mira a la cara con el brazo flexionado delante). Sin IK:
 *   vrradar_arm_from_controller() deduce VrRadarArm de la pose del agarre del mando izquierdo (convención OpenXR: -Z hacia los
 *   dedos, +Y hacia el dorso con la mano plana).
 *   HUESO INVISIBLE del panel con el mando: punto del antebrazo a along_m (0,07 m) del origen del agarre hacia el codo (+Z del agarre), desplazado lift_m
 *   (0,035 m) a lo largo del dorso (+Y del agarre = normal del panel, lejos de la palma: convención de vr/body y vrgame.h). Sigue la pose REAL del mando 1:1,
 *   rígido y sin suavizado; la oclusión por el brazo del personaje la da la prueba de profundidad (RadarPanel). Si en un dispositivo +Y resultara ser el lado
 *   de la palma, RadarPanel.flipDorsal invierte la normal.
 *   El quad queda sobre el dorso, a `along_m` de la muñeca hacia el codo, con el eje "derecha" de la imagen hacia la mano (como la
 *   corona de un reloj de pulsera izquierdo: el norte del radar queda hacia delante con el antebrazo cruzado ante el pecho).
 *
 * 5. Integración en el host (vr/radar/host_integration.patch, aplicar con `patch -p1` desde la raíz; probado con vrhost_run): vrhost.c llama a
 *    vrradar_split / vrradar_build ANTES de vr_scene_build, ensambla el HUD con s.hud, copia la escena del radar al slot y la sirve con
 *    mv_host_radar(); on_start instala gpu_capture_set_mark_hook(vrradar_on_mark) (runtime_mark.patch); exe_loaded/bound declaran y enlazan
 *    la HLE `vrradar_draw` en 0x8003bd9c (hle_bind_fixed). Con código generado: --hle hle_vrradar.txt + vr_radar_names.json.
 *    Runtime: añadir hle_vrradar.txt a HLETXT y un stub débil `hle_vrradar_draw` (rt_orig) como el de la cámara.
 *
 * 6. Dibujo (app/Sources/RadarPanel.swift, render/metal/vr_radar.metal): ver cabeceras de esos ficheros.
 */
#include <stdint.h>
#include "gpu.h"
#include "vr_scene.h"

/* ------------------------------------------------------------------------------------------------ marcas */
#define VRRADAR_MARK_BEGIN 0x52A001u
#define VRRADAR_MARK_END   0x52A002u

void vrradar_reset(void);                                 /* al reiniciar la RAM/runtime y cada vez que se (re)enciende la captura (la numeración de seq vuelve a 0) */
void vrradar_on_mark(uint32_t id, uint32_t seq);          /* gancho de gpu_capture_set_mark_hook (hilo de emulación) */
/* 1 si la primitiva capturada con ese seq la dibujó la función del radar. Los rangos se conservan unos cuantos fotogramas. */
int vrradar_seq_is_radar(uint32_t seq);
uint32_t vrradar_ranges(uint32_t *lo, uint32_t *hi, uint32_t max);   /* depuración: rangos [lo,hi) pendientes */

/* ------------------------------------------------------------------------------------------------ percepción */
typedef struct {
    int   enabled;       /* 0: preset ORIGINAL, no se amplía nada */
    float range_mult;    /* alcance máximo / alcance original (VrpConfig.range_mult) */
    float periph_frac;   /* fracción del semiángulo desde la que es periferia (VrpConfig.periph_frac; >= 1: sin periferia) */
    float periph_near;   /* la periferia solo rige para d > periph_near * L (VrpConfig.periph_near); entre L y periph_near*L todo es núcleo (>= range_mult: sin periferia) */
} VrRadarPerc;
/* Recibe la configuración de percepción activa (copiarla cada vez que cambie el preset; barato). Ver vr_radar_vrp.h. */
void vrradar_set_perception(const VrRadarPerc *p);
VrRadarPerc vrradar_get_perception(void);

/* ------------------------------------------------------------------------------------------------ separación */
typedef struct {
    int present;                 /* hay primitivas del radar en esta lista */
    uint32_t n_ops;              /* primitivas del radar */
    int rect[4];                 /* x, y, w, h de la ventana completa del radar (con marco) en coordenadas de VRAM / vértices 2D */
    int center[2];               /* centro del mapa (donde está el jugador), mismas coordenadas */
    const GpuDrawList *hud;      /* lista sin radar (== dl si no hay radar) */
    const GpuDrawList *radar;    /* NULL si no hay radar */
} VrRadarSplit;
/* Válidos hasta la siguiente llamada. Devuelve 0 o <0 sin memoria. */
int vrradar_split(const GpuDrawList *dl, VrRadarSplit *out);

/* ------------------------------------------------------------------------------------------------ escena del radar */
typedef struct {
    int present;
    int rect[4];                 /* ventana a recortar (VRAM) */
    const VrVtx *v; uint32_t nv; /* triángulos 2D (clase 2) en coordenadas de VRAM, en orden de dibujo */
    const VrBatch *b; uint32_t nb;   /* ev_end / src_op ya en índices de s.hud->ops (la misma lista que la escena del HUD) */
    const VrState *states; uint32_t nstates;
    uint32_t n_cones, n_cones_ext;   /* conos de soldado dibujados / ampliados */
    uint32_t n_ext_tris;
    uint32_t mode;               /* 0 mapa normal, 1 estado ALERTA/EVASIÓN/INTERFERENCIA (sin conos) */
} VrRadarScene;
/* Ensambla la escena del radar con vr_scene_build (sobre s->radar) y copia el resultado a memoria propia ANTES de que el host
 * llame a vr_scene_build para el HUD (esa llamada reutiliza los búferes de vr_scene). Válida hasta la siguiente llamada. */
int vrradar_build(const VrRadarSplit *s, VrRadarScene *out);

/* ------------------------------------------------------------------------------------------------ anclaje */
typedef struct { float elbow[3], wrist[3], dorsal[3]; } VrRadarArm;   /* espacio VR, metros */
typedef struct {
    float width_m;       /* ancho de la pantalla (def. 0,07) */
    float height_m;      /* alto; <= 0: proporcional a la ventana del radar (def. 0 -> 71:54) */
    float along_m;       /* del centro de la muñeca (origen del agarre) al centro del quad, hacia el codo (def. 0,07) */
    float lift_m;        /* del eje del antebrazo al plano de la pantalla, a lo largo de la normal del dorso (def. 0,035) */
    float bezel_m;       /* ancho del marco oscuro (def. 0,004) */
    float bezel_back_m;  /* el marco queda tanto por detrás del plano de la pantalla (def. 0,002) */
    int   rot90;         /* giro extra de la imagen en pasos de 90° (def. 0) */
    float gaze_boost;    /* brillo extra máximo al mirarlo (def. 0,35) */
    float gaze_cos_in;   /* coseno del ángulo mirada-quad con brillo completo (def. 0,93 ~ 21°) */
    float gaze_cos_out;  /* sin brillo extra por debajo de este coseno (def. 0,75 ~ 41°) */
} VrRadarWristCfg;
typedef struct {
    int valid;
    float center[3], right[3], up[3], normal[3];   /* normal = hacia fuera del brazo (hacia el observador) */
    float half_w, half_h, bezel, bezel_back;
    float corners[4][3];         /* esquinas de la pantalla: arriba-izq, arriba-der, abajo-der, abajo-izq (visto desde fuera) */
    float model[16];             /* columna-mayor: x=right*2*half_w, y=up*2*half_h, z=normal, t=center (quad unidad [-0,5,0,5]) */
} VrRadarQuad;

VrRadarWristCfg vrradar_wrist_cfg_default(void);
/* aspect = w/h de la ventana del radar (71/54) para height_m <= 0. Devuelve 1 si la pose es válida (finitos, hueso no nulo). */
int vrradar_wrist_quad(const VrRadarArm *arm, const VrRadarWristCfg *cfg, float aspect, VrRadarQuad *out);
/* Sin IK: pose de agarre del mando izquierdo (columna-mayor, espacio VR) -> antebrazo aproximado (antebrazo 0,27 m, hacia atrás). */
int vrradar_arm_from_controller(const float grip[16], VrRadarArm *out);
/* Brillo extra 0..gaze_boost según el ángulo entre la mirada (eye, forward unitario) y el quad; 0 si el quad da la espalda. */
float vrradar_gaze(const VrRadarQuad *q, const float eye[3], const float forward[3], const VrRadarWristCfg *cfg);
