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
        /* Sprite texturizado sin página (ni aprendida de una lista anterior): el renderer lo dibujaría con la página 0, una textura que no es la suya. Mejor esperar a aprenderla. */
        if ((it->flags & NSFX_F_TEXTURED) && (it->flags & NSFX_F_TPAGE_UNKNOWN)) continue;
        /* Cartel de tamaño en píxeles que el motor no emitió y sin tamaño en mundo aprendido: no se inventa una profundidad (antes salían carteles de cientos de metros) */
        if (it->flags & NSFX_F_NOSIZE) continue;
        /* sin sitio; el cielo (siempre al final de la escena) sale entero o no sale */
        if (n >= cap || (it->role == NSFX_ROLE_SKY && sc->n_items - i > cap - n)) { if (truncated) *truncated = 1; break; }
        NsEffect *e = &out[n++];
        memset(e, 0, sizeof *e);
        e->depth_bias = it->raise; e->abr = it->abr; e->tpage = it->tpage; e->clut = it->clut; e->src_key = it->prim; e->idx = it->idx; e->role = it->role; e->sub = (uint8_t)(it->sub & 0xFF);
        e->flags = (uint16_t)(((it->flags & NSFX_F_SEMI) ? NS_EF_SEMI : 0) | ((it->flags & NSFX_F_TEXTURED) ? NS_EF_TEXTURED : 0) | ((it->flags & NSFX_F_GOURAUD) ? NS_EF_GOURAUD : 0) |
                              ((it->flags & NSFX_F_ONESIDE) ? NS_EF_ONESIDE : 0) | ((it->flags & NSFX_F_VIEWLOCKED) ? NS_EF_ON_CAMERA : 0));
        memcpy(e->rgb, it->rgb, sizeof e->rgb); memcpy(e->uv, it->uv, sizeof e->uv);
        if (it->role == NSFX_ROLE_SKY) e->flags |= NS_EF_NOFOG;   /* el fondo no pasa por la niebla del motor (no es geometría de la escena) */
        const float (*P)[3] = it->pos;   /* mundo del motor (los pegados a la cámara: con la cámara actual, que es la de la cabeza en VR) */
        float H = sc->cam.proj_h;
        e->nv = it->nv;
        switch (it->shape) {
        case NSFX_SH_BILLBOARD:
            e->kind = NS_EK_BILLBOARD; e->nv = 4; e->flags |= NS_EF_WORLD_SIZE;
            memcpy(e->p[0], P[0], sizeof e->p[0]);
            memcpy(e->box, it->off, sizeof e->box);   /* tamaño en mundo: el que salió de la proyección exacta del motor (o el aprendido de su emisión) */
            e->ref_h = H;
            break;
        case NSFX_SH_RIBBON:
            e->kind = NS_EK_QUAD; e->nv = 2; e->flags |= NS_EF_LINE;
            memcpy(e->p[0], P[0], sizeof e->p[0]); memcpy(e->p[1], P[1], sizeof e->p[1]);
            e->flags |= NS_EF_WORLD_SIZE; e->width_px = it->width[0];
            break;
        default:
            e->kind = NS_EK_QUAD; e->nv = it->shape == NSFX_SH_TRI ? 3 : 4;
            for (int v = 0; v < e->nv; v++) memcpy(e->p[v], P[v], sizeof e->p[v]);
            break;
        }
    }
    return n;
}

void ns_fx_export_screen(const NsFxScene *sc, NsScreenFx *o) {
    if (!o) return;
    memset(o, 0, sizeof *o);
    if (!sc) return;
    const NsFxScreen *s = &sc->screen;
    o->status = s->status;
    if (s->water.active) {
        o->flags |= NS_SFX_WATER;
        memcpy(o->water_min, s->water.box_min, sizeof o->water_min); memcpy(o->water_max, s->water.box_max, sizeof o->water_max); memcpy(o->water_tint, s->water.tint, sizeof o->water_tint);
        o->water_phase = s->water.phase; o->water_wave_rows = s->water.wave_rows; o->water_wave_period = s->water.wave_period; o->water_bar_rows = s->water.bar_rows;
    }
    if (s->gas.active) {
        o->flags |= NS_SFX_GAS;
        o->gas_intensity = s->gas.intensity; o->gas_inner_px = s->gas.inner_px; o->gas_outer_px = s->gas.outer_px; o->gas_aspect = s->gas.aspect; o->gas_pulse = s->gas.pulse_amp;
        memcpy(o->gas_in, s->gas.col_inner, sizeof o->gas_in); memcpy(o->gas_out, s->gas.col_outer, sizeof o->gas_out); o->gas_abr = s->gas.abr & 3;
    }
    if (s->fade.active) { o->flags |= NS_SFX_FADE; o->fade_amount = s->fade.amount; o->fade_white = s->fade.white; o->fade_in = s->fade.fade_in; }
    if (s->flash) o->flags |= NS_SFX_FLASH;
    if (s->mode_a) o->flags |= NS_SFX_MODE_A;
    if (s->mode_b) o->flags |= NS_SFX_MODE_B;
    if (s->aux) o->flags |= NS_SFX_AUX;
}
