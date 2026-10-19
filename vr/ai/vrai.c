/* vrai: ver vrai.h. */
#include "vrai.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "bridge.h"
#include "cpu.h"
#include "rtpatch.h"
#include "vr_activity.h"
#include "vr_activity_rt.h"
#include "vr_ai_scan.h"
#include "vr_cameras.h"
#include "vr_perception.h"
#include "vr_perception_rt.h"

/* Ventana de código de las overlays de zona (base común de carga) y función de reproducción posicional de sonidos del
 * ejecutable principal (edición europea) con su guarda de hash. */
#define OV_LO 0x800C4380u
#define OV_HI 0x80100000u
#define SE_PLAY_POS 0x800335a0u
#define SE_PLAY_NW 67u
#define SE_PLAY_HASH 0xcd503c2b4fcaf495ull
#define SE_SEND 0x80089e40u            /* envío de la orden de sonido (a0 = orden, a1 = 0) */
#define STAGE_CODE 0x800AC568u

static _Atomic int opt_perc = VRP_VR, opt_cam = 1, opt_act = 1, opt_motion, opt_range100 = 150, opt_head = 1;
static _Atomic int dirty = 1;
static void (*listener)(const VrpConfig *);
static int inited, installed;
static int cur_perc = -1, cur_cam = -1, cur_act = -1;     /* lo aplicado en el hilo de emulación */
static VrActivity act;
static VraiStats S;
static uint32_t ov_sample, ov_located, ov_stable;
static int ov_have_located;

void vrai_options_default(VraiOptions *o) { o->perception = VRP_VR; o->cameras = 1; o->activity = 1; }
void vrai_set_options(const VraiOptions *o) {
    int p = o->perception;
    if (p < 0 || p > 2) p = VRP_VR;
    atomic_store(&opt_perc, p); atomic_store(&opt_cam, o->cameras != 0); atomic_store(&opt_act, o->activity != 0);
    atomic_store(&dirty, 1);
}
void vrai_get_options(VraiOptions *o) {
    o->perception = atomic_load(&opt_perc); o->cameras = atomic_load(&opt_cam); o->activity = atomic_load(&opt_act);
}
void vrai_set_perception(int p) {
    VraiOptions o;
    vrai_get_options(&o);
    o.perception = p;
    vrai_set_options(&o);
}
void vrai_set_range_scale(float s) { if (!(s >= 1.0f)) s = 1.0f; if (s > 3.0f) s = 3.0f; atomic_store(&opt_range100, (int)(s * 100.0f + 0.5f)); atomic_store(&dirty, 1); }
float vrai_get_range_scale(void) { return (float)atomic_load(&opt_range100) / 100.0f; }
void vrai_set_head_perception(int on) { atomic_store(&opt_head, on != 0); atomic_store(&dirty, 1); }
void vrai_feed_head(const float off[3]) { vrp_rt_set_head(off); }
void vrai_set_perception_listener(void (*fn)(const VrpConfig *cfg)) { listener = fn; }
void vrai_set_motion(int moving) { atomic_store(&opt_motion, moving != 0); }
VrpConfig vrai_perception_config(void) {
    VrpConfig c;
    vrp_config_preset(&c, (VrpPreset)atomic_load(&opt_perc));
    if (c.enabled) c.range_scale = vrai_get_range_scale();
    return c;
}

/* ---------------------------------------------------------------------------------------------------- sonido de cámaras */
static uint32_t se_calls, se_played, se_dropped;
/* a0 = puntero a la posición (3 x s16) del emisor, a1 = identificador del sonido. Solo cambia los sonidos de cámara: el resto
 * y el modo original pasan al código real. */
static void se_patch(Cpu *c) {
    uint32_t pos = c->r[4], se = c->r[5], cmd = 0;
    int r = vrc_se_intercept(emu_ram, pos, se, &cmd);
    se_calls++;
    if (r == VRC_SE_PASS) { rt_call_orig(c, SE_PLAY_POS); return; }
    if (r == VRC_SE_DROP) { se_dropped++; c->r[2] = 0xFFFFFFFFu; return; }
    se_played++;
    c->r[4] = cmd; c->r[5] = 0;
    rt_call(c, SE_SEND);
}

/* ---------------------------------------------------------------------------------------------------- aplicar ajustes */
static void notify_listener(void) {
    if (!listener) return;
    VrpConfig c;
    vrp_config_preset(&c, (VrpPreset)cur_perc);
    if (c.enabled) c.range_scale = vrai_get_range_scale();
    listener(&c);
}

static void apply_options(void) {
    int p = atomic_load(&opt_perc), cm = atomic_load(&opt_cam), ac = atomic_load(&opt_act);
    atomic_store(&dirty, 0);
    if (!installed) return;
    vrp_rt_set_range_scale(vrai_get_range_scale());
    vrp_rt_set_head_enabled(atomic_load(&opt_head));
    if (p != cur_perc) { cur_perc = p; vrp_rt_set_preset((VrpPreset)p); notify_listener(); }
    else notify_listener();
    if (cm != cur_cam) {
        int first = cur_cam < 0;
        cur_cam = cm;
        if (cm) {
            const RtPatchGuard g = {SE_PLAY_NW, SE_PLAY_HASH};
            if (rt_patch_fn(SE_PLAY_POS, se_patch, &g)) fprintf(stderr, "[vrai] no se pudo parchear el sonido de las cámaras\n");
        } else if (!first) {
            rt_unpatch(SE_PLAY_POS);
        }
        VrcConfig vc;
        vrc_config_get(&vc);
        vc.bullets_destroy = cm; vc.pickup_game_bullets = cm;
        vrc_config_set(&vc);
        VrcSoundCfg sc;
        vrc_sound_get(&sc);
        sc.mode = cm ? VRC_SND_NATURAL : VRC_SND_ORIGINAL;
        vrc_sound_set(&sc);
    }
    if (ac != cur_act) {
        int first = cur_act < 0;
        cur_act = ac;
        if (ac) {
            int ir = vra_rt_install_fnpatch(); if (ir) { fprintf(stderr, "[vrai] no se pudo parchear la actividad fuera de cámara (código %d; hashes %016llx %016llx)\n", ir, (unsigned long long)rt_code_hash(VRA_EU_POINTCHECK, VRA_EU_POINTCHECK_NW), (unsigned long long)rt_code_hash(VRA_EU_CLIP_DRAW, VRA_EU_CLIP_DRAW_NW)); cur_act = 0; ac = 0; }   /* sin parches no hay modo VR de actividad */
        } else if (!first) {
            vra_rt_uninstall();
        }
        vra_set_mode(&act, VRA_CLASS_SOLDIER, ac ? VRA_VR : VRA_ORIGINAL);
        vra_set_mode(&act, VRA_CLASS_SENSOR, ac ? VRA_VR : VRA_ORIGINAL);
    }
}

void vrai_on_exe_loaded(void) {
    /* La RAM se recarga: los parches viejos ya no valen (cualquier sustitución de la partida anterior) */
    rt_unpatch_all();
    installed = 0;
    cur_perc = cur_cam = cur_act = -1;
    ov_have_located = 0; ov_stable = 0; ov_sample = 0;
    se_calls = se_played = se_dropped = 0;
    memset(&S, 0, sizeof S);
    vrc_reset_all();
    vra_init(&act);
    vra_rt_attach(&act);
    VrpPreset p = (VrpPreset)atomic_load(&opt_perc);
    vrp_rt_init(p);
    inited = 1;
    atomic_store(&dirty, 1);
}

int vrai_on_bound(void) {
    if (!inited) return -1;
    installed = 1;
    cur_perc = cur_cam = cur_act = -1;   /* nada instalado todavía: apply_options instala lo que pida cada ajuste */
    apply_options();
    S.installed = 1;
    return 0;
}

/* ---------------------------------------------------------------------------------------------------- por fotograma */
static uint32_t ov_hash(void) {
    uint32_t h = 2166136261u;
    for (unsigned i = 0; i < 160; i++) {
        uint32_t a = OV_LO + (((OV_HI - OV_LO) / 160u) & ~3u) * i;
        h = (h ^ vai_r32(emu_ram, a)) * 16777619u;
    }
    for (int i = 0; i < 8; i++) h = (h ^ vai_r8(emu_ram, STAGE_CODE + (uint32_t)i)) * 16777619u;
    return h;
}

void vrai_frame(const BridgeState *bs, uint32_t frame) {
    if (!inited || !installed) return;
    if (atomic_load(&dirty)) apply_options();
    vrp_rt_set_external_motion(atomic_load(&opt_motion));
    vrp_rt_frame((float)frame * 0.02f);
    /* ¿overlay nueva? (muestra de la ventana de código + código de zona; se busca cuando lleva 12 fotogramas sin cambiar) */
    uint32_t h = ov_hash();
    if (h != ov_sample) { ov_sample = h; ov_stable = 0; }
    else if (ov_stable < 1000) ov_stable++;
    if (ov_stable == 12 && (!ov_have_located || ov_located != h)) {
        ov_have_located = 1; ov_located = h;
        S.relocates++;
        S.overlay_found = vrp_rt_overlay(OV_LO, OV_HI);
        vrc_reset();
    }
    if (cur_act && bs) vra_frame(&act, emu_ram, bs);
    if (cur_cam) S.cameras_seen = vrc_frame(emu_ram, NULL);
}

void vrai_stats(VraiStats *s) {
    VraiStats r = S;
    const VrpCtx *x = vrp_rt_ctx();
    if (x) { r.vision_calls = x->st.calls; r.extended_hits = x->st.extended_hits; r.suspects = x->st.suspects; r.alerts_far = x->st.alerts_far; }
    r.se_calls = se_calls; r.se_played = se_played; r.se_dropped = se_dropped;
    r.patches = (int)rt_patch_count();
    *s = r;
}
