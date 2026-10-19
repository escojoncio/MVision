/* bridge: lectura de la RAM emulada. Ver bridge.h. Solo lectura, sin asignaciones dinámicas. */
#include "bridge.h"
#include <stdlib.h>
#include <string.h>

/* Los accesos son memcpy de la RAM de la consola (little-endian) a enteros del host: solo válido en host LE. */
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "bridge.c asume un host little-endian"
#endif

/* ---------- acceso seguro ---------- */

/* Dirección de consola -> offset en los 2 MB (acepta KSEG0 0x80..., KSEG1 0xA0... y 0x00...). -1 si no cabe. */
static long ram_off(uint32_t addr, uint32_t len) {
    uint32_t a = addr;
    if (a >= 0x80000000u && a < 0x80200000u) a -= 0x80000000u;
    else if (a >= 0xA0000000u && a < 0xA0200000u) a -= 0xA0000000u;
    else if (a >= 0x200000u) return -1;
    if (len > BRIDGE_RAM_SIZE || a > BRIDGE_RAM_SIZE - len) return -1;
    return (long)a;
}

int bridge_addr_ok(uint32_t addr, uint32_t len) { return ram_off(addr, len) >= 0; }

typedef struct { const uint8_t *r; } Ram;

static uint32_t rd32(Ram m, uint32_t addr) {
    long o = ram_off(addr, 4);
    uint32_t v = 0;
    if (o >= 0) memcpy(&v, m.r + o, 4);   /* sin alineación obligatoria; el host es little-endian */
    return v;
}
static int16_t rds16(Ram m, uint32_t addr) {
    long o = ram_off(addr, 2);
    int16_t v = 0;
    if (o >= 0) memcpy(&v, m.r + o, 2);
    return v;
}
static uint16_t rdu16(Ram m, uint32_t addr) { return (uint16_t)rds16(m, addr); }
static uint8_t rd8(Ram m, uint32_t addr) {
    long o = ram_off(addr, 1);
    return o >= 0 ? m.r[o] : 0;
}
static BVec3 rdv3(Ram m, uint32_t addr) {
    BVec3 v = {rds16(m, addr), rds16(m, addr + 2), rds16(m, addr + 4)};
    return v;
}
/* Puntero a datos en RAM del usuario (por encima de la zona del kernel), alineado a 4, con `len` bytes legibles. */
static int in_user_ram(uint32_t p) { return p >= 0x80010000u && p < 0x80200000u; }   /* solo KSEG0 por encima del kernel */
static int ptr_ok(uint32_t p, uint32_t len) {
    return in_user_ram(p) && (p & 3u) == 0 && ram_off(p, len) >= 0;
}

int bridge_peek32(const uint8_t *ram, uint32_t addr, uint32_t *out) {
    if (!ram || ram_off(addr, 4) < 0) return 0;
    Ram m = {ram};
    *out = rd32(m, addr);
    return 1;
}
int bridge_peek16(const uint8_t *ram, uint32_t addr, int16_t *out) {
    if (!ram || ram_off(addr, 2) < 0) return 0;
    Ram m = {ram};
    *out = rds16(m, addr);
    return 1;
}

/* ---------- lectores por grupo ---------- */

/* Variables de partida: equipo, inventario, vida. */
static uint32_t read_vars(Ram m, BridgeState *s) {
    uint32_t ok = 0;
    int16_t v[96];
    for (int i = 0; i < 96; i++) v[i] = rds16(m, BA_VARS + 2u * (uint32_t)i);
    /* Sanidad de la estructura: dificultad -1..3, disco 1..2 */
    if (v[BRIDGE_V_DIFFICULTY] < -1 || v[BRIDGE_V_DIFFICULTY] > 3) return 0;
    if (v[BRIDGE_V_DISC] < 1 || v[BRIDGE_V_DISC] > 2) return 0;
    ok |= BRIDGE_OK_VARS;

    s->life = v[BRIDGE_V_LIFE];
    s->life_max = v[BRIDGE_V_LIFE_MAX];
    /* vida entre 0 y el doble del máximo (admite sobrecarga transitoria) */
    if (s->life_max >= 1 && s->life_max <= 4096 && s->life >= 0 && s->life <= 2 * s->life_max) ok |= BRIDGE_OK_LIFE;
    else s->life = s->life_max = 0;

    if (v[BRIDGE_V_STANCE] < 0 || v[BRIDGE_V_STANCE] > 3) { memset(s, 0, sizeof *s); return 0; }
    s->stance = (int8_t)v[BRIDGE_V_STANCE];
    s->weapon = v[BRIDGE_V_WEAPON];
    s->item = v[BRIDGE_V_ITEM];
    int good = (s->weapon >= -1 && s->weapon < BRIDGE_N_WEAPONS) && (s->item >= -1 && s->item < BRIDGE_N_ITEMS);
    /* Entradas: -1 = no lo tiene; el bit 0x8000 en las que sí tiene marca "bloqueado" (el juego lo pone con |=). */
    for (int i = 0; i < BRIDGE_N_WEAPONS; i++) {
        int16_t a = v[BRIDGE_V_WEAPON_AMMO + i];
        if (a != -1 && (a & 0x8000)) { s->weapon_disabled |= (uint16_t)(1u << i); a = (int16_t)(a & 0x7FFF); }
        s->weapon_ammo[i] = a;
        s->weapon_max[i] = v[BRIDGE_V_WEAPON_MAX + i];
        if (a < -1 || a > 999 || s->weapon_max[i] < 0 || s->weapon_max[i] > 999) good = 0;
    }
    for (int i = 0; i < BRIDGE_N_ITEMS; i++) {
        int16_t c = v[BRIDGE_V_ITEM_COUNT + i];
        if (c != -1 && (c & 0x8000)) { s->item_disabled |= (uint32_t)(1u << i); c = (int16_t)(c & 0x7FFF); }
        s->item_count[i] = c;   /* consumibles: unidades; llaves/tarjeta/bomba: nivel, máscara de bits o temporizador */
    }
    for (int i = 0; i < 3; i++) s->item_max[i] = v[BRIDGE_V_ITEM_MAX + i];
    /* equipada => la tiene */
    if (good && s->weapon >= 0 && s->weapon_ammo[s->weapon] < 0) good = 0;
    if (good && s->item >= 0 && s->item_count[s->item] < 0) good = 0;
    if (good) ok |= BRIDGE_OK_EQUIP;
    else {   /* grupo no válido: todo a cero (el bit de valid es lo que manda) */
        s->weapon = s->item = 0;
        memset(s->weapon_ammo, 0, sizeof s->weapon_ammo);
        memset(s->weapon_max, 0, sizeof s->weapon_max);
        memset(s->item_count, 0, sizeof s->item_count);
        memset(s->item_max, 0, sizeof s->item_max);
        s->weapon_disabled = 0;
        s->item_disabled = 0;
    }
    return ok;
}

static uint32_t read_player(Ram m, BridgeState *s) {
    uint32_t ok = 0;
    s->player_pos = rdv3(m, BA_PLAYER_POS);
    s->player_map = (int32_t)rd32(m, BA_PLAYER_MAP);
    s->player_action = (int32_t)rd32(m, BA_PLAYER_ACTION);
    s->player_status = rd32(m, BA_PLAYER_STATUS);
    s->o2 = rds16(m, BA_O2);
    s->magazine = rds16(m, BA_MAGAZINE);
    s->magazine_max = rds16(m, BA_MAGAZINE_MAX);
    s->disable_weapon = rd32(m, BA_DISABLE_WEAPON);
    s->disable_item = rd32(m, BA_DISABLE_ITEM);
    /* Banderas coherentes: el bit de "intruso" y los bits altos de depuración no deben aparecer */
    if (!(s->player_status & 0x80000000u) && s->magazine >= 0 && s->magazine <= 999 && s->magazine_max >= 0 &&
        s->magazine_max <= 999)
        ok |= BRIDGE_OK_PLAYER_ST;
    uint32_t st = s->player_status;
    s->crouching = (st & BRIDGE_PS_SQUAT) != 0;
    s->prone = (st & BRIDGE_PS_GROUND) != 0;
    s->on_wall = (st & BRIDGE_PS_WALL) != 0;
    s->knocking = (st & BRIDGE_PS_KNOCK) != 0;
    s->first_person = (st & (BRIDGE_PS_WATCH | BRIDGE_PS_WATCH_PEEK)) != 0;
    s->in_box = (st & BRIDGE_PS_BOX) != 0;
    s->moving = (st & BRIDGE_PS_MOVE) != 0;
    s->hidden = (st & BRIDGE_PS_HIDDEN) != 0;
    s->damaged = (st & BRIDGE_PS_DAMAGED) != 0;
    s->in_water = (st & BRIDGE_PS_IN_WATER) != 0;

    uint32_t c = rd32(m, BA_PLAYER_CTRL);
    s->ctl_addr = 0;
    if (ptr_ok(c, BC_SIZE) && rdu16(m, c + BC_NAME) == BRIDGE_PLAYER_HASH) {
        s->ctl_addr = c;
        s->ctl_pos = rdv3(m, c + BC_MOV);
        s->ctl_rot = rdv3(m, c + BC_ROT);
        s->ctl_height = rds16(m, c + BC_HEIGHT);
        ok |= BRIDGE_OK_PLAYER_CTL;
        /* player_pos solo es fiable si hay jugador vivo en la lista de controles y ambas posiciones coinciden: en
         * títulos, cargas y cambios de zona conserva el valor inicial de la partida (p. ej. una posición fija)
         * mientras el bloque de control aún no existe. */
        if (abs((int)s->player_pos.x - s->ctl_pos.x) <= 128 && abs((int)s->player_pos.y - s->ctl_pos.y) <= 128 &&
            abs((int)s->player_pos.z - s->ctl_pos.z) <= 128)
            ok |= BRIDGE_OK_PLAYER_POS;
    }
    return ok;
}

static int mat_col_norm_ok(const int16_t m[3][3]) {
    for (int c = 0; c < 3; c++) {
        int64_t n = 0;
        for (int r = 0; r < 3; r++) n += (int64_t)m[r][c] * m[r][c];
        /* |columna| debe ser ~4096 (admite el reescalado de la fila Y del motor): 3500..4700 */
        if (n < 3500LL * 3500LL || n > 4700LL * 4700LL) return 0;
    }
    /* columnas casi ortogonales entre sí (|producto escalar| < ~8 % de 4096^2): descarta matrices con basura */
    for (int a = 0; a < 3; a++)
        for (int b = a + 1; b < 3; b++) {
            int64_t d = 0;
            for (int r = 0; r < 3; r++) d += (int64_t)m[r][a] * m[r][b];
            if (d > 1400000LL || d < -1400000LL) return 0;
        }
    return 1;
}

static uint32_t read_camera(Ram m, BridgeState *s) {
    uint32_t ok = 0;
    uint32_t e = BA_VIEW_CHANNEL + BCH_CAM;
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) s->eye_axes[r][c] = rds16(m, e + 2u * (uint32_t)(r * 3 + c));
    for (int i = 0; i < 3; i++) {
        s->eye_pos[i] = (int32_t)rd32(m, e + 20u + 4u * (uint32_t)i);
        s->view_t[i] = (int32_t)rd32(m, BA_VIEW_CHANNEL + BCH_CAM_INV + 20u + 4u * (uint32_t)i);
    }
    s->eye_focal = rds16(m, BA_VIEW_CHANNEL + BCH_SCREEN);
    int pos_ok = 1;
    for (int i = 0; i < 3; i++)
        if (s->eye_pos[i] < -(1 << 20) || s->eye_pos[i] > (1 << 20)) pos_ok = 0;
    if (pos_ok && mat_col_norm_ok(s->eye_axes) && s->eye_focal > 0 && s->eye_focal <= 4096) ok |= BRIDGE_OK_EYE;

    s->cam_pos = rdv3(m, BA_CAMERA + BCAM_POS);
    s->cam_target = rdv3(m, BA_CAMERA + BCAM_TARGET);
    s->cam_rot = rdv3(m, BA_CAMERA + BCAM_ROT);
    s->cam_track = (int32_t)rd32(m, BA_CAMERA + BCAM_TRACK);
    s->cam_zoom = rds16(m, BA_CAMERA + BCAM_ZOOM);
    s->cam_first_person = rds16(m, BA_CAMERA + BCAM_FIRST);
    s->cam_type = rds16(m, BA_CAMERA + BCAM_TYPE);
    if (s->cam_zoom >= 64 && s->cam_zoom <= 8192 && (s->cam_first_person == 0 || s->cam_first_person == 1) &&
        s->cam_type >= 0 && s->cam_type < 16)
        ok |= BRIDGE_OK_CAMERA;
    return ok;
}

static void copy_name(Ram m, uint32_t p, char out[20]) {
    out[0] = 0;
    if (!in_user_ram(p)) return;
    int n = 0;
    for (; n < 19; n++) {
        long o = ram_off(p + (uint32_t)n, 1);
        if (o < 0) { out[0] = 0; return; }
        uint8_t ch = m.r[o];
        if (ch == 0) break;
        if (ch < 0x20 || ch > 0x7E) { out[0] = 0; return; }
        out[n] = (char)ch;
    }
    out[n] = 0;
}

static uint32_t read_actors(Ram m, BridgeState *s) {
    s->n_actors = 0;
    int all_ok = 1;
    for (int lv = 0; lv < BRIDGE_N_ACTOR_LISTS; lv++) {
        uint32_t base = BA_ACTOR_LISTS + 68u * (uint32_t)lv;
        uint32_t endn = base + 32u;
        /* nodo de inicio: prev == 0; nodo de fin: next == 0, act == 0 */
        if (rd32(m, base + 0) != 0 || rd32(m, endn + 4) != 0 || rd32(m, endn + 8) != 0) { all_ok = 0; continue; }
        uint32_t cur = rd32(m, base + 4), prev = base;
        int steps = 0;
        while (cur != endn) {
            if (!ptr_ok(cur, 32) || ++steps > 1024) { all_ok = 0; break; }
            /* lista doblemente enlazada coherente: el prev de cada nodo es el nodo anterior */
            if (rd32(m, cur) != prev) { all_ok = 0; break; }
            uint32_t nxt = rd32(m, cur + 4);
            uint32_t act = rd32(m, cur + 8);
            if (act != 0 && s->n_actors >= BRIDGE_MAX_ACTORS) all_ok = 0;   /* lista truncada = incompleta */
            if (act != 0 && s->n_actors < BRIDGE_MAX_ACTORS) {
                BridgeActor *a = &s->actors[s->n_actors++];
                a->addr = cur;
                a->level = (uint8_t)lv;
                a->act = act;
                a->die = rd32(m, cur + 12);
                a->name_ptr = rd32(m, cur + 20);
                copy_name(m, a->name_ptr, a->name);
                if (!a->name[0]) a->name_ptr = 0;
                a->runtime = (int32_t)rd32(m, cur + 24);
                a->count = (int32_t)rd32(m, cur + 28);
            }
            prev = cur;
            cur = nxt;
        }
        if (cur == endn && rd32(m, endn) != prev) all_ok = 0;
    }
    return all_ok ? BRIDGE_OK_ACTORS : 0;
}

static uint32_t read_controls(Ram m, BridgeState *s) {
    s->n_controls = 0;
    int32_t n = (int32_t)rd32(m, BA_N_CONTROLS);
    if (n < 1 || n > BRIDGE_MAX_CONTROLS) return 0;
    int bad = 0;
    for (int i = 0; i < n; i++) {
        uint32_t c = rd32(m, BA_CONTROL_LIST + 4u * (uint32_t)i);
        if (!ptr_ok(c, BC_SIZE)) { bad++; continue; }
        BridgeControl *k = &s->controls[s->n_controls++];
        k->addr = c;
        k->name_id = rdu16(m, c + BC_NAME);
        k->pos = rdv3(m, c + BC_MOV);
        k->rot = rdv3(m, c + BC_ROT);
        k->height = rds16(m, c + BC_HEIGHT);
        uint32_t mp = rd32(m, c + BC_MAP);
        k->map_bits = ptr_ok(mp, 4) ? rd32(m, mp) : 0;
        k->radar = rdu16(m, c + BC_RADAR);
        k->alert_state = (uint8_t)(k->radar >> 12);
        k->cone_dir = rdu16(m, c + BC_CONE);
        k->cone_dis = rdu16(m, c + BC_CONE + 2);
        k->cone_range = rdu16(m, c + BC_CONE + 4);
        k->cone_r = rdu16(m, c + BC_CONE + 6);
        k->is_player = (k->name_id == BRIDGE_PLAYER_HASH);
        k->actor = -1;
        for (int a = 0; a < s->n_actors; a++)
            if (s->actors[a].addr + 0x20u == c) { k->actor = (int16_t)a; break; }
    }
    /* la entrada 0 es siempre el jugador (o un marcador de relleno si aún no existe) */
    return bad == 0 ? BRIDGE_OK_CONTROLS : 0;
}

static uint32_t read_targets(Ram m, BridgeState *s) {
    s->n_targets = 0;
    int32_t n = (int32_t)rd32(m, BA_N_TARGETS);
    if (n < 0 || n > BRIDGE_MAX_TARGETS) return 0;
    for (int i = 0; i < n; i++) {
        uint32_t t = BA_TARGET_ARRAY + (uint32_t)BT_SIZE * (uint32_t)i;
        uint16_t cls = rdu16(m, t + BT_CLASS);
        if (cls == 0) continue; /* hueco libre */
        BridgeTarget *b = &s->targets[s->n_targets++];
        b->addr = t;
        b->cls = cls;
        b->side = rdu16(m, t + BT_SIDE);
        b->map = rdu16(m, t + BT_MAP);
        b->damaged = rdu16(m, t + BT_DAMAGED);
        b->center = rdv3(m, t + BT_CENTER);
        b->half_size = rdv3(m, t + BT_BOX);
        b->vital = rds16(m, t + BT_VITAL);
        b->damage = rds16(m, t + BT_DAMAGE);
        b->faint = rds16(m, t + BT_FAINT);
        b->weapon = (int32_t)rd32(m, t + BT_WEAPON);
    }
    return BRIDGE_OK_TARGETS;
}

/* Asociación enemigo-blanco (heurística, confianza media): emparejamiento codicioso EXCLUSIVO por distancia
 * horizontal creciente (un blanco no se reparte entre dos controles), solo blancos enemigos disponibles del mismo
 * mapa y a <= 700 unidades. Sin empate: gana el primero por índice. */
static uint32_t derive_enemies(BridgeState *s) {
    s->n_enemies = 0;
    for (int i = 0; i < s->n_controls && s->n_enemies < BRIDGE_MAX_ENEMIES; i++) {
        const BridgeControl *c = &s->controls[i];
        if (c->is_player || !(c->radar & 1u)) continue; /* visible en el radar */
        BridgeEnemy *e = &s->enemies[s->n_enemies++];
        e->control = (int16_t)i;
        e->target = -1;
        e->pos = c->pos;
        e->rot = c->rot;
        e->alert_state = c->alert_state > 2 ? 2 : c->alert_state;
        e->has_cone = (c->radar & 4u) != 0;
        e->cone_dir = c->cone_dir;
        e->cone_dis = c->cone_dis;
        e->cone_range = c->cone_range;
    }
    uint8_t used[BRIDGE_MAX_TARGETS] = {0};
    for (;;) {
        long best = 700L * 700L + 1;
        int be = -1, bt = -1;
        for (int k = 0; k < s->n_enemies; k++) {
            const BridgeEnemy *e = &s->enemies[k];
            if (e->target >= 0) continue;
            const BridgeControl *c = &s->controls[e->control];
            for (int j = 0; j < s->n_targets; j++) {
                const BridgeTarget *t = &s->targets[j];
                if (used[j] || t->side != 2 || !(t->cls & 1)) continue;
                if (c->map_bits && t->map && !(t->map & c->map_bits)) continue;
                long dx = t->center.x - c->pos.x, dz = t->center.z - c->pos.z;
                long d = dx * dx + dz * dz;
                if (d < best) { best = d; be = k; bt = j; }
            }
        }
        if (be < 0) break;
        s->enemies[be].target = (int16_t)bt;
        used[bt] = 1;
    }
    for (int k = 0; k < s->n_enemies; k++) {
        BridgeEnemy *e = &s->enemies[k];
        if (e->target < 0) continue;
        const BridgeTarget *t = &s->targets[e->target];
        e->vital = t->vital;
        e->faint = t->faint;
        e->down = (t->cls & 32) != 0;
    }
    return BRIDGE_OK_ENEMIES;
}

static uint32_t read_area(Ram m, BridgeState *s) {
    int n = 0;
    for (; n < 8; n++) {
        uint8_t ch = rd8(m, BA_STAGE_CODE + (uint32_t)n);
        if (ch == 0) break;
        /* solo alfanuméricos y '_' (un código a medio copiar o basura no pasa) */
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_')) {
            memset(s->stage_code, 0, sizeof s->stage_code);
            return 0;
        }
        s->stage_code[n] = (char)ch;
    }
    if (n >= 8) {   /* sin terminador NUL dentro de los 8 bytes: no es una cadena válida */
        memset(s->stage_code, 0, sizeof s->stage_code);
        return 0;
    }
    s->stage_code[n] = 0;
    s->area_id = rds16(m, BA_AREA_ID);
    for (int i = 0; i < 8; i++) s->area_history[i] = rds16(m, BA_AREA_HISTORY + 2u * (uint32_t)i);
    return n >= 1 ? BRIDGE_OK_AREA : 0;
}

static uint32_t read_alert(Ram m, BridgeState *s) {
    s->alert_mode = (int32_t)rd32(m, BA_ALERT_MODE);
    s->alert_level = (int32_t)rd32(m, BA_ALERT_LEVEL);
    s->alert_max = (int32_t)rd32(m, BA_ALERT_MAX);
    s->noise_power = (int32_t)rd32(m, BA_NOISE_POWER);
    s->noise_len = (int32_t)rd32(m, BA_NOISE_LEN);
    s->noise_pos = rdv3(m, BA_NOISE_POS);
    if (s->alert_mode < 0 || s->alert_mode > 3 || s->alert_level < 0 || s->alert_level > 100000) return 0;
    return BRIDGE_OK_ALERT;
}

static uint32_t read_mode(Ram m, BridgeState *s) {
    s->game_status = rd32(m, BA_GAME_STATUS);
    s->pause_level = (int32_t)rd32(m, BA_PAUSE_LEVEL);
    s->menu_state = rd8(m, BA_MENU_WORK + 0x2A);
    if (s->pause_level < 0 || s->pause_level > 15 || s->menu_state > 7) return 0;
    uint32_t g = s->game_status;
    s->comms = (s->menu_state & 4) != 0;
    s->menu_open = (s->menu_state & 3) != 0;
    s->paused = (s->pause_level & 2) != 0;
    s->stopped = (s->pause_level & 1) != 0;
    s->on_screen_text = (g & BRIDGE_GS_VOX_STREAM) != 0;
    /* Escena: demo del motor, o escena con el mando retirado (menú y radar anulados a la vez) */
    s->cinematic = (g & (BRIDGE_GS_DEMO | BRIDGE_GS_PADDEMO)) != 0 ||
                   ((g & BRIDGE_GS_MENU_DISABLE) && (g & BRIDGE_GS_LIFE_OFF) && (g & BRIDGE_GS_RADAR_OFF));
    s->in_control = !s->cinematic && !s->comms && !s->paused && !s->menu_open && !s->stopped &&
                    !(g & BRIDGE_GS_GAME_OVER);
    return BRIDGE_OK_MODE;
}

/* Pone a cero los campos de los grupos no válidos (contrato: grupo no válido = todo a cero). */
static void clear_invalid(BridgeState *s, uint32_t ok) {
    if (!(ok & BRIDGE_OK_PLAYER_POS)) memset(&s->player_pos, 0, sizeof s->player_pos);
    if (!(ok & BRIDGE_OK_PLAYER_CTL)) {
        memset(&s->ctl_pos, 0, sizeof s->ctl_pos);
        memset(&s->ctl_rot, 0, sizeof s->ctl_rot);
        s->ctl_height = 0;
        s->ctl_addr = 0;
    }
    if (!(ok & BRIDGE_OK_PLAYER_ST)) {
        s->player_map = s->player_action = 0;
        s->player_status = 0;
        s->o2 = s->magazine = s->magazine_max = 0;
        s->disable_weapon = s->disable_item = 0;
        s->crouching = s->prone = s->on_wall = s->knocking = s->first_person = s->in_box = s->moving = s->hidden =
            s->damaged = s->in_water = 0;
    }
    if (!(ok & BRIDGE_OK_EYE)) {
        memset(s->eye_pos, 0, sizeof s->eye_pos);
        memset(s->eye_axes, 0, sizeof s->eye_axes);
        memset(s->view_t, 0, sizeof s->view_t);
        s->eye_focal = 0;
    }
    if (!(ok & BRIDGE_OK_CAMERA)) {
        memset(&s->cam_pos, 0, sizeof s->cam_pos);
        memset(&s->cam_target, 0, sizeof s->cam_target);
        memset(&s->cam_rot, 0, sizeof s->cam_rot);
        s->cam_zoom = s->cam_first_person = s->cam_type = 0;
        s->cam_track = 0;
    }
    if (!(ok & BRIDGE_OK_ACTORS)) { s->n_actors = 0; memset(s->actors, 0, sizeof s->actors); }
    if (!(ok & BRIDGE_OK_CONTROLS)) { s->n_controls = 0; memset(s->controls, 0, sizeof s->controls); }
    if (!(ok & BRIDGE_OK_TARGETS)) { s->n_targets = 0; memset(s->targets, 0, sizeof s->targets); }
    if (!(ok & BRIDGE_OK_ENEMIES)) { s->n_enemies = 0; memset(s->enemies, 0, sizeof s->enemies); }
    if (!(ok & BRIDGE_OK_ALERT)) {
        s->alert_mode = s->alert_level = s->alert_max = s->noise_power = s->noise_len = 0;
        memset(&s->noise_pos, 0, sizeof s->noise_pos);
    }
    if (!(ok & BRIDGE_OK_AREA)) {
        memset(s->stage_code, 0, sizeof s->stage_code);
        s->area_id = 0;
        memset(s->area_history, 0, sizeof s->area_history);
    }
    if (!(ok & BRIDGE_OK_MODE)) {
        s->game_status = 0;
        s->pause_level = 0;
        s->menu_state = 0;
        s->cinematic = s->comms = s->paused = s->menu_open = s->stopped = s->in_control = s->on_screen_text = s->cam_ctl = 0;
    }
}

uint32_t bridge_read(const uint8_t *ram, BridgeState *out) {
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    if (!ram) return 0;
    Ram m = {ram};
    uint32_t ok = 0;
    ok |= read_vars(m, out);
    if (!(ok & BRIDGE_OK_VARS)) { memset(out, 0, sizeof *out); return 0; } /* sin variables coherentes no hay partida */
    ok |= read_player(m, out);
    ok |= read_camera(m, out);
    ok |= read_actors(m, out);
    ok |= read_controls(m, out);
    ok |= read_targets(m, out);
    if ((ok & BRIDGE_OK_CONTROLS) && (ok & BRIDGE_OK_TARGETS)) ok |= derive_enemies(out);
    ok |= read_alert(m, out);
    ok |= read_area(m, out);
    ok |= read_mode(m, out);
    /* Hay jugador controlable en la escena: bloque de control vivo, posición coherente y estado sano. Durante el
     * título, las cargas y los cambios de zona no existe (las demás variables conservan valores iniciales y
     * "parecen" válidas), así que NO basta con los demás grupos. */
    const uint32_t need = BRIDGE_OK_PLAYER_POS | BRIDGE_OK_PLAYER_CTL | BRIDGE_OK_PLAYER_ST;
    out->has_player = (ok & need) == need;
    if ((ok & BRIDGE_OK_MODE) && (!out->has_player || (out->player_status & (BRIDGE_PS_NOT_PLAYABLE | BRIDGE_PS_GAME_OVER))))
        out->in_control = 0;
    /* vida a cero: muerte en curso (el juego retira el control y pone su cámara de muerte; las banderas de fin de partida llegan más tarde) */
    if ((ok & BRIDGE_OK_LIFE) && out->life_max > 0 && out->life <= 0) out->in_control = 0;
    out->cam_ctl = out->in_control;
    if (!out->in_control && (ok & BRIDGE_OK_MODE) && out->has_player && (out->menu_open || out->paused) &&
        !out->cinematic && !out->comms && !out->stopped &&
        !(out->game_status & BRIDGE_GS_GAME_OVER) && !(out->player_status & (BRIDGE_PS_NOT_PLAYABLE | BRIDGE_PS_GAME_OVER)) &&
        !((ok & BRIDGE_OK_LIFE) && out->life_max > 0 && out->life <= 0))
        out->cam_ctl = 1;
    clear_invalid(out, ok);
    out->valid = ok;
    return ok;
}

const char *bridge_mode_name(const BridgeState *s) {
    if (!(s->valid & BRIDGE_OK_MODE)) return "?";
    if (!s->has_player) return "sin_jugador";   /* título, carga o cambio de zona */
    if (s->comms) return "comms";
    if (s->paused) return "pausa";
    if (s->menu_open) return "menu";
    if (s->cinematic) return "escena";
    if (s->stopped) return "detenido";
    return "juego";
}
