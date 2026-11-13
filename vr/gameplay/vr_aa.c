/* vr_aa: ver vr_aa.h. Núcleo sin dependencias del runtime (se prueba con RAM sintética). */
#include <math.h>
#include <string.h>
#include "vr_aa.h"

#define RAM_MASK 0x1FFFFFu
#define PI_F 3.14159265358979f

/* Direcciones/offsets del juego (edición europea; vr_fire_names.json). */
#define LOCK_TARGET_PTR 0x800BF080u   /* blanco seguido (lo lee el misil) */
#define LA_TARGET       0x30u         /* actor del buscador: blanco seguido */
#define LA_FRAMES       0x34u         /* fotogramas seguidos con el mismo blanco */
#define LA_LOST         0x38u         /* fotogramas sin candidato */
#define LA_NCAND        0x21ACu       /* candidatos en pantalla */
#define ACTOR_LIFE      4             /* VBlanks sin paso del buscador tras los que se da por muerto */

static int ptr_ok(uint32_t a, uint32_t n) { return a >= 0x80010000u && a < 0x80200000u && n <= 0x200000u && (a & RAM_MASK) + n <= RAM_MASK + 1u; }
static uint32_t rd_u32(const uint8_t *r, uint32_t a) { uint32_t v; memcpy(&v, r + (a & RAM_MASK), 4); return v; }
static int32_t rd_s32(const uint8_t *r, uint32_t a) { return (int32_t)rd_u32(r, a); }

static struct {
    VrAaCfg cfg;
    int aiming, run;                  /* gesto vigente / VBlanks consecutivos cumpliéndolo */
    int scope_on; VrFireMap map; WPose sight;
    VrAaLock lk; int age;
    float last_h;
} S;

void vraa_cfg_default(VrAaCfg *c) {
    memset(c, 0, sizeof *c);
    c->enter_m = 0.30f; c->leave_m = 0.42f; c->axis_deg = 40.0f; c->sight_back_m = 0.0f; c->on_vbl = 3; c->h_scale = 1.0f;
}
void vraa_init(const VrAaCfg *c) {
    memset(&S, 0, sizeof S);
    if (c) S.cfg = *c; else vraa_cfg_default(&S.cfg);
    if (!(S.cfg.enter_m > 0.02f)) S.cfg.enter_m = 0.02f;
    if (!(S.cfg.leave_m > S.cfg.enter_m)) S.cfg.leave_m = S.cfg.enter_m + 0.05f;
    if (!(S.cfg.axis_deg > 1.0f) || S.cfg.axis_deg > 90.0f) S.cfg.axis_deg = 40.0f;
    if (!(S.cfg.sight_back_m >= 0.0f) || S.cfg.sight_back_m > 0.5f) S.cfg.sight_back_m = 0.10f;
    if (S.cfg.on_vbl < 1) S.cfg.on_vbl = 1;
    if (!(S.cfg.h_scale >= 0.0625f) || S.cfg.h_scale > 16.0f) S.cfg.h_scale = 1.0f;     /* (!(..) también rechaza NaN) */
}
void vraa_set_h_scale(float k) { S.cfg.h_scale = (k >= 0.0625f && k <= 16.0f) ? k : 1.0f; }
const VrAaCfg *vraa_config(void) { return &S.cfg; }

/* ================================================== gesto ================================================== */
static int finite_pose(const WPose *p) {
    return isfinite(p->p.x) && isfinite(p->p.y) && isfinite(p->p.z) && isfinite(p->q.x) && isfinite(p->q.y) && isfinite(p->q.z) && isfinite(p->q.w);
}
/* delante (-Z local) y arriba (+Y local) de una pose; false si el cuaternión no vale */
static int pose_axes(const WPose *p, float f[3], float u[3]) {
    WQuat q = p->q;
    float n = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (!(n > 1e-6f) || !isfinite(n)) return 0;
    q.x /= n; q.y /= n; q.z /= n; q.w /= n;
    f[0] = -2 * (q.x * q.z + q.w * q.y); f[1] = -2 * (q.y * q.z - q.w * q.x); f[2] = -(1 - 2 * (q.x * q.x + q.y * q.y));
    u[0] = 2 * (q.x * q.y - q.w * q.z); u[1] = 1 - 2 * (q.x * q.x + q.z * q.z); u[2] = 2 * (q.y * q.z + q.w * q.x);
    return 1;
}

int vraa_gesture(const WPose *head, const WPose *sight, int equipped) {
    int ok = 0;
    if (equipped && head && sight && finite_pose(head) && finite_pose(sight)) {
        float hf[3], hu[3], sf[3], su[3];
        if (pose_axes(head, hf, hu) && pose_axes(sight, sf, su)) {
            /* el ojo se apoya detrás del punto de referencia (la cara de la pantalla LED; sight_back_m = 0), sobre el eje del tubo */
            float rear[3] = {sight->p.x - sf[0] * S.cfg.sight_back_m, sight->p.y - sf[1] * S.cfg.sight_back_m, sight->p.z - sf[2] * S.cfg.sight_back_m};
            float dx = head->p.x - rear[0], dy = head->p.y - rear[1], dz = head->p.z - rear[2];
            float d = sqrtf(dx * dx + dy * dy + dz * dz);
            float cs = hf[0] * sf[0] + hf[1] * sf[1] + hf[2] * sf[2];
            float lim = cosf(S.cfg.axis_deg * (PI_F / 180.0f));
            if (S.aiming) ok = d < S.cfg.leave_m && cs > lim * 0.5f;           /* salir: más tolerante */
            else ok = d < S.cfg.enter_m && cs > lim;
        }
    }
    if (ok) { if (S.run < 1 << 20) S.run++; } else S.run = 0;
    if (S.aiming) { if (!ok) S.aiming = 0; }
    else if (S.run >= S.cfg.on_vbl) S.aiming = 1;
    return S.aiming;
}
int vraa_aiming(void) { return S.aiming; }

/* ================================================== vista del visor ================================================== */
static int16_t q12(float v) { float r = v * 4096.0f; if (r > 32767.0f) r = 32767.0f; if (r < -32768.0f) r = -32768.0f; return (int16_t)lrintf(r); }
static int32_t rc32(double v) { if (v > 1.0e9) v = 1.0e9; if (v < -1.0e9) v = -1.0e9; return (int32_t)llround(v); }

int vraa_view_matrix(const VrFireMap *map, const WPose *sight, VrFireMat *out) {
    if (!map || !map->valid || !sight || !out || !(map->units_per_m > 0) || !finite_pose(sight)) return 0;
    float fu[3], uu[3], f[3], u[3];
    if (!pose_axes(sight, fu, uu)) return 0;
    vrfire_dir_to_world(map, fu, f);
    vrfire_dir_to_world(map, uu, u);
    /* derecha = delante x arriba (mundo del motor: Y arriba, +X a la izquierda del +Z); abajo = -arriba, ortonormalizado */
    float r[3] = {f[1] * u[2] - f[2] * u[1], f[2] * u[0] - f[0] * u[2], f[0] * u[1] - f[1] * u[0]};
    float rn = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    if (!(rn > 1e-4f)) return 0;
    for (int i = 0; i < 3; i++) r[i] /= rn;
    float d[3] = {f[1] * r[2] - f[2] * r[1], f[2] * r[0] - f[0] * r[2], f[0] * r[1] - f[1] * r[0]};   /* abajo = delante x derecha */
    memset(out, 0, sizeof *out);
    const float *row[3] = {r, d, f};
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) out->m[3 * i + j] = q12(row[i][j]);
    float p[3] = {sight->p.x, sight->p.y, sight->p.z}, w[3];
    vrfire_pos_to_world(map, p, w);
    int32_t e[3] = {rc32(w[0]), rc32(w[1]), rc32(w[2])};
    for (int i = 0; i < 3; i++)
        out->t[i] = rc32(-((double)out->m[3 * i] * e[0] + (double)out->m[3 * i + 1] * e[1] + (double)out->m[3 * i + 2] * e[2]) / 4096.0);
    return 1;
}

void vraa_set_scope(int on, const VrFireMap *map, const WPose *sight) {
    S.scope_on = 0;
    if (!on || !map || !map->valid || !sight || !finite_pose(sight)) return;
    S.map = *map; S.sight = *sight; S.scope_on = 1;
}
int vraa_take_view(VrFireMat *out) {
    if (!S.scope_on || !out) return 0;
    return vraa_view_matrix(&S.map, &S.sight, out);
}

/* ================================================== estado del buscador ================================================== */
uint32_t vraa_game_target(const uint8_t *ram) { return ram ? rd_u32(ram, LOCK_TARGET_PTR) : 0; }

void vraa_after_step(const uint8_t *ram, uint32_t actor, int swapped) {
    if (!ram || !ptr_ok(actor, LA_NCAND + 4u)) return;
    VrAaLock *l = &S.lk;
    l->alive = 1; l->actor = actor; S.age = 0;
    l->target = rd_u32(ram, actor + LA_TARGET);
    l->frames = rd_s32(ram, actor + LA_FRAMES);
    l->lost = rd_s32(ram, actor + LA_LOST);
    l->candidates = rd_s32(ram, actor + LA_NCAND);
    if (!l->target) l->frames = 0;
    l->converged = l->target && l->frames >= VRAA_BOX_FRAMES;
    l->steps++;
    if (swapped) l->swapped++;
}
void vraa_tick(void) {
    if (S.age < 1 << 20) S.age++;
    if (S.age > ACTOR_LIFE && S.lk.alive) {
        int steps = S.lk.steps, sw = S.lk.swapped;
        memset(&S.lk, 0, sizeof S.lk);
        S.lk.steps = steps; S.lk.swapped = sw;
    }
}
void vraa_lock_info(VrAaLock *out) { *out = S.lk; }

void vraa_sight(VrAaSight *o) {
    memset(o, 0, sizeof *o);
    o->active = S.scope_on && S.lk.alive;
    if (S.scope_on) o->pose = S.sight;
    o->locked = S.lk.alive && S.lk.target != 0;
    o->converged = S.lk.converged;
    if (S.last_h > 1.0f) o->half_fov_deg = atanf(160.0f / S.last_h) * (180.0f / PI_F);
}
void vraa_note_h(float h) { S.last_h = h; }
int vraa_view_focal(int engine_focal) {
    float h = (float)engine_focal * S.cfg.h_scale;
    if (!(h >= 16.0f)) h = 16.0f;
    if (h > 32767.0f) h = 32767.0f;
    return (int)lrintf(h);
}
