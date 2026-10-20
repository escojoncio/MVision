#include "ns_fxcmp.h"
#include "ns_fx_export.h"
#include "ns_gpu.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void norm3(float *a) { float l = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]); if (l > 1e-9f) { a[0] /= l; a[1] /= l; a[2] /= l; } }
static float dist3(const float *a, const float *b) { float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2]; return sqrtf(x * x + y * y + z * z); }
static void cross3(const float *a, const float *b, float *o) { o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0]; }
static float len3(const float *a) { return sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]); }

static float mm_per_unit(const float *m) {
    if (!m) return 1.0f;
    double s = 0;
    for (int c = 0; c < 3; c++) s += sqrt((double)m[c] * m[c] + (double)m[4 + c] * m[4 + c] + (double)m[8 + c] * m[8 + c]);
    float v = (float)(s / 3.0 * 1000.0);
    return v > 1e-6f ? v : 1.0f;
}

/* esquinas nativas -> vista de la lista: M^T (p - ojo), con la Y multiplicada por el factor vertical del motor */
static void to_list_view(const float eye[3], const float M[9], float ys, const float p[3], float v[3]) {
    float d[3] = {p[0] - eye[0], p[1] - eye[1], p[2] - eye[2]};
    for (int r = 0; r < 3; r++) v[r] = M[r] * d[0] + M[3 + r] * d[1] + M[6 + r] * d[2];
    v[1] *= ys;
}

static float area_quad(const float c[4][3], int nq) {
    float a[3], b[3], n[3];
    if (nq == 3) {
        for (int r = 0; r < 3; r++) { a[r] = c[1][r] - c[0][r]; b[r] = c[2][r] - c[0][r]; }
        cross3(a, b, n); return 0.5f * len3(n);
    }
    for (int r = 0; r < 3; r++) { a[r] = c[3][r] - c[0][r]; b[r] = c[2][r] - c[1][r]; }   /* diagonales del orden de paquete (Z) */
    cross3(a, b, n); return 0.5f * len3(n);
}

static void insert_worst(NsFxCmpResult *r, const NsFxCmpRow *row) {
    int pos = (int)r->n_worst;
    if (pos < NFC_WORST) r->n_worst++;
    else if (row->score <= r->worst[NFC_WORST - 1].score) return;
    else pos = NFC_WORST - 1;
    while (pos > 0 && r->worst[pos - 1].score < row->score) { r->worst[pos] = r->worst[pos - 1]; pos--; }
    r->worst[pos] = *row;
}

/* El hardware recorta las coordenadas de pantalla a 11 bits con signo: una operación con un vértice en -1024 / 1023 (o marcado GPU_VF_SAT) ya no describe la geometría del elemento. */
static int op_saturated(const GpuOp *o) {
    for (int v = 0; v < o->nv && v < 4; v++) {
        int x = o->v[v].x - o->dox, y = o->v[v].y - o->doy;
        if (x <= -1024 || x >= 1023 || y <= -1024 || y >= 1023 || (o->v[v].vvalid && (o->v[v].vflags & GPU_VF_SAT))) return 1;
    }
    if (o->type == GPU_OP_RECT) { int x = o->v[0].x - o->dox + o->rw, y = o->v[0].y - o->doy + o->rh; if (x >= 1023 || y >= 1023 || o->v[0].x - o->dox <= -1024 || o->v[0].y - o->doy <= -1024) return 1; }
    return 0;
}
static int fcmpf(const void *a, const void *b) { float x = *(const float *)a, y = *(const float *)b; return x < y ? -1 : x > y; }

int ns_fxcmp_run(const NsFxScene *sc, const NsFxListCam *lc, const float *to_vr, const GpuDrawList *dl, const uint8_t *cls, const int32_t *item, const uint8_t *exact, NsFxCmpResult *out) {
    if (!out) return -1;
    memset(out, 0, sizeof *out);
    if (!sc || !dl || !cls || !item || !sc->cam.valid) return -1;
    const float ys = sc->cam.y_scale > 0 ? sc->cam.y_scale : NS_OVERSCAN_Y, H = sc->cam.proj_h > 1 ? sc->cam.proj_h : 320.0f;
    out->mm_per_unit = mm_per_unit(to_vr);
    out->n_items = sc->n_items;

    /* cámara de la lista */
    float eye[3], M[9];
    if (lc) { memcpy(eye, lc->eye, sizeof eye); memcpy(M, lc->M, sizeof M); }
    else { memcpy(eye, sc->cam.eye, sizeof eye); for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) M[3 * r + c] = sc->cam.axes[r][c]; }
    for (int c = 0; c < 3; c++) { float col[3] = {M[c], M[3 + c], M[6 + c]}; norm3(col); M[c] = col[0]; M[3 + c] = col[1]; M[6 + c] = col[2]; }   /* las matrices del registro vienen cuantizadas */

    /* cámara del juego de la escena (la que usa el renderer para el tamaño de los carteles) */
    NsCamera cam;
    memset(&cam, 0, sizeof cam);
    memcpy(cam.eye, sc->cam.eye, sizeof cam.eye);
    for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) cam.rot[3 * r + c] = sc->cam.axes[r][c];
    cam.h = sc->cam.proj_h; cam.overscan_y = ys;

    /* geometría en unidades del motor (to_vr = identidad): se mide en el mismo espacio que la lista */
    static const float ident[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    NsGpuFxParams prm;
    memset(&prm, 0, sizeof prm);
    prm.to_vr = ident; prm.cam = &cam;
    memcpy(prm.head, eye, sizeof eye); memcpy(prm.eyes[0], eye, sizeof eye); memcpy(prm.eyes[1], eye, sizeof eye); prm.n_eyes = 1;
    for (int r = 0; r < 3; r++) { prm.head_right[r] = M[3 * r]; prm.head_up[r] = -M[3 * r + 1]; }   /* eje Y de la vista = abajo */

    uint8_t *claimed = (uint8_t *)calloc(sc->n_items ? sc->n_items : 1, 1);
    float *pos_list = (float *)malloc(sizeof(float) * (dl->nops ? dl->nops : 1));
    if (!claimed || !pos_list) { free(claimed); free(pos_list); return -2; }
    uint32_t npos = 0;
    double sum_pos = 0, sum_px = 0, sum_lr = 0; uint32_t n_lr = 0;

    for (uint32_t oi = 0; oi < dl->nops; oi++) {
        if (cls[oi] != NSFX_OP_FX || item[oi] < 0 || (uint32_t)item[oi] >= sc->n_items) continue;
        const GpuOp *o = &dl->ops[oi];
        const NsFxItem *it = &sc->items[item[oi]];
        out->n_ops_fx++;
        claimed[item[oi]] = 1;
        /* geometría nativa del elemento: un NsEffect (la exportación de verdad) -> ns_gpu_fx_build */
        NsFxItem tmp = *it; tmp.flags &= (uint16_t)~NSFX_F_TPAGE_UNKNOWN;   /* la exportación omite los sprites sin página: aquí se mide igualmente (con la página supuesta) */
        NsFxScene one = *sc; one.items = &tmp; one.n_items = 1;
        NsEffect e;
        if (ns_fx_export(&one, 1, &e, 1, NULL) != 1) { out->n_nomeas++; continue; }
        NsGpuFxVert vb[16]; NsGpuFxRun runs[4]; uint64_t keys[4]; NsGpuFxOut go;
        if (ns_gpu_fx_build(&e, 1, &prm, vb, 16, runs, 4, keys, 4, &go) != 0) { out->n_nomeas++; continue; }
        if (go.n_skipped || go.n_keys == 0) { out->n_nomeas++; continue; }
        if (go.n_opaque + go.n_semi == 0) { out->n_noface++; continue; }
        const int nq = (it->shape == NSFX_SH_TRI) ? 3 : 4;
        float q[4][3];   /* esquinas nativas en el mundo del motor */
        for (int k = 0; k < 3; k++) memcpy(q[k], vb[k].p, sizeof q[k]);
        if (nq == 4) memcpy(q[3], vb[5].p, sizeof q[3]);
        float lv[4][3];  /* ... y en la vista de la lista */
        for (int k = 0; k < nq; k++) to_list_view(eye, M, ys, q[k], lv[k]);

        NsFxCmpRow row;
        memset(&row, 0, sizeof row);
        row.op = (int32_t)oi; row.item = item[oi]; row.role = it->role; row.shape = it->shape; row.prim = it->prim; row.idx = it->idx;
        row.near_match = (exact && exact[oi] != NSFX_OP_FX) ? 1 : 0;
        row.culled = (it->flags & NSFX_F_ENGINE_CULLED) != 0;
        /* Los carteles (rectángulos proyectados por el motor) traen en la lista esquinas relativas al centro, no posición de vista absoluta: se miden en píxeles. */
        int all_view = o->type == GPU_OP_POLY && o->nv >= 3 && it->shape != NSFX_SH_BILLBOARD && it->shape != NSFX_SH_RIBBON, clipped = 0;
        for (int v = 0; v < o->nv && all_view; v++) {
            if (!o->v[v].vvalid || (o->v[v].vflags & GPU_VF_BEHIND)) all_view = 0;
            else if ((o->v[v].vflags & GPU_VF_SAT) || o->v[v].vz < 64.0f) clipped = 1;   /* el motor recortó el vértice contra la pantalla / plano cercano: su posición no es la del elemento */
        }
        if (all_view && clipped) { out->n_clip++; continue; }
        if (!all_view && op_saturated(o)) { out->n_sat++; continue; }
        if (!all_view && o->type == GPU_OP_POLY && (it->shape == NSFX_SH_QUAD || it->shape == NSFX_SH_TRI)) { out->n_clip++; continue; }   /* polígono de mundo con algún vértice detrás de la cámara o sin vista: recortado */
        float zc = 0, cn[3] = {0, 0, 0};
        for (int k = 0; k < nq; k++) { for (int r = 0; r < 3; r++) cn[r] += lv[k][r] / (float)nq; }
        zc = cn[2] > 1.0f ? cn[2] : 1.0f;
        if (all_view) {
            row.how = 0;
            float co[3] = {0, 0, 0}; float op[4][3];
            for (int v = 0; v < o->nv; v++) { op[v][0] = o->v[v].vx; op[v][1] = o->v[v].vy; op[v][2] = o->v[v].vz; for (int r = 0; r < 3; r++) co[r] += op[v][r] / (float)o->nv; }
            float d = dist3(cn, co);
            row.pos_mm = d * out->mm_per_unit; row.pos_px = d * H / (co[2] > 1.0f ? co[2] : 1.0f);
            const int bb = it->shape == NSFX_SH_BILLBOARD || it->shape == NSFX_SH_RIBBON;
            float sn, so;
            if (bb) {   /* cartel / cinta: extensión en la vista (ancho x alto), no el área por diagonales (el orden de vértices del paquete puede ser otro) */
                float xn0 = 1e30f, xn1 = -1e30f, yn0 = 1e30f, yn1 = -1e30f, xo0 = 1e30f, xo1 = -1e30f, yo0 = 1e30f, yo1 = -1e30f;
                for (int k = 0; k < nq; k++) { if (lv[k][0] < xn0) xn0 = lv[k][0]; if (lv[k][0] > xn1) xn1 = lv[k][0]; if (lv[k][1] < yn0) yn0 = lv[k][1]; if (lv[k][1] > yn1) yn1 = lv[k][1]; }
                for (int k = 0; k < o->nv; k++) { if (op[k][0] < xo0) xo0 = op[k][0]; if (op[k][0] > xo1) xo1 = op[k][0]; if (op[k][1] < yo0) yo0 = op[k][1]; if (op[k][1] > yo1) yo1 = op[k][1]; }
                sn = sqrtf(fmaxf(0, (xn1 - xn0) * (yn1 - yn0))); so = sqrtf(fmaxf(0, (xo1 - xo0) * (yo1 - yo0)));
            } else {
                sn = sqrtf(area_quad(lv, nq)); so = sqrtf(area_quad(op, o->nv == 3 ? 3 : 4));
                if (o->nv == nq) {   /* misma numeración: error máximo por vértice */
                    float mx = 0;
                    for (int k = 0; k < nq; k++) if (!(o->v[k].vflags & GPU_VF_AMBIG)) { float dd = dist3(lv[k], op[k]); if (dd > mx) mx = dd; }
                    row.vtx_mm = mx * out->mm_per_unit;
                }
            }
            row.size_it = sn * out->mm_per_unit; row.size_op = so * out->mm_per_unit;
            row.ratio = so > 1e-3f ? sn / so : 0;
            out->n_view++;
        } else {
            /* sin posición de vista (rectángulo / sprite): se compara en píxeles de la pantalla del motor */
            row.how = 1;
            float cxo, cyo, wo, ho;
            if (o->type == GPU_OP_RECT) { wo = (float)o->rw; ho = (float)o->rh; cxo = (float)(o->v[0].x - o->dox) + 0.5f * wo; cyo = (float)(o->v[0].y - o->doy) + 0.5f * ho; }
            else if (o->type == GPU_OP_POLY && o->nv >= 3) {
                float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f;
                for (int v = 0; v < o->nv; v++) { float x = (float)(o->v[v].x - o->dox), y = (float)(o->v[v].y - o->doy); if (x < x0) x0 = x;
                if (x > x1) x1 = x;
                if (y < y0) y0 = y;
                if (y > y1) y1 = y; }
                wo = x1 - x0; ho = y1 - y0; cxo = 0.5f * (x0 + x1); cyo = 0.5f * (y0 + y1);
            } else { out->n_nomeas++; continue; }
            int behind = 0;
            float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f;
            for (int k = 0; k < nq; k++) {
                if (lv[k][2] < 1.0f) { behind = 1; break; }
                float x = H * lv[k][0] / lv[k][2], y = H * lv[k][1] / lv[k][2];
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
                if (y < y0) y0 = y;
                if (y > y1) y1 = y;
            }
            if (behind || !(wo > 0) || !(ho > 0)) { out->n_nomeas++; continue; }
            float wn = x1 - x0, hn = y1 - y0;
            float dx = 0.5f * (x0 + x1) - cxo, dy = 0.5f * (y0 + y1) - cyo;
            row.pos_px = sqrtf(dx * dx + dy * dy); row.pos_mm = row.pos_px * zc / H * out->mm_per_unit;
            row.size_it = sqrtf(fmaxf(0, wn * hn)); row.size_op = sqrtf(wo * ho);
            row.ratio = row.size_op > 1e-3f ? row.size_it / row.size_op : 0;
            out->n_screen++;
        }
        /* mezcla / textura / página / CLUT: lo que el renderer usará frente a lo que la GPU dibujó */
        row.semi_it = (it->flags & NSFX_F_SEMI) != 0; row.semi_op = (o->flags & GPU_F_SEMI) != 0;
        row.tex_it = (it->flags & NSFX_F_TEXTURED) != 0; row.tex_op = (o->flags & GPU_F_TEXTURED) != 0;
        row.abr_it = (uint8_t)(e.abr & 3); row.abr_op = (uint8_t)(o->abr & 3);
        row.tpage_it = (uint16_t)(e.tpage & 0x19Fu);
        row.tpage_op = (uint16_t)(((o->tx / 64) & 15) | (((o->ty / 256) & 1) << 4) | ((o->tp & 3) << 7));
        row.clut_it = e.clut; row.clut_op = (uint16_t)(((o->clx / 16) & 63) | ((o->cly & 511) << 6));
        row.unk = (uint8_t)(((it->flags & NSFX_F_TPAGE_UNKNOWN) ? 1 : 0) | ((it->flags & NSFX_F_ABR_UNKNOWN) ? 2 : 0));
        if (row.semi_it != row.semi_op || (row.semi_it && row.abr_it != row.abr_op)) row.bad |= NFC_BAD_BLEND;
        if (row.tex_it != row.tex_op) row.bad |= NFC_BAD_TEX;
        else if (row.tex_op && (row.tpage_it != row.tpage_op || row.clut_it != row.clut_op)) row.bad |= NFC_BAD_PAGE;
        if (row.pos_px > 3.0f && row.pos_mm > 30.0f) row.bad |= NFC_BAD_POS;
        if (row.ratio < 0.8f || row.ratio > 1.25f) row.bad |= NFC_BAD_SIZE;
        float lr = row.ratio > 1e-3f ? fabsf(logf(row.ratio)) : 3.0f;
        row.score = fmaxf(row.pos_px / 2.0f, 10.0f * lr);
        if (row.bad & (NFC_BAD_BLEND | NFC_BAD_TEX | NFC_BAD_PAGE)) row.score += 20.0f;
        out->n_cmp++;
        if (row.near_match) out->n_near++;
        if (row.bad & NFC_BAD_POS) out->pos_bad++;
        if (row.bad & NFC_BAD_SIZE) out->size_bad++;
        if (row.bad & NFC_BAD_BLEND) out->blend_bad++;
        if (row.bad & NFC_BAD_TEX) out->tex_bad++;
        if (row.bad & NFC_BAD_PAGE) out->page_bad++;
        if (row.unk & 1) out->unk_page++;
        if (row.unk & 2) out->unk_abr++;
        if (row.role < NSFX_ROLE_COUNT) out->by_role[row.role]++;
        sum_pos += row.pos_mm; sum_px += row.pos_px;
        if (row.pos_mm > out->pos_max_mm) out->pos_max_mm = row.pos_mm;
        pos_list[npos++] = row.pos_mm;
        if (row.ratio > 1e-3f) { sum_lr += logf(row.ratio); n_lr++; }
        insert_worst(out, &row);
    }
    if (out->n_cmp) {
        out->pos_mean_mm = (float)(sum_pos / out->n_cmp); out->pos_mean_px = (float)(sum_px / out->n_cmp);
        qsort(pos_list, npos, sizeof(float), fcmpf);
        out->pos_p90_mm = pos_list[(size_t)((npos - 1) * 0.9)];
    }
    out->ratio_mean = n_lr ? expf((float)(sum_lr / n_lr)) : 0;
    /* elementos: no emitidos por el motor pero con el centro dentro de la pantalla de la cámara de la lista; fiables sin ninguna operación */
    for (uint32_t i = 0; i < sc->n_items; i++) {
        const NsFxItem *it = &sc->items[i];
        int nv = it->shape == NSFX_SH_BILLBOARD ? 1 : it->shape == NSFX_SH_TRI ? 3 : it->shape == NSFX_SH_RIBBON ? 2 : 4;
        if (it->flags & NSFX_F_ENGINE_CULLED) {
            out->n_items_culled++;
            float c[3] = {0, 0, 0};
            for (int v = 0; v < nv; v++) { float l[3]; to_list_view(eye, M, ys, it->pos[v], l); for (int r = 0; r < 3; r++) c[r] += l[r] / (float)nv; }
            if (c[2] > 32.0f) {
                float sx = H * c[0] / c[2], sy = H * c[1] / c[2];
                if (fabsf(sx) <= 0.5f * 320.0f && fabsf(sy) <= 0.5f * 240.0f) out->n_culled_inview++;
            }
        } else if (!(it->flags & NSFX_F_LOW_CONF) && !(it->flags & NSFX_F_VIEWLOCKED)) {
            out->n_items_reliable++;
            if (!claimed[i]) out->n_items_unclaimed++;
        }
    }
    for (uint32_t i = 0; i < dl->nops; i++) {   /* huérfanas: semitransparentes que la lista dibuja y ninguna extracción explica */
        const GpuOp *o = &dl->ops[i];
        if (cls[i] != NSFX_OP_2D && cls[i] != NSFX_OP_MESH_GUESS) continue;
        if (!(o->flags & GPU_F_SEMI)) continue;
        if (o->type == GPU_OP_POLY && o->v[0].vvalid) out->n_orphan_semi3d++;
        else if (o->type == GPU_OP_RECT && (o->flags & GPU_F_TEXTURED) && o->rw >= 8 && o->rh >= 8 && !o->cx2) out->n_orphan_rect_semi++;
    }
    free(claimed); free(pos_list);
    return 0;
}

size_t ns_fxcmp_format(const NsFxCmpResult *r, uint32_t frame, char *buf, size_t cap) {
    if (!buf || !cap) return 0;
    size_t o = 0;
#define APP(...) do { if (o < cap) { int w_ = snprintf(buf + o, cap - o, __VA_ARGS__); if (w_ > 0) o += (size_t)w_ < cap - o ? (size_t)w_ : cap - o - 1; } } while (0)
    APP("[fx-cmp] fr=%u ops_efecto=%u medidas=%u (vista=%u pantalla=%u tolerante=%u) sin_medida=%u recortadas=%u saturadas=%u cara_oculta=%u | posicion: media=%.1fmm (%.2fpx) p90=%.1fmm max=%.1fmm fuera(>3px y >30mm)=%u | tamano nativo/lista: media=x%.2f fuera[0.8,1.25]=%u | mezcla_distinta=%u textura_distinta=%u pagina_o_clut_distinta=%u (pagina_supuesta=%u abr_supuesto=%u) | elementos=%u no_emitidos=%u (dentro_de_pantalla=%u) fiables=%u sin_operacion=%u huerfanas(semi_3d=%u semi_rect=%u)\n",
        frame, r->n_ops_fx, r->n_cmp, r->n_view, r->n_screen, r->n_near, r->n_nomeas, r->n_clip, r->n_sat, r->n_noface, r->pos_mean_mm, r->pos_mean_px, r->pos_p90_mm, r->pos_max_mm, r->pos_bad,
        r->ratio_mean, r->size_bad, r->blend_bad, r->tex_bad, r->page_bad, r->unk_page, r->unk_abr, r->n_items, r->n_items_culled, r->n_culled_inview, r->n_items_reliable, r->n_items_unclaimed, r->n_orphan_semi3d, r->n_orphan_rect_semi);
    for (uint32_t i = 0; i < r->n_worst; i++) {
        const NsFxCmpRow *w = &r->worst[i];
        APP("[fx-cmp]   peor %u: elem=%d rol=%s forma=%s prim=%08x/%u via=%s%s medida=%s pos=%.1fmm (%.1fpx)%s tam=%.0f/%.0f%s (x%.2f) mezcla elem[semi=%u abr=%u tex=%u pag=%03x clut=%04x%s] lista[semi=%u abr=%u tex=%u pag=%03x clut=%04x] fallo=%s%s%s%s%s\n",
            i + 1, w->item, ns_fx_role_name(w->role), ns_fx_shape_name(w->shape), w->prim, w->idx, w->near_match ? "tolerante" : "exacta", w->culled ? " no_emitido" : "", w->how ? "pantalla" : "vista",
            w->pos_mm, w->pos_px, w->how ? "" : "", w->size_it, w->size_op, w->how ? "px" : "mm", w->ratio,
            w->semi_it, w->abr_it, w->tex_it, w->tpage_it, w->clut_it, w->unk ? (w->unk == 3 ? " SUPUESTO:pag+abr" : (w->unk == 1 ? " SUPUESTO:pag" : " SUPUESTO:abr")) : "",
            w->semi_op, w->abr_op, w->tex_op, w->tpage_op, w->clut_op,
            w->bad ? "" : "ninguno", (w->bad & NFC_BAD_POS) ? " posicion" : "", (w->bad & NFC_BAD_SIZE) ? " tamano" : "", (w->bad & NFC_BAD_BLEND) ? " mezcla" : "", (w->bad & (NFC_BAD_TEX | NFC_BAD_PAGE)) ? " textura" : "");
    }
    {   /* reparto por rol de las medidas */
        APP("[fx-cmp]   roles medidos:");
        int any = 0;
        for (int k = 1; k < NSFX_ROLE_COUNT; k++) if (r->by_role[k]) { APP(" %s=%u", ns_fx_role_name(k), r->by_role[k]); any = 1; }
        if (!any) APP(" ninguno");
        APP("\n");
    }
#undef APP
    return o;
}
