#include "ns_present.h"
#include <string.h>

NsPresentCfg ns_present_cfg_default(void) { NsPresentCfg c = { 1500ull * 1000000ull, 700ull * 1000000ull, 250ull * 1000000ull }; return c; }
void ns_present_reset(NsPresentState *s) { if (s) memset(s, 0, sizeof *s); }

int ns_present_step(NsPresentState *s, const NsPresentCfg *cfgp, const NsPresentIn *in, NsPresentOut *out) {
    NsPresentState tmp; if (!s) { memset(&tmp, 0, sizeof tmp); s = &tmp; }
    NsPresentCfg cfg = cfgp ? *cfgp : ns_present_cfg_default();
    NsPresentOut o; memset(&o, 0, sizeof o);
    uint32_t why = 0;
    if (!in) { o.mode = s->have_good ? NS_PR_HOLD : NS_PR_BLACK; why |= NS_PRW_NO_SCENE; }
    else {
        if (!in->renderer_ok) why |= NS_PRW_NO_RENDERER;
        if (!in->have_scene) why |= NS_PRW_NO_SCENE;
        else {
            if (!in->scene_fit) why |= NS_PRW_UNFIT;
            if (!in->meshes_ok) why |= NS_PRW_MESHES;
            if (in->now_ns > in->scene_t_ns && in->now_ns - in->scene_t_ns > cfg.stale_ns) why |= NS_PRW_STALE;
        }
        if (!in->coherent) why |= NS_PRW_INCOHERENT;
        /* solo lo que impide DIBUJAR una escena entera decide; el resto (vieja, incoherente) se registra */
        const uint32_t blocking = NS_PRW_NO_RENDERER | NS_PRW_NO_SCENE | NS_PRW_UNFIT | NS_PRW_MESHES;
        if (!(why & blocking)) {
            o.mode = NS_PR_CURRENT; o.fade = 1.0f;
            s->have_good = 1; s->last_good_ns = in->now_ns; s->held_frames = 0; s->black_frames = 0;
        }
    }
    if (o.mode == NS_PR_CURRENT && (!in || (why & (NS_PRW_NO_RENDERER | NS_PRW_NO_SCENE | NS_PRW_UNFIT | NS_PRW_MESHES)))) o.mode = NS_PR_HOLD;   /* (inalcanzable; defensivo) */
    if (o.mode != NS_PR_CURRENT) {
        if (!s->have_good) { o.mode = NS_PR_BLACK; o.fade = 0.0f; }
        else {
            uint64_t now = in ? in->now_ns : s->last_good_ns;
            uint64_t el = now > s->last_good_ns ? now - s->last_good_ns : 0;
            if (el <= cfg.hold_ns) { o.mode = NS_PR_HOLD; o.fade = 1.0f; }
            else if (cfg.fade_ns && el < cfg.hold_ns + cfg.fade_ns) {
                o.mode = NS_PR_HOLD;
                o.fade = 1.0f - (float)(el - cfg.hold_ns) / (float)cfg.fade_ns;
            } else { o.mode = NS_PR_BLACK; o.fade = 0.0f; }
        }
        if (o.mode == NS_PR_HOLD) s->held_frames++; else s->black_frames++;
    }
    o.why = why;
    if (o.mode == NS_PR_CURRENT) s->n_current++; else if (o.mode == NS_PR_HOLD) s->n_hold++; else s->n_black++;
    if (out) *out = o;
    return o.mode;
}
