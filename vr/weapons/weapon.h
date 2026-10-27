/* Armas en mano (VR): contrato común entre el núcleo y cada arma.
 * C puro y sin dependencias de plataforma: se prueba en headless y se renderiza con Metal en la app.
 * Unidades: metros, segundos, radianes. Ejes: +Y arriba, -Z hacia delante (convención visionOS). */
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef struct { float x, y, z; } WVec3;
typedef struct { float x, y, z, w; } WQuat;
typedef struct { WVec3 p; WQuat q; } WPose;

/* ---------- Geometría ---------- */
typedef struct {
    float pos[3], nrm[3];
    float uv[2];
    uint8_t rgba[4];      /* color de vértice (color plano/gouraud + textura opcional) */
} WVertex;

typedef struct {
    const char *name;
    const WVertex *v; uint32_t nv;
    const uint16_t *idx; uint32_t ni;   /* triángulos */
    uint16_t texture;                   /* 0 = sin textura */
} WMesh;

/* Pieza articulada: transformación local respecto al padre + mesh. El estado del arma
 * desplaza/rota piezas por canal (corredera, cerrojo, gatillo, cargador, tapa...). */
typedef enum { WCH_NONE, WCH_SLIDE, WCH_BOLT, WCH_TRIGGER, WCH_HAMMER, WCH_MAG, WCH_LID,
               WCH_SAFETY, WCH_SCREEN, WCH_ROUND, WCH_CUSTOM0, WCH_CUSTOM1, WCH_COUNT } WChannel;

typedef struct {
    const char *name;
    int parent;              /* -1 = raíz (marco del arma) */
    WPose rest;
    int mesh;                /* índice en WeaponDef.meshes, -1 = punto vacío */
    WChannel channel;
    WVec3 axis;              /* traslación (m) o rotación (rad) por unidad de canal */
    uint8_t rotates;         /* 0 = traslada a lo largo de axis, 1 = rota alrededor de axis */
} WPart;

/* ---------- Puntos de interacción (en espacio del arma) ---------- */
typedef enum { WP_GRIP, WP_SUPPORT, WP_MUZZLE, WP_EYE, WP_MAGWELL, WP_CHARGE, WP_SCREEN, WP_COUNT } WPoint;

/* ---------- Física de retroceso (resorte-amortiguador sobre la pose relativa a la mano) ---------- */
typedef struct {
    float kick_back, kick_up;   /* impulso lineal (m/s) y angular de cabeceo (rad/s) por disparo */
    float kick_yaw_rand;        /* ruido de guiñada (rad/s) */
    float stiffness, damping;   /* resorte hacia la pose de agarre */
    float two_hand_factor;      /* reducción con la segunda mano en WP_SUPPORT */
} WRecoil;

typedef enum { WF_SEMI, WF_AUTO, WF_BOLT, WF_SINGLE_SHOT, WF_THROWN, WF_PLACED } WFireMode;

/* ---------- Entrada por frame ---------- */
typedef struct {
    WPose head, hand[2];        /* espacio mundo de la app; hand[0] = mano del arma */
    float trigger[2], grip[2];  /* 0..1 */
    uint32_t buttons[2];        /* bits WB_* */
    int support_held;           /* la mano 1 sujeta WP_SUPPORT */
    float dt;
    uint32_t game_flags;        /* bits WGF_*: estado del juego que el arma muestra pero no decide (la capa de integración los rellena; 0 = ninguno) */
} WInput;
enum { WB_A = 1, WB_B = 2, WB_STICK = 4, WB_MAG_RELEASE = 8 };
enum { WGF_SUPP = 1 };      /* el juego tiene el supresor puesto en la pistola: se dibuja y su punta es la boca */

/* Modos de recarga (opciones de la app). */
typedef enum { WR_MANUAL, WR_SIMPLIFIED, WR_AUTOMATIC } WReloadMode;

/* ---------- Puente con el juego (lo implementa la capa de integración sobre la RAM emulada) ---------- */
typedef struct {
    int  (*ammo_reserve)(int weapon_id);             /* munición total del juego fuera del cargador */
    void (*ammo_take)(int weapon_id, int n);          /* descuenta del contador original (n<0 = devolver) */
    void (*fire)(int weapon_id, WPose muzzle_world);  /* el juego crea el proyectil/impacto */
    void (*haptic)(int hand, float strength, float seconds);
    void (*sound)(int weapon_id, int event);          /* WS_* */
    int  (*targets)(WVec3 *out_world, int max);       /* objetivos fijables visibles (aéreos, vehículos) */
    int  (*projectile)(int weapon_id, WPose *out);    /* pose del proyectil guiado activo; 0 si no hay */
    void (*steer)(int weapon_id, float yaw, float pitch); /* guiado del proyectil (-1..1) */
    void *user;
} WGameBridge;
enum { WS_FIRE, WS_DRY, WS_MAG_OUT, WS_MAG_IN, WS_SLIDE_BACK, WS_SLIDE_FWD, WS_BOLT, WS_LOCK_TONE, WS_LOCKED,
       /* arrojadizas y colocables (w_throw.h los alias como W5_S_*) */
       WS_DRAW, WS_PIN, WS_LEVER, WS_THROW, WS_PLACE, WS_CAP, WS_BTN, WS_LEGS, WS_EMPTY, WS_COUNT };

/* ---------- Vistas auxiliares (render-to-texture) ----------
 * Un arma puede pedir que el motor renderice la escena del juego desde una cámara y la
 * muestre sobre una pieza (pantalla del lanzador, visor digital, mira telescópica). */
typedef struct {
    int active;
    WPose camera;          /* espacio mundo */
    float fov_y;           /* rad */
    int target_part;       /* pieza sobre la que se compone (su mesh tiene uv 0..1) */
    int width, height;     /* resolución pedida */
    int use_game_camera;   /* 1 = usar la cámara que el propio juego calcula (p.ej. vista del proyectil) */
    uint32_t overlay;      /* bits WO_*: retícula, marco de fijación, telemetría... dibujados por el arma */
} WAuxView;
enum { WO_RETICLE = 1, WO_LOCK_BOX = 2, WO_TELEMETRY = 4, WO_NOISE = 8, WO_SCANLINES = 16 };

typedef struct WeaponDef WeaponDef;
typedef struct WeaponState WeaponState;

/* Objeto suelto/sostenido en mundo (cargador nuevo, vaina expulsada, cartucho, accesorio...).
 * Lo coloca y simula el arma; el renderizador dibuja def->meshes[mesh] con la pose mundo.
 * (Añadido al contrato por el núcleo: sin esto no se podría ver el cargador en la mano libre.) */
#define W_MAX_HELD 4
typedef struct { int active; int mesh; WPose world; } WHeld;

/* Estado común (cada arma puede extenderlo con su propio struct que empiece por este). */
struct WeaponState {
    const WeaponDef *def;
    float channel[WCH_COUNT];   /* valor actual de cada canal */
    WPose recoil_offset;        /* desplazamiento por retroceso respecto al agarre */
    WVec3 recoil_vel, recoil_angvel;
    int mag_rounds, chambered, mag_inserted;
    int phase;                  /* máquina de estados propia del arma */
    float t;                    /* tiempo en fase */
    WAuxView aux;
    WHeld held[W_MAX_HELD];     /* objetos sueltos/sostenidos (zero-init = ninguno) */
    /* Binding en tiempo de ejecución (modelos cargados del disco): si no son NULL sustituyen a
     * def->meshes/def->parts. Se rellenan con weapon_bind() (wcore.c). Un mesh con nv==0 = hueco vacío.
     * Las meshes enlazadas siguen el orden de def->mesh_names. */
    const WMesh *meshes; int nmeshes;
    const WPart *parts; int nparts;
};

struct WeaponDef {
    const char *id;             /* código interno neutro: "pist", "rifle", "sniper", "guided", "aa", "throw_*", "placed_*" */
    int game_weapon_id;         /* único en todo el registro (lo comprueba test_registry) */
    const WMesh *meshes; int nmeshes;   /* meshes compiladas: las armas no las usan (nmeshes = 0) */
    const WPart *parts; int nparts;
    WPose points[WP_COUNT];     /* todos con cuaternión válido (identidad si no se usa el punto) */
    WRecoil recoil;
    WFireMode mode;
    float rpm;
    int mag_capacity;
    size_t state_size;          /* tamaño del struct de estado propio (>= sizeof(WeaponState)) */
    /* Ciclo de vida: la memoria llega a cero (calloc) la primera vez. init(s) deja TODO el estado (también
     * la extensión propia, hasta state_size) a cero salvo lo que fija él, pone s->def, recoil_offset en
     * identidad y held[].mesh = -1, y CONSERVA el binding (meshes/parts) que hubiera. El orden normal es
     * init -> weapon_bind; un init posterior (reinicio) no deshace el binding. init no necesita que el
     * llamador asigne s->def. update ignora el cuadro si in->dt es NaN o <= 0 y recorta dt a 0,1 s. */
    void (*init)(WeaponState *s);
    void (*update)(WeaponState *s, const WInput *in, WReloadMode rm, const WGameBridge *g);
    /* Nombre lógico de cada hueco de mesh, terminado en NULL. Las piezas con mesh == -1 se asocian a la
     * mesh que tenga su mismo nombre (weapon_part_mesh); los objetos sueltos (held[].mesh) usan el
     * índice directo en esta lista. Va al final: no rompe inicializadores. */
    const char *const *mesh_names;
};

/* ---------- Núcleo (vr/weapons/wcore.c) ---------- */
WPose wpose_mul(WPose a, WPose b);
WVec3 wpose_apply(WPose a, WVec3 v);
WQuat wquat_axis_angle(WVec3 axis, float rad);
/* Transformaciones de mundo de las piezas (hasta max); devuelve cuántas ha escrito (<= nparts). */
int wcore_part_poses(const WeaponState *s, WPose weapon_world, WPose *out, int max);
/* Pose del arma en mundo = mano · agarre⁻¹ · retroceso. */
WPose wcore_weapon_world(const WeaponState *s, const WInput *in);
/* Integra el resorte de retroceso; wcore_kick aplica un disparo. */
void wcore_recoil_step(WeaponState *s, const WInput *in, float dt);
void wcore_kick(WeaponState *s, const WInput *in, uint32_t seed);
/* Utilidades de interacción: distancia de una mano a un punto del arma (mundo). */
float wcore_hand_dist(const WeaponState *s, const WInput *in, int hand, WPoint p);

/* Binding de modelos en tiempo de ejecución: meshes (n) y, opcional, partes (np; NULL = las del def). */
void weapon_bind(WeaponState *s, const WMesh *meshes, int n, const WPart *parts, int np);
const WMesh *weapon_meshes(const WeaponState *s, int *n);
const WPart *weapon_parts(const WeaponState *s, int *n);
/* Nº de nombres en def->mesh_names (0 si no hay). */
int weapon_mesh_count(const WeaponDef *d);
/* Mesh efectiva de la pieza i: parts[i].mesh si es >= 0; si no, el índice (en mesh_names) del nombre de
 * la pieza. -1 si no tiene. Con binding, el índice es el de las meshes enlazadas. */
int weapon_part_mesh(const WeaponState *s, int i);

/* Registro de armas. */
extern const WeaponDef *const w_registry[];
extern const int w_registry_count;
