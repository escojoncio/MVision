/* aitest: herramienta de validación de la IA para VR (vr/ai) sobre el host completo en MV_NOGEN (datos locales, no se sube).
 * Arranca vrhost igual que vrhost_run, con un guion de mando de arranque y un guion de pruebas por fotograma:
 *
 *   MV_SIGS=firmas.json aitest <exe> <disco> <frames> <script> [dir_salida]
 *
 * Órdenes del guion (una por línea, # comenta; F = fotograma absoluto del host, direcciones y valores en hex):
 *   poke F addr HEX            escribe bytes (cadena hexadecimal) en la RAM del juego
 *   pokes F addr texto         escribe una cadena ASCII terminada en cero
 *   gcall F func a0 a1 a2 a3   llama a una función del juego (CPU guardada y restaurada); v0 sale en el log
 *   stage F nombre [1]         con 1 solo fija la zona (hash + GM_SetArea; luego gcall de la función de continuar); sin 1 pide al juego cambiar de zona (hash del nombre + GM_SetArea + petición de carga)
 *   ai F p c a                 cambia los ajustes de la IA (percepción 0/1/2, cámaras, actividad)
 *   place F x y z              coloca al jugador (unidades del motor, decimal) escribiendo posición y bloque de control
 *   hold F n x y z             como place, pero cada fotograma durante n fotogramas
 *   front F n D idx            n fotogramas: coloca al jugador a D unidades delante del soldado nº idx de la tabla (según su dirección de mirada) y registra su visión
 *   pad F mask dur             pulsación extra del mando (máscara hex, duración en fotogramas)
 *   probe F n every            cada `every` fotogramas, n veces: estado del jugador, soldados (tabla de percepción), cámaras y contadores
 *   fb F                       vuelca el fotograma que muestra el host (PPM) en dir_salida
 *   log F texto                escribe una marca en el log
 * Salida: todo por stderr con prefijo [ait]. Termina con código 0.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "bridge.h"
#include "bridge_addrs.h"
#include "cpu.h"
#include "host.h"
#include "sys.h"
#include "vr_cameras.h"
#include "vr_perception_rt.h"
#include "vrai.h"
#include "vrhost.h"

#define RAMM 0x1FFFFFu
enum { K_POKE, K_POKES, K_GCALL, K_STAGE, K_AI, K_PLACE, K_HOLD, K_PAD, K_PROBE, K_FB, K_LOG, K_FRONT };
typedef struct { int kind; uint32_t f; uint32_t u[8]; int n, every; char s[160]; double d[3]; } Ev;
static Ev EV[512];
static int nev;
static const char *outdir = ".";
static int in_call;

typedef struct { uint32_t start, dur, every; uint16_t mask; } PadEv;
static PadEv PADX[32];
static int npadx;
static const PadEv PADBASE[] = {{120, 6, 0, 1u << 3}, {400, 6, 0, 1u << 3}, {0, 6, 97, 1u << 13}, {0, 6, 211, 1u << 7}};
static uint16_t pad_at(uint32_t f) {
    uint16_t b = 0;
    for (int i = 0; i < npadx; i++) { const PadEv *p = &PADX[i]; if (f >= p->start && f < p->start + p->dur) b |= p->mask; }
    for (unsigned i = 0; i < sizeof PADBASE / sizeof PADBASE[0]; i++) {
        const PadEv *p = &PADBASE[i];
        if (p->every) { if (f >= p->every && f % p->every < p->dur && f < 7000) b |= p->mask; }
        else if (f >= p->start && f < p->start + p->dur) b |= p->mask;
    }
    return b;
}

static void load_script(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) { perror(path); exit(1); }
    char l[400];
    while (fgets(l, sizeof l, fp) && nev < 512) {
        char k[32] = "";
        if (l[0] == '#' || sscanf(l, "%31s", k) != 1) continue;
        Ev *e = &EV[nev];
        memset(e, 0, sizeof *e);
        const char *a = l + strspn(l, " \t") + strlen(k);
        if (!strcmp(k, "poke")) { e->kind = K_POKE; sscanf(a, "%u %x %159s", &e->f, &e->u[0], e->s); }
        else if (!strcmp(k, "pokes")) { e->kind = K_POKES; sscanf(a, "%u %x %159s", &e->f, &e->u[0], e->s); }
        else if (!strcmp(k, "gcall")) { e->kind = K_GCALL; sscanf(a, "%u %x %x %x %x %x", &e->f, &e->u[0], &e->u[1], &e->u[2], &e->u[3], &e->u[4]); }
        else if (!strcmp(k, "stage")) { e->kind = K_STAGE; sscanf(a, "%u %15s %d", &e->f, e->s, &e->n); }
        else if (!strcmp(k, "ai")) { e->kind = K_AI; sscanf(a, "%u %u %u %u", &e->f, &e->u[0], &e->u[1], &e->u[2]); }
        else if (!strcmp(k, "place")) { e->kind = K_PLACE; sscanf(a, "%u %lf %lf %lf", &e->f, &e->d[0], &e->d[1], &e->d[2]); e->n = 1; }
        else if (!strcmp(k, "hold")) { e->kind = K_HOLD; sscanf(a, "%u %d %lf %lf %lf", &e->f, &e->n, &e->d[0], &e->d[1], &e->d[2]); }
        else if (!strcmp(k, "front")) { e->kind = K_FRONT; sscanf(a, "%u %d %lf %d", &e->f, &e->n, &e->d[0], &e->u[0]); }
        else if (!strcmp(k, "pad")) { e->kind = K_PAD; unsigned m = 0, d = 0; sscanf(a, "%u %x %u", &e->f, &m, &d); if (npadx < 32) { PADX[npadx].start = e->f; PADX[npadx].mask = (uint16_t)m; PADX[npadx].dur = d; npadx++; } continue; }
        else if (!strcmp(k, "probe")) { e->kind = K_PROBE; sscanf(a, "%u %d %d", &e->f, &e->n, &e->every); if (e->every < 1) e->every = 1; }
        else if (!strcmp(k, "fb")) { e->kind = K_FB; sscanf(a, "%u", &e->f); }
        else if (!strcmp(k, "log")) { e->kind = K_LOG; sscanf(a, "%u", &e->f); const char *q = strchr(a + strspn(a, " "), ' '); snprintf(e->s, sizeof e->s, "%s", q ? q + 1 : ""); char *nl = strchr(e->s, '\n'); if (nl) *nl = 0; }
        else { fprintf(stderr, "[ait] orden desconocida: %s", l); continue; }
        nev++;
    }
    fclose(fp);
}

static int16_t r16(uint32_t a) { return *(int16_t *)(psx_ram + (a & RAMM)); }
static uint32_t r32(uint32_t a) { return *(uint32_t *)(psx_ram + (a & RAMM)); }
static void w16(uint32_t a, int16_t v) { *(int16_t *)(psx_ram + (a & RAMM)) = v; }

static void place_player(double x, double y, double z) {
    int16_t p[3] = {(int16_t)lrint(x), (int16_t)lrint(y), (int16_t)lrint(z)};
    for (int i = 0; i < 3; i++) w16(BA_PLAYER_POS + 2u * i, p[i]);
    uint32_t ctl = r32(BA_PLAYER_CTRL);
    if (ctl >= 0x80000000u && ctl < 0x80200000u) for (int i = 0; i < 3; i++) w16(ctl + BC_MOV + 2u * i, p[i]);
}

static void probe(Cpu *c, uint32_t f) {
    fprintf(stderr, "[ait]   cpu ra=%08x sp=%08x a0=%08x v0=%08x gp+EC=%08x gp+F0=%08x gp+E8=%08x\n", c->r[31], c->r[29], c->r[4], c->r[2], r32(0x800AC574u), r32(0x800AC578u), r32(0x800AC56Cu));
    char st[8] = {0};
    memcpy(st, psx_ram + (0x800AC568u & RAMM), 5);
    fprintf(stderr, "[ait] f=%u zona=%s jugador=(%d,%d,%d) ctl=%08x mapa=%08x\n", f, st, r16(BA_PLAYER_POS), r16(BA_PLAYER_POS + 2), r16(BA_PLAYER_POS + 4), r32(BA_PLAYER_CTRL), r32(BA_PLAYER_MAP));
    VraiStats s; vrai_stats(&s);
    fprintf(stderr, "[ait]   vrai: instalado=%d overlay=%d reubic=%u visión=%u ampliada=%u sospechas=%u alertas_lejos=%u | sonido llam=%u reprod=%u silenc=%u | cámaras=%d parches=%d\n", s.installed, s.overlay_found,
            s.relocates, s.vision_calls, s.extended_hits, s.suspects, s.alerts_far, s.se_calls, s.se_played, s.se_dropped, s.cameras_seen, s.patches);
    const VrpCtx *x = vrp_rt_ctx();
    if (x && x->sites.found) {
        for (int i = 0; i < VRP_MAX_ENTRIES; i++) {
            const VrpEntry *e = &x->tab[i];
            if (!e->actor) continue;
            int16_t sp[3] = {r16(e->actor + 0x20), r16(e->actor + 0x22), r16(e->actor + 0x24)};
            double dx = sp[0] - r16(BA_PLAYER_POS), dz = sp[2] - r16(BA_PLAYER_POS + 4);
            fprintf(stderr, "[ait]   soldado %08x pos=(%d,%d,%d) dist_xz=%.0f | modelo: acc=%.2f dist=%u salida=%d lejos=%d\n", e->actor, sp[0], sp[1], sp[2], sqrt(dx * dx + dz * dz), e->acc, e->last_dist_q, e->last_out, e->seen_far);
        }
    }
    const VrcTable *t = vrc_table();
    if (t) for (int i = 0; i < t->n; i++) {
        const VrcCamera *k = &t->cam[i];
        double dx = k->pos[0] - r16(BA_PLAYER_POS), dz = k->pos[2] - r16(BA_PLAYER_POS + 4);
        fprintf(stderr, "[ait]   cámara %08x fam=%d pos=(%d,%d,%d) dist_xz=%.0f rango=%d mitad=%d estado=%d vivo=%d activa=%d golpes=%d blanco=%08x dentro_cono=%d\n", k->actor, (int)k->family, k->pos[0], k->pos[1], k->pos[2],
                sqrt(dx * dx + dz * dz), k->range, k->half, (int)k->status, k->alive, k->active, k->hits, k->target, 0);
    }
    VrcSoundEvent se[8];
    int n = vrc_sound_events(se, 8);
    if (n > 0) fprintf(stderr, "[ait]   sonidos de cámara registrados: %d\n", n);
}

static int stage_req(Cpu *c, const char *name, int do_req) {
    const uint32_t buf = 0x800B5F18u;   /* nombre de zona de las variables de partida (cabe sobrado) */
    memset(psx_ram + (buf & RAMM), 0, 8);
    memcpy(psx_ram + (buf & RAMM), name, strlen(name));
    Cpu save = *c;
    uint32_t sp = 0x801FFF00u;
    c->r[29] = sp; c->r[4] = buf;
    in_call = 1; rt_call(c, 0x80016DE8u); in_call = 0;
    uint32_t h = c->r[2] & 0xFFFFu;
    *c = save;
    if (do_req != 2) {
    c->r[29] = sp; c->r[4] = h; c->r[5] = buf;
    in_call = 1; rt_call(c, 0x8002B124u); in_call = 0;   /* GM_SetArea(hash, nombre) */
    *c = save;
    }
    if (do_req) *(uint32_t *)(psx_ram + (0x800AC574u & RAMM)) = (h << 16) | 0xE0u;   /* petición de carga: (hash << 16) | 0xE0 */
    fprintf(stderr, "[ait] zona pedida: %s hash=%04x\n", name, h);
    return 0;
}

static int hold_left; static double hold_p[3];
static int front_left, front_idx; static double front_d;
static void front_step(uint32_t f) {
    const VrpCtx *x = vrp_rt_ctx();
    if (!x || !x->sites.found) return;
    const VrpLayout *L = x->sites.lay;
    int k = 0;
    for (int i = 0; i < VRP_MAX_ENTRIES; i++) {
        const VrpEntry *e = &x->tab[i];
        if (!e->actor) continue;
        if (k++ != front_idx) continue;
        uint32_t a = e->actor;
        double face = r16(a + L->o_facedir) & 4095, ang = face * 2.0 * M_PI / 4096.0;
        double sx = r16(a + 0x20), sy = r16(a + 0x22), sz = r16(a + 0x24);
        /* lectura de lo que vio la prueba en el fotograma que acaba de pasar (antes de recolocar) */
        fprintf(stderr, "[ait] front f=%u sold=%08x len=%d ang=%d face=%d sndir=%d sndis=%d flag=%d | modelo acc=%.2f out=%d lejos=%d | vrp: lejos=%u sosp=%u\n", f, a, r16(a + L->o_len), r16(a + L->o_angle), (int)face,
                (int)(r32(a + L->o_sndir) & 4095), (int)r32(a + L->o_sndis), r16(a + L->o_flag), e->acc, e->last_out, e->seen_far, x->st.extended_hits, x->st.suspects);
        place_player(sx + front_d * sin(ang), sy, sz + front_d * cos(ang));
        return;
    }
}
static void hook(void *cpu, uint32_t f) {
    if (in_call) return;
    Cpu *c = cpu;
    if (front_left > 0) { front_step(f); front_left--; }
    if (hold_left > 0) { place_player(hold_p[0], hold_p[1], hold_p[2]); hold_left--; }
    for (int i = 0; i < nev; i++) {
        Ev *e = &EV[i];
        if (e->kind == K_PROBE) { if (f >= e->f && e->n > 0 && (f - e->f) % (uint32_t)e->every == 0) { probe(c, f); e->n--; e->u[7] = 1; } continue; }
        if (e->f != f) continue;
        switch (e->kind) {
        case K_POKE: { size_t n = strlen(e->s) / 2; for (size_t k = 0; k < n; k++) { unsigned v = 0; sscanf(e->s + 2 * k, "%2x", &v); psx_ram[((e->u[0] & RAMM) + k) & RAMM] = (uint8_t)v; } break; }
        case K_POKES: memcpy(psx_ram + (e->u[0] & RAMM), e->s, strlen(e->s) + 1); break;
        case K_GCALL: {
            Cpu save = *c;
            c->r[29] = 0x801FFF00u; c->r[4] = e->u[1]; c->r[5] = e->u[2]; c->r[6] = e->u[3]; c->r[7] = e->u[4];
            in_call = 1; rt_call(c, e->u[0]); in_call = 0;
            fprintf(stderr, "[ait] gcall f=%u %08x -> v0=0x%08x\n", f, e->u[0], c->r[2]);
            *c = save;
            break; }
        case K_STAGE: stage_req(c, e->s, e->n == 0 ? 1 : e->n == 2 ? 2 : 0); break;
        case K_AI: mv_vrhost_set_ai((int)e->u[0], (int)e->u[1], (int)e->u[2]); fprintf(stderr, "[ait] f=%u IA: percepción=%u cámaras=%u actividad=%u\n", f, e->u[0], e->u[1], e->u[2]); break;
        case K_FRONT: front_left = e->n; front_d = e->d[0]; front_idx = (int)e->u[0]; break;
        case K_PLACE: place_player(e->d[0], e->d[1], e->d[2]); break;
        case K_HOLD: hold_left = e->n; hold_p[0] = e->d[0]; hold_p[1] = e->d[1]; hold_p[2] = e->d[2]; break;
        case K_LOG: fprintf(stderr, "[ait] f=%u %s\n", f, e->s); break;
        default: break;
        }
    }
}

int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "uso: %s <exe> <disco> <frames> <guion> [dir_salida]\n", argv[0]); return 1; }
    const char *exe = argv[1], *disc = argv[2];
    uint32_t nframes = (uint32_t)strtoul(argv[3], 0, 0);
    load_script(argv[4]);
    if (argc > 5) { outdir = argv[5]; mkdir(outdir, 0755); }
    if (getenv("MV_SIGS")) mv_host_set_sigs(getenv("MV_SIGS"));
    mkdir("build-host", 0755);
    mkdir("build-host/card", 0755);
    mv_vrhost_install();
    mv_vrhost_set_test_hook(hook);
    mv_host_set_turbo(1);
    int r = mv_host_start(exe, disc, "build-host/card");
    if (r) { fprintf(stderr, "mv_host_start = %d: %s\n", r, mv_host_error()); return 1; }
    uint32_t last = 0xFFFFFFFFu;
    static uint8_t fbuf[MV_HOST_FB_BYTES];
    for (;;) {
        uint32_t f = mv_host_frame_count();
        if (mv_host_state() == MV_HOST_ERROR) { fprintf(stderr, "[ait] error del motor en f=%u: %s\n", f, mv_host_error()); return 2; }
        if (f >= nframes) break;
        if (f == last) { usleep(200); continue; }
        last = f;
        mv_host_pad(pad_at(f), 128, 128, 128, 128);
        float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 1.65f, 0, 1};
        mv_vr_set_head(m, 0.0);
        for (int i = 0; i < nev; i++) if (EV[i].kind == K_FB && EV[i].f == f) {
            int w = 0, h = 0;
            if (mv_host_framebuffer(fbuf, sizeof fbuf, &w, &h, 0)) {
                char p[600];
                snprintf(p, sizeof p, "%s/fb_%05u.ppm", outdir, f);
                FILE *o = fopen(p, "wb");
                if (o) { fprintf(o, "P6\n%d %d\n255\n", w, h); for (int k = 0; k < w * h; k++) fwrite(fbuf + 4 * k, 1, 3, o); fclose(o); }
                fprintf(stderr, "[ait] fb f=%u %dx%d -> %s\n", f, w, h, p);
            }
        }
    }
    fprintf(stderr, "[ait] fin f=%u\n", mv_host_frame_count());
    return 0;
}
