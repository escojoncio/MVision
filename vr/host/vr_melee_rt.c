/* Ganchos de vr_melee sobre el motor: cada uno delega en la lógica pura (vr/input/vr_melee.c) y ejecuta el original con rt_call_orig. */
#include "vr_melee_rt.h"
#include <string.h>
#include "cpu.h"
#include "rtpatch.h"

static Vrm V;
static int inited, installed;
Vrm *vrmelee_instance(void) { if (!inited) { vrm_init(&V); inited = 1; } return &V; }
int vrmelee_rt_installed(void) { return installed; }

/* Ejecuta una función del motor con (a0, a1) conservando el estado de registros de quien llama. */
static void call_game(void *user, uint32_t fn, uint32_t a0, uint32_t a1) {
    Cpu *c = (Cpu *)user;
    uint32_t keep[32]; memcpy(keep, c->r, sizeof keep);
    c->r[4] = a0; c->r[5] = a1;
    rt_call_orig(c, fn);
    memcpy(c->r, keep, sizeof keep);
}
static VrmEng eng(Cpu *c) { VrmEng e = { emu_ram, c, call_game }; return e; }

#define A0 c->r[4]
#define A1 c->r[5]

static void h_action_in(Cpu *c) {
    VrmEng e = eng(c);
    if (vrm_action_input(vrmelee_instance(), &e, A0)) { c->r[2] = 1; return; }
    rt_call_orig(c, VRM_FN_ACTION_IN);
}
static void h_knock(Cpu *c) {
    VrmEng e = eng(c);
    if (vrm_knock_check(vrmelee_instance(), &e, A0, A1)) return;
    rt_call_orig(c, VRM_FN_KNOCK_CK);
}
static void h_grab(Cpu *c) {
    VrmEng e = eng(c); uint32_t w = A0, t = A1;
    vrm_pre_grab(vrmelee_instance(), &e, w, t);
    rt_call_orig(c, VRM_FN_GRAB);
    vrm_post_grab(vrmelee_instance(), &e, w, t);
}
static void h_hit(Cpu *c) {
    VrmEng e = eng(c); uint32_t w = A0;
    vrm_pre_hit(vrmelee_instance(), &e, w);
    rt_call_orig(c, VRM_FN_HIT);
    vrm_post_hit(vrmelee_instance(), &e, w, c->r[2]);
}
static void h_move(Cpu *c) {
    VrmEng e = eng(c);
    vrm_move_target(vrmelee_instance(), &e, A0, A1);
    rt_call_orig(c, VRM_FN_MOVE_TGT);
}
static void h_punch_tick(Cpu *c) {
    VrmEng e = eng(c);
    vrm_punch_tick(vrmelee_instance(), &e, A0, A1);
    rt_call_orig(c, VRM_FN_PUNCH_TICK);
}
static void h_hold_tick(Cpu *c) {
    VrmEng e = eng(c);
    if (vrm_hold_tick(vrmelee_instance(), &e, A0)) return;
    rt_call_orig(c, VRM_FN_HOLD_TICK);
}
static void h_sqz_tick(Cpu *c) {
    VrmEng e = eng(c);
    if (vrm_squeeze_tick(vrmelee_instance(), &e, A0, A1)) return;
    rt_call_orig(c, VRM_FN_SQZ_TICK);
}
static void h_drag(Cpu *c) {
    VrmEng e = eng(c); uint32_t w = A0; uint16_t saved;
    vrm_pre_drag(vrmelee_instance(), &e, w, &saved);
    rt_call_orig(c, VRM_FN_DRAG);
    vrm_post_drag(vrmelee_instance(), &e, w, saved);
}

typedef struct { uint32_t addr; FuncPtr fn; unsigned nw; uint64_t hash; } Site;
static const Site SITES[] = {
    {VRM_FN_ACTION_IN,  h_action_in,  40, 0xbba46494a5ca50c9ull},
    {VRM_FN_KNOCK_CK,   h_knock,      40, 0x83662edb4ff1c651ull},
    {VRM_FN_GRAB,       h_grab,       40, 0x197dc7a81d15f4dfull},
    {VRM_FN_HIT,        h_hit,        40, 0x815a023cd3c9e5d7ull},
    {VRM_FN_MOVE_TGT,   h_move,        9, 0x2a7f9cb4d6db4288ull},
    {VRM_FN_PUNCH_TICK, h_punch_tick, 40, 0x3380f5b1fe5121d6ull},
    {VRM_FN_HOLD_TICK,  h_hold_tick,  40, 0x943ef52b4547283aull},
    {VRM_FN_SQZ_TICK,   h_sqz_tick,   40, 0x9698d4a15131c8c9ull},
    {VRM_FN_DRAG,       h_drag,       30, 0xe9d0725671461ca7ull},
};
#define NSITE (sizeof SITES / sizeof SITES[0])

int vrmelee_rt_install(void) {
    vrmelee_instance();
    if (installed) return 0;
    for (unsigned i = 0; i < NSITE; i++) {
        RtPatchGuard g = {SITES[i].nw, SITES[i].hash};
        int r = rt_patch_fn(SITES[i].addr, SITES[i].fn, &g);
        if (r != RT_PATCH_OK) {
            for (unsigned k = 0; k < i; k++) rt_unpatch(SITES[k].addr);
            return -1 - (int)i;
        }
    }
    vrm_reset(&V);
    installed = 1;
    return 0;
}
void vrmelee_rt_uninstall(void) {
    if (!installed) return;
    for (unsigned i = 0; i < NSITE; i++) rt_unpatch(SITES[i].addr);
    installed = 0;
}
void vrmelee_rt_frame(const VrmFrame *f) {
    if (!installed) return;
    VrmEng e = { emu_ram, NULL, NULL };
    vrm_frame(vrmelee_instance(), &e, f);
}
