/* vr_reload: ver vr_reload.h. Núcleo sin dependencias del runtime. */
#include <string.h>
#include "vr_reload.h"

#define PAD_R2 (1u << 9)
#define PAD_R1 (1u << 11)
#define ABORT_NOT_OK 40            /* VBlanks sin control del jugador que abortan una maniobra */
#define EMPTY_RETRY_VBL 150        /* VBlanks de espera antes de repetir una maniobra de EMPTY que falló (el juego no cargó: arma bloqueada, menú...) */
#define NO_MAG_VBL 12              /* VBlanks seguidos con el arma VR sin balas antes de contar toda la munición del juego como reserva (el último disparo VR lo consume el juego 1-2 fotogramas después: sin balas en la reserva todavía) */

static struct {
    VrReloadCfg cfg;
    int state, t;                  /* estado y VBlanks en él */
    int seq_t;                     /* VBlanks dentro de la maniobra de pulsaciones */
    int retries;                   /* repeticiones de la maniobra actual */
    int mag_before;                /* cargador del juego al empezar */
    int lost_t;                    /* VBlanks seguidos sin el arma equipada */
    int reequip_t;                 /* >0: pulsando R1 para volver a equipar (VBlanks restantes) */
    int not_ok_t;
    int seen_not_ready;            /* EMPTY: se ha visto el arma VR no lista (el gesto empezó) */
    int idle_t;                    /* EMPTY: VBlanks seguidos con el cargador del juego a 0 y reserva > 0 sin que el juego recargue (negativo = espera tras un fallo) */
    int empty_run;                 /* la maniobra en curso se lanzó desde EMPTY (al terminar vuelve a EMPTY a esperar el gesto del arma VR) */
    int pickup;                    /* se recogió munición (el total del juego subió) estando en EMPTY y el arma VR aún no se ha rearmado */
    int arm;                       /* READY: emitir arm_ready en este VBlank */
    int pk_w, pk_total;            /* arma y total del juego del VBlank anterior (detección de recogida) */
    int adopt;                     /* forzar el cargador VR al del juego en este VBlank */
    int virt_taken;                /* balas cogidas de la reserva y aún no cargadas */
    int no_mag;                    /* VBlanks seguidos con el arma VR sin balas dentro (VrReloadIn.no_mag), saturado */
    int g_total, g_mag, g_weapon;  /* última lectura (para el libro virtual) */
    int last_w;                    /* último arma válida vista (cambiar de arma anula el libro virtual) */
    int total_vbl;                 /* VBlanks de la maniobra completa (latencia) */
    VrReloadStats st;
} R;

void vrreload_cfg_default(VrReloadCfg *c) {
    memset(c, 0, sizeof *c);
    c->method = VRR_VIA_R1_DOUBLE; c->taps = 2; c->press_vbl = 2; c->gap_vbl = 2; c->verify_vbl = 60; c->max_retries = 2;
    c->empty_wait_vbl = 25; c->lost_weapon_vbl = 24;
}
void vrreload_init(const VrReloadCfg *c) {
    memset(&R, 0, sizeof R);
    if (c) R.cfg = *c; else vrreload_cfg_default(&R.cfg);
    if (R.cfg.taps < 1) R.cfg.taps = 1;
    if (R.cfg.taps > 4) R.cfg.taps = 4;
    if (R.cfg.press_vbl < 2) R.cfg.press_vbl = 2;
    if (R.cfg.gap_vbl < 2) R.cfg.gap_vbl = 2;
    if (R.cfg.verify_vbl < 8) R.cfg.verify_vbl = 8;
    if (R.cfg.max_retries < 0) R.cfg.max_retries = 0;
    if (R.cfg.empty_wait_vbl < 8) R.cfg.empty_wait_vbl = 8;
    if (R.cfg.lost_weapon_vbl < 4) R.cfg.lost_weapon_vbl = 4;
    R.state = VRR_OFF; R.last_w = -1; R.pk_w = -1;
}
const VrReloadCfg *vrreload_config(void) { return &R.cfg; }
void vrreload_stats(VrReloadStats *s) { *s = R.st; }
int vrreload_state(void) { return R.state; }
int vrreload_pickup_pending(void) { return R.pickup; }

void vrreload_read_game(const BridgeState *bs, VrReloadGame *g) {
    memset(g, 0, sizeof *g);
    g->weapon = -1;
    if (!bs) return;
    g->ok = (bs->valid & BRIDGE_OK_ALL_CORE) == BRIDGE_OK_ALL_CORE && bs->has_player && bs->in_control;
    g->weapon = bs->weapon; g->mag = bs->magazine; g->mag_max = bs->magazine_max;
    if (g->weapon >= 0 && g->weapon < BRIDGE_N_WEAPONS) g->total = bs->weapon_ammo[g->weapon];
    g->menu = bs->menu_state != 0 || bs->pause_level != 0;
}

/* ---- maniobra del mando ---- */
static unsigned maneuver_bit(void) { return R.cfg.method == VRR_VIA_R2_TAPS ? PAD_R2 : PAD_R1; }
static int maneuver_len(void) { return R.cfg.taps * R.cfg.press_vbl + (R.cfg.taps - 1) * R.cfg.gap_vbl; }
static int maneuver_level(int t) {
    int p = R.cfg.press_vbl, period = R.cfg.press_vbl + R.cfg.gap_vbl;
    return t >= 0 && (t % period) < p && t / period < R.cfg.taps;
}

static void enter(int s) { R.state = s; R.t = 0; }
static void start_maneuver(const VrReloadGame *g, int is_retry) {
    enter(VRR_PRESS);
    R.seq_t = 0; R.lost_t = 0; R.reequip_t = 0;
    if (!is_retry) { R.retries = 0; R.mag_before = g->mag; R.total_vbl = 0; R.st.sequences++; }
}
static void finish_ok(const VrReloadGame *g) {
    (void)g;
    R.st.ok++; R.st.last_latency_vbl = R.total_vbl; R.virt_taken = 0; R.adopt = 1;
    if (R.empty_run) { R.empty_run = 0; R.idle_t = 0; enter(VRR_EMPTY); return; }   /* el juego ya tiene su cargador: el arma VR sigue seca hasta su gesto (o hasta el rearme por recogida) */
    enter(VRR_READY);
}
static void finish_fail(void) {
    R.st.fails++;
    if (R.empty_run) { R.empty_run = 0; R.idle_t = -EMPTY_RETRY_VBL; enter(VRR_EMPTY); return; }   /* sin cargar nada no hay nada que adoptar: se reintenta pasado un tiempo */
    R.adopt = 1;
    enter(VRR_READY);
}

void vrreload_frame(const VrReloadGame *g, const VrReloadIn *in, VrReloadOut *out) {
    memset(out, 0, sizeof *out);
    out->sync_rounds = -1; out->sync_chambered = -1;
    out->state = R.state;
    if (!g || !in) return;
    R.g_total = g->total; R.g_mag = g->mag; R.g_weapon = g->weapon; R.no_mag = in->no_mag ? (R.no_mag < 1000 ? R.no_mag + 1 : R.no_mag) : 0;
    /* El libro virtual solo tiene sentido con balas de la reserva dentro del arma VR sin que el juego las haya movido aún: sin balas en el arma VR (se reinició, se expulsó el cargador) no hay nada
     * «cogido», y sin munición en el juego tampoco; un resto huérfano dejaba la reserva en 0 aunque se recogieran balas y la pistola no se podía recargar. */
    if (R.no_mag >= NO_MAG_VBL || g->total <= 0) R.virt_taken = 0;
    if (g->weapon >= 0) { if (R.last_w >= 0 && g->weapon != R.last_w) R.virt_taken = 0; R.last_w = g->weapon; }   /* cargadores cogidos de otra arma no cuentan */
    out->show_mag = g->mag > 0 ? g->mag : 0;
    out->show_reserve = g->total - g->mag - R.virt_taken; if (out->show_reserve < 0) out->show_reserve = 0;
    /* Recogida de munición: el total del juego del arma sube (item.c add_ammo suma a GM_Weapons[arma] y no toca GM_Magazine). Solo importa con el arma en EMPTY (o con la maniobra de EMPTY en curso). */
    int rose = g->weapon >= 0 && g->weapon == R.pk_w && g->total > R.pk_total && R.pk_total >= 0;
    R.pk_w = g->weapon; R.pk_total = g->total;
    R.t++;
    int mine = g->ok && in->weapon_id >= 0 && g->weapon == in->weapon_id;
    int busy = R.state == VRR_PRESS || R.state == VRR_VERIFY;

    /* el jugador deja de ser controlable (menú, pausa, cinemática): una maniobra a medias se aborta pasado un tiempo. Los
     * toques del mando siguen su temporización aunque el juego abra un menú un instante (el toque de R2 lo abre). */
    if (!g->ok) {
        if (busy && ++R.not_ok_t > ABORT_NOT_OK) { finish_fail(); R.not_ok_t = 0; busy = 0; }
        if (!busy) { R.not_ok_t = 0; R.state = VRR_OFF; out->state = VRR_OFF; return; }
        if (R.state == VRR_VERIFY) { R.total_vbl++; goto tail; }
    } else {
        R.not_ok_t = 0;
    }

    if (!g->ok && R.state != VRR_PRESS) goto tail;
    if (rose && g->ok && (R.state == VRR_EMPTY || R.empty_run)) { if (!R.pickup) R.st.pickups++; R.pickup = 1; }
    switch (R.state) {
    case VRR_OFF:
        R.pickup = 0; R.empty_run = 0; R.arm = 0;
        if (mine) {
            R.adopt = 1; R.seen_not_ready = 0;
            enter(g->mag <= 0 && g->mag_max > 0 ? VRR_EMPTY : VRR_READY);
            if (R.state == VRR_EMPTY) { R.st.empties++; R.idle_t = 0; }
        }
        break;
    case VRR_READY:
        if (!mine) { enter(VRR_OFF); break; }
        if (g->mag_max > 0 && g->mag <= 0) { R.st.empties++; R.seen_not_ready = !in->ready; R.idle_t = 0; enter(VRR_EMPTY); break; }
        if (in->mag_in_edge && g->mag < g->mag_max && g->total > g->mag) start_maneuver(g, 0);
        break;
    case VRR_EMPTY:
        if (!mine) { enter(VRR_OFF); break; }
        if (!in->ready) R.seen_not_ready = 1;
        if (R.pickup && g->mag > 0) {                  /* recogida con el arma seca y el juego ya cargó su cargador: el arma VR se deja lista sin exigir el gesto */
            R.arm = 1; R.adopt = 1; R.virt_taken = 0; R.pickup = 0; R.st.ok++; R.st.arms++; enter(VRR_READY); break;
        }
        if (R.seen_not_ready && in->ready && g->mag > 0) {   /* gesto terminado */
            R.adopt = 1; R.virt_taken = 0; R.pickup = 0; R.st.ok++; enter(VRR_READY); break;
        }
        /* si el juego recargó solo mientras el arma VR sigue vacía: no se copia (bloqueo hasta el gesto) */
        /* el juego solo recarga solo con el arma levantada (sna_init.c sub_80057BF0): con el disparo VR bloqueado nunca se pulsa el ataque, así que con el cargador a 0 y reserva se provoca sin esperar al gesto */
        if (g->mag <= 0 && g->total > 0 && !g->menu) {
            if (++R.idle_t > R.cfg.empty_wait_vbl) { R.idle_t = 0; R.empty_run = 1; R.st.empty_runs++; start_maneuver(g, 0); }
        } else if (R.idle_t > 0) R.idle_t = 0;
        break;
    case VRR_PRESS:
        R.total_vbl++;
        if (R.seq_t < maneuver_len()) {
            if (maneuver_level(R.seq_t)) out->pad_force = (uint16_t)maneuver_bit();
            R.seq_t++;
        }
        if (R.seq_t >= maneuver_len()) enter(VRR_VERIFY);
        break;
    case VRR_VERIFY: {
        R.total_vbl++;
        int equipped = g->weapon == in->weapon_id;
        int loaded = equipped && g->mag > 0 && (g->mag >= g->mag_max || g->mag > R.mag_before);
        if (R.reequip_t > 0) { out->pad_force = (uint16_t)PAD_R1; R.reequip_t--; }
        if (loaded) { finish_ok(g); break; }
        if (!equipped) R.lost_t++; else R.lost_t = 0;
        if (!equipped && R.lost_t == R.cfg.lost_weapon_vbl) {       /* un toque se perdió: el arma sigue desequipada */
            if (R.retries >= R.cfg.max_retries) { finish_fail(); break; }
            R.retries++; R.st.retries++; R.reequip_t = R.cfg.press_vbl; R.lost_t = 0;
        } else if (R.t > R.cfg.verify_vbl && equipped) {              /* equipada pero sin recargar: repetir la maniobra */
            if (R.retries >= R.cfg.max_retries) { finish_fail(); break; }
            R.retries++; R.st.retries++;
            start_maneuver(g, 1);
        }
        break; }
    }

tail:
    busy = R.state == VRR_PRESS || R.state == VRR_VERIFY;
    out->state = R.state;
    out->busy = busy;
    if (busy) out->pad_mask = (uint16_t)(PAD_R1 | PAD_R2);
    else out->pad_force = 0;           /* maniobra terminada en este VBlank: no dejar un toque suelto que desequiparía */
    out->fire_allowed = R.state == VRR_READY && g->mag > 0;
    if (R.state == VRR_EMPTY) { out->show_mag = 0; out->sync_rounds = 0; out->sync_chambered = 0; }
    else if (R.state == VRR_READY) {
        int rounds = g->mag > 0 ? g->mag - 1 : 0, ch = g->mag > 0 ? 1 : 0;
        out->sync_rounds = rounds; out->sync_chambered = ch;          /* tope hacia abajo */
        if (R.adopt) { out->sync_force = 1; R.adopt = 0; }            /* recarga completada o arma recién equipada: fijar */
        if (R.arm) { out->arm_ready = 1; R.arm = 0; }                 /* recogida con el arma seca: el anfitrión rearma el arma VR */
    }
}

int vrreload_ammo_reserve(int weapon_id) {
    if (weapon_id != R.g_weapon) return 0;
    if (R.no_mag >= NO_MAG_VBL) return R.g_total > 0 ? R.g_total : 0;     /* arma VR sin balas: el cargador del juego no está «dentro» del arma, todo es reserva */
    int v = R.g_total - R.g_mag - R.virt_taken;
    return v > 0 ? v : 0;
}
void vrreload_ammo_take(int weapon_id, int n) {
    if (weapon_id != R.g_weapon) return;
    R.virt_taken += n;
    if (R.virt_taken < 0) R.virt_taken = 0;
}
void vrreload_bridge_fill(WGameBridge *b) { b->ammo_reserve = vrreload_ammo_reserve; b->ammo_take = vrreload_ammo_take; }
