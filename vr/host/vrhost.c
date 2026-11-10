/* vrhost: ver vrhost.h. Todo lo que toca el estado del runtime corre en el hilo de emulación (ganchos de host_hooks.h);
 * solo la cola de escenas, el BridgeState publicado y la marca de "hay consumidor" se comparten con otros hilos. */
#include "vrhost.h"
#include "vrdump.h"
#include "vrprov.h"
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "bridge.h"
#include "cpu.h"
#include "gpu.h"
#include "prov.h"
#include "hlebind.h"
#ifdef MV_NOGEN
#include "rtpatch.h"
#endif
#include "host.h"
#include "host_hooks.h"
#include "vr_body.h"
#include "vr_bodylive.h"
#include "vr_camera.h"
#include "vr_heading.h"
#include "vr_headcol.h"
#include "vr_usermap.h"
#include "vr_cine.h"
#include "vr_fire.h"
#include "vr_hit.h"
#include "vr_hzd.h"
#include "vr_cameras.h"
#include "vr_radar.h"
#include "vr_radar_vrp.h"
#include "vr_scene.h"
#include "vr_layer2d.h"
#include "vr_layer2d_async.h"
#include "vr_smooth.h"
#include "vr_view.h"
#include "vrai.h"
#include "vr_melee_rt.h"
#include "vrgame.h"
#include "hle.h"
#include "ns_dump.h"
#include "ns_extract.h"
#include "ns_fx.h"
#include "ns_hook.h"
#include "ns_hook_rt.h"
#include "ns_hand.h"
#include "ns_player.h"
#include "ns_wphide.h"
#include "ns_solid.h"
#include "bridge_addrs.h"

void hle_vrcam_reset(void);   /* vr_camera_hle.c */
void hle_vrradar_reset(void); /* vr/radar/vr_radar_hle.c */
void hle_vrfire_reset(void);  /* vr/gameplay/vr_fire_hle.c */
#define RADAR_DRAW_ADDR 0x8003bd9cu   /* vr/radar/vr_radar_names.json (edición europea) */
#define BODY_SORT_ADDR 0x80018304u    /* vr/body/vr_names_body.json (fin del dibujado del canal 3D: vrbody_restore) */

/* Funciones del motor sustituidas por la capa de cámara y sus direcciones (edición europea del EXE). Deben coincidir con
 * vr/camera/vr_names.json (make -C vr/host check). Se enlazan por dirección porque no hay firma de librería para ellas. */
static const struct { const char *name; uint32_t addr; } SITES[] = {
    {"vrcam_view", 0x80017564u},
    {"vrcam_padorg", 0x80016d98u},
    {"vrcam_trans", 0x8001e840u},
    {"vrcam_draw", 0x80018110u},
    {"vrcam_overscan", 0x80017780u},
    {"vrcam_pack", 0x8001e1fcu},
};
#define NSITES (sizeof SITES / sizeof SITES[0])

/* Funciones del motor que crean el proyectil del jugador (vr/gameplay/vr_fire_names.json; make -C vr/host check). */
static const struct { const char *name; uint32_t addr; } FIRE_SITES[] = {
    {"vrfire_bullet", 0x800779a8u},
    {"vrfire_flash_a", 0x8005e6dcu},
    {"vrfire_flash_b", 0x8005e358u},
    {"vrfire_rmissile", 0x8006e128u},
    {"vrfire_amissile", 0x8006eca0u},
    {"vrfire_mine", 0x8006c4b4u},
    {"vrfire_charge", 0x8006b69cu},
    {"vrfire_grenade", 0x8006af98u},
    {"vrfire_hide", 0x80018c8cu},
    {"vrfire_aalock", 0x80071fc8u},
    {"vrfire_aaview", 0x800711acu},
};
#define NFIRE (sizeof FIRE_SITES / sizeof FIRE_SITES[0])
static int body_ok, fire_ok;
static uint32_t ik_ap0, ik_dr0;          /* vrbody_stats.applies / arms_driven en la captura nativa de este fotograma (ns_ik_idle) */

/* Huella del código del buscador antiaéreo (edición europea): prólogo de cada función y las dos llamadas internas que usa el sustituto
 * (aaview desde el paso; carga de H del canal desde aaview). Sin ella no se enlazan (las direcciones fijas no se verifican por firma). */
static int aa_code_ok(void) {
    static const struct { uint32_t addr, word; } W[] = {
        {0x80071fc8u, 0x27BDFEA0u},   /* paso: addiu sp,sp,-352 */
        {0x80072010u, 0x0C01C46Bu},   /* paso: jal aaview */
        {0x800711acu, 0x27BDFFE8u},   /* aaview: addiu sp,sp,-24 */
        {0x800711c8u, 0x0C005E06u},   /* aaview: jal (carga de H y rectángulo del canal) */
    };
    for (unsigned i = 0; i < sizeof W / sizeof *W; i++) {
        uint32_t w;
        memcpy(&w, emu_ram + (W[i].addr & (EMU_RAM_SIZE - 1u)), 4);
        if (w != W[i].word) return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------------------------------------ estado compartido */
#define NSLOT 6        /* 1 en manos del consumidor + 1 en construcción + hasta NQ en cola */
#define NQ 4

typedef struct {
    GpuDrawList list;                    /* copia compacta: solo los eventos de VRAM (ops) y los datos de subida */
    GpuOp *ops;      size_t ops_cap;
    uint16_t *data;  size_t data_cap;
    uint16_t *vram;  int has_vram;
    VrVtx *v;        size_t v_cap;
    VrBatch *b;      size_t b_cap;
    VrState *st;     size_t st_cap;
    uint8_t *tc;     size_t tc_cap;          /* VrScene.tri_cause (un byte por triángulo) */
    uint64_t *tk;    size_t tk_cap;          /* VrScene.tri_fx (identidad de efecto por triángulo) */
    uint32_t *tsrc;  size_t tsrc_cap;        /* VrScene.tri_src (primitiva de origen por triángulo) */
    VrScene sc;
    VrHcMap hcmap;                       /* correspondencia usuario <-> mundo de la cámara con que se emitió esta escena (límite de la cabeza) */
    uint8_t frm[VR_FRAME_W * VR_FRAME_H];    /* copia de VrFilter.frame (máscara de visión: la memoria de vr_scene se reutiliza en la siguiente lista) */
    VrVtx *rv;       size_t rv_cap;          /* escena del radar de muñeca (copia propia; ev_end en índices de `list`) */
    VrBatch *rb;     size_t rb_cap;
    VrState *rst;    size_t rst_cap;
    VrRadarScene rs;
    VrSmSample smp;                      /* muestra de la fluidez de la escena (vr_smooth.h): se alimenta al entregarla */
} Slot;

enum { S_FREE = 0, S_BUILD, S_QUEUED, S_HELD };

static pthread_mutex_t MX = PTHREAD_MUTEX_INITIALIZER;
static Slot slot[NSLOT];
static int sstate[NSLOT];
static int queue[NQ], q_head, q_n, held = -1;
static BridgeState pub_bs;
static int pub_valid;
static MvVrGameState pub_gs;
static MvVrHostStats stats;
static _Atomic uint64_t last_poll_ns;
/* Límite de la cabeza contra el escenario (vr_headcol): el almacén lo alimenta el hilo de emulación y lo consulta el de render. */
static pthread_mutex_t HCMX = PTHREAD_MUTEX_INITIALIZER;
static VrHcMap hc_disp;                  /* correspondencia de la escena entregada al consumidor (protegida por MX) */
static int hc_on = 1;
static float hc_last_eye[3]; static int hc_have_eye;

/* ------------------------------------------------------------------------------------------------ estado del hilo de emulación */
static BridgeState bs;                   /* último puente leído */
/* Capa 2D fiel y en alta resolución (vr_layer2d.h) en un HILO TRABAJADOR (vr_layer2d_async.h): el hilo de emulación solo copia la lista a una cola (memcpy) y el render recoge
 * la última capa publicada con mv_vrhost_hud2d_acquire; nadie espera. MV_HUD2D=0 la apaga (el renderer vuelve al shader). MV_HUD2D_SCALE=1..6 fija la escala objetivo (defecto 4);
 * la escala baja sola si el trabajador tarda (mide su propio coste) y sube al recuperarse. */
static VrL2Async *l2as[VPC_L_N];   /* una capa 2D por clase de procedencia (vr_prov.h): 0 HUD, 1 ventana del selector / pausa, 2 pantalla completa. Solo la vista mundo reparte; en las demás todo va a la 0 */
static VrProvLearn prov_learn;     /* WINDOW aprendido por id de procedencia (toda la sesión; hilo de emulación) */
static VrFxHold fx_hold;           /* efectos de pantalla (viñeta, bandas, mira) con retención; se publican en MvVrGameState.fx */
static uint8_t *l2_opd[VPC_L_N]; static size_t l2_opd_cap;   /* op_dest por capa */
static uint8_t l2_busy[VPC_L_N];   /* la capa auxiliar tiene contenido publicado (su última lista no estaba vacía) */
static uint32_t pc_cls[VPC_N], pc_lists;   /* primitivas del panel por clase desde el último registro [prov2d] */
static uint64_t pc_log_ns;
static _Atomic int l2d_enabled = 1;
static _Atomic int l2d_smooth = VL_SMOOTH_ORIGINAL;   /* original = rasterizado exacto de la GPU y replicado a escala entera: el texto sale idéntico y nítido */
static int l2d_target = 4;
static VrView view;                      /* vista vigente: mundo / cine (vr_view.h) */
static VrCine cine;                      /* planos de la cinemática del motor en curso (vr_cine.h) */
static int last_scene3d, last_video;     /* de la última lista ensamblada: geometría 3D sustancial con cámara válida / fotograma de vídeo */
static uint32_t mdec_seen_total, mdec_last_vb;   /* contador del MDEC en la última lectura / VBlank en que creció (0 = nunca) */
static int last_cover;                   /* recubrimientos a pantalla completa (fundidos) de la última lista */
static int vr_ok;                        /* EXE de la edición esperada: parche aplicado y HLE de cámara enlazadas */
static int cap_on;
static VrSceneOpts sopts;
static void (*scene_tap)(const GpuDrawList *, const BridgeState *, const VrScene *, uint32_t);

static MvVrListTap list_tap;             /* solo herramientas de prueba: ve cada lista y su escena recién ensamblada (hilo de emulación) */
static uint32_t *pre;                    /* prefijo de eventos de VRAM por índice de op (compactación) */
static size_t pre_cap;

static uint64_t mono_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

/* ------------------------------------------------------------------------------------------------ fluidez (vr_smooth.h) */
static pthread_mutex_t SMX = PTHREAD_MUTEX_INITIALIZER;   /* protege SM (hilo de render: push al entregar, eval por fotograma) */
static VrSmooth SM;
static int sm_init;
static double (*clock_fn)(void);
static uint32_t exe_gen;                 /* cargas del ejecutable (cambia la época de las muestras) */
static VrSmCorr sm_last; static uint32_t sm_last_poll;   /* con SMX: última corrección evaluada y cuántas veces había tomado escena el renderer (ns_polls) al evaluarla */
static uint32_t ns_polls(void);          /* llamadas del renderer a ns_bridge_acquire (una por fotograma de render, después de mv_vrhost_smooth) */
static VblRing body_ring;                /* con SMX: poses del jugador de las últimas escenas publicadas (vr_bodylive.h) */
static VblPose body_pend;                /* hilo de emulación: pose de la escena capturada y aún sin publicar */
static int ns_cap_pend;                  /* hilo de emulación: hay una escena con cámara VR capturada y aún sin publicar */
static _Atomic uint32_t body_key;        /* clave de instancia del jugador de la última escena con pose */
static _Atomic uint32_t body_held;       /* clave de instancia del objeto del juego que cuelga de la mano en la última escena con pose (la granada), 0 = ninguno */
static double turn_base;                 /* mv_vr_turn_total() - vrcam_turn_total() al reiniciar la cámara (vrcam_init pone el total a 0) */

static double now_s(void) { return clock_fn ? clock_fn() : (double)mono_ns() * 1e-9; }
void mv_vrhost_set_clock(double (*fn)(void)) { clock_fn = fn; }

static void sm_ensure(void) {   /* con SMX */
    if (sm_init) return;
    VrSmoothCfg c;
    vrsm_cfg_default(&c);
    { const char *e = getenv("MV_SMOOTH"); if (e && e[0] == '0') c.enabled = 0; }
    { const char *e = getenv("MV_SMOOTH_LEAD"); if (e) { float v = (float)atof(e); if (v >= 0.0f && v <= 0.1f) c.lead_s = v; } }
    vrsm_init(&SM, &c);
    sm_init = 1;
}

void mv_vrhost_set_smooth(int on) {
    pthread_mutex_lock(&SMX);
    sm_ensure();
    SM.cfg.enabled = on != 0;
    if (!on) vrsm_reset(&SM);
    pthread_mutex_unlock(&SMX);
}

void mv_vrhost_smooth(VrSmCorr *out) {
    if (!out) return;
    pthread_mutex_lock(&SMX);
    sm_ensure();
    vrsm_eval(&SM, now_s(), mv_vr_turn_total(), out);
    sm_last = *out; sm_last_poll = ns_polls();
    pthread_mutex_unlock(&SMX);
}

/* Cuerpo del jugador a la cadencia del renderer (vr_bodylive.h; hilo de render, al armar la tabla de piezas de la escena nativa `sc`, DESPUÉS de mv_vrhost_smooth de ese mismo
 * fotograma): matriz de mundo (NsXf, 3x4) de la pieza k de la instancia inst que deja al jugador y a sus manos propias fijos respecto a la cabeza e interpolados entre escenas.
 * 0 = dejar la matriz de la escena (no es del jugador, sin dos poses, o la fluidez no se evaluó en este fotograma: desactivada, cinemática, diorama). «Este fotograma» = el renderer
 * ha tomado escena del puente como mucho una vez desde la evaluación (0 = fotograma con la escena retenida a la espera de la VRAM, que no vuelve a tomarla). */
int32_t mv_vrhost_body_piece(const void *scene, uint32_t inst, uint32_t k, void *out_xf) {
    const NsScene *sc = (const NsScene *)scene;
    if (!sc || !out_xf || inst >= sc->n_inst) return 0;
    uint32_t key = sc->inst[inst].key;
    const uint32_t held = atomic_load(&body_held);
    const int own_hand = ns_key_is_hand(key) || (held && key == held);
    if (key != atomic_load(&body_key) && !own_hand) return 0;
    VblHand now[2];    /* mano propia y objeto en la mano: el marco del mando de AHORA, como el arma (vbl_piece mueve la pieza de la escena a él) */
    for (int s = 0; s < 2; s++) { memset(&now[s], 0, sizeof now[s]); if (own_hand) now[s].ok = vrgame_hand_frame_now(s, now[s].p, now[s].q); }
    pthread_mutex_lock(&SMX);
    const int smooth = sm_last.active && ns_polls() - sm_last_poll <= 1u;   /* sin corrección este fotograma: m NULL (el cuerpo no interpolado no se toca; lo que sigue al mando se dibuja igual) */
    int ok = vbl_piece(&body_ring, sc, inst, k, sm_last.alpha, smooth ? sm_last.m : NULL, now, (NsXf *)out_xf);
    pthread_mutex_unlock(&SMX);
    return ok;
}

void mv_vrhost_smooth_stats(VrSmoothStats *s) {
    if (!s) return;
    pthread_mutex_lock(&SMX);
    sm_ensure();
    vrsm_stats(&SM, s);
    pthread_mutex_unlock(&SMX);
}

/* Muestra de una escena recién ensamblada (hilo de emulación). valid = mundo en espacio de usuario con cámara VR en primera persona. */
static void sm_sample(VrSmSample *smp, const VrScene *sc, const VrCamRecord *r) {
    memset(smp, 0, sizeof *smp);
    smp->t_wall = now_s();
    if (!r || !r->active || !sc->user_space || !sc->has_3d || !sc->world_valid || sc->cine_active) return;
    float K = sc->user_k * sc->scale;
    if (!(K > 0.0f) || !isfinite(K)) return;
    /* la reflexión de z de vr_scene se cancela: P = R * k*scale * (W - O) + c (ver vr_smooth.h) */
    vrsm_make_sample(smp, sc->origin_game, sc->user_R, K, sc->user_p_ref, sc->user_eye_h, (double)r->vbl / 50.0, r->turn_total_deg + turn_base, r->epoch + (exe_gen << 16), smp->t_wall);
}

static int grow(void **p, size_t *cap, size_t need, size_t esz) {
    if (need <= *cap) return 1;
    size_t n = *cap * 2;
    if (n < need) n = need;
    if (n < 256) n = 256;
    void *q = realloc(*p, n * esz);
    if (!q) return 0;
    *p = q; *cap = n;
    return 1;
}


/* ------------------------------------------------------------------------------------------------ render nativo (vr/native) */
/* Modo de render: NATIVO es el único camino del mundo 3D (la escena que el extractor lee de la RAM); la lista de la GPU solo alimenta la capa 2D, los eventos de VRAM y las marcas de efectos.
 * MV_RENDER=gpu ya no existe: se avisa y se ignora. */
#ifndef MV_RENDER_DEFAULT_NATIVE
#define MV_RENDER_DEFAULT_NATIVE 1
#endif
static NsBridge *_Atomic ns_br;          /* se crea una sola vez (ns_bridge_get); nunca se destruye */
static pthread_mutex_t NSMX = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int render_req = MV_RENDER_DEFAULT_NATIVE, render_locked, renderer_ready;
static int dump_point;                   /* 1 = el volcado en curso se tomó en el punto de enganche */
/* Definición DÉBIL vacía (una referencia débil sin definir no enlaza en Mach-O): ns_extract la sustituye con una fuerte que registra el extractor en el enganche. */
__attribute__((weak)) void ns_extract_register(void) {}
__attribute__((weak)) void ns_extract_glue_stats(NsExtractGlueStats *o) { if (o) memset(o, 0, sizeof *o); }   /* idem: la fuerte está en ns_extract_glue.c */
/* Comprobación de cobertura de la lista de la GPU por la escena nativa (ns_cover_glue.c, solo en la app): definiciones débiles vacías para las pruebas de escritorio. */
__attribute__((weak)) void ns_cover_glue_post_ik(const uint8_t *ram) { (void)ram; }
__attribute__((weak)) void ns_cover_glue_submit(const uint8_t *ram, const uint8_t *scratch, const GpuOp *ops, uint32_t nops, uint32_t frame) { (void)ram; (void)scratch; (void)ops; (void)nops; (void)frame; }
__attribute__((weak)) void ns_cover_glue_submit_cam(const uint8_t *ram, const uint8_t *scratch, const GpuOp *ops, uint32_t nops, uint32_t frame, const float *eye, const float *M) { (void)ram; (void)scratch; (void)ops; (void)nops; (void)frame; (void)eye; (void)M; }
__attribute__((weak)) size_t ns_cover_glue_poll(char *buf, size_t cap) { (void)buf; (void)cap; return 0; }
/* Decodificador de vídeo (runtime/mdec.c): palabras entregadas desde el arranque. En las pruebas de escritorio sin MDEC vale siempre 0 (no hay FMV). */
__attribute__((weak)) uint32_t mdec_out_words_total(void) { return 0; }
/* Manos nuevas (ns_hand.c, solo en la app; vr/body/vr_hand_skin.c): definición débil vacía para las pruebas de escritorio, donde la manopla del motor se queda como está. */
__attribute__((weak)) int ns_hand_apply(NsScene *sc, NsMeshStore *st, const NsPlayerMap *map, const NsHandIn *in) { (void)sc; (void)st; (void)map; (void)in; return 0; }
static _Atomic int hands_new = 1;        /* menú VR «Manos: nuevas / originales» (defecto nuevas) */
void mv_vrhost_set_hands_new(int on) { atomic_store(&hands_new, on ? 1 : 0); }
int mv_vrhost_hands_new(void) { return atomic_load(&hands_new); }
/* Predicado ÚNICO: las manos nuevas están en uso (ajuste activado Y render nativo listo). Lo usan el cierre de la escena, la supresión del puño deformado y el punto de golpe. */
static int hands_active(void) { return atomic_load(&hands_new) && atomic_load(&renderer_ready) && atomic_load(&render_req) != 0; }
int mv_vrhost_hands_active(void) { return hands_active(); }
static void ns_hands_finalize(NsScene *sc, NsMeshStore *st, const NsPlayerMap *map, void *u);
static void hands_precapture(uint8_t *ram);
_Static_assert(NS_PLAYER_BODY_PTR == BA_PLAYER_BODY, "ns_player y el puente deben coincidir en el puntero del jugador");
_Static_assert(NS_WPHIDE_PLAYER_CTRL == BA_PLAYER_CTRL, "ns_wphide y el puente deben coincidir en el puntero de control del jugador");
static void dump_at_stage(Cpu *c);
static int ns_gate(void *u);
static void ns_solid_tap(const NsScene *s, const NsMeshStore *st, void *u);
static void ns_frame_check(void);
static void ns_ctx(NsScene *sc, NsCtxOut *o, void *u);
_Static_assert(VR_SKIP_WORLD == NS_SKIP_WORLD && VR_SKIP_FX == NS_SKIP_FX, "las marcas de batch y la decisión del consumidor deben coincidir");

void mv_vrhost_set_render_mode(int mode) { (void)mode; }   /* el modo gpu ya no existe: siempre nativo */
int mv_vrhost_render_only_native(void) { return 0; }   /* el modo «solo nativo» ya no existe */
int mv_vrhost_render_mode(void) { return atomic_load(&render_req) ? 1 : 0; }
static int native_mode(void) { return atomic_load(&render_req) != 0; }   /* siempre 1 (el modo gpu ya no existe) */
int mv_vrhost_render_env_locked(void) { return atomic_load(&render_locked); }
void mv_vrhost_native_renderer_ready(int ready) { atomic_store(&renderer_ready, ready != 0); }
int mv_vrhost_native_available(void) { return ns_hook_has_extractor() && ns_hook_installed() && atomic_load(&renderer_ready); }
/* Puente de la escena nativa: se crea la primera vez que alguien lo pide (instalación del anfitrión o interfaz) y a partir de ahí nunca es NULL (salvo sin memoria). */
static NsBridge *ns_bridge_get(void) {
    NsBridge *b = atomic_load(&ns_br);
    if (b) return b;
    pthread_mutex_lock(&NSMX);
    b = atomic_load(&ns_br);
    if (!b) {
        NsBridgeCaps bc = {512, 8192, 256, 2048};
        b = ns_bridge_create(&bc);
        if (b) {
            ns_hook_init(b);
            ns_hook_set_context(ns_ctx, NULL);
            ns_hook_set_gate(ns_gate, NULL);
            ns_hook_set_scene_tap(ns_solid_tap, NULL);
            ns_hook_set_finalize(ns_hands_finalize, NULL);
            atomic_store(&ns_br, b);
        }
    }
    pthread_mutex_unlock(&NSMX);
    return b;
}
struct NsBridge *mv_vrhost_native_bridge(void) { return ns_bridge_get(); }
static uint32_t ns_polls(void) {
    NsBridgeStats st = {0};
    ns_bridge_stats(atomic_load(&ns_br), &st);
    return st.polls;
}

/* Mundo del motor -> espacio recentrado del usuario (3x4 fila-mayor, metros): la MISMA regla que vr_scene con user_map_on (la reflexión de Z se cancela):
 *   P = R * (k * S * (W - origen)) - R * (0, eye_h, 0) + p_ref.
 * Contrato v2: mundo del motor Y arriba y mano derecha, sin inversión (la reflexión de Z de vr_scene se cancela con el mapa de usuario). */
static int ns_world_to_vr(const VrCamRecord *r, float to_vr[12], VrSmSample *smp) {
    VrSceneOpts o = sopts;
    float org[3];
    vr_scene_player_origin(&bs, &o, org);
    org[0] = (float)r->eye[0]; org[2] = (float)r->eye[2];   /* origen bajo el ojo VR de ese fotograma (como build_one) */
    vrcam_scene_user_map(&o, r, org[1]);
    if (smp)   /* muestra del suavizador de ESTA escena (mismo registro, mismo mapa): sm_sample hace lo mismo para la lista de la GPU */
        vrsm_make_sample(smp, org, o.user_R, o.user_k * o.scale, o.user_p_ref, o.user_eye_h_m, (double)r->vbl / 50.0, r->turn_total_deg + turn_base, r->epoch + (exe_gen << 16), now_s());
    return vrcam_native_to_vr(&o, org, to_vr);
}
/* Último empujón de una muestra de la escena nativa al suavizador (ns del reloj mono): mientras llegue, las escenas de la GPU marcadas para el nativo no empujan la suya. */
static _Atomic uint64_t ns_smp_push_ns;
static VrSmSample ns_smp_pend;           /* muestra de la escena nativa capturada y aún sin publicar (hilo de emulación): se empuja al publicarla (ns_pub_flush) */
#define NS_SMP_FRESH_NS 100000000ull

/* Contexto de la captura (hilo de emulación, en la etapa «pantalla»): zona, cámara VR y mapa mundo -> usuario, y qué hacer con el jugador. */
static Cpu *ns_cpu_last;   /* CPU del último enganche de fotograma (hilo de emulación, el mismo que llama a ns_ctx): para leer los registros de cálculo OFX/OFY */
static void ns_ctx(NsScene *sc, NsCtxOut *o, void *u) {
    (void)u;
    sc->area_id = (uint16_t)bs.area_id;
    sc->list_index = gpu_capture_next_index() + 1u;   /* la lista de la GPU que contendrá el dibujo de esta escena: el renderer no la dibuja con una época de mallas nueva antes de tener la VRAM de esa lista */
    if (ns_cpu_last) {   /* centro de proyección de la pantalla del motor (control 24/25 de la GTE, 16.16): lo usan los efectos de pantalla y la ventana de la pantalla 3D */
        float ox = (float)((int32_t)gte_cfc2(ns_cpu_last, 24)) / 65536.0f, oy = (float)((int32_t)gte_cfc2(ns_cpu_last, 25)) / 65536.0f;
        if (ox > 0.0f && ox < 320.0f && oy > 0.0f && oy < 240.0f) { sc->cam.ofx = ox; sc->cam.ofy = oy; }
    }
    const VrCamRecord *r = vrcam_last_record();
    int core = (bs.valid & BRIDGE_OK_ALL_CORE) == BRIDGE_OK_ALL_CORE && bs.has_player;
    if (!r || !r->active || !core) return;                 /* cinemática / sin cámara VR: sin mapa VR, el 3D sigue saliendo de la lista de la GPU */
    for (int i = 0; i < 3; i++) sc->cam.eye[i] = (float)r->eye[i];
    memcpy(sc->cam.rot, r->M, sizeof sc->cam.rot);
    sc->cam.h = (float)r->screen; sc->cam.overscan_y = 1.0f; sc->cam.vr_active = 1;   /* con la cámara VR el motor no aplica el sobreescaneo (contrato v2) */
    VrSmSample nsmp;
    if (ns_world_to_vr(r, sc->to_vr, &nsmp)) sc->valid |= NS_SV_TOVR_OK;
    ns_cap_pend = 1; body_pend.n = 0;
    if (nsmp.valid && view.kind != VRV_K_ENGINE) ns_smp_pend = nsmp;   /* el suavizador corrige la escena que se dibuja: la nativa lleva el último registro, la lista el anterior */
    { VrBodyStats b; vrbody_stats(&b); ik_ap0 = b.applies; ik_dr0 = b.arms_driven; }
    o->player_key = ns_player_key(emu_ram);
    o->hide_head = 1;                                       /* primera persona: la cabeza propia no se dibuja (misma regla que vrcam_hide_head) */
    o->refresh_parts = body_ok ? NS_PARTS_BODY : 0;   /* brazos + pelvis, piernas, torso y cuello: torso_follow las reescribe tras la captura */
}
/* Manos nuevas: cierre de la escena (hilo de emulación, ns_hook_tick, escena apta con el jugador enlazado). Agarre y brazos manejados de la IK del último vrbody_apply. */
static void ns_hands_finalize(NsScene *sc, NsMeshStore *st, const NsPlayerMap *map, void *u) {
    (void)u;
    ns_wphide_apply(sc, emu_ram, vrfire_hide_enabled());   /* el arma del juego en la mano de Snake (la lista de la GPU la oculta con +0x4C = 0, el extractor nativo no lo lee) */
    if ((sc->screen.flags & NS_SFX_GAS) && vrhost_duct_ring(sc->cam.vr_active, (bs.player_status & BRIDGE_PS_INTRUDE) != 0, sc->screen.gas_abr)) sc->screen.flags &= ~NS_SFX_GAS;   /* oscurecimiento de la 1.ª persona de gateo del juego: sin viñeta en VR (vr_view.h) */
    NsHandIn in;
    memset(&in, 0, sizeof in);
    in.enabled = hands_active();
    if (in.enabled) {
        for (int s = 0; s < 2; s++) { float g = 0; in.driven[s] = vrbody_hand_state(s, &g, NULL, NULL); in.grip[s] = in.driven[s] ? g : 0.0f; }
        for (int s = 0; s < 2; s++) vrgame_hand_pose_in(s, &in.item_pose[s], &in.trigger[s], &in.thumb[s]);   /* objeto en la mano (arma por vr_grip), gatillo y pulgar */
        ns_hand_apply(sc, st, map, &in);
    }
    /* Granada (arma que se dibuja con el modelo del juego): el juego la cuelga de la mano de su ANIMACIÓN (la derecha); se lleva a la mano que la IK dejó en el mando del arma, con la relación mano-arma del juego. */
    uint32_t held_key = 0;
    const int held_side = vrgame_native_hand();
    if (held_side >= 0) {
        float Ra[9], ta[3], Rd[9], td[3];
        if (vrbody_hand_anim(1, Ra, ta) && vrbody_hand_state(held_side, NULL, Rd, td) && ns_wphide_carry(sc, emu_ram, Ra, ta, Rd, td, held_side == 0) > 0) held_key = ns_wphide_key(emu_ram);
    }
    VblHand hc[2];       /* marco de cada mano del mando con el que se simuló esta escena: el cuerpo vivo mueve la mano propia y la granada desde él hasta el mando de ahora (vbl_piece) */
    for (int s = 0; s < 2; s++) { memset(&hc[s], 0, sizeof hc[s]); hc[s].ok = vrgame_hand_frame_sim(s, hc[s].p, hc[s].q); }
    uint32_t live = 0;   /* brazos que la IK lleva con el mando (vr_body: lado 0 = izquierdo, piezas 7-9; 1 = derecho, 2-4): van en su última pose, como el arma */
    for (int s = 0; s < 2; s++) { float g = 0; if (vrbody_hand_state(s, &g, NULL, NULL)) live |= s ? (1u << 2) | (1u << 3) | (1u << 4) : (1u << 7) | (1u << 8) | (1u << 9); }
    if (vbl_capture(&body_pend, sc, map, live, held_key, held_side, hc)) { atomic_store(&body_key, map->key); atomic_store(&body_held, held_key); }   /* pose del cuerpo (ya con la IK) y de las manos propias para el renderer (mv_vrhost_body_piece) */
}
/* Justo ANTES de publicar la escena (observador de ns_hook_tick, tras el cierre): su muestra entra en el suavizador y su pose del cuerpo en el anillo, las dos bajo SMX. No al
 * capturarla (el renderer seguiría dibujando la escena anterior varios ms con el movimiento de la nueva) ni después de publicarla (el renderer podría tomar la escena nueva sin
 * su pose: mv_vrhost_body_piece no la reconocería; al revés sí: reconoce la escena anterior). */
static void ns_pub_flush(void) {
    if (!ns_cap_pend) return;
    pthread_mutex_lock(&SMX);
    if (ns_smp_pend.valid) { sm_ensure(); vrsm_push(&SM, &ns_smp_pend, now_s()); }
    vbl_push(&body_ring, &body_pend);
    pthread_mutex_unlock(&SMX);
    if (ns_smp_pend.valid) atomic_store(&ns_smp_push_ns, mono_ns() | 1);
    ns_smp_pend.valid = 0; body_pend.n = 0; ns_cap_pend = 0;
}
/* Antes de cada captura: con las manos nuevas Y el render nativo en uso, la malla de la manopla del motor NO se deforma en puño (la mano nueva hace el puño y su forma
 * de reposo debe ser estable); con la lista de la GPU (o manos originales) sigue el puño deformado de siempre. */
static void hands_precapture(uint8_t *ram) {
    vrbody_set_fist_suppressed(hands_active());
    vrbody_prefist(ram);
}
/* Colisión de la cabeza con la escena NATIVA (vr_headcol): la escena trae el escenario y los objetos sólidos en 360 grados, también lo que está detrás o a los lados de la
 * cámara del juego (la lista de la GPU solo trae lo que ésta ve). Se recoge en el hilo de emulación justo antes de publicar la escena (ns_hook_set_scene_tap) y REEMPLAZA la
 * lista nativa del almacén (sin fantasmas). ns_solid.c solo está en la app; en el host de escritorio esta definición débil vacía hace que valga solo la lista de la GPU. */
__attribute__((weak)) size_t ns_solid_collect(const NsScene *sc, const NsMeshStore *st, const NsSolidOpts *opt, float *tris, NsSolidSrc *src, size_t cap, NsSolidStats *stats) {
    (void)sc; (void)st; (void)opt; (void)tris; (void)src; (void)cap;
    if (stats) memset(stats, 0, sizeof *stats);
    return 0;
}
static float hc_nat_buf[HC_NAT_MAX * 9];
static struct { _Atomic uint32_t scenes, no_mesh, overflow, retried; _Atomic uint32_t last_tris; } hcn;
static VrHcOut hc_last;                  /* última consulta (bajo HCMX), solo para el registro */
/* Última escena nativa publicada (la que verá el renderer): bits de validez, efectos de pantalla que trae (NsScreenFx.flags; 0 sin NS_SV_FX; sin el anillo si su intensidad es 0) y sincronismo
 * en que se publicó. Hilo de emulación (ns_solid_tap); lo consulta prov_covered en el mismo hilo: garantía de cobertura nativa de vr_prov.h. */
static uint32_t nat_valid, nat_sfx, nat_vb; static int nat_have;
static void ns_solid_tap(const NsScene *s, const NsMeshStore *st, void *u) {
    (void)u;
    ns_pub_flush();
    nat_have = s != NULL; nat_valid = s ? s->valid : 0; nat_vb = (uint32_t)hle_frame_count();
    nat_sfx = s && (s->valid & NS_SV_FX) ? (s->screen.gas_intensity > 0 ? s->screen.flags : s->screen.flags & ~(uint32_t)NS_SFX_GAS) : 0;
    if (!hc_on || !s || !st || !(s->valid & NS_SV_TOVR_OK) || !(s->valid & NS_SV_GAME)) return;   /* cinemática / sin cámara VR: no hay ancla */
    NsSolidOpts o; memset(&o, 0, sizeof o);
    float jt[16][3];
    int nj = vrcam_model_joints(emu_ram, jt, 16);
    float upm = s->units_per_m > 1.0f ? s->units_per_m : 1000.0f;
    memcpy(o.center, s->cam.eye, sizeof o.center);
    o.skip_key = ns_player_key(emu_ram);
    o.joints = (const float (*)[3])jt; o.n_joints = nj > 0 ? nj : 0; o.body_units = vrhc_config()->body_m * upm;
    float radius = (vrhc_config()->keep_m + 1.0f) * upm;   /* margen sobre el almacén de la GPU: la cabeza física puede estar lejos del ancla */
    NsSolidStats S;
    size_t n = 0;
    for (int tryn = 0; tryn < 3; tryn++) {   /* si no cabe, se reduce el radio (el barrido solo necesita lo cercano); aun así la consulta funde a negro si se desborda */
        o.radius = radius;
        n = ns_solid_collect(s, st, &o, hc_nat_buf, NULL, HC_NAT_MAX, &S);
        if (!S.overflow) break;
        radius *= 0.5f;
        atomic_fetch_add_explicit(&hcn.retried, 1, memory_order_relaxed);
    }
    pthread_mutex_lock(&HCMX);
    vrhc_feed_world(hc_nat_buf, (int)n, S.overflow != 0);   /* truncada: la consulta funde a negro por seguridad */
    pthread_mutex_unlock(&HCMX);
    atomic_fetch_add_explicit(&hcn.scenes, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&hcn.no_mesh, S.pieces_no_mesh, memory_order_relaxed);
    atomic_fetch_add_explicit(&hcn.overflow, S.overflow ? 1u : 0u, memory_order_relaxed);
    atomic_store_explicit(&hcn.last_tris, (uint32_t)n, memory_order_relaxed);
}

static void ns_post_ik(uint8_t *ram) { ns_hook_post_ik(ram); ns_cover_glue_post_ik(ram); }
/* Capa de brazos inactiva: vrbody_apply corrió después de la captura de este fotograma y no manejó ningún brazo (mandos sin pose válida: en los registros, `pose_valida I=0 D=0`,
 * `IK: aplicaciones=+25 brazos=+0` y 25 escenas «sin IK» por segundo => valid=0x47, escena-no-apta, GPU o mundo sin dibujar en solo nativo con el jugador quieto). */
static int ns_ik_idle(void *u) {
    (void)u;
    VrBodyStats b;
    vrbody_stats(&b);
    return b.applies > ik_ap0 && b.arms_driven == ik_dr0;
}

/* Vigilancia por fotograma (hilo de emulación, tras el sincronismo): (M4) con la capa de cuerpo activa cada captura debe llevar la pose IK de los brazos
 * (ik_refresh crece a la par que captures); (B4) reintento de la instalación y reinstalación si otra overlay pisó la etapa. Se registra como mucho una vez por segundo. */
static void ns_frame_check(void) {
    static uint32_t cap0, ik0, miss0, idle0, pub0, nfr, capw0, capb0, capd0;
    static uint64_t last_log_ns;
    NsHookStats st;
    ns_hook_stats(&st);
#ifdef MV_NOGEN
    if (vr_ok && (++nfr & 31u) == 0) {
        unsigned stale0 = ns_hook_stale_count(), dstale0 = ns_hook_dyn_stale_count();
        uint32_t dyn0 = ns_hook_dyn_addr();
        int was = ns_hook_installed(), rc0 = ns_hook_last_rc();
        ns_hook_poll();
        if (ns_hook_stale_count() != stale0) fprintf(stderr, "[vrhost] etapa de pantalla cambiada (otra overlay): enganche del render nativo %s\n", ns_hook_installed() ? "reinstalado" : "retirado");
        else if (!was && ns_hook_installed()) fprintf(stderr, "[vrhost] enganche del render nativo instalado en un reintento\n");
        else if (!was && ns_hook_last_rc() != rc0) fprintf(stderr, "[vrhost] enganche del render nativo sin instalar (%d)\n", ns_hook_last_rc());
        if (ns_hook_dyn_addr() != dyn0 || ns_hook_dyn_stale_count() != dstale0) fprintf(stderr, "[vrhost] enganche dinamico de la entrada 0 de etapas: 0x%08x -> 0x%08x\n", (unsigned)dyn0, (unsigned)ns_hook_dyn_addr());
    }
#endif
    uint64_t t = mono_ns();
    if (t - last_log_ns < 1000000000ull) return;
    last_log_ns = t;
    {   /* cobertura de la lista de la GPU por la escena nativa (resultado del hilo trabajador, ~1 por segundo) */
        static char cbuf[8192];
        if (ns_cover_glue_poll(cbuf, sizeof cbuf)) fputs(cbuf, stderr);
    }
    {   /* colisión de la cabeza (una línea por segundo): de dónde sale la geometría y qué hizo la última consulta */
        static uint32_t hs0, hn0, ho0, hr0;
        VrHcOut q; int nat, gpu;
        pthread_mutex_lock(&HCMX); q = hc_last; nat = vrhc_native_count(); gpu = vrhc_count(); pthread_mutex_unlock(&HCMX);
        uint32_t hs = atomic_load(&hcn.scenes), hn = atomic_load(&hcn.no_mesh), ho = atomic_load(&hcn.overflow), hr = atomic_load(&hcn.retried);
        if (hc_on && (hs || gpu))
            fprintf(stderr, "[cabeza] geometria: nativa=%d tris (vigente=%d, escenas/s=%u, mallas_ausentes=+%u, desbordes=+%u, radio_reducido=+%u) gpu=%d tris | ultima consulta: candidatos=%d expulsion=%.3fm holgura=%.3fm atraviesa=%.2f negro=%.2f recorte=%.3fm%s\n",
                    nat, q.native_live, hs - hs0, hn - hn0, ho - ho0, hr - hr0, gpu, q.n_cand, q.pushed_m, q.clear_m, q.through, q.fade, q.corr_len, q.overflow ? " DESBORDE" : "");
        hs0 = hs; hn0 = hn; ho0 = ho; hr0 = hr;
    }
    uint32_t dcap = st.captures - cap0, dik = st.ik_refresh - ik0, dmiss = st.ik_missed - miss0, didle = st.ik_idle - idle0;
    cap0 = st.captures; ik0 = st.ik_refresh; miss0 = st.ik_missed; idle0 = st.ik_idle;
    {   /* Diagnóstico por segundo (una línea cada una), siempre: qué extrajo el enganche del motor y cómo van las manos. Se mide todo junto para no ir a ciegas. */
        static NsExtractGlueStats g0;
        static VrBodyStats b0;
        NsExtractGlueStats g;
        ns_extract_glue_stats(&g);
        if (st.captures || g.calls)
            fprintf(stderr, "[captura] por s: escenas=%u publicadas=%u sin_IK=%u | ultima: cola=%u instancias=%u escenario=%u piezas=%u ocultas=%u descartes(grupo=%u oculto=%u otros=%u) | por s: mallas_nuevas=%u memo_obsoleto=%u cambios_zona=%u epocas=%u almacen_lleno=%u\n",
                    dcap, st.published - pub0, dmiss, g.last_queue, g.last_inst, g.last_static, g.last_pieces, g.last_hidden_pieces, g.last_drop_group, g.last_drop_hidden, g.last_drop_other,
                    g.mesh_new - g0.mesh_new, g.memo_stale - g0.memo_stale, g.zone_changes - g0.zone_changes, g.epoch_opened - g0.epoch_opened, g.store_full - g0.store_full);
        g0 = g; pub0 = st.published;
        if (st.captures || capw0 || capb0 || capd0)   /* de dónde sale cada captura: envoltura de la etapa «pantalla» o respaldo del fin del bucle de etapas; la entrada 0 de la tabla delata una copia en overlay (estándar 0x8001d324) */
            fprintf(stderr, "[captura] canales por s: envoltura=%u dinamico=%u respaldo=%u | dinamico_en=0x%08x | entrada0_etapas=0x%08x (estandar 0x%08x%s)\n", st.cap_wrap - capw0, st.cap_dyn - capd0, st.cap_backup - capb0, (unsigned)ns_hook_dyn_addr(), ns_hook_stage_entry(), (unsigned)NS_STAGE_ADDR,
                    ns_hook_stage_entry() == NS_STAGE_ADDR ? "" : ", DISTINTA");
        capw0 = st.cap_wrap; capb0 = st.cap_backup; capd0 = st.cap_dyn;
        if (body_ok) {
            VrBodyStats b;
            vrbody_stats(&b);
            float raw[2] = {0, 0};
            int32_t ok[2] = {0, 0};
            mv_vrgame_grip_diag(raw, ok);
            const VrBodyConfig *bc = vrbody_config();
            float minc = bc ? bc->fist_min_grip : 0.03f;
            const char *est[2];
            float cur[2], w[2];
            int drv[2];
            for (int i = 0; i < 2; i++) {
                const VrBodyDebug *d = vrbody_debug(i);
                cur[i] = d ? d->grip : 0.0f; w[i] = d ? d->w : 0.0f; drv[i] = d ? d->driven : 0;
                est[i] = !drv[i] ? "sin_IK" : cur[i] * w[i] >= 0.9f ? "puno" : cur[i] * w[i] >= minc ? "cerrando" : "abierta";
            }
            fprintf(stderr, "[manos] grip_crudo I=%.2f D=%.2f pose_valida I=%d D=%d | aplicado I=%.2f D=%.2f peso I=%.2f D=%.2f | estado I=%s D=%s | puno_malla: deformadas=+%u omitidas=+%u (fist=%d minimo=%.2f) | IK: aplicaciones=+%u brazos=+%u | own_hands=%d glv_pub=%d piezas_ocultas=+%u | estirados=+%u hombro=+%u antebrazo_recortado=+%u factor_estirado medio=%.2f max=%.2f\n",
                    raw[0], raw[1], (int)ok[0], (int)ok[1], cur[0], cur[1], w[0], w[1], est[0], est[1],
                    b.fist_applies - b0.fist_applies, b.fist_skipped - b0.fist_skipped, bc ? bc->fist_enabled : -1, minc, b.applies - b0.applies, b.arms_driven - b0.arms_driven,
                    bc ? bc->own_hands : -1, vrgame_glove_pub(), b.hidden_pieces - b0.hidden_pieces,
                    b.stretched - b0.stretched, b.shoulder_limit - b0.shoulder_limit, b.wrist_limit - b0.wrist_limit,
                    b.stretched > b0.stretched ? (b.stretch_sum - b0.stretch_sum) / (float)(b.stretched - b0.stretched) : 1.0f, b.stretch_max);
            fprintf(stderr, "[cuerpo] torso_follow=%d aplicado=+%u (guinada torso=%.1f mirada=%.1f dif=%.1f grados) | doble_cara: activa=%d modelos=+%u | piezas_cerca_ocultas=+%u\n",
                    bc ? bc->torso_follow : -1, b.torso_applies - b0.torso_applies, b.torso_yaw, b.torso_gaze, b.torso_gaze - b.torso_yaw,
                    (int)(b.two_sided - b0.two_sided > 0), b.two_sided - b0.two_sided, b.near_hidden - b0.near_hidden);
            b0 = b;
        }
    }
    if (body_ok && dcap > 0 && (dik < dcap || dmiss))
        fprintf(stderr, "[vrhost] render nativo: %u capturas, %u con pose IK, %u con la capa de brazos inactiva (aptas, brazos del juego), %u sin IK (esas escenas se pintan por GPU); jugador enlazado %u\n", dcap, dik, didle, dmiss, st.player_bound);
}

/* ¿Esta escena de la lista ADMITE el 3D nativo? Solo cámara VR en primera persona (no cinemáticas) y 3D de mundo válido. No depende del modo ni del estado del puente:
 * el productor no decide nada, solo marca (mark_3d); quien dibuja decide con ns_native_decide y su estado real de ese fotograma (A1). */
static int native_for_scene(const VrCamRecord *r, const VrScene *sc, int cine_cand) {
    return native_mode() && r && r->active && !cine_cand && sc->has_3d && sc->world_valid && view.kind != VRV_K_ENGINE;   /* en el modo gpu nada se marca: la lista se dibuja entera */
}
/* Marca los batches 3D (NO quita ninguno): kind 0 -> VR_SKIP_WORLD, kind 1 -> VR_SKIP_FX. Con sc.native_3d = 0 no hay marcas y se dibuja todo. */
static void mark_3d(Slot *s) {
    for (uint32_t i = 0; i < s->sc.nb; i++) s->b[i].skip_if_native = s->b[i].kind == 0 ? VR_SKIP_WORLD : s->b[i].kind == 1 ? VR_SKIP_FX : 0;
    s->sc.native_3d = 1;
}

/* Compuerta de la captura (M1): con el modo gpu (o sin renderer nativo listo) no se captura ni se extrae nada, salvo para un volcado pendiente en el punto de enganche. */
enum { DS_IDLE = 0, DS_REQ, DS_CAP, DS_WRITE };
static _Atomic int dump_state;   /* escrito bajo DMX; la compuerta de la captura lo lee sin bloquear */
/* Capturas pendientes del pedido en curso (bajo DMX; DN_*: vrdump.h). */
static int dump_need;
static char dump_label[48];   /* etiqueta del pedido en curso (point=<etiqueta> en el meta; vacía = stage/frame): autovolcado */
static int dump_stage_mode(void) {
    static int env = -1;
    if (env < 0) { const char *e = getenv("MV_DUMP_AT"); env = e && !strcmp(e, "stage"); }
    return env;
}
static int ns_gate(void *u) {
    (void)u;
    if (atomic_load(&render_req) && atomic_load(&renderer_ready)) return 1;
    return dump_stage_mode() && atomic_load(&dump_state) == DS_REQ;
}

/* ------------------------------------------------------------------------------------------------ cola de escenas */
int mv_host_scene(VrScene *out) {
    atomic_store(&last_poll_ns, mono_ns() | 1);
    if (!out) return 0;
    pthread_mutex_lock(&MX);
    if (!q_n) { pthread_mutex_unlock(&MX); return 0; }
    if (held >= 0) sstate[held] = S_FREE;
    int i = queue[q_head];
    q_head = (q_head + 1) % NQ;
    q_n--;
    sstate[i] = S_HELD;
    held = i;
    *out = slot[i].sc;
    hc_disp = slot[i].hcmap;

    VrSmSample smp = slot[i].smp;
    {   /* escena de la lista marcada para el nativo y muestra nativa reciente: el suavizador ya tiene la de la escena que se dibuja; la de la lista es de otro registro */
        uint64_t np = atomic_load(&ns_smp_push_ns);
        if (smp.valid && slot[i].sc.native_3d && np && mono_ns() - np < NS_SMP_FRESH_NS) smp.valid = -1;   /* -1 = no empujar */
    }
    stats.delivered++;
    pthread_mutex_unlock(&MX);
    if (smp.valid >= 0) {
        pthread_mutex_lock(&SMX);
        sm_ensure();
        vrsm_push(&SM, &smp, now_s());
        pthread_mutex_unlock(&SMX);
    }
    return 1;
}

static int take_free_slot(void) {
    int r = -1;
    pthread_mutex_lock(&MX);
    if (q_n < NQ)
        for (int i = 0; i < NSLOT; i++) if (sstate[i] == S_FREE) { sstate[i] = S_BUILD; r = i; break; }
    pthread_mutex_unlock(&MX);
    return r;
}

static void drop_queue(void) {
    pthread_mutex_lock(&MX);
    while (q_n) { sstate[queue[q_head]] = S_FREE; q_head = (q_head + 1) % NQ; q_n--; }
    pthread_mutex_unlock(&MX);
}

/* Copia la escena recién ensamblada (memoria interna de vr_scene, reutilizada en la siguiente llamada) y los eventos de VRAM
 * de la lista a memoria del slot. De la lista solo se conservan los eventos (subida / copia / relleno) compactados; ev_end y
 * src_op de los batches se reescriben a índices de esa lista compacta. Devuelve 0 o <0 sin memoria. */
static const uint64_t *g_opkey;   /* identidad de efecto por primitiva de la lista que se ensambla (build_one; NULL = ninguna) */
static int fill_slot(Slot *s, const GpuDrawList *l, const VrScene *sc) {
    size_t nv = sc->v ? sc->nv : 0, nb = sc->b ? sc->nb : 0, ns = sc->states ? sc->nstates : 0;
    if (!grow((void **)&s->v, &s->v_cap, nv, sizeof *s->v) || !grow((void **)&s->b, &s->b_cap, nb, sizeof *s->b) ||
        !grow((void **)&s->st, &s->st_cap, ns, sizeof *s->st) || !grow((void **)&s->tc, &s->tc_cap, nv / 3 + 1, sizeof *s->tc) ||
        !grow((void **)&s->tk, &s->tk_cap, nv / 3 + 1, sizeof *s->tk) || !grow((void **)&s->tsrc, &s->tsrc_cap, nv / 3 + 1, sizeof *s->tsrc))
        return -1;
    int any_fx = 0;
    if (nv && sc->tri_src) memcpy(s->tsrc, sc->tri_src, (nv / 3) * sizeof *s->tsrc);
    for (size_t t = 0; t < nv / 3; t++) {
        uint64_t k = (g_opkey && sc->tri_src && sc->tri_src[t] < l->nops) ? g_opkey[sc->tri_src[t]] : 0;
        s->tk[t] = k; any_fx |= k != 0;
    }
    if (nv) memcpy(s->v, sc->v, nv * sizeof *s->v);
    if (nv && sc->tri_cause) memcpy(s->tc, sc->tri_cause, nv / 3); else memset(s->tc, 0, nv / 3 + 1);
    if (nb) memcpy(s->b, sc->b, nb * sizeof *s->b);
    if (ns) memcpy(s->st, sc->states, ns * sizeof *s->st);

    size_t nops = l->nops;
    if (!grow((void **)&pre, &pre_cap, nops + 1, sizeof *pre)) return -1;
    uint32_t nev = 0;
    for (size_t i = 0; i < nops; i++) {
        pre[i] = nev;
        int t = l->ops[i].type;
        if (t == GPU_OP_UPLOAD || t == GPU_OP_VCOPY || t == GPU_OP_FILL) nev++;
    }
    pre[nops] = nev;
    if (!grow((void **)&s->ops, &s->ops_cap, nev, sizeof *s->ops)) return -1;
    uint32_t k = 0;
    for (size_t i = 0; i < nops; i++) {
        int t = l->ops[i].type;
        if (t == GPU_OP_UPLOAD || t == GPU_OP_VCOPY || t == GPU_OP_FILL) s->ops[k++] = l->ops[i];
    }
    for (size_t i = 0; i < nb; i++) {
        VrBatch *b = &s->b[i];
        b->ev_end = pre[b->ev_end < nops ? b->ev_end : nops];
        b->src_op = pre[b->src_op < nops ? b->src_op : nops];
    }
    if (!grow((void **)&s->data, &s->data_cap, l->ndata, sizeof *s->data)) return -1;
    if (l->ndata) memcpy(s->data, l->data, (size_t)l->ndata * sizeof *s->data);
    s->has_vram = 0;
    if (l->vram_init) {
        size_t n = (size_t)GPU_VRAM_W * GPU_VRAM_H;
        if (!s->vram) s->vram = malloc(n * sizeof *s->vram);
        if (!s->vram) return -1;
        memcpy(s->vram, l->vram_init, n * sizeof *s->vram);
        s->has_vram = 1;
    }

    memset(&s->list, 0, sizeof s->list);
    s->list.index = l->index;
    s->list.host_frame = l->host_frame;
    s->list.reason = l->reason;
    s->list.disp = l->disp;
    s->list.ops = nev ? s->ops : NULL;
    s->list.nops = nev;
    s->list.data = l->ndata ? s->data : NULL;
    s->list.ndata = l->ndata;
    s->list.vram_init = s->has_vram ? s->vram : NULL;

    s->sc = *sc;
    s->sc.v = nv ? s->v : NULL;
    s->sc.b = nb ? s->b : NULL;
    s->sc.states = ns ? s->st : NULL;
    s->sc.tri_cause = nv ? s->tc : NULL;
    s->sc.tri_src = nv ? s->tsrc : NULL;
    s->sc.tri_fx = any_fx ? s->tk : NULL;
    s->sc.src = &s->list;
    if (sc->filter.frame_on && sc->filter.frame && sc->filter.frame_w == VR_FRAME_W && sc->filter.frame_h == VR_FRAME_H) {
        memcpy(s->frm, sc->filter.frame, sizeof s->frm);
        s->sc.filter.frame = s->frm;
    } else {
        s->sc.filter.frame_on = 0; s->sc.filter.frame = NULL; s->sc.filter.frame_w = s->sc.filter.frame_h = 0;
    }
    return 0;
}

/* Copia la escena del radar (memoria interna de vr_radar, reutilizada en la siguiente llamada) al slot. Sus ev_end/src_op ya vienen en
 * índices de la lista HUD (split.hud) y por eso se reescriben igual que los del HUD con el prefijo de eventos `pre` (fill_slot). */
static int fill_radar(Slot *s, const VrRadarScene *r, size_t nops_hud) {
    memset(&s->rs, 0, sizeof s->rs);
    if (!r || !r->present || !r->v || !r->b) return 0;
    if (!grow((void **)&s->rv, &s->rv_cap, r->nv, sizeof *s->rv) || !grow((void **)&s->rb, &s->rb_cap, r->nb, sizeof *s->rb) ||
        !grow((void **)&s->rst, &s->rst_cap, r->nstates, sizeof *s->rst))
        return -1;
    memcpy(s->rv, r->v, r->nv * sizeof *s->rv);
    memcpy(s->rb, r->b, r->nb * sizeof *s->rb);
    if (r->nstates) memcpy(s->rst, r->states, r->nstates * sizeof *s->rst);
    for (size_t i = 0; i < r->nb; i++) {
        VrBatch *b = &s->rb[i];
        b->ev_end = pre[b->ev_end < nops_hud ? b->ev_end : nops_hud];
        b->src_op = pre[b->src_op < nops_hud ? b->src_op : nops_hud];
    }
    s->rs = *r;
    s->rs.v = s->rv; s->rs.b = s->rb; s->rs.states = r->nstates ? s->rst : NULL;
    return 0;
}

/* Registro de cámara con el que se emitió la lista (por número de secuencia del primer polígono con posición de vista).
 * *has3d = 1 si la lista tiene algún polígono con posición de vista (si no, no hay registro que esperar). */
static const VrCamRecord *list_record(const GpuDrawList *l, int *has3d) {
    *has3d = 0;
    for (uint32_t i = 0; i < l->nops; i++)
        if (l->ops[i].type == GPU_OP_POLY && l->ops[i].v[0].vvalid) { *has3d = 1; return vrcam_seq_record(l->ops[i].seq); }
    return NULL;
}

/* Efectos nativos: a cada primitiva de la lista se le asigna la identidad del elemento de efectos (ns_fx_item_key) o de malla (NSFX_KEY_MESH) que la sustituye en
 * el render nativo. El render solo oculta (por triángulo) lo que confirma haber dibujado; el resto sigue por GPU. Se usa la RAM del final del fotograma: puede traer los
 * paquetes del fotograma siguiente, de ahí la casación tolerante (ns_fx_classify_near) con la cámara de la lista. NULL si no hay nada que asignar. */
static NsFxScene *hx_fx;
static int hx_cov;   /* hx_fx frente a la RAM de la lista en curso: 0 sin extraer, 1 extraída, -1 fallida (se pone a 0 al empezar cada lista en build_one) */
/* Una sola extracción por lista (ns_fx_extract no es idempotente: g_shadow_tex y g_szl); la comparten prov_covered y fx_classify_list. */
static int hx_extract(void) {
    if (!hx_cov) {
        if (!hx_fx) hx_fx = ns_fx_scene_create(0);
        NsFxOpts fo = ns_fx_opts_default();
        hx_cov = hx_fx && ns_fx_extract(emu_ram, &fo, hx_fx) == 0 ? 1 : -1;
    }
    return hx_cov > 0;
}
static uint8_t *hx_cls; static int32_t *hx_item; static uint64_t *hx_key; static size_t hx_cap;
static const uint64_t *fx_classify_list(const GpuDrawList *hl, const VrCamRecord *r) {
    size_t n = hl ? hl->nops : 0;
    if (!n) return NULL;
    if (n > hx_cap) {
        size_t nc = hx_cap ? hx_cap : 1024;
        while (nc < n) nc *= 2;
        uint8_t *a = (uint8_t *)realloc(hx_cls, nc); if (!a) return NULL; hx_cls = a;
        int32_t *b = (int32_t *)realloc(hx_item, nc * sizeof *b); if (!b) return NULL; hx_item = b;
        uint64_t *c = (uint64_t *)realloc(hx_key, nc * sizeof *c); if (!c) return NULL; hx_key = c;
        hx_cap = nc;
    }
    if (!hx_extract()) return NULL;
    memset(hx_cls, 0, n);
    for (size_t i = 0; i < n; i++) hx_item[i] = -1;
    if (ns_fx_classify(emu_ram, hx_fx, hl, hx_cls, NULL, hx_item, NULL) != 0) return NULL;
    NsFxListCam lc;
    if (r) { for (int i = 0; i < 3; i++) lc.eye[i] = (float)r->eye[i]; memcpy(lc.M, r->M, sizeof lc.M); }
    ns_fx_classify_near(hx_fx, r ? &lc : NULL, hl, hx_cls, hx_item);
    ns_fx_learn(hx_fx, hl, hx_item);   /* página de textura y modo de mezcla que el paquete no trae: se recuerdan para los fotogramas siguientes */
    for (size_t i = 0; i < n; i++) {
        uint64_t k = 0;
        if ((hx_cls[i] == NSFX_OP_FX || hx_cls[i] == NSFX_OP_FX_PACK) && hx_item[i] >= 0 && (uint32_t)hx_item[i] < hx_fx->n_items) k = ns_fx_item_key(&hx_fx->items[hx_item[i]]);
        else if (hx_cls[i] == NSFX_OP_MESH) k = NSFX_KEY_MESH;
        hx_key[i] = k;
    }
    return hx_key;
}

/* Garantía de cobertura nativa para la clasificación por procedencia (VrProvLearn.covered; hilo de emulación, dentro de build_one). Todo exige el renderer nativo preparado y la última escena
 * nativa publicada apta y reciente (vrhost_native_sfx; aptitud como NativeRenderer.ingest: cinemática del motor = partida + listas + mapa; si no, ns_scene_native_ok): con una escena no apta
 * o sin escenas nuevas el renderer repite la última válida, con los efectos de AQUELLA. Anillo / fundido: además esa escena trae el efecto. Paquete: además esa escena trae efectos
 * (NS_SV_FX) y el paquete tiene un elemento exportable en la escena de efectos de la RAM actual (ns_fx_covers_packet; una extracción por lista, perezosa). */
static int prov_covered(void *ctx, int what, uint32_t pkt_addr) {
    (void)ctx;
    const uint32_t cine_need = NS_SV_GAME | NS_SV_LISTS_OK | NS_SV_MAP_OK;
    int ready = native_mode() && atomic_load(&renderer_ready) && nat_have;
    int fit = view.kind == VRV_K_ENGINE ? (nat_valid & cine_need) == cine_need && !(nat_valid & NS_NATIVE_BAD) : ns_scene_native_ok(nat_valid);
    uint32_t age = (uint32_t)hle_frame_count() - nat_vb;
    if (what == VPC_COV_GAS) return vrhost_native_sfx(ready, fit, nat_sfx, age, NS_SFX_GAS);
    if (what == VPC_COV_FADE) return vrhost_native_sfx(ready, fit, nat_sfx, age, NS_SFX_FADE);
    if (what != VPC_COV_PACKET || !vrhost_native_sfx(ready, fit, nat_valid, age, NS_SV_FX)) return 0;
    return hx_extract() && ns_fx_covers_packet(emu_ram, hx_fx, pkt_addr);
}

/* Subidas de la lista (CPU -> VRAM) que cubren el área mostrada: mide un fotograma de vídeo descomprimido tanto si llega de una vez como en tiras de 16 px (decodificador
 * del juego). Sin solapes entre subidas del mismo fotograma, que es lo normal (vr_view.h). */
static int list_uploads_cover_display(const GpuDrawList *l) {
    long long sum = 0;
    for (uint32_t i = 0; i < l->nops; i++) {
        const GpuOp *op = &l->ops[i];
        if (op->type == GPU_OP_UPLOAD) sum += vrview_upload_area(l->disp.x, l->disp.y, l->disp.w, l->disp.h, l->disp.depth24, op->rx, op->ry, op->rw, op->rh);
    }
    return vrview_uploads_cover(sum, l->disp.w, l->disp.h, l->disp.depth24);
}

/* Registro «[gpu3d]» (hilo de emulación, una línea por segundo como mucho): qué hizo el ensamblador con las primitivas de las listas de ese segundo. Es el único diagnóstico del visor. */
#define GPU3D_NEAR_EYE_M 0.10f   /* modo gpu: ningún triángulo 3D pequeño con TODOS sus vértices a menos de esta distancia (m) del ojo del motor (VrSceneOpts.eye_near_m) */
static struct { uint32_t lists, prims, p3d, tris3d, p2d, rect, rect_semi_tex, near_drop, near_clip, behind_drop, nohp, post, ambig, incoh, recon, huge, nan;
              uint32_t groups_max, ref_n, ref_total, quad3d, quad_noncoh, tri_cause[5], cause_tris, coplanar, coplanar_skip, hw_cull, rect_hp, rect3d, billboard;  int cam_head; uint64_t t_log; } g3;
static void gpu3d_note(const VrScene *sc, const VrCamRecord *r) {
    const VrStats *st = &sc->stats;
    g3.lists++;
    g3.prims += st->n_poly + st->n_rect + st->n_line;
    g3.rect += st->n_rect; g3.rect_semi_tex += st->n_rect_semi_tex;
    g3.p3d += st->n_poly3d;
    g3.tris3d += st->n_tris3d;
    g3.p2d += st->n_poly2d + st->n_rect + st->n_line;
    g3.near_drop += st->n_near_eye;
    g3.near_clip += st->n_clipped;
    g3.behind_drop += st->drop_behind;
    g3.nohp += st->drop_partial;
    g3.ambig += st->drop_ambig; g3.incoh += st->drop_incoh; g3.recon += st->n_recon; g3.huge += st->drop_huge; g3.nan += st->drop_nan;
    if (st->n_ref_groups > g3.groups_max) g3.groups_max = st->n_ref_groups;
    g3.ref_n += st->n_ref_group; g3.ref_total += st->n_ref_total;
    g3.quad3d += st->n_quad3d; g3.quad_noncoh += st->n_quad_noncoh;
    g3.coplanar += st->n_coplanar; g3.coplanar_skip += st->n_coplanar_skip;
    g3.hw_cull += st->n_hw_cull; g3.rect_hp += st->n_rect_hp; g3.rect3d += st->n_rect3d; g3.billboard += st->n_billboard;
    for (uint32_t i = 0; sc->b && sc->tri_cause && i < sc->nb; i++) {   /* causas de duda de los triángulos 3D (VR_TC_*) */
        if (sc->b[i].kind > 1) continue;
        for (uint32_t j = 0; j < sc->b[i].count; j += 3) {
            uint8_t c = sc->tri_cause[(sc->b[i].first + j) / 3];
            g3.cause_tris++;
            for (int k = 0; k < 5; k++) g3.tri_cause[k] += (c >> k) & 1u;   /* VR_TC_AMBIG, SAT, BEHIND, RECON, DERIVED = bits 0..4 */
        }
    }
    for (uint32_t i = 0; sc->b && i < sc->nb; i++) g3.post += sc->b[i].kind == 3;   /* lotes de filtro de pantalla (clase 3: el post-proceso por ojo del renderer los sustituye) */
    g3.cam_head = r && r->active;
}
/* ------------------------------------------------------------------------------------------------ autovolcado
 * Tras construir cada lista (hilo de emulación): si la lista es anómala (VrStats: n_hw_cull, drop_incoh, quads no coherentes, triángulos con arista > 10 m) o la vista del mundo está tapada (triángulos pegados
 * al ojo que cubren > 60 % de la ventana, o un TILE negro opaco a pantalla completa tras el 3D) se pide un volcado simple con point=auto_<motivo> (decisión pura: vrdump.h autodump_decide; máx. 8 por sesión, 2 por motivo,
 * >= 5 s entre dos intentos). Carpeta: mv_vrhost_set_autodump_dir (sin ella no se hace nada). */
static pthread_mutex_t DMX;   /* definición con valor inicial más abajo */
static int dump_request_l(const char *dir, const char *dir2, const char *label);
static char autodump_dir[480];
static AutoDumpState ad_st = {0, -1.0, {0}};
void mv_vrhost_set_autodump_dir(const char *dir) {
    pthread_mutex_lock(&DMX);
    snprintf(autodump_dir, sizeof autodump_dir, "%s", dir ? dir : "");
    pthread_mutex_unlock(&DMX);
}
/* Fracción (suma de áreas proyectadas sin descontar solapes, tope 1) de la ventana del motor tapada por triángulos opacos con sus tres vértices a < 0,15 m del ojo del motor. */
static float autodump_near_cover(const VrScene *sc) {
    VrScreenWindow w = vr_scene_screen_window(&sc->filter);
    if (!w.ok || !sc->b || !sc->v) return 0.0f;
    float ax[3][3], l2[3], inv[3];   /* inv = 1 / norma de cada eje, fuera del bucle de triángulos */
    for (int c = 0; c < 3; c++) {
        for (int r = 0; r < 3; r++) ax[c][r] = sc->game_view[c * 4 + r];
        l2[c] = ax[c][0]*ax[c][0] + ax[c][1]*ax[c][1] + ax[c][2]*ax[c][2];
        if (!(l2[c] > 1e-12f)) return 0.0f;
        inv[c] = 1.0f / sqrtf(l2[c]);
    }
    float warea = (w.left + w.right) * (w.up + w.down), tot = 0.0f;
    for (uint32_t i = 0; i < sc->nb; i++) {
        if (sc->b[i].kind != 0) continue;
        for (uint32_t j = 0; j + 2 < sc->b[i].count; j += 3) {
            float q[3][2]; int ok = 1;
            for (int k = 0; k < 3 && ok; k++) {
                const float *p = sc->v[sc->b[i].first + j + k].pos;
                float d[3] = {p[0] - sc->eye_vr[0], p[1] - sc->eye_vr[1], p[2] - sc->eye_vr[2]};
                float vx = (d[0]*ax[0][0] + d[1]*ax[0][1] + d[2]*ax[0][2]) * inv[0], vy = (d[0]*ax[1][0] + d[1]*ax[1][1] + d[2]*ax[1][2]) * inv[1], vz = (d[0]*ax[2][0] + d[1]*ax[2][1] + d[2]*ax[2][2]) * inv[2];
                if (!(vz > 0.01f) || sqrtf(vx*vx + vy*vy + vz*vz) >= 0.15f) ok = 0;
                else {
                    q[k][0] = fminf(fmaxf(vx / vz, -w.left), w.right); q[k][1] = fminf(fmaxf(vy / vz, -w.up), w.down);   /* la pantalla del motor: x a la derecha, y hacia abajo */
                }
            }
            if (!ok) continue;
            tot += 0.5f * fabsf((q[1][0] - q[0][0]) * (q[2][1] - q[0][1]) - (q[2][0] - q[0][0]) * (q[1][1] - q[0][1]));
        }
    }
    float f = tot / warea;
    return f > 1.0f ? 1.0f : f;
}
/* TILE negro opaco que cubre >= 90 % del área mostrada y viene DESPUÉS de algún polígono 3D (no el borrado de inicio de lista). */
static int autodump_black_tile(const GpuDrawList *l) {
    int seen3d = 0;
    for (uint32_t i = 0; i < l->nops; i++) {
        const GpuOp *op = &l->ops[i];
        if (op->type == GPU_OP_POLY && op->v[0].vvalid) seen3d = 1;
        else if (seen3d && op->type == GPU_OP_RECT && !(op->flags & (GPU_F_TEXTURED | GPU_F_SEMI)) && !op->v[0].r && !op->v[0].g && !op->v[0].b &&
                 l->disp.w > 0 && l->disp.h > 0 && op->rw >= 0.9f * l->disp.w && op->rh >= 0.9f * l->disp.h) return 1;
    }
    return 0;
}
static void autodump_note(const VrScene *sc, const GpuDrawList *l) {
    pthread_mutex_lock(&DMX);
    int has_dir = autodump_dir[0] != 0;
    pthread_mutex_unlock(&DMX);
    if (ad_st.n >= AD_MAX_N || !has_dir) return;
    if (!sc->has_3d && !sc->stats.n_hw_cull) return;
    AutoDumpIn in; memset(&in, 0, sizeof in);
    in.world = view.kind == VRV_K_WORLD;
    in.n_hw_cull = sc->stats.n_hw_cull; in.drop_incoh = sc->stats.drop_incoh; in.quad_noncoh = sc->stats.n_quad_noncoh;
    for (uint32_t i = 0; sc->b && sc->v && i < sc->nb; i++) {   /* triángulos 3D con alguna arista > 10 m (espacio de salida, metros) */
        if (sc->b[i].kind > 1) continue;
        for (uint32_t j = 0; j + 2 < sc->b[i].count; j += 3) {
            const float *a = sc->v[sc->b[i].first + j].pos, *b = sc->v[sc->b[i].first + j + 1].pos, *c = sc->v[sc->b[i].first + j + 2].pos;
            const float *e[3][2] = {{a, b}, {b, c}, {c, a}};
            for (int k = 0; k < 3; k++) {
                float dx = e[k][0][0] - e[k][1][0], dy = e[k][0][1] - e[k][1][1], dz = e[k][0][2] - e[k][1][2];
                if (dx*dx + dy*dy + dz*dz > 100.0f) { in.long_edge++; break; }
            }
        }
    }
    if (in.world) { in.near_cover = autodump_near_cover(sc); in.black_cover = autodump_black_tile(l); }
    double t = now_s();
    int why = autodump_decide(&ad_st, &in, t);
    if (why == AD_NONE) return;
    uint32_t val = why == AD_NEAR_COVER ? (uint32_t)(in.near_cover * 100.0f + 0.5f) : why == AD_BLACK_COVER ? 1u : why == AD_HWCULL ? in.n_hw_cull : why == AD_INCOH ? in.drop_incoh : why == AD_QUAD ? in.quad_noncoh : in.long_edge;
    char dir[560], label[48];
    pthread_mutex_lock(&DMX);
    snprintf(dir, sizeof dir, "%s", autodump_dir);
    pthread_mutex_unlock(&DMX);
    snprintf(label, sizeof label, "auto_%s", autodump_name(why));
    size_t n = strlen(dir);
    snprintf(dir + n, sizeof dir - n, "/auto%02d_%s", ad_st.n + 1, autodump_name(why));
    int rc = dump_request_l(dir, NULL, label);
    fprintf(stderr, "[autodump] motivo=%s n=%u sesion=%d/%d carpeta=%s resultado=%s\n", autodump_name(why), val, ad_st.n + (rc == 0), AD_MAX_N, dir, rc == 0 ? "pedido" : "no pedido (ocupado)");
    ad_st.last_s = t;   /* también un intento fallido espera AD_GAP_S */
    if (rc == 0) { ad_st.n++; ad_st.by[why]++; }
}

/* ------------------------------------------------------------------------------------------------ procedencia (runtime/prov.h, vrprov.h)
 * Con la captura y la capa 2D encendidas (capture_set / frame_hook_body) el runtime anota quién escribió cada palabra de RAM y la captura etiqueta cada primitiva con el id de su ámbito
 * (GpuOp.prov). Aquí: registro [prov] una vez por segundo (hilo de emulación) y volcado automático la primera vez que aparece un actor de interés o la vista pasa a selector/pausa/códec. */
static VrProvAcc prov_acc;
static VrProvTrig prov_trig;
static uint64_t prov_log_ns;
static void prov_log(void) {
    uint64_t t = mono_ns();
    if (t - prov_log_ns < 1000000000ull) return;
    prov_log_ns = t;
    char b[4096];
    int n = vrprov_report(&prov_acc, vrprov_view(view.kind, view.reason), b, sizeof b);
    if (n > 0) fprintf(stderr, "%s ids=%u tabla_llena=%lu pila_llena=%lu\n", b, rt_prov_n, rt_prov_full, rt_prov_ovf);
}
static void prov_autodump(void) {
    if (!rt_prov_on) return;
    pthread_mutex_lock(&DMX);
    int has_dir = autodump_dir[0] != 0;
    pthread_mutex_unlock(&DMX);
    if (!has_dir) return;
    unsigned present = 0;
    if (bs.valid & BRIDGE_OK_ACTORS)
        for (int i = 0; i < bs.n_actors; i++) { int k = vrprov_actor_idx(bs.actors[i].name); if (k >= 0) present |= 1u << k; }
    int code = vrprov_pick(&prov_trig, present, vrprov_view(view.kind, view.reason));
    if (code < 0) return;
    char dir[560], label[48];
    pthread_mutex_lock(&DMX);
    snprintf(dir, sizeof dir, "%s", autodump_dir);
    pthread_mutex_unlock(&DMX);
    snprintf(label, sizeof label, "auto_prov_%s", vrprov_code_name(code));
    size_t n = strlen(dir);
    snprintf(dir + n, sizeof dir - n, "/prov_%s", vrprov_code_name(code));
    if (dump_request_l(dir, NULL, label) != 0) return;   /* ocupado: se reintenta en el siguiente fotograma */
    vrprov_done(&prov_trig, code);
    fprintf(stderr, "[prov] autodump motivo=%s carpeta=%s pedido\n", vrprov_code_name(code), dir);
}

static void gpu3d_log(void) {
    uint64_t t = mono_ns();
    if (t - g3.t_log < 1000000000ull) return;
    uint64_t t0 = g3.t_log;
    g3.t_log = t;
    if (!t0) return;   /* la primera llamada solo arranca el reloj */
    static GteHpStats hs_prev;   /* vértices de primitivas resueltos por dirección de memoria (hps.h) / por valor de SXY, del último segundo */
    GteHpStats hs;
    gte_hp_stats(&hs);
    fprintf(stderr, "[gpu3d] fotograma=%u listas=%u prims=%u 3D_con_hp=%u (triangulos=%u) plano2D=%u cerca_descartados=%u cerca_recortados=%u detras_descartados=%u 3D_sin_hp(respaldo)=%u post=%u drop_ambig=%u drop_partial=%u drop_incoh=%u reconstruidos=%u drop_huge=%u drop_nan=%u n_rect=%u n_rect_semi_tex=%u n_hw_cull=%u n_rect_hp=%u n_rect3d=%u n_billboard=%u | render=%s hp=%d vista=%s camara=%s hp_por_direccion=%u hp_por_valor=%u\n",
            (unsigned)hle_frame_count(), g3.lists, g3.prims, g3.p3d, g3.tris3d, g3.p2d, g3.near_drop, g3.near_clip, g3.behind_drop, g3.nohp, g3.post, g3.ambig, g3.nohp, g3.incoh, g3.recon, g3.huge, g3.nan, g3.rect, g3.rect_semi_tex, g3.hw_cull, g3.rect_hp, g3.rect3d, g3.billboard,
            native_mode() ? "nativo" : "gpu", gte_hp_enabled(), vrview_cine_type(view.kind), g3.cam_head ? "head" : "game", hs.res_addr - hs_prev.res_addr, hs.res_val - hs_prev.res_val);
    hs_prev = hs;
    {   /* [geom]: geometría 3D sospechosa de la lista de la GPU (una línea por segundo) */
        double ct = g3.cause_tris ? (double)g3.cause_tris : 1.0;
        fprintf(stderr, "[geom] listas=%u grupos(H,ofx,ofy)_max=%u prims_en_dominante=%.1f%% (%u de %u) cuadrilateros=%u aceptados_sin_coplanaridad=%.1f%% (%u) tris=%u causa: AMBIG=%.1f%% SAT=%.1f%% BEHIND=%.1f%% RECON=%.1f%% DERIVED=%.1f%% | pares_coplanares_corregidos=%u sin_revisar=%u\n",
                g3.lists, g3.groups_max, g3.ref_total ? 100.0 * g3.ref_n / g3.ref_total : 0.0, g3.ref_n, g3.ref_total,
                g3.quad3d, g3.quad3d ? 100.0 * g3.quad_noncoh / g3.quad3d : 0.0, g3.quad_noncoh, g3.cause_tris,
                100.0 * g3.tri_cause[0] / ct, 100.0 * g3.tri_cause[1] / ct, 100.0 * g3.tri_cause[2] / ct, 100.0 * g3.tri_cause[3] / ct, 100.0 * g3.tri_cause[4] / ct,
                g3.coplanar, g3.coplanar_skip);
    }
    int ch = g3.cam_head;
    memset(&g3, 0, sizeof g3);
    g3.cam_head = ch; g3.t_log = t;
}

/* Caja (BRIDGE_PS_BOX): la máscara de visión de la última lista (vrhost_build) y el estado anterior, para el registro [caja] al entrar/salir. */
static int box_mask_on, box_mask_rect[4], box_prev, box_prev_mask;
static void build_one(const GpuDrawList *l, int si) {
    hx_cov = 0;   /* la lista nueva aún no tiene extracción de efectos (hx_extract) */
    /* Los polígonos de una lista se transformaron con la cámara construida un frame del motor ANTES de que el VBlank lea la
     * RAM: la matriz de vista del puente puede ser ya la siguiente. El registro de cámara de la lista (vrcam) trae la correcta. */
    int has3d = 0;
    const VrCamRecord *r = (bs.valid & BRIDGE_OK_EYE) ? list_record(l, &has3d) : NULL;
    int differs = 0;
    int32_t save_pos[3];
    int16_t save_axes[3][3];
    VrSceneOpts o = sopts;
    if (!native_mode()) { o.eye_near_m = GPU3D_NEAR_EYE_M; o.line3d_px = 1.0f; o.coplanar_m = 0.005f; o.rect3d = 1; o.billboard = 1; }   /* modo gpu: nada pegado al ojo, recorte contra el plano cercano; líneas del mundo en 3D; z-fight de coplanares, sprites con vista de la GTE como carteles 3D y resplandores orientados al ojo solo aquí (ver vr_scene.h) */
    if (vrhost_drop_world2d(native_mode(), view.kind, view.reason)) o.drop_world2d = 1;   /* el nativo dibuja el mundo y las cinemáticas del motor: los polígonos sin vista dentro del pase 3D no son HUD y no van al panel 2D (vr_scene.h); no en UI/vídeo ni con la ventana del selector o la pausa encima (sus menús son 2D) */
    if (rt_prov_on) {   /* procedencia: aprende la ventana del selector / pausa y clasifica las primitivas del panel (vr_prov.h); sin ella todo es UNKNOWN = HUD */
        int pst = view.kind != VRV_K_WORLD ? VPC_ST_OTHER : view.reason == VRV_R_WORLD ? VPC_ST_CLOSED : view.reason == VRV_R_SELECTOR || view.reason == VRV_R_PAUSE_WORLD ? VPC_ST_OPEN : VPC_ST_OTHER;
        vpc_learn_note(&prov_learn, l, pst);
        o.op_class = vpc_op_class_cb; o.op_class_ctx = &prov_learn;
        prov_learn.covered = prov_covered;   /* WORLD / FX_SCREEN solo con garantía de que el nativo lo dibuja en esta lista */
    }
    if (!native_mode() && view.kind == VRV_K_ENGINE) { o.keep_late_clear = 1; o.veil_as_2d = 1; }   /* pantalla 3D del motor (gpu): el 2D se compone dentro del estéreo; el relleno opaco tras el 3D (créditos) y los velos se pintan como 2D */
    int cine_cut = 0;
    {   /* Cinemática del motor (ni mundo ni vídeo/2D puros): la cámara es la del guion, nunca la de la cabeza. Planos y origen fijo por plano. */
        int rs = view.reason;
        int cine_reason = rs == VRV_R_NO_GAME || rs == VRV_R_CINEMATIC || rs == VRV_R_LOST_CONTROL || rs == VRV_R_DEAD;
        if (!cine_reason || (r && r->active)) vrcine_camera(&cine, 0, bs.eye_pos, (const float[9]){0});
        else if (r) cine_cut = vrcine_camera(&cine, 1, r->eye, r->M);
    }
    int cine_cand = r && !r->active && cine.have;
    if (cine_cand) {   /* origen fijo del plano: el mundo VR no se desplaza con el personaje dentro del plano */
        o.origin_mode = 1;
        for (int i = 0; i < 3; i++) o.origin_game[i] = (float)cine.origin[i];
    }
    memcpy(save_pos, bs.eye_pos, sizeof save_pos);
    memcpy(save_axes, bs.eye_axes, sizeof save_axes);
    if (r) {
        for (int i = 0; i < 3; i++) {
            if (bs.eye_pos[i] != r->eye[i]) differs = 1;
            bs.eye_pos[i] = r->eye[i];
            for (int j = 0; j < 3; j++) {
                int16_t v = (int16_t)lrintf(r->M[3 * i + j] * 4096.0f);
                if (bs.eye_axes[i][j] != v) differs = 1;
                bs.eye_axes[i][j] = v;
            }
        }
        if (r->active) {   /* origen del mundo VR = bajo el ojo VR de ese frame (no la posición del jugador de un frame después) */
            float org[3];
            vr_scene_player_origin(&bs, &o, org);
            o.origin_mode = 1;
            o.origin_game[0] = (float)r->eye[0]; o.origin_game[1] = org[1]; o.origin_game[2] = (float)r->eye[2];
#ifndef MV_NO_USER_MAP     /* (solo pruebas A/B: sin la correspondencia mundo -> usuario, como antes de §49) */
            vrcam_scene_user_map(&o, r, org[1]);
#endif
        }
    }
    /* Radar de muñeca: se separa del HUD (la escena del HUD se ensambla con la lista SIN sus primitivas) y su propia escena se
     * ensambla y copia ANTES de vr_scene_build (que reutiliza sus búferes). Sin marcas del radar (el juego lo oculta/apaga) no hay nada. */
    VrRadarSplit rsplit;
    VrRadarScene rscene;
    memset(&rscene, 0, sizeof rscene);
    const GpuDrawList *hl = l;
    if (vl2a_split() && vrradar_split(l, &rsplit) == 0 && rsplit.present) {   /* sin troceo (defecto, vl2a_set_split) el radar del juego se queda en la capa 2D completa: no hay escena de radar ni quad en el brazo */
        hl = rsplit.hud;
        if (vrradar_build(&rsplit, &rscene) < 0) memset(&rscene, 0, sizeof rscene);
    }
    uint64_t t0 = mono_ns();
    VrScene sc;
    int rc = vr_scene_build(hl, &bs, &o, &sc);
    /* Solo recubrimientos (fundidos/destellos) sin tiras ni copias de pantalla (menús, vídeos, transiciones): el post-proceso solo cubre la vista 3D y no el
     * panel 2D, así que se reconstruye sin filtro y el fundido va al panel como antes. Con tiras (efecto submarino) se conserva el filtro. */
    /* Las gafas (líneas de exploración) y la máscara de visión (máscara de gas, caja) no son fundidos: se conserva el filtro (clase 3b) para que su tinte, sus líneas y su marco
     * cubran toda la vista; también un destello/fundido durante la partida (hay 3D y el jugador está en escena) se aplica sobre la vista en vez de en el panel. */
    int cover_n = sc.filter.active ? sc.filter.n_covers : 0;
    int keep3b = vrhost_keep_filter(native_mode(), cine_cand, (native_mode() && last_scene3d) || sc.stats.n_tris3d >= 12, sc.has_3d, sc.world_valid, sc.filter.n_scan, sc.filter.frame_on, sc.filter.n_strips);   /* cinemática del motor en nativo: fundidos y destellos van al panel 2D de la pantalla 3D (el post-proceso por ojo no se aplica allí); en gpu el mundo y la cinemática conservan el filtro y el renderer lo aplica con su post-proceso por ojo (RenderRG.planPost); gafas y marco de máscara fuera del mundo van al panel */
    if (rc == 0 && o.screen_filter && sc.filter.active && sc.filter.n_strips == 0 && sc.filter.n_polys == 0 && !keep3b) {
        VrSceneOpts o2 = o;
        o2.screen_filter = 0;
        rc = vr_scene_build(hl, &bs, &o2, &sc);
    }
    sc.filter.frame_in_box = vrhost_box_mask(native_mode(), bs.in_box, sc.filter.frame_on);   /* la máscara de visión (frame) solo se aplica en el nativo con el jugador en la caja */
    {   /* diagnóstico de la caja: ¿hay máscara de visión y qué abertura (celdas con resta < 128: bounding box)? */
        box_mask_on = rc == 0 && sc.filter.frame_on && sc.filter.frame && sc.filter.frame_w == VR_FRAME_W && sc.filter.frame_h == VR_FRAME_H;
        box_mask_rect[0] = box_mask_rect[1] = 1 << 20; box_mask_rect[2] = box_mask_rect[3] = -1;
        if (box_mask_on) for (int y = 0; y < VR_FRAME_H; y++) for (int x = 0; x < VR_FRAME_W; x++) if (sc.filter.frame[y * VR_FRAME_W + x] < 128) {
            if (x < box_mask_rect[0]) box_mask_rect[0] = x;
            if (y < box_mask_rect[1]) box_mask_rect[1] = y;
            if (x > box_mask_rect[2]) box_mask_rect[2] = x;
            if (y > box_mask_rect[3]) box_mask_rect[3] = y;
        }
    }
    double ms = (double)(mono_ns() - t0) * 1e-6;
    if (rc == 0) { gpu3d_note(&sc, r); autodump_note(&sc, l); }
    if (rc == 0 && scene_tap) scene_tap(hl, &bs, &sc, l->host_frame);

    if (rc == 0 && list_tap) list_tap(l, hl, &sc, &o, &bs);
    if (rc == 0) {
        last_scene3d = r && !r->active && sc.stats.n_tris3d >= 12;
        last_cover = cover_n;
        /* vídeo pregrabado: sin 3D y con subidas a VRAM que cubren (juntas) al menos el 60 % del área mostrada. Un FMV real sube tiras de 16 px (una por columna de
         * macrobloques), no un solo rectángulo: se suma el área de cada subida recortada al rectángulo mostrado (en medias palabras: 24 bits = 3/2 por píxel). */
        int vid = 0;
        if (!sc.has_3d) vid = list_uploads_cover_display(l);
        last_video = vid;
    }
    sc.cine_active = view.kind == VRV_K_ENGINE;
    sc.cine_shot = cine.shot;
    sc.cine_cut = cine_cut;
    sc.cine_cover = cover_n;
    memcpy(bs.eye_pos, save_pos, sizeof save_pos);
    memcpy(bs.eye_axes, save_axes, sizeof save_axes);

    {   /* efectos por procedencia (viñeta, bandas, prismáticos): solo en la vista mundo; en las demás el 2D ya lo lleva todo */
        VrFx fxn;
        vpc_fx_extract(&prov_learn, l, r ? r->screen : 0, &fxn);
        if (view.kind != VRV_K_WORLD) { memset(&fxn, 0, sizeof fxn); fxn.scope_zoom = 1.0f; }
        vpc_fx_hold(&fx_hold, &fxn);
    }
    /* Capa 2D fiel: se encola (copia) para el hilo trabajador. Con escena fallida solo se aplican los eventos de VRAM (la VRAM espejo debe verlos todos). En vista de cine (UI / vídeo)
     * la capa es el fotograma ENTERO; en las demás, solo las primitivas que vr_scene mandó al panel. Vista mundo (VL_PANEL): el panel se reparte por clase de procedencia en 3 capas
     * (HUD / ventana / pantalla completa; FX_* y SCOPE_FRAME no van al 2D); las capas 1 y 2 solo se encolan si tienen primitivas o las tenían (para publicar el vacío). */
    if (atomic_load(&l2d_enabled)) {
        if (!l2as[0]) l2as[0] = vl2a_new();
        if (l2as[0]) {
            vl2a_options_native(l2as[0]);   /* raster 1 fijo (sin x4 ni escala adaptativa): el escalado y el filtro xBR los hace la GPU del renderer (XRShaders.metal xr_xbr3); l2d_target / l2d_smooth / MV_HUD2D_SCALE quedan sin efecto */
            int l2_full = view.kind == VRV_K_UI || view.kind == VRV_K_VIDEO ? VL_FULL : view.kind == VRV_K_ENGINE ? VL_PANEL_VERIFIED : VL_PANEL;
            int vtag = (view.kind << 8) | (view.reason & 255);
            const uint8_t *od = rc == 0 && sc.op_dest && sc.n_op_dest >= hl->nops ? sc.op_dest : NULL;
            uint32_t lc[VPC_L_N] = {0, 0, 0};
            int routed = od && l2_full == VL_PANEL && sc.op_cls;
            if (od && hl->nops > l2_opd_cap) {
                size_t nc = (size_t)hl->nops + 256;
                uint8_t *q[VPC_L_N]; int okm = 1;
                for (int k = 0; k < VPC_L_N; k++) { q[k] = (uint8_t *)realloc(l2_opd[k], nc); if (!q[k]) okm = 0; else l2_opd[k] = q[k]; }
                if (okm) l2_opd_cap = nc; else routed = 0;
            }
            if (routed && hl->nops <= l2_opd_cap) {
                vpc_split_dest(od, sc.op_cls, hl->nops, l2_opd, lc);
                { uint32_t t[VPC_N]; vpc_tally(od, sc.op_cls, hl->nops, t); for (int k = 0; k < VPC_N; k++) pc_cls[k] += t[k]; pc_lists++; }
            } else routed = 0;
            vl2a_set_view(l2as[0], vtag);
            vl2a_submit(l2as[0], hl, routed ? l2_opd[VPC_L_HUD] : od, l2_full, 0);
            for (int k = 1; k < VPC_L_N; k++) {
                int has = routed ? lc[k] > 0 : 0;
                if (!(has || l2_busy[k] || !hl->vram_start)) continue;   /* sin primitivas ahora ni antes: nada que publicar (cada lista trae su VRAM de partida: la capa no se desincroniza) */
                if (!l2as[k]) l2as[k] = vl2a_new();
                if (!l2as[k] || hl->nops > l2_opd_cap) continue;
                vl2a_options_native(l2as[k]);
                vl2a_set_view(l2as[k], vtag);
                if (!routed) memset(l2_opd[k], 0, hl->nops);   /* otras vistas / escena fallida: capa vacía (se publica una vez y no vuelve a encolarse) */
                if (vl2a_submit(l2as[k], hl, l2_opd[k], VL_PANEL, 0)) l2_busy[k] = (uint8_t)has;   /* sin reparto: op_dest a cero (capa vaciada), no NULL (NULL = solo eventos: la capa no se borraría) */
            }
            { static uint64_t l2_log_ns; uint64_t nn = mono_ns();   /* una línea por segundo */
              if (nn - l2_log_ns >= 1000000000ull) { char lb[1024]; l2_log_ns = nn; if (vl2a_report(l2as[0], lb, sizeof lb) > 0) fprintf(stderr, "%s\n", lb); } }
            if (rt_prov_on && mono_ns() - pc_log_ns >= 1000000000ull) {   /* [prov2d]: clases del panel en el último segundo; UNKNOWN va al HUD */
                pc_log_ns = mono_ns();
                if (pc_lists) fprintf(stderr, "[prov2d] vista=%s listas=%u HUD_WORLD=%u WINDOW=%u FULL=%u FX_VIGNETTE=%u FX_LETTERBOX=%u SCOPE_FRAME=%u SCOPE_TEXT=%u UNKNOWN=%u FX_SCREEN=%u WORLD=%u | ventanas_aprendidas=%u descartadas=%u | fx: vineta=%.2f radio=%.2f bandas=%.2f mira=%d zoom=%.2f\n",
                    vrprov_view_name(vrprov_view(view.kind, view.reason)), pc_lists, pc_cls[VPC_HUD_WORLD], pc_cls[VPC_WINDOW], pc_cls[VPC_FULL], pc_cls[VPC_FX_VIGNETTE], pc_cls[VPC_FX_LETTERBOX], pc_cls[VPC_SCOPE_FRAME], pc_cls[VPC_SCOPE_TEXT], pc_cls[VPC_UNKNOWN], pc_cls[VPC_FX_SCREEN], pc_cls[VPC_WORLD],
                    prov_learn.n_learned, prov_learn.n_demoted, (double)fx_hold.fx.vignette, (double)fx_hold.fx.vign_radius, (double)fx_hold.fx.letterbox, (int)fx_hold.fx.scope_on, (double)fx_hold.fx.scope_zoom);
                memset(pc_cls, 0, sizeof pc_cls); pc_lists = 0;
            }
        }
    }
    sm_sample(&slot[si].smp, &sc, r);   /* (r = registro de cámara de la lista; antes de fill_slot: sc.v/b son memoria interna de vr_scene) */
    int native_now = rc == 0 && native_for_scene(r, &sc, cine_cand);
    g_opkey = native_now ? fx_classify_list(hl, r) : NULL;
    int ok = rc == 0 && fill_slot(&slot[si], hl, &sc) == 0 && fill_radar(&slot[si], &rscene, hl->nops) == 0;
    g_opkey = NULL;
    if (ok && native_now) mark_3d(&slot[si]);   /* solo marca: la lista de la GPU queda entera (casco de colisión de la cabeza, modo gpu, caída del render nativo) */
    memset(&slot[si].hcmap, 0, sizeof slot[si].hcmap);
    if (ok && hc_on && r && r->active && sc.user_space && sc.world_valid) {
        VrHcMap hm;
        if (vrhc_map_from_record(&hm, r)) {
            float jt[16][3];
            int nj = native_mode() ? 0 : vrcam_model_joints(emu_ram, jt, 16);
            pthread_mutex_lock(&HCMX);
            float dx = hc_last_eye[0] - hm.eye[0], dz = hc_last_eye[2] - hm.eye[2];
            if (hc_have_eye && dx * dx + dz * dz > (4.0f * hm.upm) * (4.0f * hm.upm)) vrhc_reset();   /* salto (otra zona): sin fantasmas del mundo anterior */
            memcpy(hc_last_eye, hm.eye, sizeof hc_last_eye); hc_have_eye = 1;
            /* Modo nativo: la colisión de la cabeza se alimenta SOLO de la escena nativa (ns_solid_tap) y aquí solo se fija el mapa usuario <-> mundo. Modo gpu: no hay captura nativa
             * (ns_gate), así que el escenario sale de la lista de la GPU (lo que ve la cámara). */
            if (!native_mode()) vrhc_feed_scene(&hm, &sc, (const float (*)[3])jt, nj);
            pthread_mutex_unlock(&HCMX);
            slot[si].hcmap = hm;
        }
    }
    pthread_mutex_lock(&MX);
    if (ok) {
        sstate[si] = S_QUEUED;
        queue[(q_head + q_n) % NQ] = si;
        q_n++;
        stats.built++;
        if (sc.has_3d) stats.has3d++;
        if (sc.world_valid) stats.world_valid++;
        if (r) { stats.cam_rec_found++; if (differs) stats.cam_rec_differs++; }
        else if (has3d && (bs.valid & BRIDGE_OK_EYE)) stats.cam_rec_missing++;
        stats.build_ms_avg += (ms - stats.build_ms_avg) / (double)stats.built;
        if (ms > stats.build_ms_max) stats.build_ms_max = ms;
    } else {
        sstate[si] = S_FREE;   /* no se entrega: el consumidor perdería eventos de VRAM; la siguiente resincroniza abajo */
    }
    pthread_mutex_unlock(&MX);
    if (!ok) {
        /* hueco en la secuencia de eventos de VRAM: forzar nueva instantánea completa */
        fprintf(stderr, "[vrhost] sin memoria o error al ensamblar la escena (rc=%d): se resincroniza la VRAM\n", rc);
        drop_queue();
        gpu_capture_enable(0);
        vrradar_reset();                /* la numeración de seq de la captura vuelve a 0: los rangos de marcas viejos ya no valen */
        cap_on = 0;
    }
}

/* ------------------------------------------------------------------------------------------------ gancho de frame */
static void capture_set(int on) {
    if (on == cap_on) return;
    if (on) {
        gpu_capture_set_prov(rt_prov_mem, rt_prov_tab, &rt_prov_n);   /* GpuOp.prov / GpuDrawList.prov (se rellenan con rt_prov_set encendido, ver frame_hook_body) */
        gpu_capture_hiprec(1);
        gpu_capture_vram_start(atomic_load(&l2d_enabled));   /* la capa 2D parte de la VRAM REAL de cada lista, no de un espejo por eventos */
        gpu_capture_enable(1);          /* la primera lista trae vram_init */
        vrradar_reset();                /* idem: seq vuelve a 0 */
        pthread_mutex_lock(&MX); stats.cap_resyncs++; pthread_mutex_unlock(&MX);
    } else {
        gpu_capture_enable(0);          /* libera y vacía la cola de captura */
        vrradar_reset();
        gpu_capture_hiprec(0);
        drop_queue();
    }
    cap_on = on;
}

/* Una línea al entrar/salir de la caja o al cambiar la máscara dentro de ella: banderas del jugador, cámara 1.ª persona del motor, bandera de oculto del objeto del jugador (OF_HIDDEN, 0x80 en +0x28) y máscara. */
static void box_diag(int mode, int reason, int native_ok) {
    int in = bs.in_box ? 1 : 0;
    if (in == box_prev && !(in && box_mask_on != box_prev_mask)) return;
    uint32_t key = ns_player_key(emu_ram), fl = 0;
    if (key >= 0x80000000u && key < 0x80200000u) memcpy(&fl, emu_ram + (key & (EMU_RAM_SIZE - 1u)) + 0x28u, 4);
    fprintf(stderr, "[caja] %s status=%08x cam1p=%d camtipo=%d primera_persona=%d jugador=%08x oculto_OF_HIDDEN=%d vista=%s motivo=%s nativo_apto=%d mascara=%s abertura_celdas=(%d,%d)-(%d,%d) de %dx%d\n",
            in ? (box_prev ? "mascara" : "entra") : "sale", (unsigned)bs.player_status, (int)bs.cam_first_person, (int)bs.cam_type, (int)bs.first_person, (unsigned)key, (fl & 0x80u) != 0,
            mode == VRV_WORLD ? "mundo" : "cine", vrview_reason_name(reason), native_ok, box_mask_on ? "si" : "no",
            box_mask_rect[0], box_mask_rect[1], box_mask_rect[2], box_mask_rect[3], VR_FRAME_W, VR_FRAME_H);
    box_prev = in; box_prev_mask = box_mask_on;
}

static void publish_state(void) {
    MvVrGameState g;
    memset(&g, 0, sizeof g);
    int core = (bs.valid & BRIDGE_OK_ALL_CORE) == BRIDGE_OK_ALL_CORE && bs.has_player;
    g.game = core;
    g.in_control = core && bs.in_control && !(((bs.valid & BRIDGE_OK_LIFE) && bs.life_max > 0 && bs.life <= 0));
    g.menu = !(core && bs.in_control);
    g.posture = core && bs.stance >= 0 && bs.stance <= 2 ? bs.stance : -1;
    g.cam_active = vrcam_active();
    g.first_person = bs.first_person;
    g.weapon = bs.weapon;
    g.life = bs.life; g.life_max = bs.life_max;
    {
        VrViewIn vin;
        memset(&vin, 0, sizeof vin);
        vin.game = (uint8_t)core;
        vin.in_control = (uint8_t)g.in_control;
        vin.cam_active = (uint8_t)(g.cam_active != 0);
        vin.cinematic = core && bs.cinematic; vin.comms = core && bs.comms; vin.paused = core && bs.paused;
        vin.menu_open = core && bs.menu_open; vin.stopped = core && bs.stopped;
        vin.dead = (uint8_t)(core && (((bs.valid & BRIDGE_OK_LIFE) && bs.life_max > 0 && bs.life <= 0) || (bs.player_status & BRIDGE_PS_GAME_OVER) || (bs.game_status & BRIDGE_GS_GAME_OVER)));
        vin.scene3d = (uint8_t)last_scene3d; vin.video = (uint8_t)last_video;
        vin.native_ok = (uint8_t)(ns_hook_native_ready(ns_now_ns(), 250000000ull) != 0);   /* escena nativa apta y de menos de 250 ms (misma antigüedad que ns_native_decide) */
        {   /* FMV: el decodificador de vídeo entregó píxeles en los últimos 12 VBlank (~0,24 s; un FMV da un fotograma cada 3-4) */
            uint32_t vb = (uint32_t)hle_frame_count(), tot = mdec_out_words_total();
            if (tot - mdec_seen_total >= 64u) mdec_last_vb = vb ? vb : 1u;
            mdec_seen_total = tot;
            vin.mdec = mdec_last_vb != 0 && vb - mdec_last_vb <= 12u;
        }
        g.view_mode = vrview_step(&view, &vin);
        box_diag(view.mode, view.reason, vin.native_ok);
        if (view.changed) {     /* cada cambio de modo / clase / motivo de la vista queda en el registro con su causa (banderas del puente incluidas) */
            char vb[512];
            vrview_describe(&view, &vin, vb, sizeof vb);
            fprintf(stderr, "%s | vb=%u modoBruto=%d render=%s | gs=%08x ps=%08x postura=%d 1p=%d cam1p=%d camtipo=%d\n", vb, (unsigned)hle_frame_count(), view.want, native_mode() ? "nativo" : "gpu", (unsigned)bs.game_status, (unsigned)bs.player_status, (int)bs.stance, (int)bs.first_person, (int)bs.cam_first_person, (int)bs.cam_type);
        }
        if (view.changed & VRV_CH_KIND) {   /* qué se le enseña al usuario en cine (fmv / motor / respaldo) y por qué */
            char cb[640];
            vrview_cine_describe(&view, &vin, cb, sizeof cb);
            fprintf(stderr, "%s | vb=%u\n", cb, (unsigned)hle_frame_count());
            if (view.kind == VRV_K_ENGINE && view.reason == VRV_R_NO_GAME)   /* 3D clasificado como cinemática solo porque no hay partida (no por banderas de cinemática): puede ser la caja 4:3 de un título/carga con 3D */
                fprintf(stderr, "[vista] engine-por-sin-partida escena=%d ps=%08x gs=%08x vb=%u\n", (int)vin.scene3d, (unsigned)bs.player_status, (unsigned)bs.game_status, (unsigned)hle_frame_count());
        }
        g.view_kind = view.kind;
        g.cine_shot = cine.shot; g.cine_cuts = cine.cuts; g.cine_cover = last_cover;
        g.view_reason = view.reason;
        g.cinematic = vin.cinematic; g.comms = vin.comms; g.paused = vin.paused; g.menu_open = vin.menu_open; g.stopped = vin.stopped;
        g.item = core ? bs.item : -1;
        g.alert_mode = core ? bs.alert_mode : 0;
        g.render_req = atomic_load(&render_req); g.native_avail = mv_vrhost_native_available();
        g.fx = fx_hold.fx;
        g.ammo = -1; g.ammo_max = -1;
        if (core && bs.weapon >= 0 && bs.weapon < BRIDGE_N_WEAPONS) { g.ammo = bs.weapon_ammo[bs.weapon]; g.ammo_max = bs.weapon_max[bs.weapon]; }
    }
    pthread_mutex_lock(&MX);
    memcpy(&pub_bs, &bs, sizeof bs);
    pub_valid = 1;
    pub_gs = g;
    pthread_mutex_unlock(&MX);
}

static void (*test_cb)(void *cpu, uint32_t frame);
void mv_vrhost_set_test_hook(void (*cb)(void *cpu, uint32_t frame)) { test_cb = cb; }
void mv_vrhost_set_scene_tap(void (*cb)(const GpuDrawList *, const BridgeState *, const VrScene *, uint32_t)) { scene_tap = cb; }

void mv_vrhost_set_list_tap(MvVrListTap cb) { list_tap = cb; }

/* Distancia (mm del motor) de un punto del mundo a la superficie estática más cercana (golpe en la pared); -1 si no hay geometría a menos de max_mm. */
float mv_vrhost_wall_distance(const float p_mm[3], float max_mm) {
    if (!hc_on) return -1.0f;
    pthread_mutex_lock(&HCMX);
    float d = vrhc_nearest_world(p_mm, max_mm);
    pthread_mutex_unlock(&HCMX);
    return d;
}

float mv_vrhost_ray_distance(const float o_mm[3], const float d[3], float max_mm) {
    if (!hc_on) return -1.0f;
    pthread_mutex_lock(&HCMX);
    float t = vrhc_raycast_world(o_mm, d, max_mm);
    pthread_mutex_unlock(&HCMX);
    return t;
}

/* Geometría y cámaras para la resolución de impactos de las balas (vr/gameplay/vr_hit.h). Hilo de emulación. */
static float hit_wall(const uint8_t *ram, const float o[3], const float d[3], float max) {     /* la misma geometría que corta el láser del arma (vrgame.c) */
    float t = vrhzd_raycast(ram, o, d, max, VRHZD_SKIP_SIGHT), t2 = mv_vrhost_ray_distance(o, d, t >= 0.0f ? t : max);
    return t2 >= 0.0f ? t2 : t;
}
static int hit_camera(uint8_t *ram, const float o[3], const float d[3], float max, float *t, uint32_t *target) {
    (void)ram;
    const VrcTable *tb = vrc_table();
    int i = tb ? vrc_raycast(tb, o, d, max, t, NULL) : -1;
    if (i < 0) return 0;
    *target = tb->cam[i].target;
    return 1;
}
static void hit_camera_hit(uint8_t *ram, uint32_t target, const int16_t pt[3]) {               /* el juego no la daña por sí mismo: un impacto de bala de la política VR (2 la rompen) */
    const VrcTable *tb = vrc_table();
    for (int i = 0; tb && i < tb->n; i++) if (tb->cam[i].target == target) { vr_cameras_hit(ram, tb->cam[i].actor, pt, 64); return; }
}

static float ai_corr[3];      /* último recorte de la cabeza contra el escenario (m, espacio del usuario; protegido por MX) */
static uint64_t ai_corr_t;
/* Desplazamiento de la cabeza VR respecto del ojo del personaje, para la IA (vr/ai): la cabeza dibujada (física + recorte) en el mundo del motor. */
static void ai_feed_head(void) {
    const VrCamRecord *r = vrcam_last_record();
    float hm[16], corr[3] = {0, 0, 0}, off[3];
    if (!vrcam_active() || !r || !r->active || !mv_vr_get_head(hm, NULL)) { vrai_feed_head(NULL); return; }
    for (int i = 12; i < 15; i++) if (!isfinite(hm[i])) { vrai_feed_head(NULL); return; }
    uint64_t now = mono_ns();
    pthread_mutex_lock(&MX); if (hc_on && ai_corr_t && now - ai_corr_t < 300000000ull) memcpy(corr, ai_corr, sizeof corr); pthread_mutex_unlock(&MX);
    vrp_head_offset(r->theta_play_deg, r->units_per_m, r->p_ref, &hm[12], corr, off);
    vrai_feed_head(off);
}

/* Límite de la cabeza (hilo de render). head = pose física recentrada (16 floats, columnas); corr = desplazamiento a sumar a la cabeza para dibujar. */
int mv_vrhost_head_limit(const float head[16], float dt, float corr[3], float *fade) {
    VrHcMap m;
    corr[0] = corr[1] = corr[2] = 0.0f; if (fade) *fade = 0.0f;
    if (!hc_on || !head) return 0;
    pthread_mutex_lock(&MX); m = hc_disp; pthread_mutex_unlock(&MX);
    VrHcOut o;
    pthread_mutex_lock(&HCMX);
    int r = vrhc_query(&m, (const float[3]){head[12], head[13], head[14]}, dt, &o);
    hc_last = o;
    pthread_mutex_unlock(&HCMX);
    pthread_mutex_lock(&MX); memcpy(ai_corr, o.corr, sizeof ai_corr); ai_corr_t = mono_ns(); pthread_mutex_unlock(&MX);   /* la IA ve la cabeza ya recortada contra el escenario */
    memcpy(corr, o.corr, sizeof o.corr);
    if (fade) *fade = o.fade;
    return r;
}
/* Capa 2D fiel (vr_layer2d.h): suavizado de las texturas 2D (0 original, 1 nítido, 2 suave). Cualquier hilo. */
void mv_vrhost_set_hud2d_smooth(int mode) { atomic_store(&l2d_smooth, mode < 0 ? 0 : mode > 2 ? 2 : mode); }
int mv_vrhost_hud2d_smooth(void) { return atomic_load(&l2d_smooth); }
void mv_vrhost_hud2d_stats(int *scale, double *ms) { double sm; uint32_t lag; if (l2as[0]) vl2a_stats(l2as[0], scale, ms, &sm, &lag); else { if (scale) *scale = 0; if (ms) *ms = 0; } }
int mv_vrhost_hud2d_acquire_l(int layer, const uint8_t **px, int *w, int *h, int *full, uint32_t *seq) { return layer >= 0 && layer < VPC_L_N && l2as[layer] ? vl2a_acquire(l2as[layer], px, w, h, full, seq) : -1; }
void mv_vrhost_hud2d_release_l(int layer) { if (layer >= 0 && layer < VPC_L_N && l2as[layer]) vl2a_release(l2as[layer]); }
int mv_vrhost_hud2d_layer_l(int layer, int *mode, int *view) { return layer >= 0 && layer < VPC_L_N && l2as[layer] ? vl2a_acquire_info(l2as[layer], mode, view) : 0; }
int mv_vrhost_hud2d_acquire(const uint8_t **px, int *w, int *h, int *full, uint32_t *seq) { return mv_vrhost_hud2d_acquire_l(0, px, w, h, full, seq); }
void mv_vrhost_hud2d_release(void) { mv_vrhost_hud2d_release_l(0); }
int mv_vrhost_hud2d_layer(int *mode, int *view) { return mv_vrhost_hud2d_layer_l(0, mode, view); }
int mv_vrhost_hud2d_accept(int mode, int layer_view, int cur_view, int overlay) { return vl2a_layer_accept(mode, layer_view, cur_view, overlay); }
void mv_vrhost_set_l2d_split(int on) { vl2a_set_split(on); }
int mv_vrhost_l2d_split(void) { return vl2a_split(); }
void mv_vrhost_set_headcol(int enable, float radius_m) {
    pthread_mutex_lock(&HCMX);
    VrHcCfg c = *vrhc_config();
    if (!c.radius_m) vrhc_config_default(&c);
    c.enabled = enable != 0;
    if (radius_m >= 0.03f && radius_m <= 0.5f) c.radius_m = radius_m;
    vrhc_init(&c);
    pthread_mutex_unlock(&HCMX);
    hc_on = enable != 0;
}

#ifdef MV_NOGEN
/* Sin recorte en las overlays de zona (vr/camera, bitácora): algunas llevan su propia copia de la visibilidad de objetos y por pieza.
 * Se muestrea la ventana de código de overlays (160 palabras); con la muestra estable 12 fotogramas se escanea y se parchean los sitios con
 * rt_patch_code (palabra original validada; se deshace al cambiar de overlay). Coste: 160 lecturas por fotograma y un escaneo por zona. */
#define OVP_LO 0x800C4380u
#define OVP_HI 0x80100000u
#define OVP_MAX 24
static int ovp_on = 0, ovp_map = 0;   /* ovp_map: parchear también la máscara de mapa de las overlays (bit 0 de MV_NOCULL) */
static uint32_t ovp_sample, ovp_done = 1; static unsigned ovp_stable;
static int ovp_ids[OVP_MAX], ovp_n;
static void ovp_release(void) { for (int i = 0; i < ovp_n; i++) rt_unpatch_code(ovp_ids[i]); ovp_n = 0; }
static void ovp_frame(void) {
    uint32_t h = 2166136261u;
    for (unsigned i = 0; i < 160; i++) {
        uint32_t a = OVP_LO + (((OVP_HI - OVP_LO) / 160u) & ~3u) * i, v;
        memcpy(&v, emu_ram + (a & 0x1FFFFFu), 4);
        h = (h ^ v) * 16777619u;
    }
    if (h != ovp_sample) { ovp_sample = h; ovp_stable = 0; return; }
    if (ovp_stable < 1000) ovp_stable++;
    if (ovp_stable != 12 || h == ovp_done) return;
    if (!ovp_on) { ovp_done = h; return; }
    ovp_done = h;
    ovp_release();
    VrCamOvSite st[OVP_MAX];
    int n = vrcam_scan_overlay(emu_ram, OVP_LO, OVP_HI, st, OVP_MAX);
    for (int i = 0; i < n; i++) {
        int id = -1;
        if (st[i].kind == 0 && !ovp_map) continue;
        if (rt_patch_code(st[i].addr, 1, &st[i].old, &st[i].neu, &id) == RT_PATCH_OK) ovp_ids[ovp_n++] = id;
    }
    if (n) fprintf(stderr, "[vrhost] overlay: %d de %d sitios de visibilidad sin recorte\n", ovp_n, n);
}
#endif

/* ------------------------------------------------------------------------------------------------ volcado (ns_dump.h)
 * Hilo de emulación: SOLO copia (memcpy) RAM/VRAM/scratch/GTE/lista a búferes reservados una vez y arma el texto de meta. La conversión a PNG y toda la
 * escritura a disco las hace un hilo trabajador propio. Estados: IDLE -> REQ (pedido) -> CAP (copiado) -> WRITE -> IDLE (resultado en dump_res). */
#define DUMP_OPS_CAP  16384u
#define DUMP_DATA_CAP (1u << 20)
#define DUMP_META_CAP 6144
static pthread_mutex_t DMX = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t DCV = PTHREAD_COND_INITIALIZER;
static pthread_t dump_thr;
static int dump_thr_on, dump_res;
static char dump_dir[512], dump_dir2[512];   /* destino de la 1ª captura (punto de enganche en la pareja) y de la 2ª (final de fotograma) */
static uint32_t dump_count;
typedef struct {
    uint8_t *ram, *scratch, *rgba;
    uint16_t *vram, *ldata;
    GpuOp *lops;
    char *meta;
    uint32_t gte[64];
    GpuDisplay disp;
    GpuDrawList list;
    ProvEntry prov[PROV_MAX];                     /* copia de la tabla de procedencia de la lista (list.prov apunta aquí) */
    int has_list;
} DumpBuf;
static DumpBuf DBS[2];                        /* dos juegos de búferes: la pareja se copia entera en el mismo fotograma mientras el trabajador escribe */
static DumpBuf *dbc = &DBS[0];                /* el juego en el que copia el hilo de emulación ahora */
#define DB (*dbc)
static int slot_busy[2];                      /* bajo DMX: el juego de búferes tiene una copia sin escribir aún */
static struct { int slot; char dir[512]; } jobq[2];
static int dump_wait;                         /* bajo DMX: ganchos de fotograma que lleva esperando la captura del punto de enganche (pareja) */
static int job_n, job_writing, dump_acc;      /* bajo DMX: copias pendientes de escribir, escritura en curso, resultado acumulado del pedido (1 ok, 2 parcial, -1 error) */

static void *dump_worker(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&DMX);
        while (job_n == 0) pthread_cond_wait(&DCV, &DMX);
        int slot = jobq[0].slot;
        char dir[512]; snprintf(dir, sizeof dir, "%s", jobq[0].dir);
        jobq[0] = jobq[1]; job_n--;
        job_writing = 1;
        if (dump_state == DS_CAP) dump_state = DS_WRITE;
        pthread_mutex_unlock(&DMX);
        DumpBuf *d = &DBS[slot];
        int fw = d->disp.fw > 0 ? d->disp.fw : d->disp.w, fh = d->disp.h, haveimg = 0;
        if (fw > 0 && fh > 0 && fw <= 1024 && fh <= 512)
            haveimg = gpu_present_rgba(&d->disp, d->vram, d->rgba, (size_t)1024 * 512) != 0;
        NsDumpSrc s = { d->ram, d->scratch, d->vram, d->gte, d->has_list ? &d->list : NULL, haveimg ? d->rgba : NULL, haveimg ? fw : 0, haveimg ? fh : 0, d->meta };
        int rc = ns_dump_write(dir, &s);
        fprintf(stderr, "[vrhost] volcado %s: %s\n", dir, rc == 0 ? "ok" : rc > 0 ? "parcial" : "ERROR");
        fprintf(stderr, rc < 0 ? "[volcado] fallo: no se pudo escribir %s\n" : "[volcado] escrito: %s%s\n", dir, rc > 0 ? " (parcial)" : "");
        pthread_mutex_lock(&DMX);
        int r = rc == 0 ? 1 : rc > 0 ? 2 : -1;
        if (dump_acc == 0 || r == -1 || (r == 2 && dump_acc == 1)) dump_acc = r;
        slot_busy[slot] = 0;
        job_writing = 0;
        if (dump_need == 0 && job_n == 0) { dump_res = dump_acc; dump_acc = 0; dump_state = DS_IDLE; }
        pthread_mutex_unlock(&DMX);
    }
    return NULL;
}

/* Reserva única de los búferes de un juego. SIN el cerrojo DMX (malloc bajo él bloquearía al hilo de emulación y al trabajador): se reserva fuera y se instala dentro si nadie lo hizo. */
static int dump_buffers_ready_slot(int i) {
    pthread_mutex_lock(&DMX);
    int have = DBS[i].ram != NULL;
    pthread_mutex_unlock(&DMX);
    if (have) return 1;
    uint8_t *ram = (uint8_t *)malloc(NS_DUMP_RAM_SIZE), *scratch = (uint8_t *)malloc(1024), *rgba = (uint8_t *)malloc((size_t)1024 * 512 * 4);
    uint16_t *vram = (uint16_t *)malloc(NS_DUMP_VRAM_BYTES), *ldata = (uint16_t *)malloc((size_t)DUMP_DATA_CAP * 2);
    GpuOp *lops = (GpuOp *)malloc((size_t)DUMP_OPS_CAP * sizeof(GpuOp));
    char *meta = (char *)malloc(DUMP_META_CAP);
    int ok = ram && scratch && rgba && vram && ldata && lops && meta;
    int used = 0;
    pthread_mutex_lock(&DMX);
    if (ok && !DBS[i].ram) { DBS[i].ram = ram; DBS[i].scratch = scratch; DBS[i].rgba = rgba; DBS[i].vram = vram; DBS[i].ldata = ldata; DBS[i].lops = lops; DBS[i].meta = meta; used = 1; }
    int have2 = DBS[i].ram != NULL;
    pthread_mutex_unlock(&DMX);
    if (!used) { free(ram); free(scratch); free(rgba); free(vram); free(ldata); free(lops); free(meta); }
    return have2;
}
static int dump_buffers_ready(void) { return dump_buffers_ready_slot(0); }

/* Pedido simple (la primera captura que llegue) o pareja (dir2 != NULL: `dir` = punto de enganche, `dir2` = final del fotograma, ambos del mismo fotograma de juego).
 * La pareja necesita el enganche de la captura nativa: sin él se degrada a un solo volcado en dir2. */
static int dump_request_l(const char *dir, const char *dir2, const char *label) {
    if (!dir || !*dir || strlen(dir) >= sizeof dump_dir) return -1;
    if (dir2 && (!*dir2 || strlen(dir2) >= sizeof dump_dir2)) return -1;
    int pair = dir2 != NULL && ns_hook_installed();
    if (!dump_buffers_ready()) return -1;
    if (pair && !dump_buffers_ready_slot(1)) pair = 0;
    int rc = -1;
    pthread_mutex_lock(&DMX);
    if (dump_state == DS_IDLE) {
        if (!dump_thr_on) {
            if (pthread_create(&dump_thr, NULL, dump_worker, NULL) != 0) { pthread_mutex_unlock(&DMX); return -1; }
            pthread_detach(dump_thr);
            dump_thr_on = 1;
        }
        if (dir2 && !pair) snprintf(dump_dir, sizeof dump_dir, "%s", dir2);
        else snprintf(dump_dir, sizeof dump_dir, "%s", dir);
        if (pair) snprintf(dump_dir2, sizeof dump_dir2, "%s", dir2);
        snprintf(dump_label, sizeof dump_label, "%s", label ? label : "");
        dump_need = dump_need_init(native_mode(), pair);   /* en gpu no hay punto de enganche: la pareja se reduce al final de fotograma, con la lista */
        dump_acc = 0;
        dump_wait = 0;
        dump_state = DS_REQ; dump_res = 0; rc = 0;
    }
    pthread_mutex_unlock(&DMX);
    return rc;
}
static int dump_request(const char *dir, const char *dir2) { return dump_request_l(dir, dir2, NULL); }
int mv_vrhost_request_dump(const char *dir) { return dump_request(dir, NULL); }
int mv_vrhost_request_dump_pair(const char *dir_stage, const char *dir_frame) { return dir_stage && dir_frame ? dump_request(dir_stage, dir_frame) : -1; }
int mv_vrhost_dump_result(void) {
    pthread_mutex_lock(&DMX);
    int r = dump_res; dump_res = 0;
    pthread_mutex_unlock(&DMX);
    return r;
}
void mv_vrhost_cancel_dump(void) {   /* sólo anula lo que aún no se copió; lo ya copiado termina de escribirse */
    pthread_mutex_lock(&DMX);
    if (dump_state == DS_REQ) {
        dump_need = 0;
        if (job_n == 0 && !job_writing) dump_state = DS_IDLE;
        else dump_state = DS_CAP;
    }
    pthread_mutex_unlock(&DMX);
}

#define MP(...) do { if (n < DUMP_META_CAP - 160) n += snprintf(DB.meta + n, (size_t)(DUMP_META_CAP - n), __VA_ARGS__); } while (0)
static int meta_rec(int n, const char *pfx, const VrCamRecord *r) {
    if (!r) return n;
    MP("%sactive=%d\n%seye=%d,%d,%d\n%sM=", pfx, r->active, pfx, r->eye[0], r->eye[1], r->eye[2], pfx);
    for (int i = 0; i < 9; i++) MP("%.6f%s", r->M[i], i < 8 ? "," : "\n");
    MP("%sscreen=%d\n%syaw=%.4f\n%spitch=%.4f\n%stheta=%.4f\n%sunits_per_m=%.3f\n", pfx, r->screen, pfx, r->yaw_deg, pfx, r->pitch_deg, pfx, r->theta_play_deg, pfx, r->units_per_m);
    MP("%sp_ref=%.5f,%.5f,%.5f\n%shead=", pfx, r->p_ref[0], r->p_ref[1], r->p_ref[2], pfx);
    for (int i = 0; i < 16; i++) MP("%.6f%s", r->head[i], i < 15 ? "," : "\n");
    return n;
}

/* Hilo de emulación. Copia a los búferes reservados y despierta al trabajador. `l` = última lista construida (o NULL). */
static void dump_capture(Cpu *c, uint32_t f, const GpuDrawList *l) {
    pthread_mutex_lock(&DMX);
    if (dump_state != DS_REQ) { pthread_mutex_unlock(&DMX); return; }
    /* qué captura toca en este punto: en la pareja, primero el enganche y, ya hecho, el final de fotograma (del mismo fotograma de juego) */
    int bit = dump_pick_bit(dump_need, dump_point);
    int slot = (bit == DN_FRAME) ? 1 : 0;
    if (!bit || slot_busy[slot] || (slot == 1 && !DBS[1].ram)) { pthread_mutex_unlock(&DMX); return; }
    dbc = &DBS[slot];
    slot_busy[slot] = 1;
    memcpy(DB.ram, emu_ram, NS_DUMP_RAM_SIZE);
    memcpy(DB.scratch, emu_scratch, 1024);
    memcpy(DB.vram, &gpu_vram[0][0], NS_DUMP_VRAM_BYTES);
    for (int i = 0; i < 32; i++) { DB.gte[i] = gte_mfc2(c, i); DB.gte[32 + i] = gte_cfc2(c, i); }
    gpu_get_display(&DB.disp);
    DB.has_list = 0;
    int list_trunc = 0;
    if (l) {
        if (l->nops <= DUMP_OPS_CAP && l->ndata <= DUMP_DATA_CAP) {
            memcpy(DB.lops, l->ops, (size_t)l->nops * sizeof(GpuOp));
            memcpy(DB.ldata, l->data, (size_t)l->ndata * 2);
            DB.list = *l; DB.list.ops = DB.lops; DB.list.data = DB.ldata; DB.list.cap_ops = l->nops; DB.list.cap_data = l->ndata;
            DB.list.prov = NULL; DB.list.nprov = 0;
            if (l->prov && l->nprov > 1 && l->nprov <= PROV_MAX) { memcpy(DB.prov, l->prov, (size_t)l->nprov * sizeof(ProvEntry)); DB.list.prov = DB.prov; DB.list.nprov = l->nprov; }
            DB.has_list = 1;
        } else list_trunc = 1;
    }
    int n = 0;
    MP("dump_index=%u\ngame_frame=%u\nvblank=%u\npoint=%s\n", ++dump_count, f, (unsigned)hle_frame_count(), dump_label[0] ? dump_label : dump_point ? "stage" : "frame");
    if (DB.has_list) MP("list_index=%u\n", l->index);
    if (list_trunc) MP("list_omitted=1\n");
    MP("area_id=%d\ngame=%d\nin_control=%d\nposture=%d\nview_mode=%d\n", bs.area_id, bs.has_player, bs.in_control, bs.stance, view.kind);
    MP("player_x=%d\nplayer_y=%d\nplayer_z=%d\n", bs.player_pos.x, bs.player_pos.y, bs.player_pos.z);
    MP("eye_x=%d\neye_y=%d\neye_z=%d\ncam_h=%d\n", bs.eye_pos[0], bs.eye_pos[1], bs.eye_pos[2], bs.eye_focal);
    MP("cam_rot=");
    for (int i = 0; i < 9; i++) MP("%d%s", bs.eye_axes[i / 3][i % 3], i < 8 ? "," : "\n");
    /* Registro de cámara: el de la lista volcada (la que fijó la vista de sus polígonos) y, aparte, el último del hilo de emulación. */
    const VrCamRecord *last = vrcam_last_record(), *lr = NULL;
    if (DB.has_list) { int has3d = 0; lr = list_record(l, &has3d); }
    MP("rec_source=%s\n", lr ? "list" : "last");
    n = meta_rec(n, "rec_", lr ? lr : last);
    if (lr && last) n = meta_rec(n, "last_rec_", last);
    float hm[16]; double ht = 0;
    if (mv_vr_get_head(hm, &ht)) {
        MP("head=");
        for (int i = 0; i < 16; i++) MP("%.6f%s", hm[i], i < 15 ? "," : "\n");
        MP("head_age=%.3f\n", mv_vr_head_age());
    }
    DB.meta[n < DUMP_META_CAP ? n : DUMP_META_CAP - 1] = 0;
    snprintf(jobq[job_n].dir, sizeof jobq[job_n].dir, "%s", bit == DN_FRAME ? dump_dir2 : dump_dir);
    jobq[job_n].slot = slot;
    job_n++;
    dump_need &= ~bit;
    if (dump_need == 0) dump_state = DS_CAP;
    pthread_cond_signal(&DCV);
    pthread_mutex_unlock(&DMX);
}
#undef MP

/* Volcado en el punto de enganche (etapa «pantalla», justo tras la captura nativa): la RAM es la misma que lee el extractor. Sin lista de la GPU (la última
 * cerrada no corresponde a este instante). Se usa con el modo nativo pedido o con MV_DUMP_AT=stage; si no, el volcado se toma en el gancho de frame como siempre. */
static int dump_wants_stage(void) {   /* hay una pareja pendiente que aún necesita su captura del punto de enganche */
    pthread_mutex_lock(&DMX);
    int r = (dump_need & DN_STAGE) != 0;
    pthread_mutex_unlock(&DMX);
    return r;
}
static void dump_at_stage(Cpu *c) {
    if (!dump_stage_mode() && !(atomic_load(&render_req) && atomic_load(&renderer_ready)) && !dump_wants_stage()) return;
    dump_point = 1;
    dump_capture(c, (uint32_t)hle_frame_count(), NULL);   /* no hace nada si no hay un pedido sin atender */
    dump_point = 0;
}

static void frame_hook_body(Cpu *c, uint32_t f) {
    ns_hook_set_frame((uint32_t)hle_frame_count());
    ns_hook_tick(emu_ram);                        /* publica la escena nativa capturada en este fotograma de juego */
    ns_frame_check();
#ifdef MV_NOGEN
    if (vr_ok) ovp_frame();
#endif
    float turn = mv_vr_take_turn();
    if (turn != 0.0f) vrhead_turn(turn);          /* gira la vista y, en vrhead_frame, el rumbo del personaje */
    { float eh; if (mv_vr_take_user_eye_height(&eh)) vrcam_set_user_eye_height(eh); }
    if (mv_vr_take_recenter()) vrcam_recenter();
    bridge_read(emu_ram, &bs);
    publish_state();
    vrcam_frame(emu_ram, &bs);
    vrhead_frame(emu_ram, &bs, bs.moving);
    if (hc_on) {   /* sin cámara VR (cinemática, menú, otra zona) el almacén de colisión no vale */
        static int was_active;
        int act = vrcam_active();
        if (was_active && !act) { pthread_mutex_lock(&HCMX); vrhc_reset(); hc_have_eye = 0; pthread_mutex_unlock(&HCMX); }
        was_active = act;
    }
    ai_feed_head();
    vrai_frame(&bs, f);
    if (test_cb) test_cb(c, f);
    vrgame_frame_end();

    uint64_t lp = atomic_load(&last_poll_ns), now = mono_ns();
    int want = lp && now - lp < 1500000000ull;
    capture_set(want);
    rt_prov_set(cap_on && atomic_load(&l2d_enabled));   /* procedencia: solo con la capa 2D (es quien la usa) */
    prov_autodump();
    if (cap_on && !gte_hp_enabled()) gpu_capture_hiprec(1);   /* el 3D de la lista necesita los vértices de vista de la GTE en alta precisión (también en el modo gpu tras cualquier apagado externo) */
    gpu3d_log();
    prov_log();
    if (!cap_on) return;   /* sin consumidor de escenas no hay lista: frame_hook vuelca sin ella */
    gpu_capture_vblank();
    for (;;) {
        int si = take_free_slot();
        if (si < 0) {   /* cola propia llena: las listas esperan en la captura (si desborda, se resincroniza sola) */
            pthread_mutex_lock(&MX); stats.full_waits++; pthread_mutex_unlock(&MX);
            break;
        }
        const GpuDrawList *l = gpu_capture_frame();
        if (!l) {
            pthread_mutex_lock(&MX); sstate[si] = S_FREE; pthread_mutex_unlock(&MX);
            break;
        }
        if (rt_prov_on) vrprov_acc(&prov_acc, l);
        build_one(l, si);
        {
            int h3 = 0; const VrCamRecord *cr = (bs.valid & BRIDGE_OK_EYE) ? list_record(l, &h3) : NULL;
            float ce[3], cm[9];
            if (cr) { for (int i = 0; i < 3; i++) ce[i] = (float)cr->eye[i]; memcpy(cm, cr->M, sizeof cm); }
            ns_cover_glue_submit_cam(emu_ram, emu_scratch, l->ops, l->nops, f, cr ? ce : NULL, cr ? cm : NULL);   /* con la cámara de la lista */
        }   /* 1 de cada N: instantánea y comprobación de cobertura en otro hilo */
        dump_capture(c, f, l);
        if (!cap_on) break;
    }
}

static void frame_hook(Cpu *c, uint32_t f) {
    ns_cpu_last = c;
    frame_hook_body(c, f);
    /* El volcado funciona SIEMPRE (intro, menús, títulos, cinemáticas, muerte): la captura del punto de enganche solo existe cuando el extractor nativo corre (con escena de partida); sin
     * ella la pareja esperaba para siempre y el gesto acababa en «sin resultado». Tras 3 ganchos de fotograma sin esa captura se descarta y se vuelca solo el final del fotograma. */
    int hold = 0;
    pthread_mutex_lock(&DMX);
    if (dump_state == DS_REQ && (dump_need & DN_STAGE)) {
        if (++dump_wait >= 3) {
            dump_need &= ~DN_STAGE;
            fprintf(stderr, "[volcado] sin captura del punto de enganche (no hay escena nativa en este momento): se vuelca solo el final del fotograma\n");
        }
    } else if (dump_state == DS_REQ && !native_mode() && ++dump_wait < 4) hold = 1;   /* modo gpu: la lista llega con el gancho; sin ella se espera hasta 4 ganchos antes de volcar sin lista */
    pthread_mutex_unlock(&DMX);
    if (!hold) dump_capture(c, f, NULL);   /* si el pedido no se atendió con una lista (sin consumidor, cola llena, sin lista nueva), se vuelca sin ella */
}

/* ------------------------------------------------------------------------------------------------ enganches de arranque */
static int head_fn(float m[16], double *t) {
    if (!vr_ok) return 0;
    double age = mv_vr_head_age();
    if (age < 0.0 || age > 0.5) return 0;   /* sin espacio inmersivo (o pose caducada): la cámara del juego manda */
    return mv_vr_get_head(m, t);
}

static void on_exe_loaded(uint8_t *ram, uint32_t base, uint32_t size) {
    vr_ok = 0;
    vrview_reset(&view);
    memset(&cine, 0, sizeof cine);
    vrcine_config_default(&cine.cfg);
    vrcine_reset(&cine);
    last_scene3d = last_video = last_cover = 0;
    ns_hook_reset();
#ifdef MV_NOGEN
    ns_hook_uninstall();
#endif
    hle_vrcam_reset();
    hle_vrradar_reset();
    vrradar_reset();
    hle_vrfire_reset();
    vrgame_reset();
    body_ok = fire_ok = 0;
    {   /* cuerpo: arranca DESACTIVADO; solo se activa si la HLE que restaura las matrices (vrbody_sort) se enlaza (on_bound) */
        VrBodyConfig bc;
        vrbody_config_default(&bc);
        bc.enabled = 0;
        vrbody_init(&bc);
        vrbody_attach_vrcam();
    }
    vrgame_set_enabled(0, 0);
#ifdef MV_NOGEN
    ovp_release(); ovp_done = 1; ovp_stable = 0;
    hle_bind_declare_fixed("vrradar_draw");
    hle_bind_declare_fixed("vrbody_sort");
    for (unsigned i = 0; i < NFIRE; i++) hle_bind_declare_fixed(FIRE_SITES[i].name);
    for (unsigned i = 0; i < NSITES; i++) hle_bind_declare_fixed(SITES[i].name);
    VrCamPatch p;
    vrcam_patch_default(&p);
    {   /* MV_NOCULL=máscara hex (pruebas A/B): bits 0-5 sitios de NOCULL, bit 6 HLE de paquetes, bit 7 overlays; defecto 0x00 (todo apagado: versión segura; ver bitácora §55) */
        const char *e = getenv("MV_NOCULL"); unsigned m = e ? (unsigned)strtoul(e, NULL, 16) : 0x00u;
        p.no_cull = (int)(m & VRCAM_NOCULL_ALL); vrcam_pack_enable((m >> 6) & 1); ovp_on = (int)((m >> 7) & 1); ovp_map = (int)(m & 1);
    }
    int n = vrcam_patch_ram(ram, &p);
    if (n < 0) {
        fprintf(stderr, "[vrhost] el ejecutable no coincide con la edición esperada: sin cámara VR (se usa la del juego)\n");
    } else {
        rt_code_invalidate(base, size);
        vr_ok = 1;
        fprintf(stderr, "[vrhost] parche de recorte aplicado (%d puntos)\n", n);
    }
#else
    (void)ram; (void)base; (void)size;   /* con código recompilado la cámara VR exige regenerarlo con el EXE parcheado: no se usa */
#endif
    VrCamConfig cfg;
    vrcam_config_default(&cfg);
    cfg.head = vr_ok ? head_fn : NULL;
    { const char *e = getenv("MV_EYE"); if (e && !strcmp(e, "body")) cfg.eye_xz = VRCAM_EYE_BODY; }
    { const char *e = getenv("MV_HEADCOL"); if (e && !strcmp(e, "0")) hc_on = 0; }   /* pruebas A/B: sin límite de la cabeza contra el escenario */   /* pruebas A/B: ojo en el centro del cuerpo (comportamiento anterior) */
    cfg.calibrate = 1;                            /* altura/escala del usuario al recentrar (vr_calib.h) */
    vrcam_init(&cfg);
    vrhead_init(NULL);
    exe_gen++;
    turn_base = mv_vr_turn_total() - vrcam_turn_total();
    pthread_mutex_lock(&MX); stats.cam_ok = vr_ok; pthread_mutex_unlock(&MX);
    if (vr_ok) vrai_on_exe_loaded();   /* IA de enemigos y cámaras: solo con la edición esperada; los parches se instalan en on_bound */
}

static void on_bound(void) {
    if (!vr_ok) return;     /* las direcciones fijas son de la edición esperada del ejecutable: sin esa comprobación no se engancha nada */
    /* Cámara primero: si falla, no se engancha nada más (las demás HLE dependen de la vista de la cámara VR). */
    for (unsigned i = 0; i < NSITES; i++) {
        if (hle_bind_fixed(SITES[i].name, SITES[i].addr) < 0) {
            fprintf(stderr, "[vrhost] no se pudo enlazar %s: sin cámara VR\n", SITES[i].name);
            vr_ok = 0;   /* head_fn devuelve 0: el motor sigue con su cámara (el parche de recorte solo amplía) */
            pthread_mutex_lock(&MX); stats.cam_ok = 0; pthread_mutex_unlock(&MX);
            return;
        }
    }
    if (hle_bind_fixed("vrradar_draw", RADAR_DRAW_ADDR) < 0) fprintf(stderr, "[vrhost] no se pudo enlazar vrradar_draw: el radar seguirá en el HUD\n");
    /* Cuerpo: vrbody_apply (en vrcam_trans) y vrbody_restore (en vrbody_sort) van en pareja; sin la segunda no se activa la primera. */
    if (hle_bind_fixed("vrbody_sort", BODY_SORT_ADDR) < 0) {
        fprintf(stderr, "[vrhost] no se pudo enlazar vrbody_sort: sin IK de los brazos\n");
    } else {
        vrbody_set_enabled(1);
        body_ok = 1;
    }
    /* Render nativo: envoltura de la etapa «pantalla» (valida la forma del código) y pose IK de los brazos hacia la escena pendiente. */
    {
        int rc = ns_hook_install();
        if (rc != NS_HOOK_OK) fprintf(stderr, "[vrhost] enganche del render nativo no instalado (%d): el 3D sale de la lista de la GPU\n", rc);
        ns_hook_set_stage_cb(dump_at_stage);
        ns_hook_set_precapture_cb(hands_precapture);
        vrbody_set_apply_hook(ns_post_ik);
        ns_hook_set_ik_idle(ns_ik_idle, NULL);
    }
    /* Disparo: todas o ninguna (con unas sí y otras no, el arma VR dispararía a medias). */
    fire_ok = 1;
    for (unsigned i = 0; i < NFIRE; i++) {
        if (!strncmp(FIRE_SITES[i].name, "vrfire_aa", 9) && !aa_code_ok()) {
            fprintf(stderr, "[vrhost] el código del buscador antiaéreo no coincide: sin arma VR\n");
            fire_ok = 0;
            break;
        }
        if (hle_bind_fixed(FIRE_SITES[i].name, FIRE_SITES[i].addr) < 0) {
            fprintf(stderr, "[vrhost] no se pudo enlazar %s: sin arma VR\n", FIRE_SITES[i].name);
            fire_ok = 0;
        }
    }
    vrgame_set_enabled(body_ok, fire_ok);
    if (vrai_on_bound() < 0) fprintf(stderr, "[vrhost] IA de enemigos no instalada\n");
    if (vrmelee_rt_install() < 0) fprintf(stderr, "[vrhost] acciones de cuerpo (golpe/agarre/toque) no instaladas\n");
    { VrHitWorld hw = {0.0f, hit_wall, hit_camera, hit_camera_hit}; vrhit_set_world(&hw); }
}

static void on_start(void) {
    /* La captura se enciende cuando aparece un consumidor de escenas (frame_hook). */
    cap_on = 0;
    gpu_capture_set_mark_hook(vrradar_on_mark);   /* runtime_mark.patch */
    sopts = vr_scene_opts_default();
    { const char *c = getenv("MV_COH"); if (c) sopts.coherence = atoi(c); }   /* A/B de la red de seguridad de vértices (§56); def. activa */
    sopts.screen_filter = 1;   /* el renderer de la app aplica la clase 3 (post-proceso por ojo: onda, tinte y recubrimiento; RenderRG.swift) */
}

/* Mando que ve el juego (hilo de emulación, cada VBlank, antes de aplicarlo): arma VR, disparo y recarga (vrgame.c). `bs` es el puente
 * del VBlank anterior. */
static uint16_t pad_hook(uint16_t buttons) {
    vrgame_set_gloves(body_ok && !native_mode(), &gpu_vram[0][0]);   /* manos propias (vr_glove) solo con la lista de la GPU: el render nativo trae sus manos */
    return vrgame_pad(buttons, &bs, emu_ram);
}

/* Radar de muñeca: copia de la percepción activa (cono original + cono ampliado más tenue; nada con el ajuste original). */
static void radar_perception(const VrpConfig *c) { vrradar_set_vrp(c); }

void mv_vrhost_set_ai_range(float scale) { vrai_set_range_scale(scale); }
void mv_vrhost_set_ai_head(int on) { vrai_set_head_perception(on); }
void mv_vrhost_set_ai(int perception, int cameras, int activity) {
    VraiOptions o;
    o.perception = perception; o.cameras = cameras; o.activity = activity;
    vrai_set_options(&o);
}

void mv_vrhost_install(void) {
    { const char *e = getenv("MV_HUD2D"); if (e && e[0] == '0') atomic_store(&l2d_enabled, 0); }
    { const char *e = getenv("MV_HUD2D_SCALE"); if (e) { int v = atoi(e); if (v >= 1 && v <= 6) l2d_target = v; } }
    { const char *e = getenv("MV_L2D_SPLIT"); vl2a_set_split(e && e[0] == '1'); }   /* troceo del 2D (paneles de regiones del HUD, ventana del menú en la mano, radar aparte): apagado salvo MV_L2D_SPLIT=1 */
    if (!atomic_load(&ns_br)) {
        ns_bridge_get();
        const char *e = getenv("MV_RENDER");
        if (e && *e) {
            /* MV_RENDER=nativo (defecto y único). «gpu» y «solo-nativo» ya no existen: se avisa y se ignoran. */
            if (!strcmp(e, "nativo") || !strcmp(e, "native")) atomic_store(&render_locked, 1);
            else fprintf(stderr, "[vrhost] MV_RENDER=%s ignorado: el único modo es nativo\n", e);
        }
        ns_extract_register();
    }
    vrai_set_perception_listener(radar_perception);
    MvHostHooks h;
    memset(&h, 0, sizeof h);
    h.exe_loaded = on_exe_loaded;
    h.bound = on_bound;
    h.start = on_start;
    h.frame = frame_hook;
    h.pad = pad_hook;
    mv_host_set_hooks(&h);
}

/* ------------------------------------------------------------------------------------------------ consultas */
void mv_vrhost_game_state(MvVrGameState *out) {
    if (!out) return;
    pthread_mutex_lock(&MX);
    if (pub_valid) {
        *out = pub_gs;
    } else {
        memset(out, 0, sizeof *out);
        out->posture = -1; out->menu = 1; out->weapon = -1;
        out->view_mode = VRV_CINEMA; out->view_reason = VRV_R_NO_GAME; out->ammo = out->ammo_max = out->item = -1;
    }
    pthread_mutex_unlock(&MX);
}

/* Escena del radar de la última escena entregada por mv_host_scene (misma validez: hasta la siguiente llamada). 0 si esa escena no
 * traía radar (el juego lo oculta, lo apaga o hay cinemática): entonces no hay quad en el brazo. */
int mv_host_radar(VrRadarScene *out) {
    int ok;
    pthread_mutex_lock(&MX);
    ok = held >= 0 && slot[held].rs.present;
    if (ok && out) *out = slot[held].rs;
    pthread_mutex_unlock(&MX);
    return ok;
}

int mv_host_bridge(struct BridgeState *out) {
    int ok;
    pthread_mutex_lock(&MX);
    ok = pub_valid;
    if (ok && out) memcpy(out, &pub_bs, sizeof pub_bs);
    pthread_mutex_unlock(&MX);
    return ok;
}

void mv_vrhost_stats(MvVrHostStats *s) {
    if (!s) return;
    pthread_mutex_lock(&MX);
    *s = stats;
    pthread_mutex_unlock(&MX);
}
