/* preview: renderiza un arma del registro a PNG/PPM en una pose/estado dado.
 *
 *   preview <id> [opciones]
 *     -o FICHERO         salida (.png o .ppm; por defecto out.png)
 *     --obj FICHERO.obj  modelo externo (grupos `o`/`g`; se enlazan por nombre de hueco de mesh;
 *                        si ningún nombre coincide se usa como una única pieza raíz). Admite `v x y z r g b`.
 *     --view V           side|left|top|under|front|rear|three|three2|sheet (sheet = 4 vistas)
 *     --size WxH         (def. 800x600)   --ss N supersampling por eje (def. 2 = MSAA x4)
 *     --radius R         radio de encuadre en m (def. 0.25)
 *     --mode M           manual|simplified|auto (modo de recarga)    --reserve N  munición de reserva
 *     --sim "T:k=v|k=v;T:..."  guion de entrada; se simula a 120 Hz hasta --at T (def. último evento)
 *          claves: trig0 trig1 grip0 grip1 (0..1) | b0 b1 (A,B,STICK,MAG unidos con +) | support (0/1)
 *                  h1@PUNTO[+dx,dy,dz] (mano 1 sobre un punto del arma: GRIP SUPPORT MUZZLE EYE MAGWELL CHARGE
 *                  + desplazamiento en espacio arma) | h1=x,y,z (mundo) | h1=belt (cadera) | h0=x,y,z
 *                  h1q=align45 (orienta la mano 1 para que un cargador sostenido quede alineado con el pozo)
 *     --ch NOMBRE=v      fuerza un canal tras la simulación (SLIDE BOLT TRIGGER HAMMER MAG LID SAFETY SCREEN ROUND CUSTOM0 CUSTOM1)
 *     --print            vuelca estado (munición, fase, canales) y los eventos del puente */
#include "wprev.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- puente de prueba ---------- */
static int g_reserve = 40, g_print = 0;
static int b_reserve(int id) { (void)id; return g_reserve; }
static void b_take(int id, int n) { (void)id; g_reserve -= n; if (g_print) printf("  [bridge] ammo_take %d -> reserva %d\n", n, g_reserve); }
static void b_fire(int id, WPose m) { (void)id; if (g_print) printf("  [bridge] fire desde (%.3f %.3f %.3f)\n", m.p.x, m.p.y, m.p.z); }
static void b_hap(int h, float s, float t) { if (g_print && s > 0) printf("  [bridge] haptic mano%d %.2f %.3fs\n", h, s, t); }
static const char *snames[WS_COUNT] = {"FIRE", "DRY", "MAG_OUT", "MAG_IN", "SLIDE_BACK", "SLIDE_FWD", "BOLT", "LOCK_TONE", "LOCKED",
                                       "DRAW", "PIN", "LEVER", "THROW", "PLACE", "CAP", "BTN", "LEGS", "EMPTY"};
static void b_snd(int id, int e) { (void)id; if (g_print) printf("  [bridge] sound %s\n", e >= 0 && e < WS_COUNT ? snames[e] : "?"); }

/* ---------- OBJ ---------- */
typedef struct { char name[32]; WVertex *v; uint16_t *i; uint32_t nv, ni, cv, ci; } Grp;
static void gadd(Grp *g, float *p, float *n, float *c) {
    if (g->nv + 1 >= g->cv) { g->cv = g->cv ? g->cv * 2 : 1024; g->v = realloc(g->v, g->cv * sizeof(WVertex)); }
    WVertex *v = &g->v[g->nv++]; memset(v, 0, sizeof *v);
    for (int k = 0; k < 3; k++) { v->pos[k] = p[k]; v->nrm[k] = n ? n[k] : 0; v->rgba[k] = (uint8_t)(c[k] * 255); }
    v->rgba[3] = 255;
}
static int load_obj(const char *path, Grp **out, int *ng) {
    FILE *f = fopen(path, "r"); if (!f) return -1;
    float (*P)[6] = NULL, (*N)[3] = NULL; int np = 0, nn = 0;
    Grp *G = calloc(64, sizeof(Grp)); int n = 0; char line[512];
    strcpy(G[0].name, "obj"); n = 1;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == 'v' && line[1] == ' ') {
            P = realloc(P, (np + 1) * sizeof *P); float *p = P[np++]; p[3] = p[4] = p[5] = 0.7f;
            sscanf(line + 2, "%f %f %f %f %f %f", p, p + 1, p + 2, p + 3, p + 4, p + 5);
        } else if (line[0] == 'v' && line[1] == 'n') {
            N = realloc(N, (nn + 1) * sizeof *N); sscanf(line + 3, "%f %f %f", &N[nn][0], &N[nn][1], &N[nn][2]); nn++;
        } else if (line[0] == 'o' || line[0] == 'g') {
            char nm[32]; if (sscanf(line + 1, "%31s", nm) == 1) {
                if (n == 1 && G[0].nv == 0) n = 0;
                strcpy(G[n].name, nm); n++;
            }
        } else if (line[0] == 'f' && n) {
            int vi[16], ni[16], k = 0; char *s = line + 1, *e;
            while (k < 16) {
                long a = strtol(s, &e, 10); if (e == s) break; s = e; long b = 0;
                if (*s == '/') { s++; strtol(s, &e, 10); s = e; if (*s == '/') { s++; b = strtol(s, &e, 10); s = e; } }
                vi[k] = (int)(a < 0 ? np + a : a - 1); ni[k] = (int)(b ? (b < 0 ? nn + b : b - 1) : -1); k++;
            }
            Grp *g = &G[n - 1];
            for (int t = 1; t + 1 < k; t++) {
                int ids[3] = {0, t, t + 1};
                for (int q = 0; q < 3; q++) {
                    int a = vi[ids[q]], b = ni[ids[q]];
                    if (g->nv > 65530) continue;
                    gadd(g, P[a], b >= 0 ? N[b] : NULL, P[a] + 3);
                    if (g->ni + 1 >= g->ci) { g->ci = g->ci ? g->ci * 2 : 1024; g->i = realloc(g->i, g->ci * 2); }
                    g->i[g->ni++] = (uint16_t)(g->nv - 1);
                }
                if (ni[0] < 0) {      /* normal plana si el OBJ no trae */
                    Grp *gg = g; WVertex *v0 = &gg->v[gg->nv - 3], *v1 = v0 + 1, *v2 = v0 + 2;
                    WVec3 a = wv(v1->pos[0] - v0->pos[0], v1->pos[1] - v0->pos[1], v1->pos[2] - v0->pos[2]);
                    WVec3 b = wv(v2->pos[0] - v0->pos[0], v2->pos[1] - v0->pos[1], v2->pos[2] - v0->pos[2]);
                    WVec3 c = wv_norm(wv_cross(a, b));
                    for (int q = 0; q < 3; q++) { v0[q].nrm[0] = c.x; v0[q].nrm[1] = c.y; v0[q].nrm[2] = c.z; }
                }
            }
        }
    }
    fclose(f); free(P); free(N); *out = G; *ng = n; return 0;
}

static WMesh *g_meshes; static WPart g_part;
static void bind_obj(WeaponState *s, const char *path) {
    Grp *G; int ng;
    if (load_obj(path, &G, &ng)) { fprintf(stderr, "no se puede leer %s\n", path); exit(1); }
    const WeaponDef *d = s->def; int matched = 0;
    if (d->mesh_names) {
        int ns = weapon_mesh_count(d); g_meshes = calloc(ns ? ns : 1, sizeof(WMesh));
        for (int i = 0; i < ns; i++) {
            g_meshes[i].name = d->mesh_names[i];
            for (int k = 0; k < ng; k++) if (!strcmp(G[k].name, d->mesh_names[i])) {
                g_meshes[i].v = G[k].v; g_meshes[i].nv = G[k].nv; g_meshes[i].idx = G[k].i; g_meshes[i].ni = G[k].ni; matched++;
            }
        }
        if (matched) { weapon_bind(s, g_meshes, ns, NULL, 0); return; }
        free(g_meshes);
    }
    /* una sola pieza raíz con todo el modelo */
    Grp all; memset(&all, 0, sizeof all);
    for (int k = 0; k < ng; k++) for (uint32_t t = 0; t < G[k].ni; t++) {
        WVertex *v = &G[k].v[G[k].i[t]]; gadd(&all, v->pos, v->nrm, (float[]){v->rgba[0] / 255.f, v->rgba[1] / 255.f, v->rgba[2] / 255.f});
        if (all.ni + 1 >= all.ci) { all.ci = all.ci ? all.ci * 2 : 1024; all.i = realloc(all.i, all.ci * 2); }
        all.i[all.ni++] = (uint16_t)(all.nv - 1);
    }
    g_meshes = calloc(1, sizeof(WMesh)); g_meshes[0].name = "obj"; g_meshes[0].v = all.v; g_meshes[0].nv = all.nv; g_meshes[0].idx = all.i; g_meshes[0].ni = all.ni;
    g_part = (WPart){"root", -1, {{0, 0, 0}, {0, 0, 0, 1}}, 0, WCH_NONE, {0, 0, 0}, 0};
    weapon_bind(s, g_meshes, 1, &g_part, 1);
}

/* ---------- guion de entrada ---------- */
typedef struct { float t; char kv[256]; } Ev;
static const char *pnames[] = {"GRIP", "SUPPORT", "MUZZLE", "EYE", "MAGWELL", "CHARGE", "SCREEN"};
static const char *chnames[] = {"NONE", "SLIDE", "BOLT", "TRIGGER", "HAMMER", "MAG", "LID", "SAFETY", "SCREEN", "ROUND", "CUSTOM0", "CUSTOM1"};

typedef struct { int mode; int pt; WVec3 off; WVec3 pos; int align45; WVec3 rotdeg; } HandCtl;

static uint32_t parse_btn(const char *s) {
    uint32_t m = 0; char b[64]; strncpy(b, s, 63); b[63] = 0;
    for (char *t = strtok(b, "+"); t; t = strtok(NULL, "+")) {
        if (!strcmp(t, "A")) m |= WB_A; else if (!strcmp(t, "B")) m |= WB_B;
        else if (!strcmp(t, "STICK")) m |= WB_STICK; else if (!strcmp(t, "MAG")) m |= WB_MAG_RELEASE; else m |= (uint32_t)atoi(t);
    }
    return m;
}

static void apply_kv(char *kv, WInput *in, HandCtl *hc) {
    char *e = strchr(kv, '='); char *at = strchr(kv, '@');
    if (at && (!e || at < e)) {                 /* h1@PUNTO+dx,dy,dz */
        *at = 0; char *rest = at + 1; char *plus = strchr(rest, '+'); hc->off = wv(0, 0, 0);
        if (plus) { *plus = 0; sscanf(plus + 1, "%f,%f,%f", &hc->off.x, &hc->off.y, &hc->off.z); }
        for (int i = 0; i < 7; i++) if (!strcmp(rest, pnames[i])) hc->pt = i;
        hc->mode = 2; return;
    }
    if (!e) return;
    *e = 0; const char *k = kv, *v = e + 1; float f = (float)atof(v);
    if (!strcmp(k, "trig0")) in->trigger[0] = f; else if (!strcmp(k, "trig1")) in->trigger[1] = f;
    else if (!strcmp(k, "grip0")) in->grip[0] = f; else if (!strcmp(k, "grip1")) in->grip[1] = f;
    else if (!strcmp(k, "b0")) in->buttons[0] = parse_btn(v); else if (!strcmp(k, "b1")) in->buttons[1] = parse_btn(v);
    else if (!strcmp(k, "support")) in->support_held = (int)f;
    else if (!strcmp(k, "h1")) {
        if (!strcmp(v, "belt")) hc->mode = 3;
        else { hc->mode = 1; sscanf(v, "%f,%f,%f", &hc->pos.x, &hc->pos.y, &hc->pos.z); }
    } else if (!strcmp(k, "h0")) { sscanf(v, "%f,%f,%f", &in->hand[0].p.x, &in->hand[0].p.y, &in->hand[0].p.z); }
    else if (!strcmp(k, "h1q")) { hc->align45 = !strcmp(v, "align45"); }
    else if (!strcmp(k, "h1rot")) { sscanf(v, "%f,%f,%f", &hc->rotdeg.x, &hc->rotdeg.y, &hc->rotdeg.z); hc->align45 = 0; }
}

static void place_hand1(const WeaponState *s, WInput *in, HandCtl *hc) {
    WPose W0 = wpose_mul(in->hand[0], wpose_inv(s->def->points[WP_GRIP]));
    if (hc->mode == 1) in->hand[1].p = hc->pos;
    else if (hc->mode == 2) in->hand[1].p = wpose_apply(W0, wv_add(s->def->points[hc->pt].p, hc->off));
    else if (hc->mode == 3) in->hand[1].p = wv_add(in->head.p, wv(0.12f, -0.70f, -0.10f));
    WQuat q = wquat_identity();
    float d2r = 3.14159265f / 180.f;
    q = wquat_mul(wquat_axis_angle(wv(0, 0, 1), hc->rotdeg.z * d2r), q);
    q = wquat_mul(wquat_axis_angle(wv(0, 1, 0), hc->rotdeg.y * d2r), q);
    q = wquat_mul(wquat_axis_angle(wv(1, 0, 0), hc->rotdeg.x * d2r), q);
    if (hc->align45) {   /* mano tal que mag_up(en mano: -45 grados sobre X) == eje del pozo */
        WQuat r = wquat_axis_angle(wv(1, 0, 0), -0.7853982f);
        q = wquat_mul(wquat_mul(W0.q, s->def->points[WP_MAGWELL].q), wquat_inv(r));
    }
    in->hand[1].q = q;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "uso: preview <id> [opciones] (ver cabecera de preview.c)\n"); return 1; }
    const char *id = argv[1], *out = "out.png", *view = "side", *obj = NULL, *sim = NULL;
    int W = 800, H = 600, ss = 2; float radius = 0.25f, at = -1; WReloadMode rm = WR_MANUAL;
    const char *chs[16]; int nch = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--obj") && i + 1 < argc) obj = argv[++i];
        else if (!strcmp(argv[i], "--view") && i + 1 < argc) view = argv[++i];
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &W, &H);
        else if (!strcmp(argv[i], "--ss") && i + 1 < argc) ss = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--radius") && i + 1 < argc) radius = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--at") && i + 1 < argc) at = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--sim") && i + 1 < argc) sim = argv[++i];
        else if (!strcmp(argv[i], "--reserve") && i + 1 < argc) g_reserve = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--mode") && i + 1 < argc) { const char *m = argv[++i]; rm = !strcmp(m, "simplified") ? WR_SIMPLIFIED : !strcmp(m, "auto") ? WR_AUTOMATIC : WR_MANUAL; }
        else if (!strcmp(argv[i], "--ch") && i + 1 < argc && nch < 16) chs[nch++] = argv[++i];
        else if (!strcmp(argv[i], "--print")) g_print = 1;
        else { fprintf(stderr, "opción desconocida: %s\n", argv[i]); return 1; }
    }
    const WeaponDef *def = NULL;
    for (int i = 0; i < w_registry_count; i++) if (w_registry[i] && !strcmp(w_registry[i]->id, id)) def = w_registry[i];
    if (!def) { fprintf(stderr, "arma '%s' no registrada/enlazada\n", id); return 1; }
    WeaponState *s = calloc(1, def->state_size);
    s->def = def; def->init(s);
    if (obj) bind_obj(s, obj);
    else fprintf(stderr, "aviso: las armas no llevan mallas compiladas; sin --obj no se dibuja nada\n");

    WGameBridge br = {.ammo_reserve = b_reserve, .ammo_take = b_take, .fire = b_fire, .haptic = b_hap, .sound = b_snd};
    WInput in; memset(&in, 0, sizeof in);
    in.head.q = in.hand[0].q = in.hand[1].q = wquat_identity();
    in.head.p = wv(0, 1.6f, 0); in.hand[0].p = wv(0, 1.3f, -0.4f);
    HandCtl hc = {0}; hc.mode = 2; hc.pt = WP_SUPPORT; hc.off = wv(0.25f, -0.1f, 0.1f);
    Ev ev[128]; int nev = 0;
    if (sim) {
        char *cp = strdup(sim);
        for (char *t = strtok(cp, ";"); t && nev < 128; t = strtok(NULL, ";")) {
            char *c = strchr(t, ':'); if (!c) continue; *c = 0; ev[nev].t = (float)atof(t);
            strncpy(ev[nev].kv, c + 1, 255); ev[nev].kv[255] = 0; nev++;
        }
    }
    if (at < 0) at = nev ? ev[nev - 1].t : 0;
    float t = 0, dt = 1.0f / 120; int ne = 0;
    place_hand1(s, &in, &hc);
    for (int step = 0; t < at + 1e-6f || step == 0; step++) {
        while (ne < nev && ev[ne].t <= t + 1e-6f) {
            char tmp[256]; strcpy(tmp, ev[ne].kv);
            for (char *k = strtok(tmp, "|"); k; k = strtok(NULL, "|")) apply_kv(k, &in, &hc);
            ne++;
        }
        place_hand1(s, &in, &hc);
        in.dt = dt;
        if (g_print && 0) printf("t=%.3f\n", t);
        def->update(s, &in, rm, &br);
        t += dt;
        if (t > at + 1e-6f) break;
    }
    for (int i = 0; i < nch; i++) {
        char *e = strchr(chs[i], '='); if (!e) continue;
        for (int c = 0; c < WCH_COUNT; c++) if (!strncmp(chs[i], chnames[c], e - chs[i]) && strlen(chnames[c]) == (size_t)(e - chs[i])) s->channel[c] = (float)atof(e + 1);
    }
    if (g_print) {
        printf("t=%.3f fase=%d cargador=%d balas=%d recamara=%d reserva=%d\n", t, s->phase, s->mag_inserted, s->mag_rounds, s->chambered, g_reserve);
        printf("canales:"); for (int c = 1; c < WCH_COUNT; c++) if (s->channel[c] != 0) printf(" %s=%.4f", chnames[c], s->channel[c]);
        printf("\nheld:"); for (int i = 0; i < W_MAX_HELD; i++) if (s->held[i].active) printf(" [%d mesh=%d]", i, s->held[i].mesh);
        printf("\n");
    }
    WPose ww = wcore_weapon_world(s, &in);
    WImage img = wimg_new(W, H); WCam cam;
    if (!strcmp(view, "sheet")) {
        const char *vs[4] = {"side", "top", "front", "three"};
        int w2 = W / 2, h2 = H / 2;
        for (int k = 0; k < 4; k++) {
            WImage sub = wimg_new(w2, h2); wprev_cam(s, ww, vs[k], radius, &cam);
            wprev_render(s, ww, &cam, &sub, ss); wimg_blit(&img, &sub, (k % 2) * w2, (k / 2) * h2); wimg_free(&sub);
        }
    } else {
        if (wprev_cam(s, ww, view, radius, &cam)) { fprintf(stderr, "vista desconocida\n"); return 1; }
        wprev_render(s, ww, &cam, &img, ss);
    }
    int r = strstr(out, ".ppm") ? wimg_write_ppm(&img, out) : wimg_write_png(&img, out);
    if (r) { fprintf(stderr, "no se pudo escribir %s\n", out); return 1; }
    return 0;
}
