/* test_ai_run: herramienta de validación de la IA para VR (vr/ai) sobre el host completo en MV_NOGEN (datos locales, no se sube).
 * Arranca vrhost igual que vrhost_run, con un guion de mando de arranque y un guion de pruebas por fotograma:
 *
 *   MV_SIGS=firmas.json test_ai_run <exe> <disco> <frames> <script> [dir_salida]
 *
 * Órdenes del guion (una por línea, # comenta; F = fotograma absoluto del host, direcciones y valores en hex):
 *   poke F addr HEX            escribe bytes (cadena hexadecimal) en la RAM del juego
 *   pokes F addr texto         escribe una cadena ASCII terminada en cero
 *   gcall F func a0 a1 a2 a3   llama a una función del juego (CPU guardada y restaurada); v0 sale en el log
 *   stage F nombre [1]         con 1 solo fija la zona (hash + función que registra la zona actual; luego gcall de la función de continuar); sin 1 pide al juego cambiar de zona (hash del nombre + registro de la zona + petición de carga)
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
#include "vr_camera.h"
#include "rtpatch.h"
#include "vrhost.h"

#define RAMM 0x1FFFFFu
enum { K_POKE, K_POKES, K_GCALL, K_STAGE, K_AI, K_PLACE, K_HOLD, K_PAD, K_PROBE, K_FB, K_LOG, K_FRONT, K_DUMP, K_CFACE, K_CSWEEP, K_CHIT, K_SPUPROBE, K_FACE, K_WATCH, K_SCENE, K_TDUMP, K_ALIST, K_CFWD, K_ATK, K_HEAD, K_TRI };
typedef struct { int kind; uint32_t f; uint32_t u[8]; int n, every; char s[160]; double d[3]; } Ev;
static Ev EV[512];
static int nev;
static const char *outdir = ".";
static int in_call;
static uint32_t atk_calls;
static volatile double head_yaw_deg;
static VrcTable MT;   /* tabla propia de cámaras (la de vrc_frame solo existe con las cámaras VR encendidas; con la IA original se escanea aquí igualmente) */

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
        else if (!strcmp(k, "dump")) { e->kind = K_DUMP; sscanf(a, "%u %159s", &e->f, e->s); }
        else if (!strcmp(k, "cface")) { e->kind = K_CFACE; sscanf(a, "%u %d %lf %d %lf %lf", &e->f, &e->n, &e->d[0], &e->u[0], &e->d[1], &e->d[2]); }
        else if (!strcmp(k, "csweep")) { e->kind = K_CSWEEP; sscanf(a, "%u %d %lf %lf %lf %d", &e->f, &e->u[0], &e->d[0], &e->d[1], &e->d[2], &e->n); }
        else if (!strcmp(k, "chit")) { e->kind = K_CHIT; sscanf(a, "%u %d %d", &e->f, &e->u[0], &e->n); }
        else if (!strcmp(k, "face")) { e->kind = K_FACE; sscanf(a, "%u %d %d", &e->f, &e->n, &e->u[0]); }
        else if (!strcmp(k, "tri")) { e->kind = K_TRI; sscanf(a, "%u %d", &e->f, &e->n); }
        else if (!strcmp(k, "head")) { e->kind = K_HEAD; sscanf(a, "%u %lf", &e->f, &e->d[0]); }
        else if (!strcmp(k, "atk")) { e->kind = K_ATK; sscanf(a, "%u %d", &e->f, &e->n); }
        else if (!strcmp(k, "cfwd")) { e->kind = K_CFWD; sscanf(a, "%u %d %lf %lf", &e->f, &e->u[0], &e->d[0], &e->d[1]); }
        else if (!strcmp(k, "alist")) { e->kind = K_ALIST; sscanf(a, "%u %d", &e->f, &e->n); }
        else if (!strcmp(k, "tdump")) { e->kind = K_TDUMP; sscanf(a, "%u %d", &e->f, &e->n); }
        else if (!strcmp(k, "scene")) { e->kind = K_SCENE; sscanf(a, "%u %d %d %lf", &e->f, &e->n, &e->u[0], &e->d[0]); }
        else if (!strcmp(k, "watch")) { e->kind = K_WATCH; sscanf(a, "%u %d %d", &e->f, &e->n, &e->u[0]); }
        else if (!strcmp(k, "spu")) { e->kind = K_SPUPROBE; sscanf(a, "%u %d", &e->f, &e->n); }
        else if (!strcmp(k, "pad")) { e->kind = K_PAD; unsigned m = 0, d = 0; sscanf(a, "%u %x %u", &e->f, &m, &d); if (npadx < 32) { PADX[npadx].start = e->f; PADX[npadx].mask = (uint16_t)m; PADX[npadx].dur = d; npadx++; } continue; }
        else if (!strcmp(k, "probe")) { e->kind = K_PROBE; sscanf(a, "%u %d %d", &e->f, &e->n, &e->every); if (e->every < 1) e->every = 1; }
        else if (!strcmp(k, "fb")) { e->kind = K_FB; sscanf(a, "%u", &e->f); }
        else if (!strcmp(k, "log")) { e->kind = K_LOG; sscanf(a, "%u", &e->f); const char *q = strchr(a + strspn(a, " "), ' '); snprintf(e->s, sizeof e->s, "%s", q ? q + 1 : ""); char *nl = strchr(e->s, '\n'); if (nl) *nl = 0; }
        else { fprintf(stderr, "[ait] orden desconocida: %s", l); continue; }
        nev++;
    }
    fclose(fp);
}

static int16_t r16(uint32_t a) { return *(int16_t *)(emu_ram + (a & RAMM)); }
static uint32_t r32(uint32_t a) { return *(uint32_t *)(emu_ram + (a & RAMM)); }
static void w16(uint32_t a, int16_t v) { *(int16_t *)(emu_ram + (a & RAMM)) = v; }

static void place_player(double x, double y, double z) {
    int16_t p[3] = {(int16_t)lrint(x), (int16_t)lrint(y), (int16_t)lrint(z)};
    for (int i = 0; i < 3; i++) w16(BA_PLAYER_POS + 2u * i, p[i]);
    uint32_t ctl = r32(BA_PLAYER_CTRL);
    if (ctl >= 0x80000000u && ctl < 0x80200000u) for (int i = 0; i < 3; i++) w16(ctl + BC_MOV + 2u * i, p[i]);
}

static void probe(Cpu *c, uint32_t f) {
    fprintf(stderr, "[ait]   cpu ra=%08x sp=%08x a0=%08x v0=%08x gp+EC=%08x gp+F0=%08x gp+E8=%08x\n", c->r[31], c->r[29], c->r[4], c->r[2], r32(0x800AC574u), r32(0x800AC578u), r32(0x800AC56Cu));
    char st[8] = {0};
    memcpy(st, emu_ram + (0x800AC568u & RAMM), 5);
    fprintf(stderr, "[ait] f=%u zona=%s jugador=(%d,%d,%d) ctl=%08x mapa=%08x\n", f, st, r16(BA_PLAYER_POS), r16(BA_PLAYER_POS + 2), r16(BA_PLAYER_POS + 4), r32(BA_PLAYER_CTRL), r32(BA_PLAYER_MAP));
    fprintf(stderr, "[ait]   ataques(llamadas a la función de ataque)=%u\n", atk_calls);
    fprintf(stderr, "[ait]   arma=%d municion=[%d..] cargador=%d/%d estado_jugador=%08x global=%08x pausa=%d\n", r16(BA_VARS + 2 * BRIDGE_V_WEAPON), r16(BA_VARS + 2 * BRIDGE_V_WEAPON_AMMO), r16(BA_MAGAZINE), r16(BA_MAGAZINE_MAX), r32(BA_PLAYER_STATUS), r32(BA_GAME_STATUS), (int)r32(BA_PAUSE_LEVEL));
    { uint32_t ps, pf; vrcam_pack_stats(&ps, &pf); fprintf(stderr, "[ait] paquetes (HLE de la etapa de polígono): vistos=%u re-emitidos=%u\n", ps, pf); }
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
    const VrcTable *t = &MT;
    if (t) for (int i = 0; i < t->n; i++) {
        const VrcCamera *k = &t->cam[i];
        double dx = k->pos[0] - r16(BA_PLAYER_POS), dz = k->pos[2] - r16(BA_PLAYER_POS + 4);
        fprintf(stderr, "[ait]   cámara %08x fam=%d pos=(%d,%d,%d) rot=(%d,%d) dist_xz=%.0f rango=%d mitad=%d estado=%d vivo=%d activa=%d golpes=%d blanco=%08x\n", k->actor, (int)k->family, k->pos[0], k->pos[1], k->pos[2], k->rot[0], k->rot[1],
                sqrt(dx * dx + dz * dz), k->range, k->half, (int)k->status, k->alive, k->active, k->hits, k->target);
        if (k->target) fprintf(stderr, "[ait]     blanco: clase=%04x +6=%04x +26=%d +28=%d +3e=%d | detectado=%d modo=%d muerte=%d act=%08x\n", (uint16_t)r16(k->target), (uint16_t)r16(k->target + 6), r16(k->target + 0x26), r16(k->target + 0x28), r16(k->target + 0x3e),
                (int)r32(k->actor + 476), (int)r32(k->actor + 0x1E0), (int)r32(k->actor + 0x1F0), r32(k->actor + 8));
    }
    VrcSoundEvent se[8];
    int n = vrc_sound_events(se, 8);
    if (n > 0) fprintf(stderr, "[ait]   sonidos de cámara registrados: %d\n", n);
}

static int stage_req(Cpu *c, const char *name, int do_req) {
    const uint32_t buf = 0x800B5F18u;   /* nombre de zona de las variables de partida (cabe sobrado) */
    memset(emu_ram + (buf & RAMM), 0, 8);
    memcpy(emu_ram + (buf & RAMM), name, strlen(name));
    Cpu save = *c;
    uint32_t sp = 0x801FFF00u;
    c->r[29] = sp; c->r[4] = buf;
    in_call = 1; rt_call(c, 0x80016DE8u); in_call = 0;
    uint32_t h = c->r[2] & 0xFFFFu;
    *c = save;
    if (do_req == 3) { fprintf(stderr, "[ait] hash(%s)=%04x\n", name, h); return 0; }
    if (do_req != 2) {
    c->r[29] = sp; c->r[4] = h; c->r[5] = buf;
    in_call = 1; rt_call(c, 0x8002B124u); in_call = 0;   /* registra la zona actual (hash, nombre) */
    *c = save;
    }
    if (do_req) *(uint32_t *)(emu_ram + (0x800AC574u & RAMM)) = (h << 16) | 0xE0u;   /* petición de carga: (hash << 16) | 0xE0 */
    fprintf(stderr, "[ait] zona pedida: %s hash=%04x\n", name, h);
    return 0;
}


/* ---- cámaras reales: colocar, barrer alcance, golpear, sonido ---- */
#include "spu.h"
static const VrcCamera *cam_at(int idx) {
    return (idx >= 0 && idx < MT.n) ? &MT.cam[idx] : NULL;
}
static void set_player_yaw(int yaw) {
    uint32_t ctl = r32(BA_PLAYER_CTRL);
    if (ctl >= 0x80000000u && ctl < 0x80200000u) w16(ctl + BC_ROT + 2u, (int16_t)(yaw & 4095));
}
/* jugador a D unidades (horizontal) de la cámara, a `ang` grados del eje de la cámara (0 = justo delante), a altura base + h, mirando a la cámara */
static int ground_y;
static void cam_place(const VrcCamera *c, double D, double ang, double h) {
    double yaw = ang >= 1000.0 ? (ang - 1000.0) * M_PI / 180.0 : c->rot[1] * 2.0 * M_PI / 4096.0 + ang * M_PI / 180.0;   /* ang >= 1000: rumbo absoluto (grados) = ang - 1000 */
    double x = c->pos[0] + D * sin(yaw), z = c->pos[2] + D * cos(yaw);
    place_player(x, ground_y + h, z);
    int face = (int)lrint(atan2(c->pos[0] - x, c->pos[2] - z) * 4096.0 / (2.0 * M_PI));
    set_player_yaw(face);
}
static int cf_left, cf_idx; static double cf_d, cf_ang, cf_h;
/* barrido de alcance */
static int sw_on, sw_idx, sw_dwell, sw_t, sw_hit; static double sw_d, sw_d1, sw_step; static uint32_t sw_f0;
static int cam_detected(const VrcCamera *c) { return r32(c->actor + 476) != 0 || r32(c->actor + 0x1E0) != 0; }
static void sweep_step(uint32_t f) {
    const VrcCamera *c = cam_at(sw_idx);
    if (!c) { sw_on = 0; return; }
    if (sw_t == 0) cam_place(c, sw_d, cf_ang, cf_h);
    else cam_place(c, sw_d, cf_ang, cf_h);
    if (sw_t >= 3 && cam_detected(c)) sw_hit = 1;
    if (++sw_t >= sw_dwell) {
        { double ddx = r16(BA_PLAYER_POS) - c->pos[0], ddy = r16(BA_PLAYER_POS + 2) - c->pos[1], ddz = r16(BA_PLAYER_POS + 4) - c->pos[2];
          fprintf(stderr, "[ait] csweep cam=%d D=%.0f ang=%.0f: %s (detectado=%d modo=%d) jugador=(%d,%d,%d) dist3d=%.0f mapa=%08x\n", sw_idx, sw_d, cf_ang, sw_hit ? "DETECTADO" : "no", (int)r32(c->actor + 476), (int)r32(c->actor + 0x1E0), r16(BA_PLAYER_POS), r16(BA_PLAYER_POS + 2), r16(BA_PLAYER_POS + 4), sqrt(ddx * ddx + ddy * ddy + ddz * ddz), r32(BA_PLAYER_MAP)); }
        if (sw_hit) { fprintf(stderr, "[ait] csweep RESULTADO cam=%d primera detección a D=%.0f (rango del script %d, mitad %d)\n", sw_idx, sw_d, c->range, c->half); sw_on = 0; return; }
        sw_t = 0; sw_d -= sw_step;
        if (sw_d < sw_d1 - 1e-9) { fprintf(stderr, "[ait] csweep RESULTADO cam=%d sin detección hasta D=%.0f\n", sw_idx, sw_d1); sw_on = 0; }
    }
    (void)f;
}
static int spu_prev[24]; static uint32_t spu_t0[24];
static void spu_dump(uint32_t f) {   /* solo las voces que acaban de arrancar (envolvente 0 -> >0) */
    for (int v = 0; v < 24; v++) {
        uint32_t b = 0x1F801C00u + 16u * (uint32_t)v;
        int adsr = (int)spu_io_read(b + 0xC, 2) & 0xFFFF;
        if (adsr && !spu_prev[v]) {
            int l = (int16_t)spu_io_read(b, 2), r = (int16_t)spu_io_read(b + 2, 2);
            fprintf(stderr, "[ait]   spu f=%u voz NUEVA %d: volL=%d volR=%d adsr=%d pitch=%04x\n", f, v, l, r, adsr, (unsigned)spu_io_read(b + 4, 2));
            spu_t0[v] = f;
        }
        if (adsr && spu_t0[v] && f == spu_t0[v] + 3) {
            int l = (int16_t)spu_io_read(b, 2), r = (int16_t)spu_io_read(b + 2, 2);
            fprintf(stderr, "[ait]   spu f=%u voz %d (+3 fotogramas): volL=%d volR=%d adsr=%d pitch=%04x\n", f, v, l, r, adsr, (unsigned)spu_io_read(b + 4, 2));
        }
        spu_prev[v] = adsr;
    }
}
static int spu_left;
static void spu_step(uint32_t f) {
    VrcSoundEvent se[8];
    int n = vrc_sound_events(se, 8);
    for (int i = 0; i < n; i++)
        fprintf(stderr, "[ait]   sonido cam f=%u se=%u cam=%d dist=%.0f vol_orig=%u vol=%u pan=%u gain=%.3f reproducido=%u\n", f, se[i].se, se[i].cam_index, se[i].dist, se[i].vol_orig, se[i].vol, se[i].pan, se[i].gain, se[i].played);
    spu_dump(f);
}
static int wa_left, wa_idx;
static uint32_t wa_prev[8];
static void watch_step(uint32_t f) {
    const VrcCamera *c = cam_at(wa_idx);
    if (!c || !c->target) return;
    uint32_t v[8] = {(uint16_t)r16(c->target), (uint16_t)r16(c->target + 6), (uint32_t)(int32_t)r16(c->target + 0x26), (uint32_t)(int32_t)r16(c->target + 0x28), (uint32_t)(int32_t)r16(c->target + 0x3e), r32(c->actor + 0x1E0), r32(c->actor + 0x1F0), (uint32_t)c->hits};
    if (memcmp(v, wa_prev, sizeof v)) {
        fprintf(stderr, "[ait] watch f=%u cam=%d clase=%04x +6=%04x +26=%d +28=%d +3e=%d modo=%u muerte=%u golpes=%u vivo=%d estado=%d\n", f, wa_idx, v[0], v[1], (int)v[2], (int)v[3], (int)v[4], v[5], v[6], v[7], c->alive, (int)c->status);
        memcpy(wa_prev, v, sizeof v);
    }
}
/* consumo de escenas VR para contar los triángulos 3D que caen sobre una cámara (visibilidad a distancia) */
static volatile int scn_left, scn_idx; static double scn_R = 0.9;
static struct { uint32_t scenes, with3d; double sum0, sum1, tri_all; uint32_t max0, max1; } SCN;
static int scn_report;
/* cobertura 3D: `tri F n` mide n fotogramas los triángulos 3D (lotes kind<=1) de la escena que publica el host con la guiñada de cabeza actual */
static int tri_left, tri_n; static double tri_sum, tri_min, tri_max, tri_yaw;
static int td_left, al_left;
static volatile int atk_left; static uint32_t atk_f;
#define ATK_FN 0x8002E208u   /* aplica el ataque de un blanco (bala, golpe, explosión) a los blancos dañables que toca */
static void atk_wrap(Cpu *c) {
    uint32_t a = c->r[4];
    int16_t pre6[64];
    for (int i = 0; i < 64; i++) pre6[i] = r16(0x800B7660u + 72u * (uint32_t)i + 6);
    int16_t pre_cls[64];
    for (int i = 0; i < 64; i++) pre_cls[i] = r16(0x800B7660u + 72u * (uint32_t)i);
    rt_call_orig(c, ATK_FN);
    atk_calls++;
    if (atk_left <= 0) return;
    if (getenv("ATK_ALL")) fprintf(stderr, "[ait] atk call f=%u a=%08x cls=%04x ra=%08x\n", atk_f, a, (uint16_t)r16(a), c->r[31]);
    int ai = (a >= 0x800B7660u && a < 0x800B7660u + 72u * 64u) ? (int)((a - 0x800B7660u) / 72u) : -1;
    int stamped = 0;
    for (int i = 0; i < 64; i++) {
        uint32_t t = 0x800B7660u + 72u * (uint32_t)i;
        if (i != ai && pre_cls[i] && !(pre6[i] & 4) && (r16(t + 6) & 4)) {
            fprintf(stderr, "[ait] ataque f=%u atacante#%d (cls=%04x +24=%04x +26=%d +3e=%d centro=(%d,%d,%d) mitad=(%d,%d,%d)) -> sella blanco #%d cls=%04x +3e=%d +26=%d +28=%d\n", atk_f, ai, (uint16_t)r16(a), (uint16_t)r16(a + 0x24), r16(a + 0x26), r16(a + 0x3e),
                    r16(a + 8), r16(a + 10), r16(a + 12), r16(a + 0x10), r16(a + 0x12), r16(a + 0x14), i, (uint16_t)r16(t), r16(t + 0x3e), r16(t + 0x26), r16(t + 0x28));
            stamped++;
        }
    }
    if (!stamped) fprintf(stderr, "[ait] ataque f=%u atacante#%d cls=%04x +24=%04x +26=%d +3e=%d centro=(%d,%d,%d) mitad=(%d,%d,%d): sin blanco sellado\n", atk_f, ai, (uint16_t)r16(a), (uint16_t)r16(a + 0x24), r16(a + 0x26), r16(a + 0x3e),
            r16(a + 8), r16(a + 10), r16(a + 12), r16(a + 0x10), r16(a + 0x12), r16(a + 0x14));
}
static void alist_step(uint32_t f) {   /* altas y bajas de actores en las 9 listas (nivel, dirección, función de actualización) */
    static uint32_t seen[9][256]; static uint32_t seen_act[9][256]; static int nseen[9], first = 1;
    uint32_t cur[9][256]; int ncur[9];
    memset(ncur, 0, sizeof ncur);
    for (int lv = 0; lv < 9; lv++) {
        uint32_t base = 0x800ADDA8u + 68u * (uint32_t)lv, endn = base + 32u, node = r32(base + 4);
        for (int st = 0; node != endn && st < 255 && node >= 0x80010000u && node < 0x80200000u; st++, node = r32(node + 4)) cur[lv][ncur[lv]++] = node;
    }
    for (int lv = 0; lv < 9; lv++) {
        for (int i = 0; i < ncur[lv]; i++) {
            int found = 0;
            for (int k = 0; k < nseen[lv]; k++) if (seen[lv][k] == cur[lv][i]) found = 1;
            if (!found && !first) fprintf(stderr, "[ait] actor ALTA f=%u nivel=%d nodo=%08x act=%08x\n", f, lv, cur[lv][i], r32(cur[lv][i] + 8));
        }
        for (int k = 0; k < nseen[lv]; k++) {
            int found = 0;
            for (int i = 0; i < ncur[lv]; i++) if (cur[lv][i] == seen[lv][k]) found = 1;
            if (!found) fprintf(stderr, "[ait] actor BAJA f=%u nivel=%d nodo=%08x act=%08x\n", f, lv, seen[lv][k], seen_act[lv][k]);
        }
        nseen[lv] = ncur[lv];
        for (int i = 0; i < ncur[lv]; i++) { seen[lv][i] = cur[lv][i]; seen_act[lv][i] = r32(cur[lv][i] + 8); }
    }
    first = 0;
    for (int lv = 0; lv < 9; lv++) for (int i = 0; i < ncur[lv]; i++) if (r32(cur[lv][i] + 8) == 0x800771acu) {   /* bala del arma: segmento (inicio +0x110, fin +0x118 / +0x120) */
        uint32_t n = cur[lv][i];
        fprintf(stderr, "[ait] bala f=%u nodo=%08x +110=(%d,%d,%d) +118=(%d,%d,%d) +120=(%d,%d,%d) +128=(%d,%d,%d) jugador=(%d,%d,%d)\n", f, n, r16(n + 0x110), r16(n + 0x112), r16(n + 0x114), r16(n + 0x118), r16(n + 0x11a), r16(n + 0x11c),
                r16(n + 0x120), r16(n + 0x122), r16(n + 0x124), r16(n + 0x128), r16(n + 0x12a), r16(n + 0x12c), r16(BA_PLAYER_POS), r16(BA_PLAYER_POS + 2), r16(BA_PLAYER_POS + 4));
    }
}
static void tdump_step(uint32_t f) {   /* blancos vivos del juego (64 x 72 B), una línea por blanco que cambió o es nuevo */
    static uint8_t prev[64][72];
    int n = (int)r32(0x800AC000u + 0x770u + 0u) ; (void)n;
    for (int i = 0; i < 64; i++) {
        const uint8_t *t = emu_ram + ((0x800B7660u + 72u * (uint32_t)i) & RAMM);
        uint16_t cls = *(const uint16_t *)t;
        if (cls == 0) { memset(prev[i], 0, 72); continue; }
        if (!memcmp(prev[i], t, 72)) continue;
        { const int16_t *hh = (const int16_t *)t; if ((hh[0] == 0xdf || hh[0] == 0x9f || hh[0] == 0x14) && hh[3] == 0 && hh[0x1F] == 0 && (hh[0x1F] == 0)) { memcpy(prev[i], t, 72); continue; } }
        memcpy(prev[i], t, 72);
        const int16_t *h = (const int16_t *)t;
        fprintf(stderr, "[ait] blanco f=%u #%d cls=%04x +2=%04x +4=%04x +6=%04x centro=(%d,%d,%d) mitad=(%d,%d,%d) +24=%04x +26=%d +28=%d +2a=%d +3c=%d +3e=%d +40=%d +42=%d +44=%08x\n", f, i, h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[8], h[9], h[10],
                h[0x12], h[0x13], h[0x14], h[0x15], h[0x1E], h[0x1F], h[0x20], h[0x21], *(const uint32_t *)(t + 0x44));
    }
}
static void chit_apply(const VrcCamera *c, int mode) {
    if (!c || !c->target) return;
    uint32_t t = c->target;
    fprintf(stderr, "[ait] chit cam=%08x modo=%d: antes clase=%04x +6=%04x +28=%d +3e=%d\n", c->actor, mode, (uint16_t)r16(t), (uint16_t)r16(t + 6), r16(t + 0x28), r16(t + 0x3e));
    w16(t + 6, (int16_t)(r16(t + 6) | 4));
    w16(t + 0x3e, (int16_t)mode);
    w16(t + 0x28, (int16_t)(r16(t + 0x28) + 20));
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
    vrc_scan(emu_ram, &MT);
    if (front_left > 0) { front_step(f); front_left--; }
    if (hold_left > 0) { place_player(hold_p[0], hold_p[1], hold_p[2]); hold_left--; }
    if (cf_left > 0) { const VrcCamera *cc = cam_at(cf_idx); if (cc) cam_place(cc, cf_d, cf_ang, cf_h); cf_left--; }
    if (sw_on) sweep_step(f);
    if (wa_left > 0) { watch_step(f); wa_left--; }
    if (scn_report && scn_left <= 0 && SCN.scenes) {
        const VrcCamera *cc = cam_at(scn_idx);
        double dd = 0;
        if (cc) { double dx = cc->pos[0] - r16(BA_PLAYER_POS), dy = cc->pos[1] - r16(BA_PLAYER_POS + 2), dz = cc->pos[2] - r16(BA_PLAYER_POS + 4); dd = sqrt(dx * dx + dy * dy + dz * dz); }
        fprintf(stderr, "[ait] scene cam=%d dist3d=%.0f R=%.2fm: escenas=%u con3D=%u triángulos3D(medio)=%.0f | triángulos sobre la cámara (z normal / z espejo): medio %.1f / %.1f, máx %u / %u\n", scn_idx, dd, scn_R, SCN.scenes, SCN.with3d,
                SCN.with3d ? SCN.tri_all / SCN.with3d : 0.0, SCN.with3d ? SCN.sum0 / SCN.with3d : 0.0, SCN.with3d ? SCN.sum1 / SCN.with3d : 0.0, SCN.max0, SCN.max1);
        scn_report = 0;
    }
    if (spu_left > 0) { spu_step(f); spu_left--; }
    if (scn_left > 0) scn_left--;
    atk_f = f;
    if (atk_left > 0) atk_left--;
    if (td_left > 0) { tdump_step(f); td_left--; }
    if (al_left > 0) { alist_step(f); al_left--; }
    for (int i = 0; i < nev; i++) {
        Ev *e = &EV[i];
        if (e->kind == K_PROBE) { if (f >= e->f && e->n > 0 && (f - e->f) % (uint32_t)e->every == 0) { probe(c, f); e->n--; e->u[7] = 1; } continue; }
        if (e->f != f) continue;
        switch (e->kind) {
        case K_POKE: { size_t n = strlen(e->s) / 2; for (size_t k = 0; k < n; k++) { unsigned v = 0; sscanf(e->s + 2 * k, "%2x", &v); emu_ram[((e->u[0] & RAMM) + k) & RAMM] = (uint8_t)v; } break; }
        case K_POKES: memcpy(emu_ram + (e->u[0] & RAMM), e->s, strlen(e->s) + 1); break;
        case K_GCALL: {
            Cpu save = *c;
            c->r[29] = 0x801FFF00u; c->r[4] = e->u[1]; c->r[5] = e->u[2]; c->r[6] = e->u[3]; c->r[7] = e->u[4];
            in_call = 1; rt_call(c, e->u[0]); in_call = 0;
            fprintf(stderr, "[ait] gcall f=%u %08x -> v0=0x%08x\n", f, e->u[0], c->r[2]);
            *c = save;
            break; }
        case K_STAGE: stage_req(c, e->s, e->n == 0 ? 1 : e->n == 2 ? 2 : e->n == 3 ? 3 : 0); break;
        case K_AI: mv_vrhost_set_ai((int)e->u[0], (int)e->u[1], (int)e->u[2]); fprintf(stderr, "[ait] f=%u IA: percepción=%u cámaras=%u actividad=%u\n", f, e->u[0], e->u[1], e->u[2]); break;
        case K_FRONT: front_left = e->n; front_d = e->d[0]; front_idx = (int)e->u[0]; break;
        case K_DUMP: { FILE *o = fopen(e->s, "wb"); if (o) { fwrite(emu_ram, 1, 0x200000, o); fclose(o); fprintf(stderr, "[ait] dump f=%u -> %s\n", f, e->s); } break; }
        case K_CFACE: ground_y = r16(BA_PLAYER_POS + 2); cf_left = e->n; cf_d = e->d[0]; cf_idx = (int)e->u[0]; cf_ang = e->d[1]; cf_h = e->d[2]; break;
        case K_CSWEEP: ground_y = r16(BA_PLAYER_POS + 2); sw_on = 1; sw_idx = (int)e->u[0]; sw_d = e->d[0]; sw_d1 = e->d[1]; sw_step = e->d[2]; sw_dwell = e->n; sw_t = 0; sw_hit = 0; cf_ang = 0; cf_h = 0; sw_f0 = f; break;
        case K_CHIT: chit_apply(cam_at((int)e->u[0]), e->n ? e->n : 1); break;
        case K_FACE: { const VrcCamera *cc = cam_at((int)e->u[0]); if (cc) { double dx = cc->pos[0] - r16(BA_PLAYER_POS), dz = cc->pos[2] - r16(BA_PLAYER_POS + 4); set_player_yaw((int)lrint(atan2(dx, dz) * 4096.0 / (2.0 * M_PI))); } break; }
        case K_SCENE: memset(&SCN, 0, sizeof SCN); scn_idx = (int)e->u[0]; scn_R = e->d[0] > 0 ? e->d[0] : 0.9; scn_left = e->n; scn_report = 1; break;
        case K_TDUMP: td_left = e->n; break;
        case K_CFWD: { const VrcCamera *cc = cam_at((int)e->u[0]);
            uint32_t ctl = r32(BA_PLAYER_CTRL);
            if (cc && ctl >= 0x80000000u && ctl < 0x80200000u) {
                double yaw = (r16(ctl + BC_ROT + 2) & 4095) * 2.0 * M_PI / 4096.0;
                int16_t np[3] = {(int16_t)lrint(r16(BA_PLAYER_POS) + e->d[0] * sin(yaw)), (int16_t)lrint(r16(BA_PLAYER_POS + 2) + e->d[1]), (int16_t)lrint(r16(BA_PLAYER_POS + 4) + e->d[0] * cos(yaw))};
                for (int q = 0; q < 3; q++) w16(cc->actor + 0x20u + 2u * q, np[q]);
                fprintf(stderr, "[ait] cfwd f=%u cam=%d -> (%d,%d,%d) (jugador yaw=%d)\n", f, (int)e->u[0], np[0], np[1], np[2], r16(ctl + BC_ROT + 2));
            } break; }
        case K_HEAD: head_yaw_deg = e->d[0]; break;
        case K_TRI: tri_left = e->n; tri_n = 0; tri_sum = 0; tri_min = 1e18; tri_max = 0; tri_yaw = head_yaw_deg; break;
        case K_ATK: if (!rt_patched(ATK_FN)) { int rr = rt_patch(ATK_FN, atk_wrap); fprintf(stderr, "[ait] parche de traza del ataque: %d\n", rr); } atk_left = e->n; break;
        case K_ALIST: al_left = e->n; break;
        case K_WATCH: wa_left = e->n; wa_idx = (int)e->u[0]; memset(wa_prev, 0xff, sizeof wa_prev); break;
        case K_SPUPROBE: spu_left = e->n; break;
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
        double hy = head_yaw_deg * M_PI / 180.0; float hc = (float)cos(hy), hs = (float)sin(hy);
        float m[16] = {hc, 0, -hs, 0, 0, 1, 0, 0, hs, 0, hc, 0, 0, 1.65f, 0, 1};
        mv_vr_set_head(m, 0.0);
        if (scn_left > 0) for (int k = 0; k < 4; k++) {
            VrScene sc;
            if (!mv_host_scene(&sc)) break;
            const VrcCamera *cc = cam_at(scn_idx);
            SCN.scenes++;
            if (!sc.has_3d || !sc.world_valid || !cc) continue;
            SCN.with3d++;
            float ex = (cc->pos[0] - sc.origin_game[0]) * sc.scale, ey = (cc->pos[1] - sc.origin_game[1]) * sc.scale, ez = (cc->pos[2] - sc.origin_game[2]) * sc.scale;
            uint32_t h0 = 0, h1 = 0, tall = 0;
            for (uint32_t bi = 0; bi < sc.nb; bi++) {
                const VrBatch *b = &sc.b[bi];
                if (b->kind > 1) continue;
                for (uint32_t j = 0; j + 2 < b->count; j += 3) {
                    const VrVtx *v = &sc.v[b->first + j];
                    tall++;
                    int in0 = 1, in1 = 1;
                    for (int q = 0; q < 3; q++) {
                        double dx = v[q].pos[0] - ex, dy = v[q].pos[1] - ey, dz0 = v[q].pos[2] - ez, dz1 = v[q].pos[2] + ez;
                        if (sqrt(dx * dx + dy * dy + dz0 * dz0) > scn_R) in0 = 0;
                        if (sqrt(dx * dx + dy * dy + dz1 * dz1) > scn_R) in1 = 0;
                    }
                    h0 += in0; h1 += in1;
                }
            }
            if (getenv("SCN_DBG") && (SCN.with3d % 10) == 1) {
                double m0 = 1e9, m1 = 1e9; float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
                for (uint32_t bi = 0; bi < sc.nb; bi++) { const VrBatch *b = &sc.b[bi]; if (b->kind > 1) continue;
                    for (uint32_t j = 0; j < b->count; j++) { const VrVtx *v = &sc.v[b->first + j];
                        double dx = v->pos[0] - ex, dy = v->pos[1] - ey, d0 = sqrt(dx * dx + dy * dy + (v->pos[2] - ez) * (v->pos[2] - ez)), d1 = sqrt(dx * dx + dy * dy + (v->pos[2] + ez) * (v->pos[2] + ez));
                        if (d0 < m0) m0 = d0; if (d1 < m1) m1 = d1;
                        for (int q = 0; q < 3; q++) { if (v->pos[q] < lo[q]) lo[q] = v->pos[q]; if (v->pos[q] > hi[q]) hi[q] = v->pos[q]; } } }
                fprintf(stderr, "[ait] scene-dbg esperado=(%.2f,%.2f,%.2f) origen=(%.0f,%.0f,%.0f) ojo=(%.2f,%.2f,%.2f) escala=%.4f vértice más cercano z/-z: %.2f / %.2f m; caja 3D=(%.1f..%.1f, %.1f..%.1f, %.1f..%.1f)\n", ex, ey, ez, sc.origin_game[0], sc.origin_game[1], sc.origin_game[2], sc.eye_vr[0], sc.eye_vr[1], sc.eye_vr[2], sc.scale, m0, m1, lo[0], hi[0], lo[1], hi[1], lo[2], hi[2]);
            }
            SCN.tri_all += tall; SCN.sum0 += h0; SCN.sum1 += h1;
            if (h0 > SCN.max0) SCN.max0 = h0;
            if (h1 > SCN.max1) SCN.max1 = h1;
        }
        if (tri_left > 0) {
            VrScene sc;
            if (mv_host_scene(&sc) && sc.has_3d) {
                double t = 0;
                for (uint32_t bi = 0; bi < sc.nb; bi++) if (sc.b[bi].kind <= 1) t += sc.b[bi].count / 3;
                tri_sum += t; tri_n++; if (t < tri_min) tri_min = t; if (t > tri_max) tri_max = t;
            }
            if (--tri_left == 0) fprintf(stderr, "[ait] tri f=%u yaw=%.0f escenas=%d triangulos3D medio=%.0f min=%.0f max=%.0f\n", f, tri_yaw, tri_n, tri_n ? tri_sum / tri_n : 0.0, tri_n ? tri_min : 0.0, tri_max);
        }
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
    { uint32_t ps, pf; vrcam_pack_stats(&ps, &pf); fprintf(stderr, "[ait] paquetes (HLE de la etapa de polígono): vistos=%u re-emitidos=%u\n", ps, pf); }
    fprintf(stderr, "[ait] fin f=%u\n", mv_host_frame_count());
    return 0;
}
