/* vrbodyg: gestos de cuerpo (vr/input/vr_gesture) sobre el juego real. Convierte manos/cabeza del espacio del usuario al mundo del
 * motor con la misma correspondencia que el disparo (VrFireMap), lee el puente y añade/retira botones del mando antes de que lo
 * vea el juego. Hilo de emulación, una vez por VBlank (vrgame_pad). Ver vr_gesture.h para los gestos y bitacora para lo medido. */
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bridge.h"
#include "host.h"
#include "vr_camera.h"
#include "vr_body.h"
#include "vr_fire.h"
#include "vr_gesture.h"
#include "vr_hzd.h"
#include "vr_hit.h"
#include "vrgame.h"
#include "vrhost.h"
#include "vr_melee_rt.h"

static vrg_t G;
static int inited, en_all = 1, en_mask = 0xF;      /* bits: 1 golpe, 2 agarre, 4 pared, 8 asomarse */
static unsigned n_events[16];

void mv_vrgame_set_gestures(int32_t enable, int32_t mask) {
    en_all = enable != 0; en_mask = (int)mask & 0xF;
}
static _Atomic int32_t g_menu_hand = -1;       /* publicado por el hilo de juego (pub_menu_hand); el hilo de render solo lo lee */
static void pub_menu_hand(void) { int h = inited ? G.menu_hold : 0; atomic_store(&g_menu_hand, h & 1 ? 0 : (h & 2 ? 1 : -1)); }
static int melee_busy, busy_force;              /* golpe/agarre del cuerpo en el último VBlank (lo lee vrgame c4_remote); busy_force: solo pruebas */
static int chest_in[2];                         /* mano física dentro de SU zona de menú en el último VBlank (lo lee vrgame c4_remote: ahí el grip abre el menú y no detona) */
void vrbodyg_reset(void) { melee_busy = 0; chest_in[0] = chest_in[1] = 0; if (inited) { vrg_reset(&G); pub_menu_hand(); } }
int vrbodyg_melee_busy(void) { return melee_busy || busy_force; }
int vrbodyg_hand_in_chest(int side) { return side >= 0 && side < 2 && chest_in[side]; }
void vrbodyg_test_force_busy(int on) { busy_force = on != 0; }
unsigned vrbodyg_event_count(int ev) { return ev >= 0 && ev < 16 ? n_events[ev] : 0; }
/* mano física que mantiene abierta la ventana de armas/objetos con el gesto del pecho (0 izquierda = L2, 1 derecha = R2); -1 ninguna. Lectura atómica del valor publicado al final de la actualización del mando. */
int32_t mv_vrgame_menu_hand(void) { return atomic_load(&g_menu_hand); }

static int act_holds(int a) { return a == 7 || a == 38 || a == 124; }

/* Bloqueo de disparo con un menú abierto (vr_gesture: vrg_fire_gate): el gatillo del arma de la mano `side` es el real o, con su ventana abierta / cualquier menú abierto / hasta soltarlo tras cerrarse, el último visto
 * antes del bloqueo (la zona del pecho no bloquea: el menú se abre con el grip). Con la mano del arma anula además el disparo en cola. Registro [disparo] bloqueado_menu= en cada cambio. Devuelve VRG_FB_* (0 libre). Una vez por VBlank y mano, tras vrbodyg_pad. */
int vrbodyg_fire_gate(int side, int weapon_hand, float *trig) {
    static int prev[2];
    if (!inited || side < 0 || side > 1 || !trig) return 0;
    const float raw = *trig;
    int why = 0;
    *trig = vrg_fire_gate(&G, side, raw, &why);
    const int canc = why && weapon_hand ? vrfire_cancel_pending() : 0;
    if (why != prev[side] || canc) {
        if (why) fprintf(stderr, "[disparo] bloqueado_menu=%s mano=%d gatillo=%.2f cola_cancelada=%d\n", vrg_fire_block_name(why), side, raw, canc);
        else fprintf(stderr, "[disparo] bloqueado_menu=fin mano=%d gatillo=%.2f\n", side, raw);
        prev[side] = why;
    }
    return why;
}

uint16_t vrbodyg_pad(uint16_t pad, const BridgeState *bs, int right_handed, const uint8_t *ram) {
    if (!inited) { vrg_init(&G); inited = 1; }
    G.cfg.enable = en_all; G.cfg.punch = !!(en_mask & 1); G.cfg.grab = !!(en_mask & 2); G.cfg.knock = !!(en_mask & 4); G.cfg.peek = !!(en_mask & 8);
    G.cfg.right_handed = right_handed;
    int core = bs && (bs->valid & BRIDGE_OK_ALL_CORE) == BRIDGE_OK_ALL_CORE && bs->has_player;
    /* R2 = DISPARAR: con arma equipada es el botón de ataque del juego; sin arma no hace nada. El cuadrado físico sigue como en el original. */
    const uint16_t pad_in = pad;
    const int ctl = core && bs->in_control;          /* capa de JUEGO VR; si no (códec, radio, menús, pausa, escenas, ventana de armas/objetos) capa de MENÚ: mando original sin tocar */
    int r2_fire = 0;                                  /* el cuadrado de este fotograma viene del gatillo (no del botón físico) */
    if ((pad & VRG_R2) && ctl) {
        pad = (uint16_t)(pad & ~VRG_R2);
        if (core && bs->weapon >= 0) { r2_fire = !(pad & VRG_SQUARE); pad |= VRG_SQUARE; }
    }
    vrg_in_t in;
    memset(&in, 0, sizeof in);
    VrFireMap map;
    vrfire_map_from_cam(&map, vrcam_last_record());
    float hm[16];
    in.active = core && bs->in_control && vrcam_active() && map.valid && mv_vr_get_head(hm, NULL);
    in.weapon = -1;
    in.weapon_busy = vrgame_weapon_engaged();
    in.throw_hold = vrgame_throw_hold();
    in.stick_dom = (pad & VRG_R3) != 0; in.stick_off = (pad & VRG_L3) != 0;
    in.dt = 0.02f;                                   /* el gancho corre una vez por VBlank (50 Hz virtuales) */
    int domside = right_handed ? 1 : 0;
    in.fist_btn[domside] = (pad & VRG_R1) != 0; in.fist_btn[1 - domside] = (pad & VRG_L1) != 0;   /* R1/L1 = botones del puño */
    VrmFrame mf; memset(&mf, 0, sizeof mf); mf.punch_side = mf.knock_side = -1;
    vrg_out_t out;
    static VrHitTarget ht[VRHIT_MAX_T];          /* enemigos golpeables con sus cápsulas por pieza (vr_hit) */
    static float fist_prev[2][3]; static int fist_prev_ok[2];
    int hn = 0;
    char wsrc[2] = {'-', '-'};                  /* fuente de la distancia a la pared por mano: c = geometría de la escena (vrhc), h = paredes de colisión del juego (HZD) */
    if (in.active) {
        int all = vrhit_scan(ram, 1, 0x01, 1, ht, VRHIT_MAX_T);
        for (int i = 0; i < all; i++) if (!ht[i].ncap) {             /* blanco sin piezas fiables: el cilindro de siempre como su única cápsula (criterio por blanco, no global) */
            VrHitCap *q = &ht[i].cap[0]; float k = map.units_per_m;
            for (int a = 0; a < 3; a++) q->a[a] = q->b[a] = ht[i].c[a];
            q->a[1] -= G.cfg.body_low * k; q->b[1] += G.cfg.body_high * k; q->r = G.cfg.body_radius * k; q->zone = VRHIT_Z_BODY; ht[i].ncap = 1;
        }
        in.cap_valid = mf.contact_valid = all > 0;
        for (int i = 0; i < all && in.cap_valid; i++) if (ht[i].vital > 0) ht[hn++] = ht[i];
    }
    if (!in.active) fist_prev_ok[0] = fist_prev_ok[1] = 0;
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
            if (!in.tracked[s]) fist_prev_ok[s] = 0;
            if (in.tracked[s]) {
                vrfire_pos_to_world(&map, p, w);
                for (int i = 0; i < 3; i++) { in.hand[s][i] = w[i] * 0.001f; mf.fist[s][i] = w[i]; }
                in.grip[s] = gr;
                mf.fist_ok[s] = 1; mf.fist_closed[s] = in.fist_btn[s];
                { float fp[3]; if (mv_vrhost_hands_active() && vrbody_fist_point(s, fp)) for (int i = 0; i < 3; i++) mf.fist[s][i] = fp[i]; }   /* el golpe sale de los nudillos de la mano nueva, no del mando */
                if (in.cap_valid) {      /* esfera del puño (0,12 m) barrida desde el VBlank anterior contra las cápsulas; un salto de pose no es un barrido */
                    const float *b = mf.fist[s], *a = fist_prev_ok[s] && fabsf(fist_prev[s][0] - b[0]) + fabsf(fist_prev[s][1] - b[1]) + fabsf(fist_prev[s][2] - b[2]) < 600.0f ? fist_prev[s] : b;
                    float pt[3], hw[3] = {in.head[0] * 1000.0f, in.head[1] * 1000.0f, in.head[2] * 1000.0f}; int z = 0, hi = vrhit_sweep(ht, hn, a, b, G.cfg.fist_radius * map.units_per_m, &z, pt);
                    if (hi >= 0) {
                        float dv[3] = {pt[0] - hw[0], pt[1] - hw[1], pt[2] - hw[2]}, dl = sqrtf(dv[0] * dv[0] + dv[1] * dv[1] + dv[2] * dv[2]);
                        float tw = dl > 300.0f ? mv_vrhost_ray_distance(hw, dv, dl) : -1.0f;      /* pared entre la cabeza y el contacto: no se golpea a través de ella */
                        if (!(tw >= 0.0f && tw < dl - 150.0f)) { in.cap_zone[s] = z; mf.contact[s] = 1; vrhit_clamp_box(&ht[hi], pt, mf.contact_pt[s]); }
                    }
                }
                memcpy(fist_prev[s], mf.fist[s], sizeof fist_prev[s]); fist_prev_ok[s] = 1;
                float dmm = mv_vrhost_wall_distance(mf.fist[s], 400.0f);     /* superficie sólida más cercana a los nudillos (geometría estática de la escena, la de la colisión de la cabeza) */
                in.wall_valid[s] = dmm >= 0.0f; in.wall_dist[s] = dmm >= 0.0f ? dmm * 0.001f : 1.0f;
                in.wall_ny[s] = 1.0f;
                if (dmm >= 0.0f && dmm < 200.0f) {      /* ¿superficie vertical? la distancia a una pared no cambia al subir o bajar el punto; la distancia al suelo cambia lo mismo que se sube */
                    float up[3] = {mf.fist[s][0], mf.fist[s][1] + 60.0f, mf.fist[s][2]}, dn[3] = {mf.fist[s][0], mf.fist[s][1] - 60.0f, mf.fist[s][2]};
                    float du = mv_vrhost_wall_distance(up, 400.0f), dd = mv_vrhost_wall_distance(dn, 400.0f);
                    if (du >= 0.0f && dd >= 0.0f) in.wall_ny[s] = fabsf(du - dd) / 120.0f;
                }
                wsrc[s] = 'c';
                if (bs->on_wall) {      /* pegado a la pared: la pared a la que se pega el personaje es una pared de colisión del juego (HZD), siempre vertical; se mide aunque la geometría de la escena (vrhc) no dé nada o no la vea vertical */
                    float dz = vrhzd_wall_dist(ram, mf.fist[s], 400.0f);
                    if (dz >= 0.0f && (dmm < 0.0f || dz < dmm)) { in.wall_valid[s] = 1; in.wall_dist[s] = dz * 0.001f; in.wall_ny[s] = 0.0f; wsrc[s] = 'h'; }
                }
            }
        }
        in.weapon = bs->weapon; in.item_scope = bs->item == 1; /* IT_Scope */ in.stance = bs->stance; in.on_wall = bs->on_wall; in.in_box = bs->in_box; in.in_water = bs->in_water;
        in.holding = act_holds(bs->player_action); in.life = bs->life;
        in.body[0] = bs->ctl_pos.x * 0.001f; in.body[1] = bs->ctl_pos.y * 0.001f; in.body[2] = bs->ctl_pos.z * 0.001f;
        in.yaw = (float)bs->ctl_rot.y * (6.2831853f / 4096.0f);
        in.view_yaw = vrcam_view_yaw_deg() * (3.14159265f / 180.0f);
        for (int i = 0; i < bs->n_enemies && in.n_en < VRG_MAX_EN; i++) {
            const BridgeEnemy *e = &bs->enemies[i];
            in.en[in.n_en].p[0] = e->pos.x * 0.001f; in.en[in.n_en].p[1] = e->pos.y * 0.001f; in.en[in.n_en].p[2] = e->pos.z * 0.001f;
            in.en[in.n_en].yaw = (float)e->rot.y * (6.2831853f / 4096.0f);
            in.en[in.n_en].down = e->down; in.en[in.n_en].vital = e->vital;
            {   /* pared de por medio: muestras cada 100 mm del segmento cabeza -> pecho del enemigo (sin los extremos: el enemigo puede estar junto a un muro) con la geometría estática de la colisión de la cabeza; solo cerca (el golpe por contacto no pasa de ahí) */
                float hx = in.head[0] * 1000.0f, hy = in.head[1] * 1000.0f, hz = in.head[2] * 1000.0f;
                float tx = (float)e->pos.x, ty = (float)e->pos.y + 300.0f, tz = (float)e->pos.z;
                float dx = tx - hx, dy = ty - hy, dz = tz - hz, len = sqrtf(dx * dx + dy * dy + dz * dz);
                int blocked = 0;
                if (len > 600.0f && len < 2500.0f) {
                    for (float t = 200.0f; t < len - 300.0f && !blocked; t += 100.0f) {
                        float q[3] = {hx + dx * t / len, hy + dy * t / len, hz + dz * t / len};
                        float d = mv_vrhost_wall_distance(q, 60.0f);
                        if (d >= 0.0f && d < 60.0f) blocked = 1;
                    }
                }
                in.en[in.n_en].blocked = blocked;
            }
            in.n_en++;
        }
    }
    vrg_pad_t pp;                                    /* manos y gatillo físicos también fuera de la capa de juego: sostienen la ventana abierta con el gesto del pecho */
    memset(&pp, 0, sizeof pp);
    pp.ctl = ctl; pp.menu_open = core && bs->menu_open; pp.pad = pad; pp.pad_in = pad_in; pp.r2_fire = r2_fire;
    for (int s = 0; s < 2; s++) { float p3[3], tg = 0.0f; pp.tracked[s] = vrgame_hand_raw(s, p3, NULL, &tg, NULL); pp.trig[s] = tg; pp.fist[s] = (pad_in & (s == domside ? VRG_R1 : VRG_L1)) != 0; }   /* grip = botón del puño de la mano (R1 de la dominante, L1 de la otra) del mando entrante, también en la capa de menú */
    const uint16_t res = vrg_step_pad(&G, &in, &out, &pp);
    melee_busy = out.punch_side >= 0 || out.grab_start || out.grab_side >= 0 || out.swinging || out.squeeze || out.throw_side >= 0;
    chest_in[0] = out.chest_in[0]; chest_in[1] = out.chest_in[1];
    pub_menu_hand();
    {   /* registro [gest] (va al log del visor): cada intento con su motivo, y cada evento */
        static const char *evn[] = {"", "golpe", "agarre", "apreton", "soltar", "pared", "asomar", "pared2", "dano", "impacto", "ventana", "lanzamiento"};
        static unsigned vb;
        vb++;
        if (out.gest.valid) {
            const vrg_gest_t *q = &out.gest;
            float ang = -1.0f, nd = 1e9f;   /* ángulo (grados) entre el rumbo del jugador y la dirección al enemigo más cercano; -1 sin enemigos */
            for (int i = 0; i < in.n_en && i < VRG_MAX_EN; i++) {
                float dx = in.en[i].p[0] - in.body[0], dz = in.en[i].p[2] - in.body[2], d = sqrtf(dx * dx + dz * dz);
                if (d < nd && d > 1e-4f) { nd = d; float c = (dx * sinf(in.yaw) + dz * cosf(in.yaw)) / d; ang = acosf(c > 1.0f ? 1.0f : (c < -1.0f ? -1.0f : c)) * (180.0f / 3.14159265f); }
            }
            fprintf(stderr, "[gest] vb=%u mano=%d %s v=%.2f m/s adelante=%.2f radial=%.2f alcance=%.2f enemigo=%.2f m grip=%.2f puno=%d contacto=%d ang_enemigo=%.0f grados\n", vb, q->side, q->reason, q->speed, q->fcos, q->rcos, q->reach, q->dist, q->grip, q->fist, q->contact, ang);
        }
        for (int s = 0; s < 2; s++) {
            const char *rs = out.pecho[s].opened ? "abierta" : "rechazada";
            if (out.pecho[s].valid) fprintf(stderr, "[pecho] vb=%u mano=%d zona=%s (%.2f m a su lado, %.2f m bajo la cabeza, %.2f m delante) hf=(lat %.3f, alt %.3f, delante %.3f) m upm=%.0f yaw_personaje=%.1f yaw_cabeza=%.1f grados dist=%.3f m (max %.2f) mantenido=%.2f s (min %.2f) resultado=%s motivo=%s rearme=%s\n", vb, s, s == 0 ? "izquierda" : "derecha", G.cfg.chest_side, G.cfg.chest_down, G.cfg.chest_fwd, out.pecho[s].hf[0], out.pecho[s].hf[1], out.pecho[s].hf[2], map.units_per_m, in.yaw * (180.0f / 3.14159265f), atan2f(in.head_fwd[0], in.head_fwd[1]) * (180.0f / 3.14159265f), out.pecho[s].dist, G.cfg.chest_radius, out.pecho[s].hold, G.cfg.chest_hold_s, rs, out.pecho[s].reason, out.pecho[s].rearm ? "bloqueado_hasta_soltar_grip" : "armado");
        }
        if (out.grab_why) fprintf(stderr, "[gest] vb=%u agarre %s mano=%d distancia=%.2f m (cuello/espalda, limite 0,30)\n", vb, out.grab_why, out.grab_side_try, out.grab_dist);
        if (out.knock_why) { int ks = out.knock_try_side >= 0 ? out.knock_try_side : 0; fprintf(stderr, "[toque] vb=%u motivo=%s mano=%d v=%.2f m/s distancia=%.3f m puno=%d ny=%.2f postura=%d en_pared=%d fuente=%c\n", vb, out.knock_why, out.knock_try_side, out.knock_speed, out.knock_dist, in.fist_btn[ks], in.wall_ny[ks], in.stance, in.on_wall, wsrc[ks]); }
        if (out.event > 0 && out.event < 12) fprintf(stderr, "[gest] vb=%u evento=%s armas_libres=%d\n", vb, evn[out.event], in.weapon < 0);
    }
    /* decisiones del cuerpo -> motor (ganchos de vr/input/vr_melee): ninguna pasa por botones */
    mf.punch_side = out.punch_side; mf.knock_side = out.knock_side;
    mf.grab_start = out.grab_start; mf.grab_side = out.grab_side_try >= 0 && out.grab_start ? out.grab_side_try : out.grab_side;
    mf.throw_side = out.throw_side; mf.throw_yaw = out.throw_yaw; mf.hold = out.grab_hold; mf.swinging = out.swinging; mf.squeeze = out.squeeze;
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
    /* Capa de MENÚ (!ctl): ningún gesto toca el mando salvo la ventana de armas/objetos abierta con el gesto del pecho, que vrg_step_pad mantiene con el grip de esa mano. */
    return res;
}
