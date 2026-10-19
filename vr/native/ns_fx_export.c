#include "ns_fx_export.h"
#include <math.h>
#include <string.h>

uint32_t ns_fx_export(const NsFxScene *sc, int include_culled, NsEffect *out, uint32_t cap, int *truncated) {
    uint32_t n = 0;
    if (truncated) *truncated = 0;
    if (!sc || !out) return 0;
    for (uint32_t i = 0; i < sc->n_items; i++) {
        const NsFxItem *it = &sc->items[i];
        if (!include_culled && (it->flags & NSFX_F_ENGINE_CULLED)) continue;
        if (n >= cap) { if (truncated) *truncated = 1; break; }
        NsEffect *e = &out[n++];
        memset(e, 0, sizeof *e);
        e->depth_bias = it->raise; e->abr = it->abr; e->tpage = it->tpage; e->clut = it->clut; e->src_key = it->prim; e->idx = it->idx; e->sub = (uint8_t)(it->sub & 0xFF);
        e->flags = (uint16_t)(((it->flags & NSFX_F_SEMI) ? NS_EF_SEMI : 0) | ((it->flags & NSFX_F_TEXTURED) ? NS_EF_TEXTURED : 0) | ((it->flags & NSFX_F_GOURAUD) ? NS_EF_GOURAUD : 0) |
                              ((it->flags & NSFX_F_ONESIDE) ? NS_EF_ONESIDE : 0) | ((it->flags & NSFX_F_VIEWLOCKED) ? NS_EF_ON_CAMERA : 0));
        memcpy(e->rgb, it->rgb, sizeof e->rgb); memcpy(e->uv, it->uv, sizeof e->uv);
        const float (*P)[3] = it->pos;   /* mundo del motor (los pegados a la cámara: con la cámara actual, que es la de la cabeza en VR) */
        float z = it->vpos[0][2] > 1.0f ? it->vpos[0][2] : 1.0f, H = sc->cam.proj_h, ys = sc->cam.y_scale;
        e->nv = it->nv;
        switch (it->shape) {
        case NSFX_SH_BILLBOARD:
            e->kind = NS_EK_BILLBOARD; e->nv = 4;
            memcpy(e->p[0], P[0], sizeof e->p[0]);
            e->box[0] = it->off[0] * H / z; e->box[1] = it->off[1] * ys * H / z; e->box[2] = it->off[2] * H / z; e->box[3] = it->off[3] * ys * H / z;
            e->ref_h = H;
            break;
        case NSFX_SH_RIBBON:
            e->kind = NS_EK_QUAD; e->nv = 2; e->flags |= NS_EF_LINE;
            memcpy(e->p[0], P[0], sizeof e->p[0]); memcpy(e->p[1], P[1], sizeof e->p[1]);
            e->width_px = it->width[0] * H / z;
            break;
        default:
            e->kind = NS_EK_QUAD; e->nv = it->shape == NSFX_SH_TRI ? 3 : 4;
            for (int v = 0; v < e->nv; v++) memcpy(e->p[v], P[v], sizeof e->p[v]);
            break;
        }
    }
    return n;
}
