/* bridge_dump: ejecuta el EXE recompilado (runtime) y, por fotograma, imprime el estado leído por el puente.
 *   bridge_dump <exe> [frames] [--disc RUTAS] [--card MCD] [--pad GUION] [--pad-every N:BOTON]
 *                  [--pad-until F] [--from F] [--every N] [--full] [--ram F:FICHERO]
 *   bridge_dump --ram-file FICHERO [--full]      (offline: lee un volcado de 2 MB, sin runtime)
 *  - Imprime una línea cada N fotogramas (por defecto 1) a partir de F: modo, zona, posición, rotación, vida, equipo,
 *    cámara, alerta, nº de actores/controles/blancos/enemigos y los grupos válidos.
 *  - --full: imprime además listas (controles, enemigos, blancos y actores).
 *  - --ram F:FICHERO: guarda la RAM (2 MB) en el fotograma F (repetible), para análisis y pruebas offline.
 *  - El guion de mando es el mismo que el de runtime/headless.c (botones activo-alto; sticks a 0x80).
 *    --pad-until F: los eventos --pad-every dejan de aplicarse desde el fotograma F (los --pad puntuales siguen).
 * Nunca escribe en la RAM del juego. */
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include "bridge.h"
#include "cdrom.h"
#include "cpu.h"
#include "gpu.h"
#include "hle.h"
#include "sys.h"

extern jmp_buf rt_fatal_jmp;
extern int rt_fatal_armed;

static uint32_t max_frames = 600, from_frame, every = 1, pad_until = 0xFFFFFFFFu;
static int full;
static uint32_t ram_f[32];
static const char *ram_n[32];
static int n_ram;

/* ---- guion de mando (igual que headless.c) ---- */
typedef struct { uint32_t start, dur, every; uint16_t mask; } PadEv;
static PadEv pad_ev[256];
static int npad_ev;
static const struct { const char *n; int bit; } pad_names[] = {
    {"select", 0}, {"l3", 1}, {"r3", 2}, {"start", 3}, {"up", 4}, {"right", 5}, {"down", 6}, {"left", 7},
    {"l2", 8}, {"r2", 9}, {"l1", 10}, {"r1", 11}, {"triangle", 12}, {"circle", 13}, {"cross", 14}, {"square", 15}};

static uint16_t pad_mask(const char *s, size_t len) {
    uint16_t m = 0;
    while (len) {
        size_t k = 0;
        while (k < len && s[k] != '+') k++;
        int ok = 0;
        for (size_t i = 0; i < sizeof pad_names / sizeof *pad_names; i++)
            if (strlen(pad_names[i].n) == k && !strncasecmp(s, pad_names[i].n, k)) { m |= (uint16_t)(1u << pad_names[i].bit); ok = 1; }
        if (!ok) return 0;
        s += k; len -= k;
        if (len) { s++; len--; }
    }
    return m;
}
static int pad_add_script(const char *g) {
    char *dup = strdup(g), *sv = NULL;
    for (char *tok = strtok_r(dup, ",", &sv); tok; tok = strtok_r(NULL, ",", &sv)) {
        char *e;
        uint32_t fr = (uint32_t)strtoul(tok, &e, 0);
        if (*e != ':' || npad_ev >= 256) { free(dup); return -1; }
        char *b = e + 1, *c2 = strchr(b, ':');
        uint32_t dur = 6;
        if (c2) dur = (uint32_t)strtoul(c2 + 1, 0, 0);
        uint16_t m = pad_mask(b, c2 ? (size_t)(c2 - b) : strlen(b));
        if (!m) { free(dup); return -1; }
        pad_ev[npad_ev++] = (PadEv){fr, dur, 0, m};
    }
    free(dup);
    return 0;
}
static int pad_add_every(const char *a) {
    char *e;
    uint32_t n = (uint32_t)strtoul(a, &e, 0);
    if (*e != ':' || !n || npad_ev >= 256) return -1;
    uint16_t m = pad_mask(e + 1, strlen(e + 1));
    if (!m) return -1;
    pad_ev[npad_ev++] = (PadEv){0, 6, n, m};
    return 0;
}
static void script_pad_frame(uint32_t f) {
    static uint16_t last = 0xFFFF;
    uint16_t b = 0;
    for (int i = 0; i < npad_ev; i++) {
        const PadEv *p = &pad_ev[i];
        if (p->every) { if (f < pad_until && f >= p->every && f % p->every < p->dur) b |= p->mask; }
        else if (f >= p->start && f < p->start + p->dur) b |= p->mask;
    }
    if (b != last) { pad_set_state(0, b, 0x80, 0x80, 0x80, 0x80); last = b; }
}

/* ---- impresión ---- */
static BridgeState st;  /* grande: fuera de la pila */

static void print_header(void) {
    printf("frame modo   zona  | pos(x,y,z) | rot_y | vida/max | arma item mag | stance estado | alerta | act ctl blq ene | valid\n");
}

static void print_state(uint32_t f, const BridgeState *s) {
    const uint8_t *r = NULL; (void)r;
    printf("%5u %-8s %-5s | %6d %6d %6d | %5d | %4d/%-4d | w%-2d i%-2d m%-2d | st%d 0x%08x%s%s%s%s%s | a%d/%d | %3d %3d %3d %3d | %04x\n",
           f, bridge_mode_name(s), s->stage_code, s->player_pos.x, s->player_pos.y, s->player_pos.z, s->ctl_rot.y,
           s->life, s->life_max, s->weapon, s->item, s->magazine, s->stance, s->player_status,
           s->crouching ? " AGACHADO" : "", s->prone ? " TUMBADO" : "", s->on_wall ? " PARED" : "",
           s->first_person ? " 1P" : "", s->in_box ? " CAJA" : "", s->alert_mode, s->alert_level, s->n_actors,
           s->n_controls, s->n_targets, s->n_enemies, s->valid);
}

static void print_lists(const BridgeState *s) {
    printf("  cam eye=(%d,%d,%d) focal=%d  eje_z=(%d,%d,%d)  juego pos=(%d,%d,%d) obj=(%d,%d,%d) zoom=%d 1P=%d tipo=%d\n",
           s->eye_pos[0], s->eye_pos[1], s->eye_pos[2], s->eye_focal, s->eye_axes[0][2], s->eye_axes[1][2],
           s->eye_axes[2][2], s->cam_pos.x, s->cam_pos.y, s->cam_pos.z, s->cam_target.x, s->cam_target.y,
           s->cam_target.z, s->cam_zoom, s->cam_first_person, s->cam_type);
    printf("  gs=0x%08x pausa=%d menu=%d  ctl=%08x pos=(%d,%d,%d) alt=%d\n", s->game_status, s->pause_level,
           s->menu_state, s->ctl_addr, s->ctl_pos.x, s->ctl_pos.y, s->ctl_pos.z, s->ctl_height);
    for (int i = 0; i < s->n_controls; i++) {
        const BridgeControl *c = &s->controls[i];
        printf("  ctl[%d] %08x id=%04x pos=(%d,%d,%d) rot=(%d,%d,%d) radar=%04x est=%d actor='%s'%s\n", i, c->addr,
               c->name_id, c->pos.x, c->pos.y, c->pos.z, c->rot.x, c->rot.y, c->rot.z, c->radar, c->alert_state,
               c->actor >= 0 ? s->actors[c->actor].name : "", c->is_player ? " (jugador)" : "");
    }
    for (int i = 0; i < s->n_enemies; i++) {
        const BridgeEnemy *e = &s->enemies[i];
        printf("  ene[%d] ctl=%d blq=%d pos=(%d,%d,%d) rot_y=%d est=%d cono=%d vital=%d faint=%d%s\n", i, e->control,
               e->target, e->pos.x, e->pos.y, e->pos.z, e->rot.y, e->alert_state, e->has_cone, e->vital, e->faint,
               e->down ? " CAIDO" : "");
    }
    for (int i = 0; i < s->n_targets; i++) {
        const BridgeTarget *t = &s->targets[i];
        printf("  blq[%d] %08x cls=%04x lado=%d mapa=%04x c=(%d,%d,%d) vital=%d dano=%d faint=%d\n", i, t->addr, t->cls,
               t->side, t->map, t->center.x, t->center.y, t->center.z, t->vital, t->damage, t->faint);
    }
    for (int i = 0; i < s->n_actors; i++) {
        const BridgeActor *a = &s->actors[i];
        printf("  act[%d] L%d %08x act=%08x die=%08x '%s'\n", i, a->level, a->addr, a->act, a->die, a->name);
    }
}

static void on_frame(Cpu *c, uint32_t f) {
    (void)c;
    alarm(10);
    if (npad_ev) script_pad_frame(f);
    for (int i = 0; i < n_ram; i++)
        if (ram_f[i] == f) {
            FILE *o = fopen(ram_n[i], "wb");
            if (o) { fwrite(psx_ram, 1, PSX_RAM_SIZE, o); fclose(o); }
        }
    if (f >= from_frame && (f - from_frame) % every == 0) {
        bridge_read(psx_ram, &st);
        print_state(f, &st);
        if (full) print_lists(&st);
    }
    if (f >= max_frames) {
        fprintf(stderr, "[bridge_dump] OK: %u frames\n", f);
        exit(0);
    }
}
static void on_alarm(int s) { (void)s; rt_fatal(NULL, "sin VBlank en 10 s"); }
static uint32_t rd32f(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

int main(int argc, char **argv) {
    const char *exe = NULL, *disc = getenv("MV_DISC"), *card = getenv("MV_CARD"), *ramfile = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--disc") && i + 1 < argc) disc = argv[++i];
        else if (!strcmp(argv[i], "--card") && i + 1 < argc) card = argv[++i];
        else if (!strcmp(argv[i], "--pad") && i + 1 < argc) {
            if (pad_add_script(argv[++i])) { fprintf(stderr, "guion --pad inválido: '%s'\n", argv[i]); return 1; }
        } else if (!strcmp(argv[i], "--pad-every") && i + 1 < argc) {
            if (pad_add_every(argv[++i])) { fprintf(stderr, "--pad-every inválido: '%s'\n", argv[i]); return 1; }
        } else if (!strcmp(argv[i], "--from") && i + 1 < argc) from_frame = (uint32_t)strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--every") && i + 1 < argc) every = (uint32_t)strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--pad-until") && i + 1 < argc) pad_until = (uint32_t)strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--full")) full = 1;
        else if (!strcmp(argv[i], "--ram-file") && i + 1 < argc) ramfile = argv[++i];
        else if (!strcmp(argv[i], "--ram") && i + 1 < argc && n_ram < 32) {
            char *e;
            ram_f[n_ram] = (uint32_t)strtoul(argv[++i], &e, 0);
            if (*e != ':') { fprintf(stderr, "--ram F:FICHERO\n"); return 1; }
            ram_n[n_ram++] = e + 1;
        } else if (!exe) exe = argv[i];
        else max_frames = (uint32_t)strtoul(argv[i], 0, 0);
    }
    if (!every) every = 1;

    if (ramfile) { /* modo offline */
        static uint8_t ram[BRIDGE_RAM_SIZE];
        FILE *f = fopen(ramfile, "rb");
        if (!f || fread(ram, 1, sizeof ram, f) != sizeof ram) { fprintf(stderr, "no se pudo leer %s (2 MB)\n", ramfile); return 1; }
        fclose(f);
        bridge_read(ram, &st);
        print_header();
        print_state(0, &st);
        print_lists(&st);
        return 0;
    }
    if (!exe) {
        fprintf(stderr, "uso: %s <exe> [frames] [--disc R] [--card MCD] [--pad G] [--pad-every N:B] [--from F] [--every N] [--full] [--ram F:FICH]\n"
                        "     %s --ram-file FICHERO\n", argv[0], argv[0]);
        return 1;
    }
    FILE *f = fopen(exe, "rb");
    if (!f) { perror(exe); return 1; }
    static uint8_t buf[PSX_RAM_SIZE + 0x800];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    if (n < 0x800 || memcmp(buf, "PS-X EXE", 8)) { fprintf(stderr, "no es PS-X EXE\n"); return 1; }
    uint32_t pc = rd32f(buf + 0x10), gp = rd32f(buf + 0x14), t = rd32f(buf + 0x18), sz = rd32f(buf + 0x1C);
    if (sz > n - 0x800 || (t & 0x1FFFFF) + sz > PSX_RAM_SIZE) { fprintf(stderr, "cabecera inválida\n"); return 1; }
    memcpy(&psx_ram[t & 0x1FFFFF], buf + 0x800, sz);
    if (disc) {
        int nd = cd_open(disc);
        fprintf(stderr, "[bridge_dump] disco(s) montado(s): %d\n", nd);
        if (!nd) return 1;
    }
    if (card && card_open(0, card)) fprintf(stderr, "[bridge_dump] aviso: no se pudo abrir la tarjeta '%s'\n", card);

    Cpu c = {0};
    c.r[28] = gp;
    c.r[29] = c.r[30] = 0x801FFFF0u;
    c.r[31] = 0;
    gte_reset();
    gpu_reset();
    rt_host_frame = on_frame;
    signal(SIGALRM, on_alarm);
    rt_fatal_armed = 1;
    print_header();
    if (setjmp(rt_fatal_jmp)) return 2;
    alarm(10);
    rt_call(&c, pc);
    fprintf(stderr, "[bridge_dump] la entrada retornó tras %u frames\n", hle_frame_count());
    return 0;
}
