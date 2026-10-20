/* Pruebas sintéticas de ns_fxcmp: medida de un efecto frente a la operación de la lista. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ns_fxcmp.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FALLO %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* Escena de un solo cuadrilátero de mundo a 2000 de profundidad, cámara en el origen mirando a +Z (vista = mundo, y abajo, factor vertical 1). */
static void mk_scene(NsFxScene *sc, NsFxItem *it, float half) {
    memset(sc, 0, sizeof *sc); memset(it, 0, sizeof *it);
    sc->cam.valid = 1; sc->cam.proj_h = 320; sc->cam.y_scale = 1.0f;
    for (int i = 0; i < 3; i++) sc->cam.axes[i][i] = 1.0f;
    sc->items = it; sc->n_items = 1; sc->cap_items = 1;
    it->shape = NSFX_SH_QUAD; it->nv = 4; it->role = NSFX_ROLE_RIPPLE; it->kind = 20;
    it->flags = NSFX_F_TEXTURED | NSFX_F_SEMI; it->gflags = GPU_F_TEXTURED | GPU_F_SEMI; it->abr = 1; it->tpage = 10 | (1 << 5) | (1 << 7); it->tp = 1; it->clut = 5 | (480 << 6); it->sz = 2000;
    const float c[4][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}};   /* orden Z del paquete: sup-izq, sup-der, inf-izq, inf-der (y abajo) */
    for (int v = 0; v < 4; v++) { it->pos[v][0] = c[v][0] * half; it->pos[v][1] = c[v][1] * half; it->pos[v][2] = 2000; memcpy(it->vpos[v], it->pos[v], sizeof it->pos[v]); for (int k = 0; k < 3; k++) it->rgb[v][k] = 128; }
}

static void mk_op(GpuOp *o, float half, float dx, int abr) {
    memset(o, 0, sizeof *o);
    o->type = GPU_OP_POLY; o->nv = 4; o->flags = GPU_F_TEXTURED | GPU_F_SEMI | GPU_F_QUAD; o->tx = 640; o->ty = 0; o->tp = 1; o->abr = (uint8_t)abr; o->clx = 16 * 5; o->cly = 480; o->proj_h = 320;
    const float c[4][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
    for (int v = 0; v < 4; v++) { o->v[v].vvalid = 1; o->v[v].vx = c[v][0] * half + dx; o->v[v].vy = c[v][1] * half; o->v[v].vz = 2000; o->v[v].x = (int16_t)(320.0f * o->v[v].vx / 2000.0f); o->v[v].y = (int16_t)(320.0f * o->v[v].vy / 2000.0f); }
}

static int run1(NsFxScene *sc, const GpuOp *o, NsFxCmpResult *r) {
    GpuDrawList dl; memset(&dl, 0, sizeof dl); dl.ops = (GpuOp *)(uintptr_t)o; dl.nops = 1; dl.disp.w = 320; dl.disp.h = 240;
    uint8_t cls[1] = {NSFX_OP_FX}; int32_t item[1] = {0};
    return ns_fxcmp_run(sc, NULL, NULL, &dl, cls, item, NULL, r);
}

int main(void) {
    NsFxScene sc; NsFxItem it; GpuOp op; NsFxCmpResult r;
    /* coincidencia exacta */
    mk_scene(&sc, &it, 300); mk_op(&op, 300, 0, 1);
    CHECK(run1(&sc, &op, &r) == 0 && r.n_cmp == 1 && r.n_view == 1, "medida n=%u vista=%u", r.n_cmp, r.n_view);
    CHECK(r.pos_max_mm < 1.0f && fabsf(r.ratio_mean - 1.0f) < 0.01f && r.blend_bad == 0 && r.tex_bad == 0 && r.page_bad == 0 && r.pos_bad == 0 && r.size_bad == 0, "exacta: pos %.2f mm ratio %.3f", r.pos_max_mm, r.ratio_mean);
    /* desplazada 120 unidades: error de posición medido */
    mk_op(&op, 300, 120, 1);
    CHECK(run1(&sc, &op, &r) == 0 && fabsf(r.pos_max_mm - 120.0f) < 2.0f && r.pos_bad == 1 && r.n_worst == 1 && (r.worst[0].bad & NFC_BAD_POS), "desplazada %.1f mm (esperado 120)", r.pos_max_mm);
    /* doble de grande en la lista: razón de tamaño 0,5 */
    mk_op(&op, 600, 0, 1);
    CHECK(run1(&sc, &op, &r) == 0 && fabsf(r.ratio_mean - 0.5f) < 0.02f && r.size_bad == 1, "tamano ratio %.3f", r.ratio_mean);
    /* mezcla distinta */
    mk_op(&op, 300, 0, 2);
    CHECK(run1(&sc, &op, &r) == 0 && r.blend_bad == 1 && (r.worst[0].bad & NFC_BAD_BLEND), "modo de mezcla distinto");
    /* textura distinta (página) */
    mk_op(&op, 300, 0, 1); op.tx = 704;
    CHECK(run1(&sc, &op, &r) == 0 && r.page_bad == 1, "página distinta");
    /* página supuesta */
    mk_scene(&sc, &it, 300); it.flags |= NSFX_F_TPAGE_UNKNOWN; mk_op(&op, 300, 0, 1);
    CHECK(run1(&sc, &op, &r) == 0 && r.unk_page == 1 && r.worst[0].unk == 1, "página supuesta contada");
    /* vértice detrás de la cámara en la lista: recortado, no comparable */
    mk_scene(&sc, &it, 300); mk_op(&op, 300, 0, 1); op.v[0].vflags = GPU_VF_BEHIND;
    CHECK(run1(&sc, &op, &r) == 0 && r.n_cmp == 0 && r.n_clip == 1, "recortada n_cmp=%u clip=%u", r.n_cmp, r.n_clip);
    /* elemento no emitido con el centro en pantalla y fiable sin operación */
    mk_scene(&sc, &it, 300); it.flags |= NSFX_F_ENGINE_CULLED; mk_op(&op, 300, 0, 1);
    { GpuDrawList dl; memset(&dl, 0, sizeof dl); dl.ops = &op; dl.nops = 1; uint8_t cls[1] = {NSFX_OP_MESH}; int32_t item[1] = {-1};
      CHECK(ns_fxcmp_run(&sc, NULL, NULL, &dl, cls, item, NULL, &r) == 0 && r.n_items_culled == 1 && r.n_culled_inview == 1, "no emitido dentro de pantalla %u", r.n_culled_inview);
      it.flags &= (uint16_t)~NSFX_F_ENGINE_CULLED;
      CHECK(ns_fxcmp_run(&sc, NULL, NULL, &dl, cls, item, NULL, &r) == 0 && r.n_items_reliable == 1 && r.n_items_unclaimed == 1, "fiable sin operación %u", r.n_items_unclaimed); }
    /* huérfana: polígono semitransparente 3D de la lista clasificado 2D */
    mk_scene(&sc, &it, 300); mk_op(&op, 300, 0, 1);
    { GpuDrawList dl; memset(&dl, 0, sizeof dl); dl.ops = &op; dl.nops = 1; uint8_t cls[1] = {NSFX_OP_2D}; int32_t item[1] = {-1};
      CHECK(ns_fxcmp_run(&sc, NULL, NULL, &dl, cls, item, NULL, &r) == 0 && r.n_orphan_semi3d == 1, "huérfana 3D %u", r.n_orphan_semi3d); }
    /* cámara de la lista distinta: el elemento se mide con ella (desplazada la cámara 100 en X -> el elemento queda 100 a la izquierda) */
    mk_scene(&sc, &it, 300); mk_op(&op, 300, -100, 1);
    { NsFxListCam lc; memset(&lc, 0, sizeof lc); lc.eye[0] = 100; lc.M[0] = lc.M[4] = lc.M[8] = 1;
      GpuDrawList dl; memset(&dl, 0, sizeof dl); dl.ops = &op; dl.nops = 1; dl.disp.w = 320; dl.disp.h = 240; uint8_t cls[1] = {NSFX_OP_FX}; int32_t item[1] = {0};
      CHECK(ns_fxcmp_run(&sc, &lc, NULL, &dl, cls, item, NULL, &r) == 0 && r.pos_max_mm < 2.0f, "con la cámara de la lista: %.2f mm", r.pos_max_mm); }
    /* formato: línea de resumen y peores; sin desbordar el búfer */
    mk_scene(&sc, &it, 300); mk_op(&op, 300, 120, 2);
    run1(&sc, &op, &r);
    { char buf[4096]; size_t n = ns_fxcmp_format(&r, 7, buf, sizeof buf);
      CHECK(n > 100 && strstr(buf, "[fx-cmp] fr=7") && strstr(buf, "peor 1") && strstr(buf, "fallo="), "formato (%zu bytes)", n);
      char small[64]; n = ns_fxcmp_format(&r, 7, small, sizeof small); CHECK(n < sizeof small && small[n] == 0, "búfer pequeño"); }
    /* argumentos nulos */
    CHECK(ns_fxcmp_run(NULL, NULL, NULL, NULL, NULL, NULL, NULL, &r) < 0 && ns_fxcmp_run(&sc, NULL, NULL, NULL, NULL, NULL, NULL, NULL) < 0, "argumentos nulos");
    if (fails) { printf("FALLA: %d comprobaciones, %d fallos\n", checks, fails); return 1; }
    printf("OK: %d comprobaciones, 0 fallos\n", checks);
    return 0;
}
