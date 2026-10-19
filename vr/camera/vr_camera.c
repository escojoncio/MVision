/* vr_camera: ver vr_camera.h. Núcleo sin dependencia del runtime: opera sobre un puntero a la RAM de 2 MB.
 *
 * ============================== Hallazgos del motor (edición europea, EXE principal) ==============================
 * Todas las direcciones son de RAM de la consola (KSEG0). Verificadas leyendo la RAM en partida y el código
 * recompilado; las del puente (bridge_addrs.h) no se repiten aquí.
 *
 * Cámara del juego
 *  - Una tarea del motor (la de cámara, función en 0x800311e8) corre cada frame: elige posición/objetivo según el
 *    modo (cenital seguida, cámaras fijas de sala, primera persona) y siempre termina llamando al constructor de
 *    matriz de cámara (0x80017564, args: a0 = canal 3D principal 0x800B2B7C, a1 = *VEC16 ojo, a2 = *VEC16 objetivo,
 *    a3 = distancia de pantalla/zoom (s16)). El constructor fija la distancia de pantalla del canal (+0x50, es la H de la GTE para
 *    todas las etapas), la matriz vista->mundo "eye" (+0x30: s16 m[3][3] 1.12 con columnas X derecha, Y abajo, Z
 *    delante, t = ojo) y su inversa (+0x10, BCH_CAM_INV), que es la que usan las etapas de pantalla. La orientación sale
 *    de (objetivo - ojo) y de un vector "arriba" fijo (0x8009E524 = (0,-4096,0)): no hay alabeo posible.
 *    Entrada previa (zoom, posición, objetivo) viva en 0x800B89E8 (ojo), +8 objetivo, +0x10 rotación, +0x18 distancia,
 *    +0x1C zoom: NO se escribe (el motor la reconstruye cada frame con interpolaciones que dependen de su valor
 *    anterior); se cambian los argumentos de la llamada (vector temporal en la pila).
 *  - Primera persona del jugador: la variable de partida [95] (puente) NO es "estoy en 1ª persona" sino la opción de
 *    "moverse en 1ª persona" (0 en la partida normal); el estado de 1ª persona es el indicador de 1ª persona de la cámara
 *    (s16 @0x800B898A, puente cam_first_person) + bit 0x1 del estado del jugador (0x800ACBE0, 0xC009 = 1ª persona mirando).
 *    La cámara del juego en 1ª persona = ojo (fuente de cámara del jugador @0x800B8938, posición: x,z del centro del cuerpo, y de la
 *    articulación de la cabeza) y mirada = (yaw del cuerpo, cabeceo) @0x800B8960, objetivo a 1000 uds; el cuerpo
 *    se hace invisible 6 frames después. Aquí NO se activa ese modo (no se escribe el estado del jugador): se toma
 *    lo útil (posición del ojo) y se sustituye la dirección por la de la cabeza, sin tocar la lógica del juego.
 *  - Cuerpo del jugador: puntero @0x800ACBB0 -> objeto animado, cuyo primer campo apunta al conjunto de partes (nº de partes s16 @+0x2E = 16,
 *    partes de 0x5C bytes desde +0x48 (stride verificado en el EXE: `addiu 0x5c` en los bucles de las etapas), contador de
 *    visibilidad s16 @+0x4C de cada parte (8 = visible; 0 = ni se transforma ni genera paquetes), traslación mundo @+0x14). Parte 6 = cabeza (su
 *    traslación Y es la altura del ojo del juego; ~1500 de pie (medido) cuando el cuerpo (parte 0) está en ~1030).
 *  - Origen angular del mando: variable global @0x800AC51C (gp+148); escribirla es la función 0x80016d98. El mando
 *    analógico da dirección = ángulo del stick + origen. La cámara la fija a yaw_cámara + 2048 cada frame.
 *
 * Recorte y visibilidad
 *  - El motor descarta por posición en pantalla comparando coordenadas de la GTE con una caja fija de +-160 x +-128
 *    (edición europea 320x256; el JP usa +-112): (1) por parte de objeto (0x80018b40: contador de visibilidad de la
 *    parte, 8 al ser visible y -1 por frame si no) y por bloque de objeto con las 8 esquinas de su caja envolvente
 *    (0x80018f54; estado de recorte del bloque s16 @+0x32: 0 = fuera, 1 = parcial, 2 = dentro), (2) por vértice al
 *    transformar los de los objetos "parciales" (marcas de lado; un polígono se descarta si sus 4 vértices comparten
 *    lado; 6 copias en 0x8001e4b8-0x8001e6f4), (3) en la subdivisión de polígonos grandes (0x800194f4). Los objetos con
 *    estado 0 no generan paquetes (se liberan a los 8 frames) y la IA de los enemigos lo usa como "no está en
 *    cámara" (giran sin suavizar y, fuera de cámara, se reubican por su ruta; ver vr/ai). patch_exe.py cambia las 44
 *    constantes inmediatas de recorte (caja a la medida de --hfov/--vfov) sin tocar H: el recorte pasa a cubrir ~140 x
 *    120 grados alrededor de la dirección del motor. Efecto en el juego (revisión): el estado de recorte del bloque
 *    alimenta a la IA, así que los enemigos dentro de 140 x 120 grados ya cuentan como "en cámara" (coincide con lo que
 *    ve el usuario; vr/ai además lo fuerza para los activos). La detección de las cámaras de vigilancia NO depende
 *    del recorte (solo su matriz de luz). Las comprobaciones de "punto visible" (0x8001c440, 0x8001c4d8, 0x8001c5e8; caja
 *    +-192 x +-152) las usan la reubicación de enemigos por ruta y, la de 0x8001c5e8, órdenes de guion "¿en pantalla?"
 *    y la cámara de fotos: reglas del juego ligadas a la cámara. Por defecto NO se parchean (12 sitios, opción
 *    --wide-points / VrCamPatch.wide_points solo para pruebas): en VR la caja de 192 px con H=160 cubre ~100 x 86
 *    grados (la original, con H=320, ~62 x 51), sin dejar que todo lo visible en 140 grados cuente para esas reglas.
 *    No hay recorte por distancia (solo la profundidad 0 de la GTE = detrás de la cámara) ni LOD por distancia
 *    (el nivel de detalle de los enemigos depende del modo de cámara, no de la distancia).
 *  - Visibilidad del escenario por mapas: cada modelo de escenario pertenece a un "grupo de mapa" y solo se dibuja si
 *    su grupo está en el grupo actual (0x800AC... ver mapa activo); lo decide el juego por zona/puertas, no la
 *    cámara. Es independiente de la dirección de mirada (una sala no activa no se dibuja mire donde mire).
 *  - Ajuste vertical de pantalla ("overscan"): al preparar las matrices de pantalla de cada objeto el motor
 *    multiplica la fila Y de la matriz de vista y su traslación por 66/64 (función 0x80017780). Con la cámara VR se
 *    anula (las posiciones de vista salen sin deformar).
 *  - Etapas del frame (tabla en 0x8009E534): pantalla 0x8001d324, recorte por objeto 0x80018c8c, transformación
 *    0x8001e840, sombreado 0x8001d76c, primitivas 0x8001ba44, subdivisión 0x8001a0c4, orden 0x8001da18. El envío al GPU
 *    es 0x80018110 (un envío por frame del motor, del frame anterior ya ordenado).
 *
 * ================================== Intervenciones (resumen; todas condicionadas a vrcam_active) ===================
 *  I1 cámara VR: sustituye 0x80017564 (args del constructor)        validar: matriz "eye" == la pedida por la cabeza
 *  I2 origen del mando: sustituye 0x80016d98                         validar: el personaje avanza hacia donde se mira
 *  I3 ocultar cabeza: sustituye 0x8001e840 (contador de visibilidad de la parte 6 = 0)  validar: sin polígonos de la parte 6 en la lista
 *  I4 sin overscan: sustituye 0x80017780                             validar: posiciones de vista métricas
 *  I5 envío: sustituye 0x80018110 (solo cuenta)                      validar: 1 envío = 1 lista capturada
 *  I6 recorte ampliado: parche de 44 inmediatos del EXE              validar: sin vacíos al girar 360 / +-60 grados
 *  I7 sin subdivisión: parche de 1 instrucción del EXE (0x8001a11c)    validar: polígonos parciales 1800 -> 25 por frame
 *  I8 H del motor 160 (en vez de 320): argumento de I1               validar: menos vértices saturados/desbordados
 */
#include "vr_camera.h"
#include "vr_calib.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define RAM_MASK 0x1FFFFFu
#define PI_F 3.14159265358979f

/* ---- acceso a RAM (siempre enmascarado, nunca fuera de los 2 MB) ---- */
static inline int32_t rd_i32(const uint8_t *r, uint32_t a) { int32_t v; memcpy(&v, r + (a & RAM_MASK & ~3u), 4); return v; }
static inline uint32_t rd_u32(const uint8_t *r, uint32_t a) { return (uint32_t)rd_i32(r, a); }
static inline int16_t rd_s16(const uint8_t *r, uint32_t a) { int16_t v; memcpy(&v, r + (a & RAM_MASK & ~1u), 2); return v; }
static inline void wr_s16(uint8_t *r, uint32_t a, int16_t v) { memcpy(r + (a & RAM_MASK & ~1u), &v, 2); }
static inline int ptr_ok(uint32_t p, uint32_t len) {
    return (p & 3u) == 0 && (p >> 29) == 4u /* KSEG0 */ && (p & RAM_MASK) + len <= BRIDGE_RAM_SIZE && (p & RAM_MASK) >= 0x10000u;
}

/* ---- direcciones propias de esta capa (EXE principal, edición europea) ---- */
#define VC_CHANNEL_MAIN   BA_VIEW_CHANNEL  /* canal 3D principal */
#define VC_PLAYER_BODY  BA_PLAYER_BODY
#define VC_OBJS_NMODELS 0x2E
#define VC_OBJS_FIRST   0x48
#define VC_OBJ_SIZE     0x5C
#define VC_OBJ_BOUND    0x4C
#define VC_OBJ_WORLD_T  0x14
#define VC_HEAD_PART    6
#define VC_MAX_MODELS   64

typedef struct {
    VrCamConfig cfg;
    int ok_count, want, latched;          /* decisión por frame (gancho) / por cámara (construcción) */
    int have_ref, need_recenter;
    int zoom_capped;                      /* VBlanks que faltan para dar por terminado el recorte de zoom (el juego pidió más que el tope) */
    int zoom_cap;                         /* >0: tope de la distancia de pantalla que pide el juego (vista de la cabeza sin aumento) */
    float theta_play0, yaw_ref, extra;    /* grados */
    double turn_total;                    /* suma de todos los giros del stick (no se envuelve ni se reinicia al recentrar) */
    float p_ref[3];
    VrCalib calib;                        /* altura/escala del último recentrado */
    float view_yaw, theta_body;           /* último frame VR */
    BridgeState bs;                       /* grande: no en la pila */
    VrCamRecord rec[32];                  /* anillo por construcción */
    uint32_t serial;
    uint32_t draw_n;
    struct { uint32_t s0, s1, serial; } draw[64];  /* envíos al GPU: rango de seq y registro asociado (anillo) */
    VrCamStats st;
    /* predicción de la mirada: última muestra de cabeza (guiñada/elevación del usuario) y velocidad angular suavizada (grados/s) */
    uint32_t vb;                          /* VBlanks vistos (vrcam_frame) */
    int hp_ok; float hp_yaw, hp_el; uint32_t hp_vb;
    float w_yaw, w_el;
    int32_t pj[3]; int pj_ok;             /* articulación de la cabeza en la construcción anterior (adelanto del ojo) */
} Vc;
static Vc S;

void vrcam_config_default(VrCamConfig *c) {
    memset(c, 0, sizeof *c);
    c->units_per_m = 1000.0f;
    c->eye_up = 110.0f; c->eye_fwd = 70.0f;
    c->loco = VRCAM_LOCO_HEAD; c->eye_xz = VRCAM_EYE_AUTO;
    c->hide_head = 1; c->override_pad = 1;
    c->on_frames = 8; c->engine_h = 160; c->draw_lag = 0; c->pitch_limit_deg = 80.0f;
    c->joint_lead = 1.0f; c->predict_s = 0.10f; c->predict_max_deg = 35.0f; c->vblank_hz = 50.0f;
}
void vrcam_init(const VrCamConfig *c) {
    memset(&S, 0, sizeof S);
    if (c) S.cfg = *c; else vrcam_config_default(&S.cfg);
    if (S.cfg.units_per_m <= 0) S.cfg.units_per_m = 1000.0f;
    if (S.cfg.on_frames < 1) S.cfg.on_frames = 1;
    if (S.cfg.pitch_limit_deg < 5 || S.cfg.pitch_limit_deg > 89) S.cfg.pitch_limit_deg = 80.0f;
    if (!(S.cfg.predict_s >= 0.0f && S.cfg.predict_s <= 0.5f)) S.cfg.predict_s = 0.0f;          /* NaN o fuera de rango: sin predicción */
    if (!(S.cfg.predict_max_deg >= 0.0f && S.cfg.predict_max_deg <= 90.0f)) S.cfg.predict_max_deg = 35.0f;
    if (!(S.cfg.vblank_hz >= 10.0f && S.cfg.vblank_hz <= 240.0f)) S.cfg.vblank_hz = 50.0f;
}
const VrCamConfig *vrcam_config(void) { return &S.cfg; }

/* ================================================ matemática pura ================================================ */
static float wrap180(float a) { a = fmodf(a + 180.0f, 360.0f); if (a < 0) a += 360.0f; return a - 180.0f; }
static float wrap360(float a) { a = fmodf(a, 360.0f); if (a < 0) a += 360.0f; return a; }

void vrcam_head_angles(const float h[16], float *yaw, float *elev) {
    float fx = -h[8], fy = -h[9], fz = -h[10];                 /* delante = -Z de la cabeza */
    float n = sqrtf(fx * fx + fy * fy + fz * fz);
    if (n < 1e-6f) { *yaw = 0; *elev = 0; return; }
    fx /= n; fy /= n; fz /= n;
    float hl = sqrtf(fx * fx + fz * fz);
    float y;
    if (hl > 0.30f) y = atan2f(-fx, -fz);                      /* delante horizontal: sin ambigüedad */
    else {                                                      /* casi vertical: el "arriba" de la cabeza apunta delante (mirando abajo) o detrás (arriba) */
        float ux = h[4], uz = h[6];
        if (fy > 0) { ux = -ux; uz = -uz; }
        y = atan2f(-ux, -uz);
    }
    *yaw = y * (180.0f / PI_F);
    float e = fy > 1 ? 1 : (fy < -1 ? -1 : fy);
    *elev = asinf(e) * (180.0f / PI_F);
}

void vrcam_dir(float yaw_deg, float pitch_deg, float d[3]) {
    float y = yaw_deg * (PI_F / 180.0f), p = pitch_deg * (PI_F / 180.0f);
    d[0] = sinf(y) * cosf(p); d[1] = -sinf(p); d[2] = cosf(y) * cosf(p);
}

static int head_finite(const float m[16]) {
    for (int i = 0; i < 16; i++) if (!isfinite(m[i])) return 0;
    /* rotación razonable: columnas ~unitarias */
    for (int c = 0; c < 3; c++) {
        float n = m[4 * c] * m[4 * c] + m[4 * c + 1] * m[4 * c + 1] + m[4 * c + 2] * m[4 * c + 2];
        if (n < 0.5f || n > 2.0f) return 0;
    }
    return 1;
}

static int16_t sat16(float v) { return (int16_t)(v > 32767.0f ? 32767 : (v < -32768.0f ? -32768 : lrintf(v))); }

/* ============================================ lectura del estado del juego ============================================ */
typedef struct { int ok; int32_t eye_x, eye_y, eye_z; float body_yaw; int prone; int joint; int32_t jx, jy, jz; } Body;

static Body read_body(const uint8_t *ram) {
    Body b = {0};
    uint32_t ob = rd_u32(ram, VC_PLAYER_BODY), ctl = rd_u32(ram, BA_PLAYER_CTRL);
    uint32_t status = rd_u32(ram, BA_PLAYER_STATUS);
    b.prone = (status & BRIDGE_PS_GROUND) != 0;
    int have_ctl = ptr_ok(ctl, BC_SIZE);
    int16_t mx = 0, my = 0, mz = 0;
    if (have_ctl) {
        mx = rd_s16(ram, ctl + BC_MOV); my = rd_s16(ram, ctl + BC_MOV + 2); mz = rd_s16(ram, ctl + BC_MOV + 4);
        b.body_yaw = (float)((uint16_t)rd_s16(ram, ctl + BC_ROT + 2) & 4095u) * (360.0f / 4096.0f);
    } else {
        mx = rd_s16(ram, BA_PLAYER_POS); my = rd_s16(ram, BA_PLAYER_POS + 2); mz = rd_s16(ram, BA_PLAYER_POS + 4);
    }
    int32_t jx = mx, jy = my + 350, jz = mz;                    /* sin cuerpo: altura media de la articulación de la cabeza */
    if (ptr_ok(ob, 8)) {
        uint32_t objs = rd_u32(ram, ob);
        if (ptr_ok(objs, VC_OBJS_FIRST + VC_OBJ_SIZE * (VC_HEAD_PART + 1))) {
            int n = rd_s16(ram, objs + VC_OBJS_NMODELS);
            if (n > VC_HEAD_PART && n <= VC_MAX_MODELS) {
                uint32_t o = objs + VC_OBJS_FIRST + VC_OBJ_SIZE * VC_HEAD_PART;
                b.joint = 1;
                jx = rd_i32(ram, o + VC_OBJ_WORLD_T); jy = rd_i32(ram, o + VC_OBJ_WORLD_T + 4); jz = rd_i32(ram, o + VC_OBJ_WORLD_T + 8);
            }
        }
    }
    /* AUTO: los ojos siguen SIEMPRE la articulación de la cabeza del modelo (andando, agachado, tumbado y en las transiciones el modelo se inclina/adelanta
     * y el centro del cuerpo queda detrás: la cabeza del usuario se veía por detrás del cuerpo). BODY: solo el centro. */
    b.jx = jx; b.jy = jy; b.jz = jz;
    int use_joint_xz = b.joint && (S.cfg.eye_xz == VRCAM_EYE_JOINT || S.cfg.eye_xz == VRCAM_EYE_AUTO);
    b.eye_x = use_joint_xz ? jx : mx; b.eye_y = jy; b.eye_z = use_joint_xz ? jz : mz;
    b.ok = have_ctl;
    return b;
}

/* Ojos del modelo del personaje ahora mismo (articulación de la cabeza + eye_up/eye_fwd según la mirada dada `view_deg`), unidades del motor.
 * Devuelve 0 si no hay modelo. Solo lee la RAM (pruebas y medidas). */
int vrcam_model_eye(const uint8_t *ram, float view_deg, float out[3]) {
    Body b = read_body(ram);
    if (!b.joint) return 0;
    uint32_t ob = rd_u32(ram, VC_PLAYER_BODY), objs = rd_u32(ram, ob);
    uint32_t o = objs + VC_OBJS_FIRST + VC_OBJ_SIZE * VC_HEAD_PART;
    float yr = view_deg * (PI_F / 180.0f);
    out[0] = (float)rd_i32(ram, o + VC_OBJ_WORLD_T) + sinf(yr) * S.cfg.eye_fwd;
    out[1] = (float)rd_i32(ram, o + VC_OBJ_WORLD_T + 4) + S.cfg.eye_up;
    out[2] = (float)rd_i32(ram, o + VC_OBJ_WORLD_T + 8) + cosf(yr) * S.cfg.eye_fwd;
    return 1;
}

/* Articulaciones del personaje (traslación mundo de cada parte del modelo, unidades del motor). Devuelve cuántas escribió (0 si no hay modelo). */
int vrcam_model_joints(const uint8_t *ram, float (*out)[3], int max) {
    uint32_t ob = rd_u32(ram, VC_PLAYER_BODY);
    if (!ptr_ok(ob, 8)) return 0;
    uint32_t objs = rd_u32(ram, ob);
    if (!ptr_ok(objs, VC_OBJS_FIRST + VC_OBJ_SIZE * (VC_HEAD_PART + 1))) return 0;
    int n = rd_s16(ram, objs + VC_OBJS_NMODELS);
    if (n <= VC_HEAD_PART || n > VC_MAX_MODELS) return 0;
    if (n > max) n = max;
    for (int i = 0; i < n; i++) {
        uint32_t o = objs + VC_OBJS_FIRST + VC_OBJ_SIZE * (uint32_t)i + VC_OBJ_WORLD_T;
        out[i][0] = (float)rd_i32(ram, o); out[i][1] = (float)rd_i32(ram, o + 4); out[i][2] = (float)rd_i32(ram, o + 8);
    }
    return n;
}

/* ================================================== gancho de frame ================================================== */
void vrcam_frame(const uint8_t *ram, const BridgeState *st) {
    if (!ram) return;
    S.vb++;
    if (!st) { bridge_read(ram, &S.bs); st = &S.bs; }
    int ok = (st->valid & BRIDGE_OK_ALL_CORE) == BRIDGE_OK_ALL_CORE && st->has_player && st->cam_ctl && S.cfg.head;   /* cam_ctl = in_control o ventana de armas/objetos / pausa en partida: la cámara VR no se apaga con un menú 2D encima */
    if (ok) { if (S.ok_count < 1 << 20) S.ok_count++; } else S.ok_count = 0;
    int want = S.ok_count >= S.cfg.on_frames;
    if (want && !S.want) { S.have_ref = 0; S.st.activations++; }
    if (!want && S.want) { S.st.deactivations++; S.have_ref = 0; }
    if (!want) S.pj_ok = 0;
    S.want = want;
    if (S.zoom_capped > 0) S.zoom_capped--;
}

void vrcam_recenter(void) { S.need_recenter = 1; }
const VrCalib *vrcam_calib(void) { return &S.calib; }
void vrcam_set_user_eye_height(float m) { S.cfg.user_eye_m = isfinite(m) && m > 0 ? m : 0.0f; S.need_recenter = 1; }
int vrcam_zoom_capped(void) { return S.zoom_capped > 0; }
void vrcam_set_zoom_cap(int cap) { S.zoom_cap = cap > 0 ? (cap > 32767 ? 32767 : cap) : 0; }
void vrcam_turn(float deg) { S.extra = wrap180(S.extra + deg); if (isfinite(deg)) S.turn_total += (double)deg; }
double vrcam_turn_total(void) { return S.turn_total; }
int vrcam_active(void) { return S.latched; }
float vrcam_view_yaw_deg(void) { return S.view_yaw; }
int vrcam_pad_origin(void) {
    float y = S.cfg.loco == VRCAM_LOCO_BODY ? S.theta_body : S.view_yaw;
    return ((int)lrintf(y * (4096.0f / 360.0f)) + 2048) & 4095;
}

/* ============================================ construcción de la cámara ============================================ */
int vrcam_plan(const uint8_t *ram, int16_t zoom, VrCamPlan *p) {
    S.st.builds++;
    S.latched = 0;
    if (!S.want || !S.cfg.head || !p) return 0;
    float h[16]; double t = 0;
    if (!S.cfg.head(h, &t) || !head_finite(h)) { S.st.head_misses++; return 0; }
    Body b = read_body(ram);
    if (!b.ok) return 0;

    /* La cámara se construye ANTES de que el juego anime el modelo en ese mismo fotograma: la articulación leída es la del fotograma anterior y el ojo iba un
     * fotograma por detrás del personaje (medido: = la velocidad de marcha, ~10 cm/fotograma andando). Se adelanta un fotograma con la velocidad medida entre las
     * dos últimas construcciones (sin extrapolar saltos: cambios de zona, teletransportes). */
    if (b.joint) {
        if (S.pj_ok && S.cfg.joint_lead > 0.0f) {
            int32_t d[3] = {b.jx - S.pj[0], b.jy - S.pj[1], b.jz - S.pj[2]};
            int small = abs(d[0]) <= 400 && abs(d[1]) <= 400 && abs(d[2]) <= 400;
            if (small) {
                if (b.eye_x == b.jx) b.eye_x += (int32_t)lrintf((float)d[0] * S.cfg.joint_lead);
                if (b.eye_z == b.jz) b.eye_z += (int32_t)lrintf((float)d[2] * S.cfg.joint_lead);
                b.eye_y += (int32_t)lrintf((float)d[1] * S.cfg.joint_lead);
            }
        }
        S.pj[0] = b.jx; S.pj[1] = b.jy; S.pj[2] = b.jz; S.pj_ok = 1;
    } else S.pj_ok = 0;

    float a_yaw, elev;
    vrcam_head_angles(h, &a_yaw, &elev);
    if (!S.have_ref || S.need_recenter) {
        S.theta_play0 = wrap360(b.body_yaw - a_yaw);
        S.yaw_ref = a_yaw; S.extra = 0;
        S.p_ref[0] = h[12]; S.p_ref[1] = h[13]; S.p_ref[2] = h[14];
        if (S.cfg.calibrate) { VrCalibCfg cc; vrcalib_cfg_default(&cc); cc.user_eye_m = S.cfg.user_eye_m; vrcalib_compute(&cc, h[13], &S.calib); }
        else { S.calib.seated = 0; S.calib.head_y_m = h[13]; S.calib.user_eye_m = 0; S.calib.scale = 1.0f; }
        S.have_ref = 1; S.need_recenter = 0; S.st.recenters++;
        S.hp_ok = 0; S.w_yaw = S.w_el = 0.0f;   /* el salto del recentrado no es una velocidad */
    }
    /* Predicción: velocidad angular de la cabeza entre dos construcciones de cámara (suavizada) y adelanto de predict_s. Solo mueve el CENTRO del recorte del
     * motor en el espacio del usuario (theta_play, que decide dónde cae el mundo, no cambia) y el cabeceo dado al motor. */
    float p_yaw = a_yaw, p_el = elev;
    {
        float dt = S.hp_ok ? (float)(S.vb - S.hp_vb) / S.cfg.vblank_hz : 0.0f;
        if (S.hp_ok && dt > 0.0f && dt <= 0.5f) {
            float wy = wrap180(a_yaw - S.hp_yaw) / dt, we = (elev - S.hp_el) / dt;
            S.w_yaw = 0.5f * S.w_yaw + 0.5f * wy; S.w_el = 0.5f * S.w_el + 0.5f * we;
        } else if (!S.hp_ok || dt > 0.5f) S.w_yaw = S.w_el = 0.0f;
        if (!S.hp_ok || dt > 0.0f) { S.hp_ok = 1; S.hp_yaw = a_yaw; S.hp_el = elev; S.hp_vb = S.vb; }
        if (S.cfg.predict_s > 0.0f) {
            float dy = S.w_yaw * S.cfg.predict_s, de = S.w_el * S.cfg.predict_s, m = S.cfg.predict_max_deg;
            dy = dy > m ? m : (dy < -m ? -m : dy);
            de = de > m ? m : (de < -m ? -m : de);
            p_yaw = a_yaw + dy; p_el = elev + de;
            p_el = p_el > 89.0f ? 89.0f : (p_el < -89.0f ? -89.0f : p_el);
        }
    }
    float view = S.cfg.loco == VRCAM_LOCO_BODY ? b.body_yaw + (p_yaw - S.yaw_ref) + S.extra : S.theta_play0 + S.extra + p_yaw;
    view = wrap360(view);
    float view_real = S.cfg.loco == VRCAM_LOCO_BODY ? b.body_yaw + (a_yaw - S.yaw_ref) + S.extra : S.theta_play0 + S.extra + a_yaw;
    float pitch = -p_el;
    if (pitch > S.cfg.pitch_limit_deg) pitch = S.cfg.pitch_limit_deg;
    if (pitch < -S.cfg.pitch_limit_deg) pitch = -S.cfg.pitch_limit_deg;

    /* ojo: articulación de la cabeza (+ desplazamiento arriba/delante según la mirada horizontal) */
    float yr = view * (PI_F / 180.0f);
    float ex = (float)b.eye_x + sinf(yr) * S.cfg.eye_fwd, ey = (float)b.eye_y + S.cfg.eye_up, ez = (float)b.eye_z + cosf(yr) * S.cfg.eye_fwd;
    p->from[0] = sat16(ex); p->from[1] = sat16(ey); p->from[2] = sat16(ez);

    /* objetivo: ojo + dirección * track, con track acotado para que el objetivo quepa en s16 (la dirección sale de
     * objetivo - ojo en el motor; a 3000 uds el redondeo a entero da < 0,01 grados) */
    float d[3]; vrcam_dir(view, pitch, d);
    float track = 3000.0f, from[3] = {(float)p->from[0], (float)p->from[1], (float)p->from[2]};
    for (int i = 0; i < 3; i++) {
        float lim = d[i] > 0 ? 32000.0f - from[i] : (d[i] < 0 ? 32000.0f + from[i] : 1e9f);
        float fi = fabsf(d[i]);
        if (fi > 1e-4f && lim / fi < track) track = lim / fi;
    }
    if (track < 600.0f) track = 600.0f;
    for (int i = 0; i < 3; i++) p->to[i] = sat16(from[i] + d[i] * track);

    if (S.zoom_cap > 0 && zoom > S.zoom_cap) { zoom = (int16_t)S.zoom_cap; S.zoom_capped = 8; }     /* mira VR: el aumento va en el visor del arma, no en la cabeza */
    int sc = S.cfg.engine_h > 0 ? (int)((int64_t)zoom * S.cfg.engine_h / 320) : zoom;
    if (sc < 16) sc = 16;
    if (sc > 32767) sc = 32767;
    p->screen = (int16_t)sc;
    p->yaw_deg = view; p->pitch_deg = pitch; p->theta_play_deg = wrap360(view - p_yaw);
    memcpy(p->head, h, sizeof h);
    memcpy(p->p_ref, S.p_ref, sizeof S.p_ref);

    S.view_yaw = wrap360(view_real); S.theta_body = b.body_yaw;   /* el rumbo del mando sigue la mirada real, no la prevista */
    S.latched = 1;
    S.st.vr_builds++;
    return 1;
}

void vrcam_commit(const uint8_t *ram, const VrCamPlan *p) {
    VrCamRecord *r = &S.rec[(S.serial + 1) & 31];
    memset(r, 0, sizeof *r);
    r->serial = ++S.serial;
    r->active = p && S.latched;
    uint32_t eye = VC_CHANNEL_MAIN + BCH_CAM;
    for (int i = 0; i < 3; i++) {
        r->eye[i] = rd_i32(ram, eye + 0x14 + 4 * i);
        for (int j = 0; j < 3; j++) r->M[3 * i + j] = (float)rd_s16(ram, eye + (uint32_t)(3 * i + j) * 2) / 4096.0f;
    }
    r->screen = rd_s16(ram, VC_CHANNEL_MAIN + BCH_SCREEN);
    r->units_per_m = S.cfg.units_per_m * (S.cfg.calibrate && S.calib.scale > 0 ? S.calib.scale : 1.0f);
    r->turn_total_deg = S.turn_total;
    r->epoch = S.st.recenters;
    r->vbl = S.vb;
    if (r->active) {
        r->yaw_deg = p->yaw_deg; r->pitch_deg = p->pitch_deg; r->theta_play_deg = p->theta_play_deg;
        memcpy(r->p_ref, p->p_ref, sizeof r->p_ref);
        memcpy(r->head, p->head, sizeof r->head);
    }
}

const VrCamRecord *vrcam_last_record(void) { return S.serial ? &S.rec[S.serial & 31] : NULL; }

void vrcam_note_draw(uint32_t seq_begin, uint32_t seq_end) {
    S.draw_n++;
    uint32_t sr = S.serial > (uint32_t)S.cfg.draw_lag ? S.serial - (uint32_t)S.cfg.draw_lag : 0;
    S.draw[S.draw_n & 63].s0 = seq_begin; S.draw[S.draw_n & 63].s1 = seq_end; S.draw[S.draw_n & 63].serial = sr;
}
uint32_t vrcam_draw_count(void) { return S.draw_n; }
const VrCamRecord *vrcam_seq_record(uint32_t seq) {
    for (uint32_t k = 0; k < 64 && k < S.draw_n; k++) {
        uint32_t n = S.draw_n - k;
        uint32_t s0 = S.draw[n & 63].s0, s1 = S.draw[n & 63].s1, sr = S.draw[n & 63].serial;
        if ((int32_t)(seq - s0) >= 0 && (int32_t)(seq - s1) < 0) {
            if (!sr || S.serial - sr >= 32) return NULL;
            const VrCamRecord *r = &S.rec[sr & 31];
            return r->serial == sr ? r : NULL;
        }
    }
    return NULL;
}

int vrcam_hide_head(uint8_t *ram) {
    uint32_t ob = rd_u32(ram, VC_PLAYER_BODY);
    if (!ptr_ok(ob, 8)) return 0;
    uint32_t objs = rd_u32(ram, ob);
    if (!ptr_ok(objs, VC_OBJS_FIRST + VC_OBJ_SIZE * (VC_HEAD_PART + 1))) return 0;
    int n = rd_s16(ram, objs + VC_OBJS_NMODELS);
    if (n <= VC_HEAD_PART || n > VC_MAX_MODELS) return 0;
    wr_s16(ram, objs + VC_OBJS_FIRST + VC_OBJ_SIZE * VC_HEAD_PART + VC_OBJ_BOUND, 0);
    return 1;
}

/* =================================================== re-proyección =================================================== */
static int inv3(const float a[9], float o[9]) {
    float c00 = a[4] * a[8] - a[5] * a[7], c01 = a[5] * a[6] - a[3] * a[8], c02 = a[3] * a[7] - a[4] * a[6];
    float det = a[0] * c00 + a[1] * c01 + a[2] * c02;
    if (fabsf(det) < 1e-6f) return 0;
    float id = 1.0f / det;
    o[0] = c00 * id; o[1] = (a[2] * a[7] - a[1] * a[8]) * id; o[2] = (a[1] * a[5] - a[2] * a[4]) * id;
    o[3] = c01 * id; o[4] = (a[0] * a[8] - a[2] * a[6]) * id; o[5] = (a[2] * a[3] - a[0] * a[5]) * id;
    o[6] = c02 * id; o[7] = (a[1] * a[6] - a[0] * a[7]) * id; o[8] = (a[0] * a[4] - a[1] * a[3]) * id;
    return 1;
}

int vrcam_delta(const VrCamRecord *r, const float h[16], VrCamDelta *o) {
    memset(o, 0, sizeof *o);
    o->A[0] = o->A[4] = o->A[8] = 1;
    if (!r || !r->active || !h) return 0;
    /* Q = inv(M^T): lleva posiciones de vista del motor (V = M^T (W - E)) a mundo relativo al ojo */
    float Mt[9] = {r->M[0], r->M[3], r->M[6], r->M[1], r->M[4], r->M[7], r->M[2], r->M[5], r->M[8]}, Q[9];
    if (!inv3(Mt, Q)) return 0;
    /* T^T: giro mundo -> espacio del usuario, T = R_y(theta_play + 180 grados) (giro antihorario, + Y arriba) */
    float th = (r->theta_play_deg + 180.0f) * (PI_F / 180.0f), cs = cosf(th), sn = sinf(th);
    float Tt[9] = {cs, 0, -sn, 0, 1, 0, sn, 0, cs};       /* T = [[cs,0,sn],[0,1,0],[-sn,0,cs]] -> T^T */
    /* Rh^T con columnas de la cabeza: derecha, arriba, atrás */
    float Rht[9] = {h[0], h[1], h[2], h[4], h[5], h[6], h[8], h[9], h[10]};
    /* A = D Rh^T T^T Q  (D = diag(1,-1,-1)) */
    float X[9], Y[9];
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) {
        X[3 * i + j] = Tt[3 * i] * Q[j] + Tt[3 * i + 1] * Q[3 + j] + Tt[3 * i + 2] * Q[6 + j];
    }
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) {
        Y[3 * i + j] = Rht[3 * i] * X[j] + Rht[3 * i + 1] * X[3 + j] + Rht[3 * i + 2] * X[6 + j];
    }
    const float D[3] = {1, -1, -1};
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) o->A[3 * i + j] = D[i] * Y[3 * i + j];
    /* b = -D Rh^T (p - p_ref) * unidades por metro */
    float dp[3] = {h[12] - r->p_ref[0], h[13] - r->p_ref[1], h[14] - r->p_ref[2]};
    for (int i = 0; i < 3; i++)
        o->b[i] = -D[i] * (Rht[3 * i] * dp[0] + Rht[3 * i + 1] * dp[1] + Rht[3 * i + 2] * dp[2]) * r->units_per_m;
    return 1;
}

void vrcam_stats(VrCamStats *s) { *s = S.st; }

/* ================================================ parche del ejecutable ================================================ */
enum { R_HX1, R_NHX, R_HY1, R_NHY, R_PX, R_PX2, R_PY, R_PY2 };
/* Valor y opcode esperados antes del parche: slti (0x0A) / addiu (0x09) / sltiu (0x0B), con el inmediato original. */
static const struct { uint8_t op; int16_t imm; } PATCH_OLD[8] = {
    {0x0A, 161}, {0x0A, -160}, {0x0A, 129}, {0x0A, -128}, {0x09, 192}, {0x0B, 385}, {0x09, 152}, {0x0B, 305}};
static const struct { uint32_t addr; uint8_t role; } PATCH_SITES[] = {
    /* recorte de cajas envolventes (dos copias) */
    {0x80018b40, R_HX1}, {0x80018b48, R_NHX}, {0x80018b54, R_HY1}, {0x80018b5c, R_NHY}, {0x80018b68, R_HX1}, {0x80018b70, R_NHX}, {0x80018b78, R_HY1}, {0x80018b80, R_NHY},
    {0x80018f54, R_HX1}, {0x80018f5c, R_NHX}, {0x80018f64, R_HY1}, {0x80018f6c, R_NHY}, {0x80018f74, R_HX1}, {0x80018f7c, R_NHX}, {0x80018f84, R_HY1}, {0x80018f8c, R_NHY},
    /* salida de la subdivisión */
    {0x800194f4, R_NHX}, {0x80019508, R_HX1}, {0x80019520, R_NHY}, {0x80019528, R_HY1},
    /* marcas de vértice fuera de pantalla al transformar (seis copias) */
    {0x8001e4b8, R_NHX}, {0x8001e4cc, R_HX1}, {0x8001e4e4, R_NHY}, {0x8001e4ec, R_HY1}, {0x8001e510, R_NHX}, {0x8001e524, R_HX1}, {0x8001e53c, R_NHY}, {0x8001e544, R_HY1},
    {0x8001e56c, R_NHX}, {0x8001e580, R_HX1}, {0x8001e598, R_NHY}, {0x8001e5a0, R_HY1}, {0x8001e608, R_NHX}, {0x8001e61c, R_HX1}, {0x8001e634, R_NHY}, {0x8001e63c, R_HY1},
    {0x8001e664, R_NHX}, {0x8001e678, R_HX1}, {0x8001e690, R_NHY}, {0x8001e698, R_HY1}, {0x8001e6c0, R_NHX}, {0x8001e6d4, R_HX1}, {0x8001e6ec, R_NHY}, {0x8001e6f4, R_HY1},
    /* "punto visible" (tres comprobaciones) */
    {0x8001c440, R_PX}, {0x8001c444, R_PX2}, {0x8001c458, R_PY}, {0x8001c45c, R_PY2}, {0x8001c4d8, R_PX}, {0x8001c4dc, R_PX2}, {0x8001c4f0, R_PY}, {0x8001c4f4, R_PY2},
    {0x8001c5e8, R_PX}, {0x8001c5ec, R_PX2}, {0x8001c600, R_PY}, {0x8001c604, R_PY2}};
#define PATCH_NSITES ((int)(sizeof PATCH_SITES / sizeof *PATCH_SITES))
#define PATCH_LIMIT 1020   /* la GTE satura SX/SY en [-1024, 1023] */
#define DIVIDE_SITE 0x8001A11Cu
#define DIVIDE_OLD  0x24020800u
#define DIVIDE_NEW  0x3C027FFFu

/* Sin recorte del motor (equivalente a NOCULL de patch_exe.py): cada sitio es UNA instrucción de rama; se comprueba la palabra original
 * (todo o nada) y se escribe la nueva. Efecto de cada uno: ver patch_exe.py y la bitácora. */
static const struct { uint32_t addr, old, neu; } NOCULL_SITES[] = {
    {0x80018D00u, 0x104000B0u, 0x00000000u},   /* la visibilidad de objetos: objeto con máscara de mapa fuera del mapa actual -> oculto */
    {0x80018D0Cu, 0x104000ADu, 0x100000ADu},   /* la visibilidad de objetos: caja envolvente del objeto (8 esquinas proyectadas) -> siempre dentro */
    {0x800188F8u, 0x104000B3u, 0x100000B3u},   /* la visibilidad por pieza: caja envolvente por pieza -> siempre dentro */
    {0x8001BAE0u, 0x104000D5u, 0x00000000u},   /* el dibujo de primitivas: primitivas del motor con máscara de mapa */
    {0x8001DB10u, 0x10400026u, 0x00000000u},   /* la ordenación: idem al ordenar */
    {0x8001E29Cu, 0x1040000Eu, 0x1000000Eu},   /* etapa de polígono: descarte por códigos de pantalla de los vértices */
};
#define NOCULL_N ((int)(sizeof NOCULL_SITES / sizeof *NOCULL_SITES))

/* Copias de las funciones de visibilidad de objetos y por pieza dentro de las overlays de zona (las demás overlays usan las del ejecutable).
 * Se localizan por firma de 3-4 instrucciones (rama condicional sobre una máscara) y el parche conserva el desplazamiento de la rama:
 *   map : `lhu v0,44(x); nop; beq v0,zero,+3; and v0,v0,rY; beq v0,zero,D; nop`  -> la 2.ª rama pasa a `nop` (equivale a obj-map)
 *   box : `andi v0,x,0x20; beq v0,zero,D; addiu a3,zero,2`                        -> rama incondicional (equivale a obj-box)
 *   part: `andi v0,x,0x10; beq v0,zero,D; addiu v1,zero,2`                         -> rama incondicional (equivale a part-box)
 * La firma `part` exige además `beq s6,zero,..; addu v1,zero,zero` justo antes (`box` y `part` del ejecutable también encajan, pero el
 * llamador solo escanea la ventana de overlays). */
int vrcam_scan_overlay(const uint8_t *ram, uint32_t lo, uint32_t hi, VrCamOvSite *out, int max) {
    int n = 0;
    if (!ram || !out || max <= 0 || lo >= hi || (lo & 3) || (hi & 3) || lo < 0x80000000u || hi > 0x80200000u) return 0;
    for (uint32_t a = lo + 8; a + 16 <= hi && n < max; a += 4) {
        uint32_t w0 = (uint32_t)rd_i32(ram, a), w1 = (uint32_t)rd_i32(ram, a + 4), w2 = (uint32_t)rd_i32(ram, a + 8);
        if (w0 == 0x10400003u && (w1 & 0xFFE0FFFFu) == 0x00401024u && (w2 >> 16) == 0x1040u && (uint32_t)rd_i32(ram, a + 12) == 0 &&
            (((uint32_t)rd_i32(ram, a - 8)) & 0xFC1FFFFFu) == 0x9402002Cu && (uint32_t)rd_i32(ram, a - 4) == 0) {
            out[n].addr = a + 8; out[n].old = w2; out[n].neu = 0; out[n].kind = 0; n++;
        } else if ((w0 & 0xFC1FFFFFu) == 0x30020020u && (w1 >> 16) == 0x1040u && w2 == 0x24070002u) {
            out[n].addr = a + 4; out[n].old = w1; out[n].neu = 0x10000000u | (w1 & 0xFFFFu); out[n].kind = 1; n++;
        } else if ((w0 & 0xFC1FFFFFu) == 0x30020010u && (w1 >> 16) == 0x1040u && w2 == 0x24030002u &&
                   ((uint32_t)rd_i32(ram, a - 4) == 0x00001821u) && (((uint32_t)rd_i32(ram, a - 8)) >> 16) == 0x12C0u) {
            out[n].addr = a + 4; out[n].old = w1; out[n].neu = 0x10000000u | (w1 & 0xFFFFu); out[n].kind = 2; n++;
        }
    }
    return n;
}

void vrcam_patch_default(VrCamPatch *p) { p->engine_h = 160; p->hfov_deg = 140; p->vfov_deg = 120; p->no_divide = 1; p->wide_points = 0; p->no_cull = VRCAM_NOCULL_ALL; }

int vrcam_patch_ram(uint8_t *ram, const VrCamPatch *pp) {
    VrCamPatch d; if (!pp) { vrcam_patch_default(&d); pp = &d; }
    if (!ram || pp->engine_h < 16 || !(pp->hfov_deg >= 20 && pp->hfov_deg <= 180) || !(pp->vfov_deg >= 20 && pp->vfov_deg <= 180)) return -1;   /* (!(..) también rechaza NaN) */
    for (int i = 0; i < PATCH_NSITES; i++) {
        uint32_t w = rd_u32(ram, PATCH_SITES[i].addr);
        int op = (int)(w >> 26), imm = (int16_t)(w & 0xFFFF);
        if (op != PATCH_OLD[PATCH_SITES[i].role].op || imm != PATCH_OLD[PATCH_SITES[i].role].imm) return -1;
    }
    for (int i = 0; i < NOCULL_N; i++) if (rd_u32(ram, NOCULL_SITES[i].addr) != NOCULL_SITES[i].old) return -1;   /* ídem: se comprueban siempre */
    if (rd_u32(ram, DIVIDE_SITE) != DIVIDE_OLD) return -1;   /* se comprueba siempre: confirma la edición del EXE aunque no se use */
    double dx = (double)pp->engine_h * tan((double)pp->hfov_deg * (PI_F / 360.0));
    double dy = (double)pp->engine_h * tan((double)pp->vfov_deg * (PI_F / 360.0));
    int hx = (pp->hfov_deg >= 179.0f || !(dx < PATCH_LIMIT)) ? PATCH_LIMIT : (int)lround(dx);   /* sin conversión de valores enormes */
    int hy = (pp->vfov_deg >= 179.0f || !(dy < PATCH_LIMIT)) ? PATCH_LIMIT : (int)lround(dy);
    if (hx > PATCH_LIMIT) hx = PATCH_LIMIT;
    if (hy > PATCH_LIMIT) hy = PATCH_LIMIT;
    if (hx < 160) hx = 160;
    if (hy < 128) hy = 128;
    const int nv[8] = {hx + 1, -hx, hy + 1, -hy, hx, 2 * hx + 1, hy, 2 * hy + 1};
    int n = 0;
    for (int i = 0; i < PATCH_NSITES; i++) {
        if (PATCH_SITES[i].role >= R_PX && !pp->wide_points) continue;   /* "punto visible": regla del juego, intacta por defecto */
        uint32_t a = PATCH_SITES[i].addr, w = rd_u32(ram, a);
        w = (w & 0xFFFF0000u) | ((uint32_t)nv[PATCH_SITES[i].role] & 0xFFFFu);
        memcpy(ram + (a & RAM_MASK), &w, 4);
        n++;
    }
    if (pp->no_divide) { uint32_t w = DIVIDE_NEW; memcpy(ram + (DIVIDE_SITE & RAM_MASK), &w, 4); n++; }
    for (int i = 0; i < NOCULL_N; i++) if ((pp->no_cull >> i) & 1) { uint32_t w = NOCULL_SITES[i].neu; memcpy(ram + (NOCULL_SITES[i].addr & RAM_MASK), &w, 4); n++; }
    return n;
}
