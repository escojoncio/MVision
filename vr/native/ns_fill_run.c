/* ns_fill_run: sesión completa del relleno (ver ns_fill.h): instantánea de la RAM del motor + lista de la GPU -> NsEffect. Mismo flujo que ns_cover_run: extrae la escena con
 * un almacén propio, la adapta a triángulos de mundo, excluye de la lista lo que ya cubren los efectos nativos (ns_fx: carteles, ondas...) y rellena el resto. */
#include "ns_fill.h"
#include "ns_extract.h"
#include "ns_fx.h"
#include "ns_player.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct NsFillSession {
    NsScene sc; NsMeshStore *st; NsFxScene *fx; NsFill *fill; int ok;
    uint8_t *fxcls; int32_t *fxit; size_t fx_cap;
    double ms_extract, ms_adapt, ms_fx;
    NsRTri *k_tr; NsAdaptSrc *k_src; size_t k_n; uint8_t *k_skip; uint32_t k_nops; NsFillCam k_cam;
};

static void keep_free(NsFillSession *s) { free(s->k_tr); free(s->k_src); free(s->k_skip); s->k_tr = NULL; s->k_src = NULL; s->k_skip = NULL; s->k_n = 0; }

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec * 1e3 + (double)t.tv_nsec * 1e-6; }

NsFillSession *ns_fill_session_create(void) {
    NsFillSession *s = (NsFillSession *)calloc(1, sizeof *s);
    if (!s) return NULL;
    s->st = ns_store_create();
    s->fill = ns_fill_create();
    if (!s->st || !s->fill || ns_scene_alloc(&s->sc, NS_MIN_INST, 8192, NS_MIN_LIGHT, 0)) { ns_fill_session_destroy(s); return NULL; }
    ns_store_set_limit(s->st, 96u * 1024u * 1024u);
    s->ok = 1;
    return s;
}
void ns_fill_session_destroy(NsFillSession *s) {
    if (!s) return;
    if (s->ok) ns_scene_free(&s->sc);
    if (s->st) ns_store_destroy(s->st);
    if (s->fx) ns_fx_scene_destroy(s->fx);
    ns_fill_destroy(s->fill);
    keep_free(s);
    free(s->fxcls); free(s->fxit);
    free(s);
}
NsFill *ns_fill_session_fill(NsFillSession *s) { return s ? s->fill : NULL; }
int ns_fill_session_zone(const NsFillSession *s, uint32_t *zone_gen, uint32_t *epoch) {
    if (!s || !s->ok) return -1;
    if (zone_gen) *zone_gen = s->sc.zone_gen;
    if (epoch) *epoch = s->sc.mesh_epoch;
    return 0;
}
const NsEffect *ns_fill_session_effects(const NsFillSession *s, uint32_t *n) { return s ? ns_fill_effects(s->fill, n) : (n ? (*n = 0, NULL) : NULL); }

int ns_fill_session_run(NsFillSession *s, const uint8_t *ram, const uint8_t *scratch, const GpuOp *ops, uint32_t nops, const NsFillRunOpts *ro, const NsFillCfg *cfg_in, uint32_t frame,
                        NsFillStats *st, int *cam_used) {
    if (!s || !s->ok || !ram) return -1;
    NsFillCfg cfg;
    if (cfg_in) cfg = *cfg_in; else ns_fill_cfg_default(&cfg);
    double t0 = now_ms();
    if (ns_extract_ex(&s->sc, s->st, ram, NS_RAM_SIZE, scratch, scratch ? 1024u : 0u, NULL, NULL) != 0) return -2;
    ns_store_ack(s->st, ns_store_epoch(s->st));   /* único consumidor */
    double t1 = now_ms();
    NsAdaptOpts ao;
    memset(&ao, 0, sizeof ao);
    NsAdaptSrc *src = NULL;
    ao.src_out = &src; ao.no_tint = 1; ao.exposure = 1.0f;
    NsRTri *tr = NULL; NsAdaptStats as;
    size_t n = ns_adapt_scene(&s->sc, s->st, &ao, &tr, &as);
    double t2 = now_ms();
    /* efectos: lo que ns_fx ya dibuja en nativo se excluye de la lista (como ns_cover_run) */
    uint8_t *skip = NULL;
    if (nops) {
        if (!s->fx) s->fx = ns_fx_scene_create(0);
        if (s->fx_cap < nops) {
            free(s->fxcls); free(s->fxit);
            s->fxcls = (uint8_t *)malloc(nops); s->fxit = (int32_t *)malloc(sizeof(int32_t) * nops); s->fx_cap = (s->fxcls && s->fxit) ? nops : 0;
        }
        skip = (uint8_t *)calloc(nops, 1);
    }
    NsFxListCam fxlc; const NsFxListCam *fxplc = NULL;
    int fx_fail = 0;   /* sin la clasificación de efectos no se sabe qué excluir: rellenar así duplicaría lo que ns_fx ya dibuja */
    if (s->fx && skip && s->fx_cap >= nops) {
        NsFxOpts fo = ns_fx_opts_default();
        if (ns_fx_extract(ram, &fo, s->fx) != 0) fx_fail = 1;
        else {
            GpuDrawList dl;
            memset(&dl, 0, sizeof dl);
            dl.ops = (GpuOp *)ops; dl.nops = nops;
            for (uint32_t i = 0; i < nops; i++) s->fxit[i] = -1;
            if (ns_fx_classify(ram, s->fx, &dl, s->fxcls, NULL, s->fxit, NULL) == 0) {
                if (ro && ro->list_eye && ro->list_M) { memcpy(fxlc.eye, ro->list_eye, sizeof fxlc.eye); memcpy(fxlc.M, ro->list_M, sizeof fxlc.M); fxplc = &fxlc; }
                ns_fx_classify_near(s->fx, fxplc, &dl, s->fxcls, s->fxit);
                for (uint32_t i = 0; i < nops; i++) {
                    const GpuOp *o = &ops[i];
                    int isfx = (s->fxcls[i] == NSFX_OP_FX || s->fxcls[i] == NSFX_OP_FX_PACK) && o->type == GPU_OP_POLY && o->nv >= 3;
                    if (isfx) for (int k = 0; k < o->nv; k++) if (!o->v[k].vvalid) isfx = 0;
                    skip[i] = (uint8_t)isfx;
                }
            } else fx_fail = 1;
        }
    }
    if (nops && !(s->fx && skip && s->fx_cap >= nops)) fx_fail = 1;
    if (fx_fail) { free(skip); free(tr); free(src); return -3; }
    double t3 = now_ms();
    s->ms_extract = t1 - t0; s->ms_adapt = t2 - t1; s->ms_fx = t3 - t2;
    NsFillNative nat = {tr, src, n, &s->sc};
    /* cámaras: la de la lista (si se da) y la de la RAM */
    NsFillCam ram_cam, list_cam;
    memcpy(ram_cam.eye, s->sc.cam.eye, sizeof ram_cam.eye); memcpy(ram_cam.R, s->sc.cam.rot, sizeof ram_cam.R);
    ram_cam.ys = (ro && ro->list_vr) ? 1.0f : (s->sc.cam.overscan_y > 0 ? s->sc.cam.overscan_y : 1.0f); list_cam.ys = ram_cam.ys;
    const NsFillCam *use = &ram_cam, *other = NULL;
    int used = 1;
    cfg.player_key = (ro && ro->player_key) ? ro->player_key : ns_player_key(ram);
    if (ro && ro->list_eye && ro->list_M) {
        memcpy(list_cam.eye, ro->list_eye, sizeof list_cam.eye); memcpy(list_cam.R, ro->list_M, sizeof list_cam.R); list_cam.ys = ram_cam.ys;
        use = &list_cam; other = &ram_cam; used = 0;
        if (!ro->no_auto_cam) {
            float pl = ns_fill_probe(s->fill, &nat, ops, nops, skip, &list_cam, &cfg), pr = ns_fill_probe(s->fill, &nat, ops, nops, skip, &ram_cam, &cfg);
            if (pr >= 0 && pr > pl + 0.10f) { use = &ram_cam; other = &list_cam; used = 1; }
        }
    }
    if (cam_used) *cam_used = used;
    double t4 = now_ms();
    int rc = ns_fill_run(s->fill, &nat, ops, nops, skip, use, other, &cfg, frame, st);
    if (rc == 0 && st) { st->cam_src = (ro && ro->list_eye && ro->list_M) ? used : 2; st->ms_total += t4 - t3; st->ms_index += 0; st->ms_extract = s->ms_extract; st->ms_adapt = s->ms_adapt; st->ms_fx = s->ms_fx; st->ms_total += s->ms_extract + s->ms_adapt + s->ms_fx; }
    keep_free(s);
    if (ro && ro->keep_native) { s->k_tr = tr; s->k_src = src; s->k_n = n; s->k_skip = skip; s->k_nops = nops; s->k_cam = *use; }
    else { free(skip); free(tr); free(src); }
    return rc;
}

int ns_fill_session_native(const NsFillSession *s, NsFillNative *nat, const uint8_t **skip, NsFillCam *cam) {
    if (!s || !s->k_tr) return -1;
    if (nat) { nat->tris = s->k_tr; nat->src = s->k_src; nat->n = s->k_n; nat->sc = &s->sc; }
    if (skip) *skip = s->k_skip;
    if (cam) *cam = s->k_cam;
    return 0;
}
