#pragma once
/* vrgame: pegamento de jugabilidad VR entre la app, el motor emulado y los módulos de vr/ (cuerpo, radar, manos/arma, disparo y recarga).
 *
 * Hilos:
 *  - HILO DE RENDER / ENTRADA (Swift): publica las manos (mv_vrgame_set_hand), lee el antebrazo para el radar, los hápticos y el estado.
 *  - HILO DE EMULACIÓN: el gancho de mando (vrgame_pad, cada VBlank, justo antes de aplicar el mando al juego) simula el arma VR,
 *    traduce disparo y recarga al mando del juego y publica las mallas del arma; el motor llama a vrpose_get_hands (IK de los brazos)
 *    en la etapa de dibujado.
 *  - HILO DE CARGA (propio): lee los modelos del arma del disco del usuario (archivo de stages) la primera vez que hacen falta.
 *
 * Espacio de las poses: el mismo que la cabeza de mv_vr_set_head (host.h): metros, Y arriba, -Z delante, origen RECENTRADO. La pose
 * que se publica es la del MANDO tal cual la da ARKit (ya multiplicada por la inversa del recentrado); aquí se convierte al marco de
 * la mano de vr/body (centro de la palma, -Z hacia los dedos, +Y al dorso) con un desplazamiento configurable. */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- entrada (hilo de render) ---- */
typedef struct {
    int32_t valid;           /* 0 = mando sin seguimiento */
    float pos[3];            /* m, espacio recentrado */
    float rot[4];            /* cuaternión x, y, z, w */
    float trigger, grip;     /* 0..1 */
    int32_t low, up, stick;  /* cara inferior (A/X), superior (B/Y), clic del stick */
    /* Apuntado del mando (ubicación "aim" del accesorio: -Z = hacia donde apunta), mismo espacio. aim_valid = 0: sin apuntado (el arma usa el agarre). */
    int32_t aim_valid;
    float aim_pos[3], aim_rot[4];
} MvVrHand;
/* side: 0 = mando izquierdo, 1 = derecho (físicos). Llamar cada fotograma. */
void mv_vrgame_set_hand(int32_t side, const MvVrHand *h);
void mv_vrgame_clear_hands(void);   /* sin mandos rastreados (mando normal, sin Sense, fuera del espacio inmersivo) */
/* right_handed: mano del arma (1 derecha); reload_mode: 0 manual, 1 simplificada, 2 automática (WReloadMode). */
void mv_vrgame_set_options(int32_t right_handed, int32_t reload_mode);
/* Sin efecto: el arma cuelga de la mano nueva por el zócalo de vr_grip (VRH_GRIP_SOCKET), ya no toma el apuntado del mando. */
void mv_vrgame_set_weapon_aim(int32_t on);
/* Marco del agarre respecto al de la pose publicada en MvVrHand.pos/rot: p (m, en ese marco) y cuaternión x,y,z,w. Defecto identidad: la app publica ya
 * la ubicación "agarre" del accesorio (centro de la palma, -Z hacia los dedos, +Y al dorso); si el visor solo da la pose del dispositivo, aquí va el desplazamiento medido. */
void mv_vrgame_set_grip_offset(const float p[3], const float q[4]);

/* ---- salida (hilo de render) ---- */
typedef struct { float elbow[3], wrist[3], dorsal[3]; } MvVrForearm;   /* mismo layout que VrRadarArm */
/* Antebrazo tras la IK del último VBlank (espacio recentrado, m). 1 = válido; 0 = el llamador usa el mando. */
int32_t mv_vrgame_forearm(int32_t side, MvVrForearm *out);
/* Antebrazo del GUANTE (el que se dibuja a la cadencia del renderer): la IK del último fotograma del motor repetida con el agarre ACTUAL del mando (vrbody_arm_resolve), mismo marco y
 * mismos datos que mv_vrgame_forearm. dev_mm = desvío (mm) del agarre vivo respecto al agarre con el que se simuló la IK; age_ms = edad de esa IK; ambos pueden ser NULL.
 * 1 = válido; 0 = no hay guante vivo (el llamador cae a mv_vrgame_forearm o al mando). */
int32_t mv_vrgame_forearm_live(int32_t side, MvVrForearm *out, float *dev_mm, float *age_ms);
/* Vértices (xyz consecutivos, m, mismo espacio que mv_vrgame_forearm_live) de la malla del antebrazo del guante con la pose viva; como mucho max_verts. Devuelve cuántos copió (0 = sin guante vivo:
 * el llamador usa el radio por defecto). El radar de muñeca coloca el panel a radio real de la malla + holgura (vrradar_wrist_quad_mesh). */
int32_t mv_vrgame_forearm_mesh(int32_t side, float *xyz, int32_t max_verts);
/* Pose del agarre (marco de vr/body) como matriz 4x4 columna-mayor; 1 si el mando está rastreado. */
int32_t mv_vrgame_hand_matrix(int32_t side, float m[16]);
/* Marcos de depuración (m, espacio recentrado; 4x4 columna-mayor: columnas = ejes x, y, z y origen). kind: 0 = lo que pide el mando (agarre con el
 * desplazamiento aplicado: centro de la palma, -Z dedos, +Y dorso), 1 = marco EFECTIVO de la mano del personaje tras la IK (misma convención; si el objetivo
 * era alcanzable coincide con el 0, si no la diferencia muestra dónde actuaron los límites), 2 = pose del arma en la mano (posición del agarre, orientación
 * del apuntado). side: mano física (0 izquierda, 1 derecha); en kind 2 se ignora (la mano del arma). 1 = válido; 0 = sin dato en el último fotograma. */
int32_t mv_vrgame_debug_frame(int32_t kind, int32_t side, float m[16]);
/* Hápticos pendientes del arma (0..1 por mando físico); se consumen (una lectura = un pulso). */
void mv_vrgame_take_haptics(float *left, float *right);
void mv_vrgame_grip_diag(float raw[2], int32_t valid[2]);   /* diagnóstico: agarre crudo de cada mano (0 izq, 1 der) y si su pose era válida */

typedef struct {
    int32_t loader;          /* 0 sin pedir, 1 cargando, 2 listo, 3 fallo */
    int32_t active;          /* arma VR mostrada y mandando */
    int32_t weapon;          /* arma VR mostrada (índice del puente), -1 ninguna */
    int32_t mag_rounds, chambered, mag_inserted;   /* arma VR */
    int32_t game_mag, game_reserve;                 /* munición del juego */
    int32_t reload_state;    /* VRR_* de vr_reload.h */
    uint32_t shots;          /* disparos VR desde el arranque */
    uint32_t throws, places, dets, rejects;   /* lanzamientos de granada, colocaciones, detonaciones y eventos rechazados (sin munición, fuera de alcance) */
    uint32_t arms_driven;    /* brazos movidos por la IK desde el arranque */
} MvVrGameStatus;
void mv_vrgame_status(MvVrGameStatus *out);

/* ---- vistas especiales del arma (hilo de render) ----
 * El arma equipada puede pedir una segunda vista de la escena o un HUD anclado a ella:
 *  - MV_VIEW_SCOPE: mira óptica del francotirador. La app renderiza la escena una segunda vez desde `cam_*` (campo vertical cam_fov_y, textura
 *    cuadrada tex_w x tex_h) y la muestra en un disco (la lente) en `surf_*` con la abertura y retícula de vr_scope.h por encima. `blend` = apertura del
 *    iris (0 = ojo lejos: no hace falta renderizar). La cabeza NO se acerca (el aumento solo está en la lente): suppress_frame pide no aplicar la
 *    máscara de visión del juego y hide_hud ocultar el panel plano del HUD mientras se mira. (Los prismáticos del juego, scope.c, no usan esta vista: el visor amplía el mundo en los dos ojos y pone su marco del HUD
 *    como MV_VIEW_AA_SIGHT; solo sight.c sin scope.c, sin prismáticos, usa un disco SCOPE.)
 *  - MV_VIEW_AA_SIGHT: visor del lanzador antiaéreo. El HUD del propio juego (mira + marco de fijación, en la textura del HUD) se dibuja en un
 *    rectángulo anclado delante del visor (`surf_*`, ya a la distancia y tamaño angular del juego); `uv` = subrectángulo (x, y, w, h) de la textura del HUD
 *    que cubre el rectángulo (el centro óptico del juego es el centro del rectángulo). hide_hud = 1.
 *  - MV_VIEW_MISSILE: vista del misil guiado en vuelo. Segunda vista de la escena desde `cam_*` (la posición y dirección del misil del juego) mostrada en
 *    una pantalla (`surf_*`, rectángulo; `blend` 0 = pantallita del lanzador .. 1 = visor grande ante el ojo) con ruido/estática (`noise`) y, al terminar el
 *    vuelo, `ended` = 1 (solo estática). `fullscreen` = pantalla grande fija ante la cara (opción de comodidad; la cabeza nunca se mueve).
 * Poses en el espacio recentrado (el de la cabeza); cara visible de la superficie = +Z local; delante de la cámara = -Z local. */
enum { MV_VIEW_NONE = 0, MV_VIEW_SCOPE = 1, MV_VIEW_AA_SIGHT = 2, MV_VIEW_MISSILE = 3 };
typedef struct {
    int32_t kind;
    float blend;
    int32_t cam_valid;
    float cam_pos[3], cam_rot[4], cam_fov_y;
    int32_t tex_w, tex_h;
    float surf_pos[3], surf_rot[4], surf_w, surf_h;
    int32_t round;              /* la superficie es un disco de diámetro surf_w (SCOPE; el arma VR siempre round = 1) */
    float uv[4];                /* AA: subrectángulo de la textura del HUD */
    int32_t hide_hud, suppress_frame;
    int32_t locked, converged;  /* AA: blanco fijado / marco cerrado */
    float zoom;                 /* SCOPE: aumento */
    int32_t fullscreen, ended;  /* MISSILE */
    float noise;                /* MISSILE: 0..1 */
} MvVrSpecialView;
/* Vista especial vigente (1 = hay; 0 = ninguna). Se publica una vez por VBlank. */
int32_t mv_vrgame_special_view(MvVrSpecialView *out);
/* Misil guiado: 0 = pantalla en el lanzador (se agranda ante el ojo al mirar por la mirilla), 1 = pantalla grande fija ante la cara. */
void mv_vrgame_set_missile_view(int32_t mode);

/* ---- uso interno (vrhost.c) ---- */
struct BridgeState;
void vrgame_reset(void);                       /* al cargar el EXE */
void vrgame_set_enabled(int body_ok, int fire_ok);   /* HLE enlazadas con éxito (sin ellas no se mueven los brazos / no se dispara) */
/* Manos propias (guantes de vr/hands/vr_glove): on = modo gpu con la IK enlazada; vram = VRAM del emulador (1024 x 512 halfwords) de la que se decodifica el atlas. Hilo de emulación,
 * antes de vrgame_pad. Con on = 0 (o sin guante construido) las piezas de los brazos del juego se dibujan como siempre; con el guante listo se ocultan las de cada brazo movido por la IK (own_hands). */
void vrgame_set_gloves(int on, const uint16_t *vram);
uint16_t vrgame_pad(uint16_t pad, const struct BridgeState *bs, uint8_t *ram);   /* una vez por VBlank, antes de aplicar el mando */
/* Pruebas (bodyrun): función llamada al principio de vrgame_pad en cada VBlank (hilo de emulación) con el nº de VBlank desde que se instaló,
 * el mando que pidió la app y el puente del VBlank anterior; devuelve el mando a usar. Dentro puede llamar a mv_vrgame_set_hand,
 * mv_vr_set_head, etc.: así las manos sintéticas van sincronizadas con el VBlank y la prueba es determinista. NULL la quita. */
typedef uint16_t (*VrGameDriver)(uint32_t vbl, uint16_t pad, const struct BridgeState *bs);
void vrgame_set_driver(VrGameDriver f);
/* Gestos de cuerpo (vr/input/vr_gesture): golpe (puño + movimiento rápido de la mano relativo a la cabeza, hacia delante y hacia fuera, con el enemigo delante), agarre/estrangulamiento
 * (gatillo con la mano abierta junto al cuello; con el enemigo ya sujeto basta el gatillo), golpear la pared, asomarse. mask: bit 1 golpe, 2 agarre, 4 pared, 8 asomarse (defecto todos).
 * Solo con el jugador al mando, cámara VR activa y SIN arma equipada ni caja/agua. Con las manos libres y rastreadas el módulo gobierna esos botones: los GRIPS solo cierran el puño (nunca
 * llegan al juego como R1/L1: la ventana de objetos/armas se abre manteniendo el clic de stick 0,35 s) y el cuadrado físico (gatillo / A de la otra mano) se retira y lo sustituye lo que decide
 * el módulo. Con arma, sin seguimiento de manos (mando normal) o fuera de control, los botones pasan tal cual. */
void mv_vrgame_set_gestures(int32_t enable, int32_t mask);
/* Orientación de las manos del personaje respecto al agarre del mando (vr/body/vr_body.h): cabeceo fijo (grados; defecto -35: el agarre del Sense está inclinado 35 grados respecto al
 * apuntado), `auto_pitch` = deducirlo del apuntado cuando el mando lo entrega, y alabeo (giro sobre el eje de los dedos; defecto 45). */
void mv_vrgame_set_hand_pose(float pitch_deg, int32_t auto_pitch, float roll_deg);
/* "Calibrar manos": cuenta atrás de `delay_s` s (pulsos hápticos cada segundo), y al terminar toma la orientación de ambos agarres (brazos rectos al frente, mandos sujetos de forma natural)
 * y guarda la corrección fija. Háptico largo en las manos calibradas. delay_s <= 0: ahora. */
void mv_vrgame_calibrate_hands(float delay_s);
float mv_vrgame_calibration_countdown(void);                           /* s que faltan (0 = no hay cuenta atrás) */
void mv_vrgame_clear_hand_calibration(void);
int32_t mv_vrgame_hand_calibration(int32_t side, float q[4]);          /* 1 si hay calibración de ese lado (0 izq, 1 der) */
void mv_vrgame_set_hand_calibration(int32_t side, const float q[4]);   /* restaurar una calibración guardada */
uint32_t mv_vrgame_calibration_serial(void);                           /* cambia cada vez que se calibra o se borra (para persistir) */
void vrbodyg_reset(void);
unsigned vrbodyg_event_count(int ev);                                  /* VRG_EV_* acumulados (pruebas) */
uint16_t vrbodyg_pad(uint16_t pad, const struct BridgeState *bs, int right_handed, const uint8_t *ram);
int32_t mv_vrgame_menu_hand(void);                                     /* mano física (0 izq, 1 der) que mantiene abierta la ventana de armas/objetos del gesto del pecho; -1 ninguna */
int vrgame_hand_raw(int side, float p[3], float *grip, float *trig, double *t_s);   /* pose del agarre (espacio usuario, m), grip (puño), gatillo y marca de tiempo (s, monótono; 0 con guion de pruebas); 1 si hay seguimiento */
/* Modelo VR (id16 en el stage residente) del arma de id de juego dado; 0 = sin modelo VR (se queda el arma del juego). */
uint16_t vrgame_weapon_model(int game_id);
int vrgame_weapon_uses_game_model(int game_id);   /* 1 = el arma se dibuja con el modelo real del juego (granadas 2, 7, 8: escena nativa, sin ocultar); su lógica VR sigue */
void vrgame_test_install(int game_id, void *def, void *state, void *rig);   /* pruebas: arma ya construida (WeaponDef, WeaponState, VrHandsRig) en la ranura de su id */
int vrgame_test_load_logic(int game_id);   /* pruebas: carga la ranura solo de lógica (modelo 0: granada 7) sin disco; 1 = lista */
void vrgame_test_clock(uint64_t ns);                                 /* pruebas: reloj inyectado que arranca en ns (0 = vuelve al reloj real); solo avanza con vrgame_test_advance */
void vrgame_test_advance(uint64_t ns);                                 /* pruebas: avanza el reloj inyectado */
void vrgame_test_set_native(int game_id, int on);                          /* pruebas: marca la ranura como «dibujada por el juego» (textura incompleta) */
int vrgame_hand_pose_in(int side, int *item_pose, float *trigger, float *thumb);   /* pose de la mano nueva (vr_hand_pose.h: HP_*), gatillo y pulgar de la mano física side; 1 si hay seguimiento (si no, HP_RELAX y ceros) */
/* Marco de la mano del mando en el espacio del usuario (m, cuaternión x,y,z,w; el del arma: agarre * desvío fijo) del lado `side` (0 izquierda / 1 derecha): _sim con el agarre filtrado con el que se simuló la última IK de ese brazo
 * (hilo de emulación; 0 si el brazo no se manejó), _now con el del mando de ahora (cualquier hilo; 0 sin seguimiento). Su movimiento rígido _sim -> _now es el que se aplica a la mano propia, igual que al arma. */
int vrgame_hand_frame_sim(int side, float p[3], float q[4]);
int vrgame_hand_frame_now(int side, float p[3], float q[4]);
int vrgame_native_hand(void);   /* lado (0/1) de la mano que sujeta el arma equipada que se dibuja con el modelo del juego (granada), -1 si no hay (hilo de emulación) */
void vrgame_set_view_hide(int on);   /* 1 = prismáticos del juego activos (scope.c con aumento en el mundo): el arma no se dibuja ni se lleva a la mano; el disparo no se toca (hilo de emulación, antes de vrgame_pad) */
int vrgame_glove_pub(void);                                              /* hueco publicado de los guantes (-1 = ninguno) */
int vrgame_weapon_engaged(void);                                       /* arma VR mostrada o recarga en curso */
int vrgame_throw_hold(void);                                           /* arrojadiza VR (2, 7, 8) en la mano del arma, sin preparar o preparada */
void vrgame_haptic(int side, float strength);                          /* pulso háptico (0..1) a la mano física */
void vrgame_frame_end(void);                   /* una vez por VBlank, tras el puente: publica el antebrazo y el estado */

#ifdef __cplusplus
}
#endif
