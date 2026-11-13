/* Cara de la mira del lanzador antiaéreo (aa_sight_face) sobre el modelo REAL (archivo de stages: argumento 1, STAGE_FILE, gamedata/stages.bin; se omite sin él) y sin modelo.
 * Medido en el modelo europeo (mm, marco del arma): la pieza "screen" es la cara trasera (z = -54) de la mira, un cuadrilátero inclinado (-35,142) (-10,162) (-117,199) (-130,175). */
#include "w_aa.h"
#include "wcut.h"
#include "wcore.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FALLO %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)
static uint8_t *slurp(const char *p, size_t *n) {
    FILE *f = p ? fopen(p, "rb") : NULL; if (!f) return NULL;
    fseek(f, 0, SEEK_END); long l = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc((size_t)l); if (!d || fread(d, 1, (size_t)l, f) != (size_t)l) { free(d); fclose(f); return NULL; }
    fclose(f); *n = (size_t)l; return d;
}
/* (x, y) local a la pantalla (relativo a su centro) dentro de su contorno (poly convexo) o de su rectangulo (npoly = 0) */
static int led_inside(const WScreenFace *f, float x, float y) {
    if (f->npoly < 3) return fabsf(x) <= 0.5f * f->w && fabsf(y) <= 0.5f * f->h;
    int pos = 0, neg = 0;
    for (int i = 0; i < f->npoly; i++) { const float *a = f->poly[i], *b = f->poly[(i + 1) % f->npoly]; float c = (b[0] - a[0]) * (y - a[1]) - (b[1] - a[1]) * (x - a[0]); if (c > 1e-7f) pos = 1; if (c < -1e-7f) neg = 1; }
    return !(pos && neg);
}
/* Ninguna pieza del modelo atraviesa la pantalla: ningun triangulo cruza el plano de la pantalla (ni el plano 1 cm por delante) dentro de su contorno (muestreado), y ningun vertice queda dentro del prisma de 1 cm delante. */
static int led_hits(const WeaponState *s, const WScreenFace *f, float plane) {
    int np, ni, hits = 0; const WPart *P = weapon_parts(s, &np); const WMesh *M = weapon_meshes(s, &ni);
    WQuat qi = wquat_inv(wquat_normalize(f->face.q));
    for (int i = 0; i < np; i++) { int mi = weapon_part_mesh(s, i); if (mi < 0) continue;
        for (uint32_t t = 0; t + 2 < M[mi].ni; t += 3) {
            WVec3 L[3];
            for (int k = 0; k < 3; k++) { const float *p = M[mi].v[M[mi].idx[t + k]].pos; L[k] = wquat_rotate(qi, wv(p[0] + P[i].rest.p.x - f->face.p.x, p[1] + P[i].rest.p.y - f->face.p.y, p[2] + P[i].rest.p.z - f->face.p.z)); }
            float mn = fminf(L[0].z, fminf(L[1].z, L[2].z)), mx = fmaxf(L[0].z, fmaxf(L[1].z, L[2].z));
            if (!(mn < plane - 1e-5f && mx > plane + 1e-5f)) continue;
            for (int e = 0; e < 3; e++) { WVec3 a = L[e], b = L[(e + 1) % 3]; if ((a.z - plane) * (b.z - plane) >= 0) continue;
                float u = (plane - a.z) / (b.z - a.z), x = a.x + u * (b.x - a.x), y = a.y + u * (b.y - a.y);
                if (led_inside(f, x, y)) hits++; } } }
    return hits;
}
int main(int argc, char **argv) {
    static AAState st; AaSightFace f;
    w_aa.init(&st.base); st.base.def = &w_aa;
    CHECK(aa_sight_face(&st.base, &f) == 0, "sin modelo enlazado no hay cara");
    CHECK(aa_sight_face(NULL, &f) == 0 && aa_sight_face(&st.base, NULL) == 0, "argumentos nulos");
    { WScreenFace lf; CHECK(aa_led_face(&st.base, &lf) == 0 && aa_led_face(NULL, &lf) == 0 && aa_led_face(&st.base, NULL) == 0, "LED: sin modelo / argumentos nulos no hay pantalla"); }
    const char *c[3] = { argc > 1 ? argv[1] : NULL, getenv("STAGE_FILE"), "../../gamedata/stages.bin" };
    uint8_t *d = NULL; size_t n = 0;
    for (int i = 0; i < 3 && !d; i++) d = slurp(c[i], &n);
    if (!d) { printf("test_aa_sight: sin modelos reales: parte real omitida\n"); }
    else {
        const char *stage = getenv("STAGE_NAME"); if (!stage) stage = "init";
        WLoadOpts o; wload_default_opts(&o); o.resident = stage;
        WLoaded m; WCutSet cs;
        if (wload_model(d, n, stage, 0x57f8, &o, &m) != 0 || wcut_run(&m, wcut_find(0x57f8), &cs) != 0) { printf("test_aa_sight: modelo no cargable: omitido\n"); free(d); return 0; }
        weapon_bind(&st.base, cs.meshes, cs.nmeshes, cs.parts, cs.nparts);
        CHECK(aa_sight_face(&st.base, &f) == 1, "cara hallada");
        CHECK(f.npoly == 4, "contorno de 4 vértices (%d)", f.npoly);
        CHECK(fabsf(f.w - 0.120f) < 0.002f && fabsf(f.h - 0.057f) < 0.002f, "caja %.1f x %.1f mm", f.w * 1000, f.h * 1000);
        CHECK(fabsf(f.face.p.x + 0.070f) < 0.002f && fabsf(f.face.p.y - 0.1705f) < 0.002f && fabsf(f.face.p.z + 0.054f - AA_FACE_LIFT) < 0.001f, "centro (%.1f %.1f %.1f) mm", f.face.p.x * 1000, f.face.p.y * 1000, f.face.p.z * 1000);
        const float exp[4][2] = { {-35, 142}, {-10, 162}, {-117, 199}, {-130, 175} };
        for (int e = 0; e < 4; e++) {
            int hit = 0;
            for (int i = 0; i < f.npoly; i++) if (fabsf((f.poly[i][0] + f.face.p.x) * 1000 - exp[e][0]) < 1.5f && fabsf((f.poly[i][1] + f.face.p.y) * 1000 - exp[e][1]) < 1.5f) hit = 1;
            CHECK(hit, "vértice esperado (%g, %g) mm", exp[e][0], exp[e][1]);
        }
        for (int i = 0; i < f.npoly; i++) CHECK(fabsf(f.poly[i][0]) <= f.w * 0.5f + 1e-5f && fabsf(f.poly[i][1]) <= f.h * 0.5f + 1e-5f, "vértice %d dentro de la caja", i);
        /* pantalla LED: fija al modelo (solo depende de las mallas), 4:3, orientada al ojo y sin atravesar la unidad de la mira ni el tubo */
        WScreenFace lf; CHECK(aa_led_face(&st.base, &lf) == 1, "LED hallada");
        CHECK(lf.npoly == 0, "LED rectangular");
        CHECK(lf.w > 0.075f && lf.w < 0.085f && fabsf(lf.w / lf.h - 4.0f / 3.0f) < 1e-3f, "LED %.1f x %.1f mm 4:3", lf.w * 1000, lf.h * 1000);
        CHECK(fabsf(sqrtf(lf.w * lf.w + lf.h * lf.h) - 0.0991f) < 0.01f, "diagonal ~ 8-10 cm (%.1f mm)", sqrtf(lf.w * lf.w + lf.h * lf.h) * 1000);
        { WVec3 n = wquat_rotate(lf.face.q, wv(0, 0, 1));
          CHECK(n.z > 0.95f && n.x > 0.05f && n.y > 0.05f, "orientada hacia el ojo (arriba y a la derecha) n=(%.2f %.2f %.2f)", n.x, n.y, n.z);
          const float hw = lf.w * 0.5f, hh = lf.h * 0.5f; float zmin = 1e9f, ymin = 1e9f;
          for (int i = 0; i < 4; i++) { WVec3 c = wquat_rotate(lf.face.q, wv((i & 1) ? hw : -hw, (i & 2) ? hh : -hh, 0)); float cz = lf.face.p.z + c.z, cy = lf.face.p.y + c.y; if (cz < zmin) zmin = cz; if (cy < ymin) ymin = cy; }
          CHECK(zmin >= -0.054f + 1e-4f, "no atraviesa la unidad de la mira (z min %.1f mm, plano trasero -54)", zmin * 1000);
          CHECK(ymin >= 0.142f, "no se apoya dentro del tubo (y min %.1f mm, tope del tubo 142)", ymin * 1000);
          CHECK(fabsf(lf.face.p.x + 0.070f) < 0.004f && fabsf(lf.face.p.y - 0.172f) < 0.004f && fabsf(lf.face.p.z + 0.044f + 0.002f) < 0.006f, "centro (%.1f %.1f %.1f) mm", lf.face.p.x * 1000, lf.face.p.y * 1000, lf.face.p.z * 1000); }
        CHECK(led_hits(&st.base, &lf, 0.0f) == 0, "un triangulo del modelo atraviesa la pantalla del Stinger");
        CHECK(led_hits(&st.base, &lf, 0.010f) == 0, "un triangulo del modelo cruza el prisma de 1 cm delante de la pantalla del Stinger");
        /* fija respecto al modelo: no depende de la entrada (mano/cabeza) ni del estado */
        { WScreenFace l2; st.base.channel[WCH_LID] = 0.7f; CHECK(aa_led_face(&st.base, &l2) == 1 && !memcmp(&lf, &l2, sizeof lf), "LED idéntica con otro estado de canales"); }
        wcut_free(&cs); wload_free(&m); free(d);
    }
    printf(fails ? "test_aa_sight: FALLO (%d)\n" : "test_aa_sight: OK\n", fails);
    return fails != 0;
}
