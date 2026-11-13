/* Pantalla LED del Nikita (guided_led_face) sobre el modelo REAL (argumento 1, STAGE_FILE, /tmp/gamedata/STAGE.DIR o gamedata/stages.bin; se omite sin el) y sin modelo.
 * Medido (mm, marco del arma): el tubo (0x15a9) tiene una unica cara +Z en su mitad delantera izquierda, plano z = -81, x -120..-30, y 97..171; el hexagono del tubo la recorta (vertices (-60, 97) y (-30, 149)). */
#include "w_guided.h"
#include "wcore.h"
#include "wcut.h"
#include "wload.h"
#include <math.h>
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
    static GuidedState st; WScreenFace f;
    w_guided.init(&st.base); st.base.def = &w_guided;
    CHECK(guided_led_face(&st.base, &f) == 0, "sin modelo enlazado no hay pantalla");
    CHECK(guided_led_face(NULL, &f) == 0 && guided_led_face(&st.base, NULL) == 0, "argumentos nulos");
    /* ruta de la vista segun el estado: sin misil / misil en vuelo / estatica / opcion de pantalla grande */
    { GuidedState *s = &st; s->base.phase = G_IDLE;
      CHECK(guided_led_route(&s->base, 0, 0) == GLED_STANDBY && guided_led_route(&s->base, 0, 1) == GLED_STANDBY, "sin misil: pantalla en espera (tambien con pantalla grande)");
      s->base.phase = G_FLIGHT;
      CHECK(guided_led_route(&s->base, 1, 0) == GLED_LIVE, "misil en vuelo: camara del misil en la pantalla LED");
      CHECK(guided_led_route(&s->base, 0, 0) == GLED_LIVE, "vuelo aun sin misil del juego: la LED (sin camara valida), no la vista completa");
      CHECK(guided_led_route(&s->base, 1, 1) == GLED_FULLSCREEN, "opcion pantalla grande en vuelo");
      s->base.phase = G_STATIC;
      CHECK(guided_led_route(&s->base, 0, 0) == GLED_STATIC && guided_led_route(&s->base, 0, 1) == GLED_FULLSCREEN, "estatica al terminar el vuelo");
      CHECK(guided_led_route(NULL, 1, 0) == GLED_STANDBY, "estado nulo");
      s->base.phase = G_IDLE; }
    const char *c[4] = { argc > 1 ? argv[1] : NULL, getenv("STAGE_FILE"), "/tmp/gamedata/STAGE.DIR", "../../gamedata/stages.bin" };
    uint8_t *d = NULL; size_t n = 0;
    for (int i = 0; i < 4 && !d; i++) d = slurp(c[i], &n);
    if (!d) { printf("test_guided_led: sin modelos reales: parte real omitida\n"); }
    else {
        WLoadOpts o; wload_default_opts(&o); o.resident = "init";
        WLoaded m; WCutSet cs;
        if (wload_model(d, n, "init", 0x15a9, &o, &m) != 0 || wcut_run(&m, wcut_find(0x15a9), &cs) != 0) { printf("test_guided_led: modelo no cargable: omitido\n"); free(d); return 0; }
        weapon_bind(&st.base, cs.meshes, cs.nmeshes, cs.parts, cs.nparts);
        CHECK(guided_led_face(&st.base, &f) == 1, "pantalla hallada");
        CHECK(fabsf(sqrtf(f.w * f.w + f.h * f.h) - GD_LED_DIAG) < 0.001f && fabsf(f.w / f.h - 0.090f / 0.074f) < 0.01f, "diagonal %.1f mm (8-10 cm) y proporcion de la cara (%.1f x %.1f mm)", sqrtf(f.w * f.w + f.h * f.h) * 1000, f.w * 1000, f.h * 1000);
        CHECK(fabsf(f.face.p.x + 0.075f) < 0.002f && fabsf(f.face.p.y - 0.134f) < 0.002f && fabsf(f.face.p.z + 0.081f - GD_LED_LIFT) < 0.001f, "centro (%.1f %.1f %.1f) mm: centrada en la cara, 3 mm por delante", f.face.p.x * 1000, f.face.p.y * 1000, f.face.p.z * 1000);
        CHECK(fabsf(f.face.q.x) + fabsf(f.face.q.y) + fabsf(f.face.q.z) < 1e-6f && f.face.q.w > 0.9999f, "sin giro: +Z local hacia el ojo");
        CHECK(f.npoly == 5, "contorno de 5 vertices recortado por el hexagono del tubo (%d)", f.npoly);
        /* dentro del contorno original de la cara (mm, marco del arma): pentagono (-120,97) (-60,97) (-30,149) (-30,171) (-120,171) */
        { const float e[5][2] = { {-0.120f, 0.097f}, {-0.060f, 0.097f}, {-0.030f, 0.149f}, {-0.030f, 0.171f}, {-0.120f, 0.171f} };
          WScreenFace big; memset(&big, 0, sizeof big); big.npoly = 5; big.face.p = f.face.p; big.w = 0.09f; big.h = 0.074f;
          for (int i = 0; i < 5; i++) { big.poly[i][0] = e[i][0] - f.face.p.x; big.poly[i][1] = e[i][1] - f.face.p.y; }
          for (int i = 0; i < f.npoly; i++) CHECK(led_inside(&big, f.poly[i][0], f.poly[i][1]), "vertice %d dentro del contorno de la cara", i); }
        for (int i = 0; i < f.npoly; i++) CHECK(fabsf(f.poly[i][0]) <= f.w * 0.5f + 1e-5f && fabsf(f.poly[i][1]) <= f.h * 0.5f + 1e-5f, "vertice %d dentro de la caja", i);
        /* no atraviesa la malla desde ningun lado: ningun triangulo de ninguna pieza cruza la pantalla ni el plano 1 cm por delante */
        CHECK(led_hits(&st.base, &f, 0.0f) == 0, "un triangulo del modelo atraviesa la pantalla");
        CHECK(led_hits(&st.base, &f, 0.010f) == 0, "un triangulo del modelo cruza el prisma de 1 cm delante de la pantalla");
        /* fija respecto al modelo: no depende del estado */
        { WScreenFace l2; st.base.phase = G_FLIGHT; st.base.channel[WCH_LID] = 0.6f; CHECK(guided_led_face(&st.base, &l2) == 1 && !memcmp(&f, &l2, sizeof f), "identica en vuelo / con la tapa abierta"); }
        wcut_free(&cs); wload_free(&m); free(d);
    }
    printf(fails ? "test_guided_led: FALLO (%d)\n" : "test_guided_led: OK\n", fails);
    return fails != 0;
}
