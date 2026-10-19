/* vr_heading: ver vr_heading.h. */
#include "vr_heading.h"
#include <math.h>
#include <string.h>
#include "vr_camera.h"

#define RAM_MASK 0x1FFFFFu
/* Rumbo del jugador en el bloque de control (s16, 4096 = vuelta): +0x0A es el que lee el dibujado/apuntado; +0x2A (rumbo del fotograma anterior) y +0x4E (rumbo
 * deseado) los usa el control del personaje, que cada fotograma de juego acerca el rumbo al deseado (factor 0,25; con el mando en reposo el deseado es el
 * rumbo de la cámara original, 2048). Medido en el juego: escribir solo +0x0A lo deshace el siguiente fotograma; escribir los tres lo conserva. */
#define HD_YAW   (BC_ROT + 2)
#define HD_PREV  0x2A
#define HD_GOAL  0x4E

typedef struct {
    VrHeadingConfig cfg;
    float pending;          /* giro del stick por aplicar al personaje (grados) */
    float unseen;           /* giros ya aplicados a la vista (vrcam_turn) que la cámara del motor aún no ha construido (la mirada que lee vrhead_frame es la del último fotograma construido) */
    uint32_t builds;        /* vr_builds de la cámara en el último vrhead_frame */
    float body, off;        /* último rumbo leído y desvío */
    float last_view;        /* mirada del VBlank anterior, para saber si la cabeza está quieta */
    float still_s;
    int have_last;
    float carry;            /* resto fraccionario del rumbo (unidades de 4096) que no cabe en el s16 */
} Hd;
static Hd H;
static VrHeadingConfig DEF;
static int def_set;
void vrhead_set_default(const VrHeadingConfig *c) { if (c) { DEF = *c; def_set = 1; } else def_set = 0; }

static float wrap180(float a) { a = fmodf(a + 180.0f, 360.0f); if (a < 0) a += 360.0f; return a - 180.0f; }
static float wrap360(float a) { a = fmodf(a, 360.0f); if (a < 0) a += 360.0f; return a; }

void vrhead_config_default(VrHeadingConfig *c) {
    memset(c, 0, sizeof *c);
    c->enabled = 1; c->margin_deg = 45.0f; c->follow_dps = 300.0f;
    c->relax_deg = 12.0f; c->relax_s = 1.2f; c->relax_dps = 45.0f; c->frame_dt_s = 0.02f;
}
void vrhead_init(const VrHeadingConfig *c) {
    memset(&H, 0, sizeof H);
    if (c) H.cfg = *c; else if (def_set) H.cfg = DEF; else vrhead_config_default(&H.cfg);
    if (!(H.cfg.margin_deg >= 0 && H.cfg.margin_deg <= 170)) H.cfg.margin_deg = 45.0f;
    if (!(H.cfg.follow_dps > 0)) H.cfg.follow_dps = 300.0f;
    if (!(H.cfg.relax_dps > 0)) H.cfg.relax_dps = 45.0f;
    if (!(H.cfg.frame_dt_s > 0 && H.cfg.frame_dt_s < 0.2f)) H.cfg.frame_dt_s = 0.02f;
    if (!(H.cfg.relax_deg >= 0 && H.cfg.relax_deg < H.cfg.margin_deg)) H.cfg.relax_deg = 0.0f;
}
const VrHeadingConfig *vrhead_config(void) { return &H.cfg; }

void vrhead_turn(float deg) {
    if (!isfinite(deg)) return;
    vrcam_turn(deg);
    if (H.cfg.enabled) { H.pending += deg; H.pending = wrap180(H.pending); }
}

float vrhead_follow_step(const VrHeadingConfig *c, float off, float still_s, float dt) {
    float a = fabsf(off), sg = off < 0 ? -1.0f : 1.0f, step = 0.0f;
    if (a > c->margin_deg) {
        step = a - c->margin_deg;
        float mx = c->follow_dps * dt;
        if (step > mx) step = mx;
    } else if (c->relax_deg > 0 && a > c->relax_deg && still_s >= c->relax_s) {
        step = a - c->relax_deg;
        float mx = c->relax_dps * dt;
        if (step > mx) step = mx;
    }
    return sg * step;
}

float vrhead_body_yaw(void) { return H.body; }
float vrhead_offset_deg(void) { return H.off; }

int vrhead_frame(uint8_t *ram, const BridgeState *st, int walking) {
    if (!ram || !st || !H.cfg.enabled) { H.pending = 0; return 0; }
    if (!vrcam_active() || !st->ctl_addr || !(st->valid & BRIDGE_OK_PLAYER_CTL)) { H.pending = 0; H.have_last = 0; H.still_s = 0; return 0; }
    uint32_t cb = st->ctl_addr & RAM_MASK, a = cb + HD_YAW;
    if ((st->ctl_addr >> 29) != 4u || (st->ctl_addr & 1u) || cb + 0x50 > BRIDGE_RAM_SIZE || cb < 0x10000u) return 0;
    int16_t raw; memcpy(&raw, ram + a, 2);
    float body = (float)((uint16_t)raw & 4095u) * (360.0f / 4096.0f);
    /* mirada efectiva: la del último fotograma de cámara + los giros del stick posteriores a él (si no, el seguimiento lee una mirada vieja y deshace parte del giro) */
    VrCamStats cs; vrcam_stats(&cs);
    if (cs.vr_builds != H.builds) { H.builds = cs.vr_builds; H.unseen = 0.0f; }
    float view = wrap360(vrcam_view_yaw_deg() + H.unseen + H.pending);
    H.unseen = wrap180(H.unseen + H.pending);
    float dt = H.cfg.frame_dt_s;

    /* cabeza quieta: la mirada casi no cambió desde el VBlank anterior (los giros del stick cuentan: mueven la mirada) */
    if (H.have_last && fabsf(wrap180(view - H.last_view)) < 15.0f * dt) H.still_s += dt; else H.still_s = 0.0f;
    H.last_view = view; H.have_last = 1;

    float delta = H.pending;                         /* giro del stick: inmediato (por pasos) o continuo (suave) */
    H.pending = 0.0f;
    float off = wrap180(view - (body + delta));      /* la mirada ya incluye el giro del stick (vrcam_turn) */
    if (!walking) delta += vrhead_follow_step(&H.cfg, off, H.still_s, dt);

    H.body = body; H.off = off;
    if (delta == 0.0f && H.carry == 0.0f) return 0;
    float units = delta * (4096.0f / 360.0f) + H.carry;
    long iu = lrintf(units);
    H.carry = units - (float)iu;
    if (iu == 0) return 0;
    uint16_t nv = (uint16_t)(((long)(raw & 4095) + iu) & 4095);
    memcpy(ram + a, &nv, 2);
    memcpy(ram + cb + HD_PREV, &nv, 2);
    memcpy(ram + cb + HD_GOAL, &nv, 2);
    H.body = wrap360((float)nv * (360.0f / 4096.0f));
    H.off = wrap180(view - H.body);
    return 1;
}
