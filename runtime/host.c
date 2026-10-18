/* Host de ventana plana: ver host.h. El hilo de emulación ejecuta el punto de entrada recompilado;
 * el gancho de VBlank (rt_host_frame) publica el frame, aplica el mando, marca el ritmo y atiende
 * la pausa. Todo el estado del runtime vive en ese hilo; el resto de hilos solo usan las funciones
 * de este fichero (mutex) y spu_pull (mutex propio de la SPU). */
#include "host.h"
#include <errno.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "cdrom.h"
#include "cpu.h"
#include "gpu.h"
#include "hle.h"
#include "hlebind.h"
#include "spu.h"
#include "sys.h"

extern jmp_buf rt_fatal_jmp;
extern int rt_fatal_armed;

/* Punto de enganche para imágenes BIN/CUE: cuando cdrom.c sepa montar una imagen (vía disc_img.h) debe
 * definir `int cd_open_image(const char *path)` (>0 = disco montado). Mientras no exista (símbolo débil
 * nulo), las entradas que no son directorios se ignoran. */
extern int cd_open_image(const char *path) __attribute__((weak));

#define FRAME_NS 20000000L   /* VBlank PAL virtual: 50 Hz */

static pthread_mutex_t MX = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t CV = PTHREAD_COND_INITIALIZER;
static MvHostState state = MV_HOST_IDLE;
static int started;
static char errmsg[512];

/* Doble búfer de vídeo: el hilo de emulación convierte en fb[back] sin bloquear y publica con el mutex. */
static uint8_t *fb[2];
static int fb_front, fb_w, fb_h;
static uint32_t fb_seq;

static struct { uint16_t buttons; uint8_t lx, ly, rx, ry; } pad_req = {0, 0x80, 0x80, 0x80, 0x80};
static _Atomic uint16_t rumble_pack;
static _Atomic int turbo;
static _Atomic uint32_t frames;

static char exe_path[1024];
static char sigs_path[1024];   /* firmas del SDK (solo modo sin código generado, MV_NOGEN) */
static pthread_t thr;

static void set_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#include <stdarg.h>
static void set_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    pthread_mutex_lock(&MX);
    vsnprintf(errmsg, sizeof errmsg, fmt, ap);
    pthread_mutex_unlock(&MX);
    va_end(ap);
}

/* ---- Vídeo ---- */

static void publish_frame(void) {
    GpuDisplay d;
    gpu_get_display(&d);
    int w = d.w, h = d.h;
    if (w <= 0 || h <= 0) { w = 320; h = 240; d.enabled = 0; }
    if (w > MV_HOST_MAX_W) w = MV_HOST_MAX_W;
    if (h > MV_HOST_MAX_H) h = MV_HOST_MAX_H;
    int back = !fb_front;   /* solo este hilo escribe fb_front */
    uint8_t *o = fb[back];
    if (!d.enabled) {
        memset(o, 0, (size_t)w * h * 4);
        for (size_t i = 3; i < (size_t)w * h * 4; i += 4) o[i] = 255;
    } else {
        int d24 = d.depth24;
        for (int y = 0; y < h; y++) {
            uint8_t *p = o + (size_t)y * w * 4;
            const uint16_t *row = gpu_vram[(d.y + y) & 511];
            if (d24) {
                const uint8_t *ln = (const uint8_t *)row;
                for (int x = 0; x < w; x++, p += 4) {
                    p[0] = ln[((d.x * 2) + x * 3 + 0) & 2047];
                    p[1] = ln[((d.x * 2) + x * 3 + 1) & 2047];
                    p[2] = ln[((d.x * 2) + x * 3 + 2) & 2047];
                    p[3] = 255;
                }
            } else {
                for (int x = 0; x < w; x++, p += 4) {
                    uint16_t c = row[(d.x + x) & 1023];
                    unsigned r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
                    p[0] = (uint8_t)(r << 3 | r >> 2);
                    p[1] = (uint8_t)(g << 3 | g >> 2);
                    p[2] = (uint8_t)(b << 3 | b >> 2);
                    p[3] = 255;
                }
            }
        }
    }
    pthread_mutex_lock(&MX);
    fb_front = back;
    fb_w = w; fb_h = h;
    fb_seq++;
    pthread_mutex_unlock(&MX);
}

uint32_t mv_host_framebuffer(uint8_t *dst, size_t dst_size, int *w, int *h, uint32_t last_seq) {
    uint32_t seq = 0;
    pthread_mutex_lock(&MX);
    if (fb[0] && fb_seq && fb_seq != last_seq && dst_size >= (size_t)fb_w * fb_h * 4) {
        memcpy(dst, fb[fb_front], (size_t)fb_w * fb_h * 4);
        if (w) *w = fb_w;
        if (h) *h = fb_h;
        seq = fb_seq;
    }
    pthread_mutex_unlock(&MX);
    return seq;
}

/* ---- Ritmo ---- */

static void ts_add(struct timespec *t, long ns) {
    t->tv_nsec += ns;
    while (t->tv_nsec >= 1000000000L) { t->tv_nsec -= 1000000000L; t->tv_sec++; }
}
static long ts_diff_ns(const struct timespec *a, const struct timespec *b) { /* a - b */
    return (long)(a->tv_sec - b->tv_sec) * 1000000000L + (a->tv_nsec - b->tv_nsec);
}

static struct timespec next_deadline;
static int have_deadline;

static void pace(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (atomic_load(&turbo)) { have_deadline = 0; return; }
    if (!have_deadline || ts_diff_ns(&now, &next_deadline) > 100000000L) { next_deadline = now; have_deadline = 1; }
    ts_add(&next_deadline, FRAME_NS);
    while (ts_diff_ns(&next_deadline, &now) > 0) {
        struct timespec t = next_deadline;
        /* sin clock_nanosleep absoluto (macOS): dormir la diferencia */
        long rem = ts_diff_ns(&t, &now);
        struct timespec s = {rem / 1000000000L, rem % 1000000000L};
        if (nanosleep(&s, NULL) && errno != EINTR) break;
        clock_gettime(CLOCK_MONOTONIC, &now);
    }
}

/* ---- Gancho de VBlank (hilo de emulación) ---- */

static void on_frame(Cpu *c, uint32_t f) {
    (void)c; (void)f;
    atomic_store(&frames, f);
    pthread_mutex_lock(&MX);
    uint16_t b = pad_req.buttons;
    uint8_t lx = pad_req.lx, ly = pad_req.ly, rx = pad_req.rx, ry = pad_req.ry;
    pthread_mutex_unlock(&MX);
    pad_set_state(0, b, lx, ly, rx, ry);

    publish_frame();

    uint8_t sm, lg;
    pad_get_rumble(0, &sm, &lg);
    atomic_store(&rumble_pack, (uint16_t)(sm << 8 | lg));

    pthread_mutex_lock(&MX);
    int was_paused = 0;
    while (state == MV_HOST_PAUSED) { was_paused = 1; pthread_cond_wait(&CV, &MX); }
    pthread_mutex_unlock(&MX);
    if (was_paused) have_deadline = 0;
    pace();
}

static void *emu_main(void *arg) {
    (void)arg;
    FILE *f = fopen(exe_path, "rb");
    static uint8_t buf[PSX_RAM_SIZE + 0x800];
    size_t n = f ? fread(buf, 1, sizeof buf, f) : 0;
    if (f) fclose(f);
    uint32_t pc = 0, gp = 0;
    {   /* validación ya hecha en mv_host_start; aquí solo se cargan los datos */
        uint32_t t, sz;
        memcpy(&pc, buf + 0x10, 4); memcpy(&gp, buf + 0x14, 4);
        memcpy(&t, buf + 0x18, 4); memcpy(&sz, buf + 0x1C, 4);
        memcpy(&psx_ram[t & 0x1FFFFF], buf + 0x800, sz);
        (void)n;
    }
#ifdef MV_NOGEN
    {   /* sin código generado: el punto de entrada es el de la cabecera y las HLE se enlazan por firmas */
        uint32_t t, sz;
        memcpy(&t, buf + 0x18, 4); memcpy(&sz, buf + 0x1C, 4);
        rt_entry = pc;
        if (hle_bind_file(sigs_path, t, sz, pc) < 0) {
            set_error("No se pudo leer el fichero de firmas del SDK.");
            pthread_mutex_lock(&MX); state = MV_HOST_ERROR; pthread_mutex_unlock(&MX);
            return NULL;
        }
        if (hle_bind_missing()) fprintf(stderr, "[host] aviso: %u funciones HLE sin enlazar: el juego puede no funcionar\n", hle_bind_missing());
    }
#else
    if (pc != rt_entry) fprintf(stderr, "[host] aviso: pc 0x%08x != entrada recompilada 0x%08x\n", pc, rt_entry);
#endif

    Cpu c = {0};
    c.r[28] = gp;
    c.r[29] = c.r[30] = 0x801FFFF0u;
    c.r[31] = 0;
    gte_reset();
    gpu_reset();
    rt_host_frame = on_frame;
    rt_fatal_armed = 1;
    if (setjmp(rt_fatal_jmp)) {
        set_error("El motor se detuvo por un error interno. Consulta el registro en Documents.");
        pthread_mutex_lock(&MX);
        state = MV_HOST_ERROR;
        pthread_mutex_unlock(&MX);
        return NULL;
    }
    rt_call(&c, pc);
    fprintf(stderr, "[host] la entrada retornó (v0=0x%08x) tras %u frames\n", c.r[2], hle_frame_count());
    set_error("El juego terminó la ejecución.");
    pthread_mutex_lock(&MX);
    state = MV_HOST_ERROR;
    pthread_mutex_unlock(&MX);
    return NULL;
}

/* ---- API ---- */

/* Monta cada entrada de la lista: directorio -> cd_open; fichero -> cd_open_image si existe. */
static int mount_discs(const char *list) {
    char *dup = strdup(list);
    int images = 0;
    if (!dup) return 0;
    char *sv = NULL;
    for (char *e = strtok_r(dup, ";", &sv); e; e = strtok_r(NULL, ";", &sv)) {
        struct stat st;
        if (!*e || stat(e, &st)) { fprintf(stderr, "[host] disco no accesible: '%s'\n", e); continue; }
        if (S_ISDIR(st.st_mode)) cd_open(e);
        else if (cd_open_image) { if (cd_open_image(e) > 0) images++; }
        else fprintf(stderr, "[host] imagen de disco no soportada todavía: '%s'\n", e);
    }
    free(dup);
    return cd_disc_count() + images;
}

static int start_impl(const char *exe, const char *disc_dir, const char *card_dir);

void mv_host_set_sigs(const char *path) { snprintf(sigs_path, sizeof sigs_path, "%s", path ? path : ""); }

int mv_host_start(const char *exe, const char *disc_dir, const char *card_dir) {
    int r = start_impl(exe, disc_dir, card_dir);
    /* Fallos de validación (ejecutable/disco): no se tocó el runtime y se puede reintentar. */
    if (r == -1 || r == -3) { pthread_mutex_lock(&MX); started = 0; pthread_mutex_unlock(&MX); }
    return r;
}

static int start_impl(const char *exe, const char *disc_dir, const char *card_dir) {
    pthread_mutex_lock(&MX);
    if (started) { pthread_mutex_unlock(&MX); return -2; }
    started = 1;
    errmsg[0] = 0;
    pthread_mutex_unlock(&MX);

    if (!exe || !*exe) { set_error("Falta el ejecutable del juego."); return -1; }
    FILE *f = fopen(exe, "rb");
    if (!f) { set_error("No se puede abrir el ejecutable del juego: %s", strerror(errno)); return -1; }
    static uint8_t hdr[0x800];
    size_t n = fread(hdr, 1, sizeof hdr, f);
    fseek(f, 0, SEEK_END);
    long total = ftell(f);
    fclose(f);
    if (n < 0x800 || memcmp(hdr, "PS-X EXE", 8)) { set_error("El ejecutable del juego no es válido."); return -1; }
    uint32_t t, sz;
    memcpy(&t, hdr + 0x18, 4); memcpy(&sz, hdr + 0x1C, 4);
    if (total < 0 || sz > (uint32_t)total - 0x800 || (t & 0x1FFFFF) + sz > PSX_RAM_SIZE ||
        sz > PSX_RAM_SIZE) { set_error("La cabecera del ejecutable del juego es inválida."); return -1; }

#ifdef MV_NOGEN
    {
        const char *sp = sigs_path[0] ? sigs_path : getenv("MV_SIGS");
        FILE *sf = sp && *sp ? fopen(sp, "rb") : NULL;
        if (!sf) { set_error("Falta el fichero de firmas del SDK."); return -1; }
        fclose(sf);
        snprintf(sigs_path, sizeof sigs_path, "%s", sp);
    }
#endif
    if (!disc_dir || !*disc_dir) { set_error("No se encontró ningún disco."); return -3; }
    if (mount_discs(disc_dir) <= 0) { set_error("No se pudo montar ningún disco."); return -3; }

    if (card_dir && *card_dir) {
        char p[1100];
        snprintf(p, sizeof p, "%s/card0.mcd", card_dir);
        if (card_open(0, p)) fprintf(stderr, "[host] aviso: no se pudo abrir la tarjeta '%s'\n", p);
    }

    fb[0] = malloc(MV_HOST_FB_BYTES);
    fb[1] = malloc(MV_HOST_FB_BYTES);
    if (!fb[0] || !fb[1]) { set_error("Sin memoria."); return -4; }
    memset(fb[0], 0, MV_HOST_FB_BYTES);
    memset(fb[1], 0, MV_HOST_FB_BYTES);
    snprintf(exe_path, sizeof exe_path, "%s", exe);

    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 64u << 20);   /* el código recompilado anida llamadas C */
    pthread_mutex_lock(&MX);
    state = MV_HOST_RUNNING;
    pthread_mutex_unlock(&MX);
    int r = pthread_create(&thr, &at, emu_main, NULL);
    pthread_attr_destroy(&at);
    if (r) {
        set_error("No se pudo crear el hilo de emulación.");
        pthread_mutex_lock(&MX);
        state = MV_HOST_ERROR;
        pthread_mutex_unlock(&MX);
        return -5;
    }
    pthread_detach(thr);
    return 0;
}

int mv_host_has_disc_img(void) { return cd_open_image != NULL; }

MvHostState mv_host_state(void) {
    pthread_mutex_lock(&MX);
    MvHostState s = state;
    pthread_mutex_unlock(&MX);
    return s;
}

const char *mv_host_error(void) {
    static char copy[512];
    pthread_mutex_lock(&MX);
    memcpy(copy, errmsg, sizeof copy);
    pthread_mutex_unlock(&MX);
    return copy;
}

uint32_t mv_host_frame_count(void) { return atomic_load(&frames); }

int mv_host_audio_pull(int16_t *out, int n) {
    if (!out || n <= 0) return 0;
    if (mv_host_state() != MV_HOST_RUNNING) { memset(out, 0, (size_t)n * 4); return 0; }
    return spu_pull(out, n);
}

void mv_host_pad(uint16_t buttons, uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry) {
    pthread_mutex_lock(&MX);
    pad_req.buttons = buttons; pad_req.lx = lx; pad_req.ly = ly; pad_req.rx = rx; pad_req.ry = ry;
    pthread_mutex_unlock(&MX);
}

void mv_host_rumble(uint8_t *small, uint8_t *large) {
    uint16_t v = atomic_load(&rumble_pack);
    if (small) *small = (uint8_t)(v >> 8);
    if (large) *large = (uint8_t)v;
}

void mv_host_pause(void) {
    pthread_mutex_lock(&MX);
    if (state == MV_HOST_RUNNING) state = MV_HOST_PAUSED;
    pthread_mutex_unlock(&MX);
}

void mv_host_resume(void) {
    pthread_mutex_lock(&MX);
    if (state == MV_HOST_PAUSED) { state = MV_HOST_RUNNING; pthread_cond_broadcast(&CV); }
    pthread_mutex_unlock(&MX);
}

void mv_host_set_turbo(int on) { atomic_store(&turbo, on != 0); }
