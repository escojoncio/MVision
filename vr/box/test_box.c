/* Pruebas de box_view: activación, transición, geometría cerrada, abertura en la dirección de la vista,
 * proporción angular respecto al lienzo del original, accesibilidad, estilos y robustez del lector.
 * Con datos reales (BOX_STAGES o ../../gamedata/stages.bin) usa el recurso del disco; sin ellos, la abertura genérica. */
#include "box_view.h"
#include "box_data.h"
#include "../weapons/wcore.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0, g_n = 0;
#define CHECK(c, ...) do { g_n++; if (!(c)) { g_fail++; printf("FALLO %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint8_t *g_dir; static size_t g_dir_len;
static uint16_t g_sight_id = 0xE2A9, g_tex_id = 0x556C;      /* ids16 que pasa la app (solo números) */
static const char *g_stage = "init";

static void load_data(void) {
    const char *path = getenv("BOX_STAGES");
    if (!path) path = "../../gamedata/stages.bin";
    if (getenv("BOX_STAGE")) g_stage = getenv("BOX_STAGE");
    if (getenv("BOX_SIGHT_ID")) g_sight_id = (uint16_t)strtol(getenv("BOX_SIGHT_ID"), NULL, 0);
    if (getenv("BOX_TEX_ID")) g_tex_id = (uint16_t)strtol(getenv("BOX_TEX_ID"), NULL, 0);
    FILE *f = fopen(path, "rb");
    if (!f) { printf("(sin datos en %s: se prueba con la abertura generica)\n", path); return; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    g_dir = (uint8_t *)malloc((size_t)n);
    if (g_dir && fread(g_dir, 1, (size_t)n, f) == (size_t)n) g_dir_len = (size_t)n;
    else { free(g_dir); g_dir = NULL; }
    fclose(f);
}

static BoxViewConfig mkcfg(void) {
    BoxViewConfig c; box_view_default_config(&c);
    c.stage = g_stage; c.sight_id = g_sight_id; c.tex_id = g_tex_id;
    return c;
}

static WPose pose_id(void) { WPose p; p.p = wv(0, 0, 0); p.q = wquat_identity(); return p; }

static void run_to_active(WPose head) {
    box_view_set_active(1);
    for (int i = 0; i < 400 && box_view_state() != BOX_ACTIVE; i++) box_view_update(head, 1.f / 90.f);
}

/* ---- trazado de rayos sobre las mallas opacas (marco local, pose de carcasa = identidad) ---- */
static int ray_tri(WVec3 o, WVec3 d, WVec3 a, WVec3 b, WVec3 c, float *t) {
    WVec3 e1 = wv_sub(b, a), e2 = wv_sub(c, a), p = wv_cross(d, e2);
    float det = wv_dot(e1, p);
    if (fabsf(det) < 1e-12f) return 0;
    float inv = 1.f / det;
    WVec3 s = wv_sub(o, a);
    float u = wv_dot(s, p) * inv; if (u < -1e-5f || u > 1.00001f) return 0;
    WVec3 q = wv_cross(s, e1);
    float v = wv_dot(d, q) * inv; if (v < -1e-5f || u + v > 1.00001f) return 0;
    float tt = wv_dot(e2, q) * inv;
    if (tt <= 1e-5f) return 0;
    *t = tt; return 1;
}
static int blocked(const BoxDraw *dr, int nd, WVec3 o, WVec3 d) {
    for (int k = 0; k < nd; k++) {
        if (dr[k].blend != BOX_BLEND_OPAQUE) continue;
        const WMesh *m = dr[k].mesh;
        for (uint32_t i = 0; i + 2 < m->ni; i += 3) {
            const WVertex *A = &m->v[m->idx[i]], *B = &m->v[m->idx[i + 1]], *C = &m->v[m->idx[i + 2]];
            float t;
            if (ray_tri(o, d, wv(A->pos[0], A->pos[1], A->pos[2]), wv(B->pos[0], B->pos[1], B->pos[2]),
                        wv(C->pos[0], C->pos[1], C->pos[2]), &t)) return 1;
        }
    }
    return 0;
}
static int in_poly(const WVec3 *p, int n, float x, float y) {
    int in = 0;
    for (int i = 0, j = n - 1; i < n; j = i++)
        if (((p[i].y > y) != (p[j].y > y)) && (x < (p[j].x - p[i].x) * (y - p[i].y) / (p[j].y - p[i].y) + p[i].x)) in = !in;
    return in;
}
static float dist_poly(const WVec3 *p, int n, float x, float y) {   /* distancia al borde */
    float best = 1e9f;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        float ex = p[i].x - p[j].x, ey = p[i].y - p[j].y, l2 = ex * ex + ey * ey;
        float t = l2 > 0 ? ((x - p[j].x) * ex + (y - p[j].y) * ey) / l2 : 0;
        t = t < 0 ? 0 : (t > 1 ? 1 : t);
        float dx = p[j].x + t * ex - x, dy = p[j].y + t * ey - y, d = sqrtf(dx * dx + dy * dy);
        if (d < best) best = d;
    }
    return best;
}

/* ---------------- pruebas ---------------- */
static void t_init_and_data(void) {
    BoxViewConfig c = mkcfg();
    int rc = box_view_init(&c, g_dir, g_dir_len);
    CHECK(rc == (g_dir ? 0 : 1), "init devuelve %d (esperado %d)", rc, g_dir ? 0 : 1);
    CHECK(!box_view_visible(), "no debe ser visible al inicio");
    BoxDraw d[BOX_MAX_DRAWS];
    CHECK(box_view_draws(d, BOX_MAX_DRAWS) == 0, "sin dibujos al inicio");
    int w, h;
    const uint8_t *tx = box_view_texture(&w, &h);
    if (g_dir) CHECK(tx && w > 0 && h > 0, "textura de cartón cargada del disco");
    else CHECK(tx == NULL, "sin datos no hay textura");
    printf("  datos: %s, baldosa %dx%d\n", g_dir ? "disco" : "generico", tx ? w : 0, tx ? h : 0);
    box_view_shutdown();
}

static void t_transition(void) {
    BoxViewConfig c = mkcfg(); c.lag_rate = 0.f;
    box_view_init(&c, g_dir, g_dir_len);
    WPose head = pose_id(); head.p = wv(0.3f, 1.6f, -1.f);
    BoxDraw d[BOX_MAX_DRAWS];
    box_view_update(head, 0.01f);
    CHECK(box_view_state() == BOX_OFF && !box_view_visible(), "inactiva sin pedirla");
    box_view_set_active(1);
    CHECK(box_view_is_active(), "set_active(1) se recuerda");
    box_view_update(head, 1.f / 90.f);
    CHECK(box_view_state() == BOX_ENTERING && box_view_visible(), "entra en transición");
    int nd = box_view_draws(d, BOX_MAX_DRAWS);
    CHECK(nd >= 2, "al entrar hay dibujos (%d)", nd);
    /* bajada monótona y sobre la cabeza */
    float prev_drop = box_view_drop(), prev_y = box_view_shell_pose().p.y - head.p.y, t = 0.f;
    int mono = 1, above = 1;
    uint32_t rev0 = box_view_revision();
    while (box_view_state() != BOX_ACTIVE && t < 3.f) {
        box_view_update(head, 1.f / 90.f); t += 1.f / 90.f;
        float dy = box_view_shell_pose().p.y - head.p.y;
        if (box_view_drop() + 1e-6f < prev_drop || dy > prev_y + 1e-6f) mono = 0;
        if (dy < -1e-5f) above = 0;
        prev_drop = box_view_drop(); prev_y = dy;
    }
    CHECK(box_view_state() == BOX_ACTIVE, "llega a ACTIVE (t=%.2f s)", t);
    CHECK(mono, "bajada monótona");
    CHECK(above, "la caja baja desde arriba, nunca por debajo de la cabeza");
    CHECK(t <= c.drop_time + c.open_time + 0.1f, "duración de la entrada razonable (%.2f s)", t);
    CHECK(fabsf(box_view_drop() - 1.f) < 1e-6f && fabsf(box_view_open() - 1.f) < 1e-6f, "puesta y abierta del todo");
    CHECK(fabsf(box_view_shell_pose().p.y - head.p.y) < 1e-6f, "ya sin elevación");
    CHECK(box_view_revision() != rev0, "la revisión cambió durante la apertura");
    uint32_t rev1 = box_view_revision();
    for (int i = 0; i < 10; i++) box_view_update(head, 1.f / 90.f);
    CHECK(box_view_revision() == rev1, "estable: sin reconstrucción en reposo");
    nd = box_view_draws(d, BOX_MAX_DRAWS);
    CHECK(nd == 2 || nd == 3, "dibujos en reposo: %d", nd);
    int has_alpha = 0;
    for (int i = 0; i < nd; i++) if (d[i].blend == BOX_BLEND_ALPHA) has_alpha = 1;
    CHECK(has_alpha, "hay una malla con mezcla alfa (degradado del borde)");

    /* salida */
    box_view_set_active(0);
    t = 0.f;
    while (box_view_state() != BOX_OFF && t < 3.f) { box_view_update(head, 1.f / 90.f); t += 1.f / 90.f; }
    CHECK(box_view_state() == BOX_OFF && !box_view_visible(), "sale y deja de ser visible (t=%.2f s)", t);
    CHECK(box_view_draws(d, BOX_MAX_DRAWS) == 0, "sin dibujos al salir");

    /* inversión a mitad de bajada: sin saltos */
    box_view_set_active(1);
    while (box_view_drop() < 0.4f) box_view_update(head, 1.f / 90.f);
    float y0 = box_view_shell_pose().p.y;
    box_view_set_active(0);
    float maxjump = 0.f, last = y0;
    for (int i = 0; i < 90 && box_view_state() != BOX_OFF; i++) {
        box_view_update(head, 1.f / 90.f);
        float y = box_view_shell_pose().p.y;
        if (fabsf(y - last) > maxjump) maxjump = fabsf(y - last);
        last = y;
    }
    CHECK(maxjump < 0.05f, "inversión continua (salto máx %.4f m)", maxjump);
    box_view_shutdown();
}

static void t_closed_and_aperture(int wide, int style) {
    BoxViewConfig c = mkcfg(); c.lag_rate = 0.f; c.style = (BoxStyle)style;
    box_view_init(&c, g_dir, g_dir_len);
    box_view_set_wide(wide);
    WPose head = pose_id();
    run_to_active(head);
    BoxDraw d[BOX_MAX_DRAWS];
    int nd = box_view_draws(d, BOX_MAX_DRAWS);
    WVec3 ol[BOX_MAX_OUT]; int no = box_view_outline(ol, BOX_MAX_OUT);
    CHECK(no >= 8, "contorno con %d puntos", no);
    const float ipd = c.ipd;
    const float eyes[3] = {0.f, -0.5f * ipd, 0.5f * ipd};
    int N = 30000, bad_escape[3] = {0, 0, 0}, esc[3] = {0, 0, 0}, central_blocked = 0, central = 0;
    int esc_both = 0;
    const float ga = 2.39996323f;
    for (int i = 0; i < N; i++) {
        float y = 1.f - 2.f * ((float)i + 0.5f) / (float)N, r = sqrtf(1.f - y * y), a = ga * (float)i;
        WVec3 dir = wv(r * cosf(a), y, r * sinf(a));
        int e_ok[3];
        for (int e = 0; e < 3; e++) {
            WVec3 o = wv(eyes[e], 0, 0);
            int bl = blocked(d, nd, o, dir);
            e_ok[e] = !bl;
            if (!bl) {
                esc[e]++;
                /* un rayo que escapa debe cruzar el panel frontal dentro de la abertura */
                int inside = 0;
                if (dir.z < -1e-4f) {
                    float tt = (-c.distance - o.z) / dir.z;
                    float px = o.x + dir.x * tt, py = o.y + dir.y * tt;
                    inside = in_poly(ol, no, px, py) || dist_poly(ol, no, px, py) < 0.001f;
                }
                if (!inside) bad_escape[e]++;
            }
        }
        if (e_ok[1] && e_ok[2]) esc_both++;
        /* rayos cercanos a la mirada (dentro del 60 % de la abertura): deben escapar desde el centro */
        if (dir.z < -1e-4f) {
            float tt = -c.distance / dir.z, px = dir.x * tt, py = dir.y * tt;
            if (in_poly(ol, no, px, py) && dist_poly(ol, no, px, py) > 0.02f && tt < 2.f) {
                central++; if (!e_ok[0]) central_blocked++;
            }
        }
    }
    char tag[64]; snprintf(tag, sizeof tag, "wide=%d estilo=%d", wide, style);
    CHECK(bad_escape[0] == 0 && bad_escape[1] == 0 && bad_escape[2] == 0,
          "[%s] cerrada: rayos escapando fuera de la abertura (c=%d i=%d d=%d)", tag, bad_escape[0], bad_escape[1], bad_escape[2]);
    CHECK(esc[0] > 0, "[%s] algo se ve por la abertura", tag);
    CHECK(central_blocked == 0, "[%s] el centro de la abertura no queda tapado (%d de %d)", tag, central_blocked, central);
    float overlap = esc[0] > 0 ? (float)esc_both / (float)esc[0] : 0.f;
    printf("  [%s] rayos que escapan: centro=%d izq=%d dcha=%d, solapamiento binocular=%.2f\n", tag, esc[0], esc[1], esc[2], overlap);
    CHECK(overlap > 0.55f, "[%s] solapamiento binocular razonable (%.2f)", tag, overlap);
    box_view_shutdown();
}

static void t_world_direction(void) {
    BoxViewConfig c = mkcfg(); c.lag_rate = 0.f;
    box_view_init(&c, g_dir, g_dir_len);
    WQuat qy = wquat_axis_angle(wv(0, 1, 0), 70.f * (float)M_PI / 180.f);
    WQuat qx = wquat_axis_angle(wv(1, 0, 0), 20.f * (float)M_PI / 180.f);
    WQuat qz = wquat_axis_angle(wv(0, 0, 1), 5.f * (float)M_PI / 180.f);
    WPose head; head.q = wquat_normalize(wquat_mul(qy, wquat_mul(qx, qz))); head.p = wv(1.f, 1.6f, -2.f);
    run_to_active(head);
    WPose sh = box_view_shell_pose();
    float d = fabsf(sh.q.x * head.q.x + sh.q.y * head.q.y + sh.q.z * head.q.z + sh.q.w * head.q.w);
    CHECK(2.f * acosf(d > 1 ? 1 : d) < 1e-3f, "sin inercia la carcasa sigue a la cabeza");
    CHECK(wv_len(wv_sub(sh.p, head.p)) < 1e-5f, "carcasa centrada en la cabeza");
    BoxDraw dr[BOX_MAX_DRAWS]; int nd = box_view_draws(dr, BOX_MAX_DRAWS);
    CHECK(nd >= 2 && wv_len(wv_sub(dr[0].pose.p, head.p)) < 1e-5f, "la pose de dibujo es la de la cabeza");
    /* mirada de la cabeza en el mundo -> marco local de la carcasa: debe cruzar la abertura */
    WVec3 fwd = wquat_rotate(head.q, wv(0, 0, -1));
    WVec3 loc = wquat_rotate(wquat_inv(sh.q), fwd);
    WVec3 ol[BOX_MAX_OUT]; int no = box_view_outline(ol, BOX_MAX_OUT);
    float tt = -c.distance / loc.z, px = loc.x * tt, py = loc.y * tt;
    CHECK(in_poly(ol, no, px, py), "la mirada cruza la abertura (%.3f, %.3f)", px, py);
    CHECK(!blocked(dr, nd, wv(0, 0, 0), loc), "la mirada no queda tapada");
    BoxAperture ap;
    CHECK(box_view_aperture(&ap) == 0, "aperture()");
    CHECK(ap.up > 0 && ap.down > 0 && ap.left > 0 && ap.right > 0, "abertura alrededor de la mirada");
    printf("  abertura: izq %.1f dcha %.1f arriba %.1f abajo %.1f grados, centro %+.1f grados\n",
           ap.left * 57.29578f, ap.right * 57.29578f, ap.up * 57.29578f, ap.down * 57.29578f, ap.center_pitch * 57.29578f);

    /* con inercia: giro brusco de 90 grados; el desfase queda acotado y la mirada sigue dentro de la abertura */
    box_view_set_lag(16.f, 2.5f);
    WPose h2 = head;
    h2.q = wquat_normalize(wquat_mul(wquat_axis_angle(wv(0, 1, 0), 90.f * (float)M_PI / 180.f), head.q));
    float maxlag = 0.f; int always_in = 1;
    for (int i = 0; i < 60; i++) {
        box_view_update(h2, 1.f / 90.f);
        WPose s = box_view_shell_pose();
        float dd = fabsf(s.q.x * h2.q.x + s.q.y * h2.q.y + s.q.z * h2.q.z + s.q.w * h2.q.w);
        float ang = 2.f * acosf(dd > 1 ? 1 : dd) * 57.29578f;
        if (ang > maxlag) maxlag = ang;
        WVec3 f2 = wquat_rotate(wquat_inv(s.q), wquat_rotate(h2.q, wv(0, 0, -1)));
        float t2 = -c.distance / f2.z;
        if (!in_poly(ol, no, f2.x * t2, f2.y * t2)) always_in = 0;
    }
    CHECK(maxlag <= 2.5f + 0.05f, "desfase angular acotado (%.2f grados)", maxlag);
    CHECK(always_in, "con inercia la mirada siempre queda dentro de la abertura");
    CHECK(maxlag > 0.1f, "la inercia existe (%.2f grados)", maxlag);
    box_view_shutdown();
}

static void t_proportions(void) {
    BoxViewConfig c = mkcfg();
    int rc = box_view_init(&c, g_dir, g_dir_len);
    (void)rc;
    /* ida y vuelta lienzo -> carcasa -> proyección del juego */
    float maxerr = 0.f;
    for (int k = 0; k < 20; k++) {
        float px = 7.f + 15.f * (float)k, py = 11.f + 12.f * (float)k;
        WVec3 q = box_view_px_to_local(px, py);
        float u = 160.f + c.focal_px * q.x / -q.z, v = c.view_cy - c.focal_px * q.y / -q.z;
        float e = fmaxf(fabsf(u - px), fabsf(v - py));
        if (e > maxerr) maxerr = e;
    }
    CHECK(maxerr < 1e-3f, "proyección del juego reproduce los píxeles (error %.5f px)", maxerr);
    /* extensión angular de la abertura = la del contorno leído, convertida con el FOV del juego */
    BoxSight s;
    int ok = 0;
    if (g_dir) {
        const uint8_t *p; size_t n;
        if (box_stage_find(g_dir, g_dir_len, g_stage, g_sight_id, 's', &p, &n) == 0 && box_sight_parse(p, n, &s) == 0) ok = 1;
    }
    if (!ok) box_sight_fallback(&s, 320, 256, 160.f, 105.f, 285.f, 110.f, 40.f, 17.f);
    float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
    for (int i = 0; i < s.nout; i++) {
        if (s.out[i].x < x0) x0 = s.out[i].x;
        if (s.out[i].x > x1) x1 = s.out[i].x;
        if (s.out[i].y < y0) y0 = s.out[i].y;
        if (s.out[i].y > y1) y1 = s.out[i].y;
    }
    BoxAperture ap; box_view_aperture(&ap);
    float cw = 0.5f * (float)s.canvas_w;
    CHECK(fabsf(ap.left - atanf((cw - x0) / c.focal_px)) < 1e-4f, "izq: %.4f", ap.left);
    CHECK(fabsf(ap.right - atanf((x1 - cw) / c.focal_px)) < 1e-4f, "dcha: %.4f", ap.right);
    CHECK(fabsf(ap.up - atanf((c.view_cy - y0) / c.focal_px)) < 1e-4f, "arriba: %.4f", ap.up);
    CHECK(fabsf(ap.down - atanf((y1 - c.view_cy) / c.focal_px)) < 1e-4f, "abajo: %.4f", ap.down);
    /* en la imagen del juego la relación ancho/alto del hueco es la del contorno */
    float aw = tanf(ap.left) + tanf(ap.right), ah = tanf(ap.up) + tanf(ap.down);
    CHECK(fabsf(aw / ah - (x1 - x0) / (y1 - y0)) < 1e-3f, "proporción ancho/alto del hueco conservada (%.3f)", aw / ah);
    /* el hueco no ocupa toda la pantalla: es alargado y queda algo por encima del centro del lienzo */
    CHECK(aw / ah > 1.5f, "abertura alargada");
    CHECK(ap.center_pitch > 0.f, "centro por encima de la mirada");
    box_view_shutdown();
}

static void t_wide_and_open(void) {
    BoxViewConfig c = mkcfg();
    box_view_init(&c, g_dir, g_dir_len);
    CHECK(!box_view_is_wide(), "accesibilidad desactivada por defecto");
    BoxAperture a0, a1, a2;
    box_view_aperture(&a0);
    box_view_set_wide(1);
    CHECK(box_view_is_wide(), "wide activo");
    box_view_aperture(&a1);
    CHECK(a1.left > a0.left && a1.right > a0.right && a1.up > a0.up && a1.down > a0.down, "la abertura ancha contiene a la normal");
    box_view_set_wide(0);
    box_view_aperture(&a2);
    CHECK(fabsf(a2.left - a0.left) < 1e-6f && fabsf(a2.up - a0.up) < 1e-6f, "volver a normal restituye");
    /* apertura animada: el estado inicial (cerrado) es más pequeño que el final */
    box_view_set_active(1);
    WPose h = pose_id();
    float area_prev = 0.f; int mono = 1;
    for (int i = 0; i < 300; i++) {
        box_view_update(h, 1.f / 90.f);
        BoxDraw d[BOX_MAX_DRAWS]; int nd = box_view_draws(d, BOX_MAX_DRAWS);
        if (nd < 1) continue;
        const WMesh *m = d[nd - 1].mesh;      /* el degradado ocupa el hueco: su caja crece con la apertura */
        float xa = 1e9f, xb = -1e9f, ya = 1e9f, yb = -1e9f;
        for (uint32_t k = 0; k < m->nv; k++) {
            xa = fminf(xa, m->v[k].pos[0]); xb = fmaxf(xb, m->v[k].pos[0]);
            ya = fminf(ya, m->v[k].pos[1]); yb = fmaxf(yb, m->v[k].pos[1]);
        }
        float area = (xb - xa) * (yb - ya);
        if (area + 1e-7f < area_prev) mono = 0;
        area_prev = area;
        if (box_view_state() == BOX_ACTIVE) break;
    }
    CHECK(mono, "la abertura crece de forma monótona al abrirse");
    box_view_shutdown();
}

static void t_mesh_validity(void) {
    for (int st = 0; st < 2; st++)
        for (int wide = 0; wide < 2; wide++) {
            BoxViewConfig c = mkcfg(); c.style = (BoxStyle)st;
            box_view_init(&c, g_dir, g_dir_len);
            box_view_set_wide(wide);
            WPose h = pose_id(); run_to_active(h);
            BoxDraw d[BOX_MAX_DRAWS]; int nd = box_view_draws(d, BOX_MAX_DRAWS);
            int nrim = 0, black_ok = 1;
            for (int k = 0; k < nd; k++) {
                const WMesh *m = d[k].mesh;
                CHECK(m->nv > 0 && m->nv < 65535 && m->ni > 0 && m->ni % 3 == 0, "malla %s con %u vértices %u índices", m->name, m->nv, m->ni);
                for (uint32_t i = 0; i < m->ni; i++) CHECK(m->idx[i] < m->nv, "índice fuera de rango");
                for (uint32_t i = 0; i < m->nv; i++) {
                    CHECK(isfinite(m->v[i].pos[0]) && isfinite(m->v[i].pos[1]) && isfinite(m->v[i].pos[2]), "posición no finita");
                    if (st == BOX_STYLE_BLACK && (m->v[i].rgba[0] || m->v[i].rgba[1] || m->v[i].rgba[2])) black_ok = 0;
                }
                if (k == 0) {   /* cantos: triángulos del casco con normal en el plano XY y extensión Z = grosor */
                    for (uint32_t i = 0; i + 2 < m->ni; i += 3) {
                        const WVertex *A = &m->v[m->idx[i]];
                        if (fabsf(A->nrm[2]) < 0.01f && fabsf(A->nrm[0]) + fabsf(A->nrm[1]) > 0.5f && A->pos[2] < -0.01f
                            && A->pos[2] > -0.5f) nrim++;
                    }
                }
            }
            CHECK(nrim >= 2 * 8, "[estilo %d wide %d] hay cantos con grosor (%d triángulos)", st, wide, nrim);
            if (st == BOX_STYLE_BLACK) {
                CHECK(black_ok, "estilo negro: todo negro puro");
                CHECK(box_view_texture(NULL, NULL) == NULL, "estilo negro sin textura");
                CHECK(d[0].texture == 0, "estilo negro: dibujo sin textura");
            } else if (g_dir) {
                CHECK(d[0].texture == 1 && d[0].mesh->texture == 1, "estilo cartón: con textura");
            }
            box_view_shutdown();
        }
}

static void t_fuzz_parser(void) {
    if (!g_dir) { printf("  (fuzz del lector omitido: sin datos)\n"); return; }
    const uint8_t *p; size_t n;
    if (box_stage_find(g_dir, g_dir_len, g_stage, g_sight_id, 's', &p, &n) != 0) { CHECK(0, "recurso no encontrado"); return; }
    uint8_t *buf = (uint8_t *)malloc(n);
    uint32_t seed = 12345u;
    int parsed = 0;
    for (int it = 0; it < 4000; it++) {
        memcpy(buf, p, n);
        int m = 1 + (int)(seed % 6u);
        for (int k = 0; k < m; k++) { seed = seed * 1664525u + 1013904223u; buf[(seed >> 8) % n] = (uint8_t)(seed >> 24); }
        seed = seed * 1664525u + 1013904223u;
        size_t len = (it % 7 == 0) ? (seed >> 8) % (n + 1) : n;
        BoxSight s;
        if (box_sight_parse(buf, len, &s) == 0) parsed++;
    }
    BoxSight s; CHECK(box_sight_parse(p, n, &s) == 0 && s.from_disc, "recurso original válido");
    CHECK(s.nout >= 8 && s.nband > 0 && s.canvas_w > 16, "contorno %d, banda %d, lienzo %dx%d", s.nout, s.nband, s.canvas_w, s.canvas_h);
    printf("  fuzz: %d/4000 mutaciones aceptadas, sin fallos\n", parsed);
    /* textura corrupta */
    const uint8_t *tp; size_t tn;
    if (box_stage_find(g_dir, g_dir_len, g_stage, g_tex_id, 'p', &tp, &tn) == 0) {
        uint8_t *tb = (uint8_t *)malloc(tn);
        for (int it = 0; it < 2000; it++) {
            memcpy(tb, tp, tn);
            for (int k = 0; k < 4; k++) { seed = seed * 1664525u + 1013904223u; tb[(seed >> 8) % tn] = (uint8_t)(seed >> 24); }
            int w, h; uint8_t *rgba = NULL;
            if (box_tex_decode(tb, tn, &w, &h, &rgba) == 0) free(rgba);
        }
        free(tb);
    }
    free(buf);
}

int main(void) {
    load_data();
    printf("== init y datos\n");      t_init_and_data();
    printf("== transición\n");        t_transition();
    printf("== geometría cerrada y abertura\n");
    t_closed_and_aperture(0, BOX_STYLE_CARDBOARD);
    t_closed_and_aperture(0, BOX_STYLE_BLACK);
    t_closed_and_aperture(1, BOX_STYLE_CARDBOARD);
    printf("== dirección de la vista\n"); t_world_direction();
    printf("== proporciones\n");      t_proportions();
    printf("== accesibilidad y apertura\n"); t_wide_and_open();
    printf("== mallas\n");            t_mesh_validity();
    printf("== lector\n");            t_fuzz_parser();
    free(g_dir);
    printf("%s: %d comprobaciones, %d fallos\n", g_fail ? "FALLO" : "OK", g_n, g_fail);
    return g_fail ? 1 : 0;
}
