/* vrbodyg: gestos de cuerpo (vr/input/vr_gesture) sobre el juego real. Convierte manos/cabeza del espacio del usuario al mundo del
 * motor con la misma correspondencia que el disparo (VrFireMap), lee el puente y añade/retira botones del mando antes de que lo
 * vea el juego. Hilo de emulación, una vez por VBlank (vrgame_pad). Ver vr_gesture.h para los gestos y bitacora para lo medido. */
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bridge.h"
#include "host.h"
#include "vr_camera.h"
#include "vr_body.h"
#include "vr_fire.h"
#include "vr_gesture.h"
#include "vrgame.h"
#include "vrhost.h"
#include "vr_melee_rt.h"

static vrg_t G;
static int inited, en_all = 1, en_mask = 0xF;      /* bits: 1 golpe, 2 agarre, 4 pared, 8 asomarse */
static unsigned n_events[16];

void mv_vrgame_set_gestures(int32_t enable, int32_t mask) {
    en_all = enable != 0; en_mask = (int)mask & 0xF;
}
void vrbodyg_reset(void) { if (inited) vrg_reset(&G); }
unsigned vrbodyg_event_count(int ev) { return ev >= 0 && ev < 16 ? n_events[ev] : 0; }

static int act_holds(int a) { return a == 7 || a == 38 || a == 124; }

uint16_t vrbodyg_pad(uint16_t pad, const BridgeState *bs, int right_handed) {
    if (!inited) { vrg_init(&G); inited = 1; }
    G.cfg.enable = en_all; G.cfg.punch = !!(en_mask & 1); G.cfg.grab = !!(en_mask & 2); G.cfg.knock = !!(en_mask & 4); G.cfg.peek = !!(en_mask & 8);
    G.cfg.right_handed = right_handed;
    int core = bs && (bs->valid & BRIDGE_OK_ALL_CORE) == BRIDGE_OK_ALL_CORE && bs->has_player;
    /* R2 = DISPARAR: con arma equipada es el botón de ataque del juego; sin arma no hace nada. El cuadrado físico sigue como en el original. */
    if (pad & VRG_R2) {
        pad = (uint16_t)(pad & ~VRG_R2);
        if (core && bs->weapon >= 0) pad |= VRG_SQUARE;
    }
    vrg_in_t in;
    memset(&in, 0, sizeof in);
    VrFireMap map;
    vrfire_map_from_cam(&map, vrcam_last_record());
    float hm[16];
    in.active = core && bs->in_control && vrcam_active() && map.valid && mv_vr_get_head(hm, NULL);
    in.weapon = -1;
    in.weapon_busy = vrgame_weapon_engaged();
    in.stick_dom = (pad & VRG_R3) != 0; in.stick_off = (pad & VRG_L3) != 0;
    in.dt = 0.02f;                                   /* el gancho corre una vez por VBlank (50 Hz virtuales) */
    int domside = right_handed ? 1 : 0;
    in.fist_btn[domside] = (pad & VRG_R1) != 0; in.fist_btn[1 - domside] = (pad & VRG_L1) != 0;   /* R1/L1 = botones del puño */
    VrmFrame mf; memset(&mf, 0, sizeof mf); mf.punch_side = mf.knock_side = -1;
    vrg_out_t out;
    if (in.active) {
        float w[3];
        vrfire_pos_to_world(&map, &hm[12], w);
        for (int i = 0; i < 3; i++) in.head[i] = w[i] * 0.001f;
        /* cantidades físicas del usuario (espacio de seguimiento): cabeza, delante de la cabeza y mano relativa a la cabeza */
        memcpy(in.head_u, &hm[12], sizeof in.head_u);
        in.head_fwd[0] = -hm[8]; in.head_fwd[1] = -hm[10];
        for (int s = 0; s < 2; s++) {
            float p[3], gr, tg;
            in.tracked[s] = vrgame_hand_raw(s, p, &gr, &tg, &in.hand_time[s]);
            if (in.tracked[s]) for (int i = 0; i < 3; i++) { in.hrel[s][i] = p[i] - hm[12 + i]; in.hand_u[s][i] = p[i]; }
            if (in.tracked[s]) {
                vrfire_pos_to_world(&map, p, w);
                for (int i = 0; i < 3; i++) { in.hand[s][i] = w[i] * 0.001f; mf.fist[s][i] = w[i]; }
                in.grip[s] = gr; in.trig[s] = tg;
                mf.fist_ok[s] = 1; mf.fist_closed[s] = in.fist_btn[s];
                float dmm = mv_vrhost_wall_distance(w, 400.0f);     /* geometría estática de la escena (la de la colisión de la cabeza) */
                in.wall_valid[s] = dmm >= 0.0f; in.wall_dist[s] = dmm >= 0.0f ? dmm * 0.001f : 1.0f;
            }
        }
        in.weapon = bs->weapon; in.stance = bs->stance; in.on_wall = bs->on_wall; in.in_box = bs->in_box; in.in_water = bs->in_water;
        in.holding = act_holds(bs->player_action); in.life = bs->life;
        in.body[0] = bs->ctl_pos.x * 0.001f; in.body[1] = bs->ctl_pos.y * 0.001f; in.body[2] = bs->ctl_pos.z * 0.001f;
        in.yaw = (float)bs->ctl_rot.y * (6.2831853f / 4096.0f);
        in.view_yaw = vrcam_view_yaw_deg() * (3.14159265f / 180.0f);
        for (int i = 0; i < bs->n_enemies && in.n_en < VRG_MAX_EN; i++) {
            const BridgeEnemy *e = &bs->enemies[i];
            in.en[in.n_en].p[0] = e->pos.x * 0.001f; in.en[in.n_en].p[1] = e->pos.y * 0.001f; in.en[in.n_en].p[2] = e->pos.z * 0.001f;
            in.en[in.n_en].yaw = (float)e->rot.y * (6.2831853f / 4096.0f);
            in.en[in.n_en].down = e->down; in.en[in.n_en].vital = e->vital;
            in.n_en++;
        }
    }
    vrg_update(&G, &in, &out);
    {   /* registro [gest] (va al log del visor): cada intento con su motivo, y cada evento */
        static const char *evn[] = {"", "golpe", "agarre", "apreton", "soltar", "pared", "asomar", "pared2", "dano", "impacto", "ventana"};
        static unsigned vb;
        vb++;
        if (out.gest.valid) {
            const vrg_gest_t *q = &out.gest;
            fprintf(stderr, "[gest] vb=%u mano=%d %s v=%.2f m/s adelante=%.2f radial=%.2f alcance=%.2f enemigo=%.2f m grip=%.2f puno=%d\n", vb, q->side, q->reason, q->speed, q->fcos, q->rcos, q->reach, q->dist, q->grip, q->fist);
        }
        if (out.grab_why) fprintf(stderr, "[gest] vb=%u agarre %s mano=%d distancia=%.2f m (cuello/espalda, limite 0,30)\n", vb, out.grab_why, out.grab_side_try, out.grab_dist);
        if (out.knock_why) fprintf(stderr, "[gest] vb=%u toque en la pared %s mano=%d v=%.2f m/s distancia=%.3f m\n", vb, out.knock_why, out.knock_try_side, out.knock_speed, out.knock_dist);
        if (out.event > 0 && out.event < 11) fprintf(stderr, "[gest] vb=%u evento=%s armas_libres=%d\n", vb, evn[out.event], in.weapon < 0);
    }
    /* decisiones del cuerpo -> motor (ganchos de vr/input/vr_melee): ninguna pasa por botones */
    mf.punch_side = out.punch_side; mf.knock_side = out.knock_side;
    mf.grab_start = out.grab_start; mf.grab_side = out.grab_side_try >= 0 && out.grab_start ? out.grab_side_try : out.grab_side;
    mf.hold = out.grab_hold; mf.squeeze = out.squeeze;
    {
        static int warned;
        if (!vrmelee_rt_installed()) {
            if ((mf.punch_side >= 0 || mf.grab_start || mf.knock_side >= 0) && !warned) { warned = 1; fprintf(stderr, "[gest] las acciones del motor no están enganchadas (ejecutable no reconocido): los gestos no actúan\n"); }
        } else vrmelee_rt_frame(&mf);
    }
    {   /* el cuerpo del personaje: manos pegadas al cuello del enemigo sujeto (en el mundo del motor, mm) */
        VrBodyGrab gb; memset(&gb, 0, sizeof gb);
        if (in.active && bs) {
            float px = (float)bs->ctl_pos.x, pz = (float)bs->ctl_pos.z;
            if (out.grab_side >= 0 && out.grab_en >= 0 && out.grab_en < bs->n_enemies) {
                const BridgeEnemy *e = &bs->enemies[out.grab_en];
                float dx = e->pos.x - px, dz = e->pos.z - pz, dd = sqrtf(dx * dx + dz * dz);
                if (dd > 1.0f) {
                    float fx = dx / dd, fz = dz / dd;
                    gb.active = 1; gb.neck[0] = e->pos.x; gb.neck[1] = (float)e->pos.y + 450.0f; gb.neck[2] = e->pos.z;
                    gb.left[0] = fz; gb.left[1] = 0; gb.left[2] = -fx;      /* izquierda del personaje mirando al enemigo */
                }
            }
        }
        vrbody_set_grab(&gb);
    }
    if (out.event > 0 && out.event < 16) n_events[out.event]++;
    if (out.haptic[0] > 0 || out.haptic[1] > 0) { vrgame_haptic(0, out.haptic[0]); vrgame_haptic(1, out.haptic[1]); }
    return (uint16_t)((pad & ~out.mask) | out.press);
}
