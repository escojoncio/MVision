/* vr_reload: ver vr_reload.h. Núcleo sin dependencias del runtime. */
#include <string.h>
#include "vr_reload.h"

#define PAD_SQUARE (1u << 15)
#define PRESS_MAX_VBL 90           /* VBlanks máximos con el botón de ataque pulsado esperando a que el juego recargue (el juego recarga en el primer tick del estado de ataque, ~17 VBlanks tras pulsar) */
#define REFLECT_SETTLE_VBL 60      /* VBlanks desde el último disparo VR antes de subir el arma VR al cargador del juego: la petición de disparo vive 60 (vr_fire req_window) y el juego gasta la bala al crearla */
#define REFLECT_LOCK_VBL 40        /* VBlanks sin disparo VR tras una recarga del juego (animación de recarga del original; duración no medida) */

static struct {
    int pk_w, pk_total;            /* arma y total del juego del VBlank anterior (detección de recogida) */
    int pickup, pickup_w;          /* se recogió munición del arma `pickup_w` con el cargador del juego a 0 y aún no se ha cargado */
    int self_load;                 /* este módulo pidió load_mag: la subida del cargador no es una recarga con animación */
    int last_w;                    /* arma VR del VBlank anterior (cambiar de arma de cargador = arma recién mostrada) */
    int fresh;                     /* arma VR recién mostrada (o vuelta de una pausa): igualar al juego en este VBlank */
    int prev_mag;                  /* cargador del juego del VBlank anterior */
    int since_shot;                /* VBlanks desde el último disparo VR (saturado) */
    int press, lock;               /* VBlanks restantes pulsando el ataque y de bloqueo tras una recarga */
    int g_total, g_mag, g_weapon;  /* última lectura (reserva mostrada) */
    VrReloadStats st;
} R;

void vrreload_init(void) {
    memset(&R, 0, sizeof R);
    R.pk_w = -1; R.pickup_w = -1; R.fresh = 1; R.since_shot = REFLECT_SETTLE_VBL; R.last_w = -1;
}
void vrreload_stats(VrReloadStats *s) { *s = R.st; }
int vrreload_pickup_pending(void) { return R.pickup; }

void vrreload_read_game(const BridgeState *bs, VrReloadGame *g) {
    memset(g, 0, sizeof *g);
    g->weapon = -1;
    if (!bs) return;
    g->ok = (bs->valid & BRIDGE_OK_ALL_CORE) == BRIDGE_OK_ALL_CORE && bs->has_player && bs->in_control;
    g->mag = bs->magazine; g->mag_max = bs->magazine_max;
    if (bs->valid & BRIDGE_OK_EQUIP) g->weapon = bs->weapon;   /* puente no válido: BridgeState a cero no es «arma 0 con total 0» (al volver, el total 0 -> N parecería una recogida) */
    if (g->weapon >= 0 && g->weapon < BRIDGE_N_WEAPONS) g->total = bs->weapon_ammo[g->weapon];
    g->menu = bs->menu_state != 0 || bs->pause_level != 0;
}

void vrreload_frame(const VrReloadGame *g, const VrReloadIn *in, VrReloadOut *out) {
    memset(out, 0, sizeof *out);
    out->set_held = -1;
    if (!g || !in) return;
    R.g_total = g->total; R.g_mag = g->mag; R.g_weapon = g->weapon;
    out->show_mag = g->mag > 0 ? g->mag : 0;
    out->show_reserve = g->total - g->mag; if (out->show_reserve < 0) out->show_reserve = 0;

    /* Recogida: el total del juego del arma sube (item.c add_ammo suma a GM_Weapons[arma] y no toca GM_Magazine). Con el cargador a 0 queda pendiente hasta que el juego lo cargue (también si ocurre sin control: pausa, menú). */
    const int rose = g->weapon >= 0 && g->weapon == R.pk_w && R.pk_total >= 0 && g->total > R.pk_total;
    R.pk_w = g->weapon; R.pk_total = g->total;
    if (rose && g->mag <= 0) { R.pickup = 1; R.pickup_w = g->weapon; }
    else if (g->mag > 0 || (g->weapon >= 0 && g->weapon != R.pickup_w)) R.pickup = 0;

    const int mine = g->ok && in->weapon_id >= 0 && g->weapon == in->weapon_id && g->mag_max > 0;
    if (in->weapon_id != R.last_w) { R.fresh = 1; R.press = 0; R.lock = 0; R.self_load = 0; R.since_shot = REFLECT_SETTLE_VBL; }
    R.last_w = in->weapon_id;
    const int prev_mag = R.prev_mag;
    R.prev_mag = g->mag;
    if (!mine) { R.fresh = 1; R.press = 0; R.lock = 0; R.self_load = 0; R.since_shot = REFLECT_SETTLE_VBL; return; }

    /* ---- espejo: arma VR = cargador del juego ---- */
    const int gm = g->mag > 0 ? g->mag : 0;
    if (in->vr_shot) R.since_shot = 0; else if (R.since_shot < 1000) R.since_shot++;
    const int event = R.fresh || g->mag > prev_mag;       /* arma recién mostrada, o el cargador del juego ha subido (recarga, recogida) */
    if (g->mag > prev_mag && !R.fresh) { if (R.self_load) R.self_load = 0; else R.lock = REFLECT_LOCK_VBL; }
    R.fresh = 0;
    if (event || in->vr_held > gm || (in->vr_held < gm && R.since_shot >= REFLECT_SETTLE_VBL)) { out->set_held = gm; R.st.syncs++; }

    /* ---- pulsar disparo con el cargador vacío: el juego recarga ---- */
    const int dry = !g->menu && g->mag <= 0 && g->total > 0;
    if (dry && !R.pickup && in->trigger_edge && R.press == 0 && R.lock == 0) { R.press = PRESS_MAX_VBL; R.st.presses++; }
    if (R.press > 0) {
        if (!dry || R.pickup) R.press = 0;
        else { out->pad_force = (uint16_t)PAD_SQUARE; R.press--; }
    }
    if (R.lock > 0 && !out->pad_force) R.lock--;

    /* ---- recogida con el arma seca: cargar el cargador del juego ---- */
    if (R.pickup && g->mag <= 0 && g->total > 0) {
        const int cap = g->weapon == 9 ? 5 : g->mag_max;     /* francotirador (arma 9): el juego recarga siempre 5 (sna_init.c:6098-6106) */
        out->load_mag = g->total < cap ? g->total : cap;
        if (!R.self_load) R.st.loads++;
        R.self_load = 1;
    }

    out->busy = R.press > 0 || R.lock > 0;
    out->fire_allowed = g->mag > 0 && !out->busy;
}

/* Anula la pulsación en curso del ataque (R.press) y rehace busy/fire_allowed del mismo VBlank (la zona de menú del anfitrión bloquea el gatillo). Solo con out de un vrreload_frame de arma propia. */
void vrreload_cancel_press(VrReloadOut *out) {
    R.press = 0; out->pad_force = 0;
    out->busy = R.lock > 0;
    out->fire_allowed = R.g_mag > 0 && !out->busy;
}

int vrreload_ammo_reserve(int weapon_id) {
    if (weapon_id != R.g_weapon) return 0;
    const int v = R.g_total - R.g_mag;
    return v > 0 ? v : 0;
}
