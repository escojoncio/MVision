/* vr_prov: ver vr_prov.h. Clasificación de las primitivas 2D por procedencia, aprendizaje de la clase WINDOW y extracción de los efectos (viñeta, bandas, mira). */
#include <math.h>
#include <string.h>
#include "vr_prov.h"
#include "vr_feedback.h"

const char *vpc_class_name(int cls) {
    static const char *const nm[VPC_N] = {"HUD_WORLD", "WINDOW", "FULL", "FX_VIGNETTE", "FX_LETTERBOX", "SCOPE_FRAME", "SCOPE_TEXT", "UNKNOWN", "FX_SCREEN", "WORLD", "SCOPE_BINO", "WEAPON_HUD"};
    return cls >= 0 && cls < VPC_N ? nm[cls] : "?";
}

int vpc_layer_of(int cls) {
    switch (cls) {
    case VPC_HUD_WORLD: case VPC_SCOPE_TEXT: case VPC_SCOPE_BINO: case VPC_UNKNOWN: case VPC_WORLD: return VPC_L_HUD;
    case VPC_WINDOW: return VPC_L_WINDOW;
    case VPC_FULL: return VPC_L_FULL;
    case VPC_WEAPON_HUD: return VPC_L_WEAPON;
    default: return -1;
    }
}

static int is_prim(const GpuOp *o) { return o->type == GPU_OP_POLY || o->type == GPU_OP_RECT || o->type == GPU_OP_LINE; }
static int id_ok(const GpuDrawList *l, unsigned id) { return l && l->prov && id > 0 && id < l->nprov && id < PROV_MAX; }
/* id de procedencia de la primitiva: quien encadenó el paquete (prov) o, si no consta, quien rellenó el comando (prov2). Si lo encadenó una rutina de ordenación, que no dibuja, quien lo escribió (prov2; 0 si no consta o también es de ordenación) */
unsigned vpc_op_id(const GpuDrawList *l, const GpuOp *o) {
    if (id_ok(l, o->prov) && vpc_is_chain(&l->prov[o->prov])) return id_ok(l, o->prov2) && !vpc_is_chain(&l->prov[o->prov2]) ? o->prov2 : 0;
    return o->prov ? o->prov : o->prov2;
}

/* ------------------------------------------------------------------------------------------------ aprendizaje */
void vpc_learn_reset(VrProvLearn *S) { if (S) memset(S, 0, sizeof *S); }

/* ¿Trae la lista primitivas de scope.c (prismáticos)? */
static int list_binocular(const GpuDrawList *l) {
    if (!l->prov) return 0;
    for (uint32_t i = 0; i < l->nops; i++) {
        const GpuOp *o = &l->ops[i];
        if (!is_prim(o)) continue;
        unsigned id = vpc_op_id(l, o);
        if (id_ok(l, id) && vpc_is_binocular(&l->prov[id])) return 1;
    }
    return 0;
}

/* ¿Trae la lista primitivas del HUD propio de un arma guiada (stnsight.c, sgtrect3.c o rmissile.c)? */
static int list_guided(const GpuDrawList *l) {
    if (!l->prov) return 0;
    for (uint32_t i = 0; i < l->nops; i++) {
        const GpuOp *o = &l->ops[i];
        if (!is_prim(o)) continue;
        unsigned id = vpc_op_id(l, o);
        if (id_ok(l, id) && vpc_is_weapon_hud(&l->prov[id])) return 1;
    }
    return 0;
}

void vpc_learn_note(VrProvLearn *S, const GpuDrawList *l, int state) {
    if (!S || !l) return;
    if (!S->draw_have || l->index != S->draw_idx) {   /* pareja de búferes: el área mostrada al cerrar la lista anterior es el otro búfer de esta */
        memcpy(S->draw_prev, S->draw_cur, sizeof S->draw_prev);
        S->draw_cur[0] = l->disp.x; S->draw_cur[1] = l->disp.y; S->draw_cur[2] = l->disp.w; S->draw_cur[3] = l->disp.h;
        S->draw_idx = l->index; S->draw_have = 1;
    }
    S->bino = list_binocular(l);
    S->guided = list_guided(l);
    if (state == VPC_ST_OTHER) return;
    S->tick++;
    if (state == VPC_ST_CLOSED) S->nclosed++;
    if (!l->prov) return;
    for (uint32_t i = 0; i < l->nops; i++) {
        const GpuOp *o = &l->ops[i];
        if (!is_prim(o)) continue;
        unsigned id = vpc_op_id(l, o);
        if (!id_ok(l, id) || S->seen[id] == S->tick) continue;
        S->seen[id] = S->tick;   /* una vez por lista y id */
        if (state == VPC_ST_CLOSED) {
            S->last_closed[id] = S->nclosed;
            if (S->win[id] == 1 && ++S->leak[id] >= VPC_LEAK_LISTS) { S->win[id] = 2; S->n_demoted++; }
        } else {
            int base = vpc_rule(&l->prov[id], o);
            int cand = base == VPC_HUD_WORLD || base == VPC_UNKNOWN;
            if (cand && S->win[id] == 0 && S->nclosed >= VPC_QUIET_LISTS && S->nclosed - S->last_closed[id] >= VPC_QUIET_LISTS) { S->win[id] = 1; S->n_learned++; }
        }
    }
}

/* ------------------------------------------------------------------------------------------------ clase de una primitiva */
int vpc_class_op(const VrProvLearn *S, const GpuDrawList *l, const GpuOp *o) {
    if (!l || !o) return VPC_UNKNOWN;
    unsigned id = vpc_op_id(l, o);
    int c = id_ok(l, id) ? vpc_rule(&l->prov[id], o) : VPC_UNKNOWN;
    const int c0 = c;   /* clase de la regla de procedencia, antes de degradar */
    int cov = S && S->covered;   /* sin garantía de cobertura nativa nada sale del 2D por WORLD / FX_SCREEN */
    if (c == VPC_FX_SCREEN && !(cov && S->covered(S->covered_ctx, vpc_screen_kind(&l->prov[id]), 0))) c = VPC_UNKNOWN;   /* el nativo no pinta ahora ese efecto de pantalla: sigue en el HUD */
    /* mosaico de censura: con el parche del mundo hecho para ESTA lista (anfitrión) sus celdas 2D salen del panel (el nativo las dibuja sobre el personaje); sin él siguen en el HUD (no se pierde la censura) */
    if (c == VPC_UNKNOWN && id_ok(l, id) && vpc_is_mosaic(&l->prov[id]) && cov && S->covered(S->covered_ctx, VPC_COV_MOSAIC, 0)) return VPC_WORLD;
    /* realimentación de pantalla (vr_feedback.h): un sprite que muestrea un frame buffer es una copia plana del render de la PS1; si el nativo reproduce el efecto sobre su imagen, sale
     * del panel. El criterio es lo que la primitiva MUESTREA (un hecho de la lista), solo para escritores sin regla propia; el mosaico tiene su parche. */
    if (c0 == VPC_UNKNOWN && cov && !(id_ok(l, id) && vpc_is_mosaic(&l->prov[id])) && vrfb_op_source(o, S->draw_prev, NULL) != VRFB_NONE && S->covered(S->covered_ctx, VPC_COV_FEEDBACK, 0)) return VPC_WORLD;   /* c0: las mismas primitivas que mide el anfitrión (feedback_from_list) */
    if (c == VPC_SCOPE_FRAME && S && S->bino) return VPC_SCOPE_BINO;   /* marco del juego de los prismáticos: a la capa HUD (el visor la pinta sobre la vista aumentada) */
    if ((c == VPC_SCOPE_FRAME || c == VPC_SCOPE_TEXT) && S && S->guided && !S->bino) return VPC_WEAPON_HUD;   /* sight.c de un arma guiada (lista con stnsight.c / sgtrect3.c / rmissile.c, sin prismáticos): rótulo y marco de su HUD, a la capa del arma */
    if (id_ok(l, id) && (c == VPC_HUD_WORLD || c == VPC_UNKNOWN) && S && S->win[id] == 1) return VPC_WINDOW;
    if (c == VPC_UNKNOWN && cov && o->pkt_addr && id_ok(l, o->prov) && vpc_is_chain(&l->prov[o->prov]) && S->covered(S->covered_ctx, VPC_COV_PACKET, o->pkt_addr))
        return VPC_WORLD;   /* del canal 3D, sin escritor con regla y con un elemento que el nativo dibuja: efecto del mundo */
    return c;
}

uint8_t vpc_op_class_cb(const GpuDrawList *l, uint32_t i, void *ctx) {
    if (!l || i >= l->nops) return VPC_UNKNOWN;
    return (uint8_t)vpc_class_op((const VrProvLearn *)ctx, l, &l->ops[i]);
}

void vpc_split_dest(const uint8_t *op_dest, const uint8_t *op_cls, uint32_t n, uint8_t *const out[VPC_L_N], uint32_t cnt[VPC_L_N]) {
    for (int k = 0; k < VPC_L_N; k++) { cnt[k] = 0; if (out[k] && n) memset(out[k], 0, n); }
    if (!op_dest) return;
    for (uint32_t i = 0; i < n; i++) {
        if (op_dest[i] != 2) continue;
        int k = vpc_layer_of(op_cls ? op_cls[i] : VPC_UNKNOWN);
        if (k < 0) continue;
        if (out[k]) out[k][i] = 2;
        cnt[k]++;
    }
}

void vpc_tally(const uint8_t *op_dest, const uint8_t *op_cls, uint32_t n, uint32_t cnt[VPC_N]) {
    memset(cnt, 0, VPC_N * sizeof *cnt);
    if (!op_dest || !op_cls) return;
    for (uint32_t i = 0; i < n; i++) if (op_dest[i] == 2) cnt[op_cls[i] < VPC_N ? op_cls[i] : VPC_UNKNOWN]++;
}

/* ------------------------------------------------------------------------------------------------ efectos */
#define FX_ROWS 512            /* filas de VRAM */
#define FX_ZOOM_MAX 24.0f
#define FX_LETTERBOX_MAX 0.6f  /* más cobertura que esto no es una banda de cinemática (borrado / fundido) */

static void fx_zero(VrFx *fx) { memset(fx, 0, sizeof *fx); fx->scope_zoom = 1.0f; }

/* Caja vertical [y0, y1) de una primitiva (coordenadas de VRAM con el offset de dibujo sumado). 0 si no tiene. */
static int prim_rows(const GpuOp *o, int *y0, int *y1) {
    if (o->type == GPU_OP_RECT) { *y0 = o->v[0].y; *y1 = o->v[0].y + o->rh; return o->rh > 0; }
    int nv = o->type == GPU_OP_LINE ? 2 : o->nv == 4 ? 4 : 3;
    int a = o->v[0].y, b = a;
    for (int k = 1; k < nv; k++) { if (o->v[k].y < a) a = o->v[k].y; if (o->v[k].y > b) b = o->v[k].y; }
    *y0 = a; *y1 = b + 1;
    return 1;
}

void vpc_fx_extract(const VrProvLearn *S, const GpuDrawList *l, int rec_screen, VrFx *fx) {
    if (!fx) return;
    fx_zero(fx);
    if (!l || !l->prov || !l->ops) return;
    const int dh = l->disp.h > 0 ? l->disp.h : 240;
    const int ox = l->disp.x, oy = l->disp.y;
    static uint8_t rows[FX_ROWS];   /* hilo de emulación (única llamada) */
    int any_bar = 0, scope = 0, nvert = 0;
    float cmax = 0.0f, minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
    float vx[256], vy[256];
    memset(rows, 0, sizeof rows);
    /* Los efectos (bandas / viñeta / mira) no dependen de la cobertura nativa (solo FX_SCREEN y WORLD, que aquí no se usan): sin gancho se evita ns_fx_covers_packet por primitiva. */
    VrProvLearn nocov;
    if (S) { nocov = *S; nocov.covered = NULL; nocov.covered_ctx = NULL; }
    for (uint32_t i = 0; i < l->nops; i++) {
        const GpuOp *o = &l->ops[i];
        if (!is_prim(o) || !id_ok(l, vpc_op_id(l, o))) continue;
        int c = vpc_class_op(S ? &nocov : NULL, l, o);
        if (c == VPC_FX_LETTERBOX) {
            int y0, y1;
            if (!prim_rows(o, &y0, &y1)) continue;
            y0 -= oy; y1 -= oy;
            if (y0 < 0) y0 = 0;
            if (y1 > dh) y1 = dh;
            if (y1 > FX_ROWS) y1 = FX_ROWS;
            for (int y = y0; y < y1; y++) rows[y] = 1;
            any_bar = 1;
        } else if (c == VPC_FX_VIGNETTE && o->type == GPU_OP_POLY) {
            int nv = o->nv == 4 ? 4 : 3;
            for (int k = 0; k < nv && nvert < 256; k++) {
                float x = (float)(o->v[k].x - ox), y = (float)(o->v[k].y - oy);
                float col = (float)(o->v[k].r > o->v[k].g ? (o->v[k].r > o->v[k].b ? o->v[k].r : o->v[k].b) : (o->v[k].g > o->v[k].b ? o->v[k].g : o->v[k].b)) / 255.0f;
                vx[nvert] = x; vy[nvert] = y; nvert++;
                if (x < minx) minx = x;
                if (x > maxx) maxx = x;
                if (y < miny) miny = y;
                if (y > maxy) maxy = y;
                if (col > cmax) cmax = col;
            }
        } else if (c == VPC_SCOPE_FRAME || c == VPC_SCOPE_TEXT || c == VPC_SCOPE_BINO || (c == VPC_WEAPON_HUD && vpc_is_scope_actor(&l->prov[vpc_op_id(l, o)]))) {
            scope = 1;   /* el sight.c / stnsight.c del Stinger siguen contando como mira (VrFx.scope_on) aunque su 2D ya no vaya al panel: comportamiento de antes */
        }
    }
    if (any_bar) {
        int n = 0;
        for (int y = 0; y < dh && y < FX_ROWS; y++) n += rows[y];
        float f = (float)n / (float)dh;
        fx->letterbox = f > FX_LETTERBOX_MAX ? 0.0f : f;
    }
    if (nvert > 0 && cmax > 0.0f) {
        float cx = 0.5f * (minx + maxx), cy = 0.5f * (miny + maxy), dmin = 1e9f;
        for (int k = 0; k < nvert; k++) { float d = hypotf(vx[k] - cx, vy[k] - cy); if (d < dmin) dmin = d; }
        fx->vignette = cmax > 1.0f ? 1.0f : cmax;
        fx->vign_radius = dmin / (0.5f * (float)dh);
    }
    if (scope) {
        fx->scope_on = 1;
        fx->binocular = S && S->bino ? 1 : 0;
        if (rec_screen > 160) { float z = (float)rec_screen / 160.0f; fx->scope_zoom = z > FX_ZOOM_MAX ? FX_ZOOM_MAX : z; }
    }
}

void vpc_fx_hold(VrFxHold *H, const VrFx *now) {
    if (!H || !now) return;
    if (now->vignette > 0.0f) { H->fx.vignette = now->vignette; H->fx.vign_radius = now->vign_radius; H->age_v = 0; }
    else if (H->age_v < VPC_FX_HOLD) H->age_v++;
    else { H->fx.vignette = 0.0f; H->fx.vign_radius = 0.0f; }
    if (now->letterbox > 0.0f) { H->fx.letterbox = now->letterbox; H->age_l = 0; }
    else if (H->age_l < VPC_FX_HOLD) H->age_l++;
    else H->fx.letterbox = 0.0f;
    if (now->scope_on) { H->fx.scope_on = 1; H->fx.binocular = now->binocular; H->fx.scope_zoom = now->scope_zoom; H->age_s = 0; }
    else if (H->age_s < VPC_FX_HOLD) H->age_s++;
    else H->fx.scope_on = 0;
    if (!H->fx.scope_on) { H->fx.scope_zoom = 1.0f; H->fx.binocular = 0; }
}
