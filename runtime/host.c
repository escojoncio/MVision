/* Host de ventana plana: ver host.h. El hilo de emulación ejecuta el punto de entrada recompilado;
 * el gancho de VBlank (rt_host_frame) publica el frame, aplica el mando, marca el ritmo y atiende
 * la pausa. Todo el estado del runtime vive en ese hilo.
 * Regla de hilos: el hilo de audio del sistema (mv_host_audio_pull) y el de render (mv_host_framebuffer) NO toman ningún
 * candado que tome el hilo de emulación: audio = anillo SPSC de la SPU + atómicos; vídeo = triple búfer por intercambio de
 * índices; mando/estado = atómicos. El único mutex (MX) protege el texto de error y la espera de la pausa (cond. var.).
 * Un hilo de vigilancia (mon_main) escribe el latido del registro cada 5 s y, si la emulación deja de avanzar 3 s, vuelca
 * el estado (PC, ra, IRQ, últimos bloques ejecutados). */
#include "host.h"
#include "host_hooks.h"
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
#include "disc_img.h"
#include "cpu.h"
#include "gpu.h"
#include "hle.h"
#include "hlebind.h"
#include "spu.h"
#include "sys.h"

extern jmp_buf rt_fatal_jmp;
extern int rt_fatal_armed;

static MvHostHooks hooks;
static uint64_t mono_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
void mv_host_set_hooks(const MvHostHooks *h) { if (h) hooks = *h; else memset(&hooks, 0, sizeof hooks); }

#define FRAME_NS 20000000L   /* VBlank PAL virtual: 50 Hz */

static pthread_mutex_t MX = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t CV = PTHREAD_COND_INITIALIZER;
static _Atomic int state = MV_HOST_IDLE;   /* escrituras bajo MX (para la variable de condición); lecturas sin candado */
static int started;
static char errmsg[512];

/* Triple búfer de vídeo sin candados. El escritor (hilo de emulación) es dueño de fb[fb_wr], el lector de fb[fb_rd] y el
 * tercero está en fb_state (bits 0-1 = índice listo, bit 2 = "nuevo"). Publicar = intercambiar mi índice por el listo; leer =
 * (si hay nuevo) intercambiar el mío por el listo. Nadie escribe un búfer que otro lee, así que la copia de ~2 MB no retiene
 * nada. fb_rmx solo serializa lectores entre sí (nunca lo toma el hilo de emulación). */
static uint8_t *fb[3];
static struct { int w, h; uint32_t seq; } fb_meta[3];
static _Atomic unsigned fb_state = 1;
static int fb_wr = 0, fb_rd = 2;
static uint32_t fb_seq;                       /* solo el hilo de emulación */
static pthread_mutex_t fb_rmx = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t fb_read_ns;           /* última lectura del vídeo (para no convertir si nadie mira) */

/* Mando 1 empaquetado: botones (16) | lx | ly | rx | ry */
static _Atomic uint64_t pad_req = ((uint64_t)0x80 << 16) | ((uint64_t)0x80 << 24) | ((uint64_t)0x80 << 32) | ((uint64_t)0x80 << 40);
static _Atomic uint16_t rumble_pack;
static _Atomic int turbo;
static _Atomic uint32_t frames;

static char exe_path[1024];
static char sigs_path[1024];   /* firmas del SDK (solo modo sin código generado, MV_NOGEN) */
static pthread_t thr, mon_thr;
static _Atomic int mon_stop;
static Cpu *wd_cpu;     /* registros del hilo de emulación (solo lectura de diagnóstico desde el vigilante) */
/* Medidas del hilo de emulación para el latido */
static _Atomic uint64_t st_audio_calls, st_audio_frames, st_audio_max_ns, st_render[2];
static _Atomic uint64_t st_work_ns, st_work_n, st_work_max_ns, st_wait_ns, st_stall_fallbacks, st_pause_ns;

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
    /* Marco completo del modo (ancho nominal con los márgenes negros del rango horizontal), no solo el área de VRAM activa:
     * el hardware no muestra nada fuera del rango, y el contenido queda donde lo dejó el juego (ver GpuDisplay). */
    int w = d.fw > 0 ? d.fw : d.w, h = d.h;
    if (w <= 0 || h <= 0) { w = 320; h = 240; d.enabled = 0; d.fw = w; d.aw = w; d.h = h; d.pad_l = 0; }
    if (w > MV_HOST_MAX_W) w = MV_HOST_MAX_W;
    if (h > MV_HOST_MAX_H) h = MV_HOST_MAX_H;
    d.fw = w; d.h = h;
    if (d.pad_l + d.aw > w) d.aw = w - d.pad_l > 0 ? w - d.pad_l : 0;
    int back = fb_wr;       /* búfer propio del escritor: nadie más lo lee */
    gpu_present_rgba(&d, NULL, fb[back], (size_t)MV_HOST_MAX_W * MV_HOST_MAX_H);
    fb_meta[back].w = w; fb_meta[back].h = h; fb_meta[back].seq = ++fb_seq;
    unsigned old = atomic_exchange_explicit(&fb_state, (unsigned)back | 4u, memory_order_acq_rel);
    fb_wr = (int)(old & 3u);   /* el que estaba listo (leído o no) vuelve a ser mío */
}

uint32_t mv_host_framebuffer(uint8_t *dst, size_t dst_size, int *w, int *h, uint32_t last_seq) {
    uint32_t seq = 0;
    atomic_store_explicit(&fb_read_ns, mono_ns() | 1, memory_order_relaxed);
    pthread_mutex_lock(&fb_rmx);
    if (fb[0] && fb[1] && fb[2]) {
        if (atomic_load_explicit(&fb_state, memory_order_acquire) & 4u) {
            unsigned old = atomic_exchange_explicit(&fb_state, (unsigned)fb_rd, memory_order_acq_rel);
            fb_rd = (int)(old & 3u);
        }
        size_t need = (size_t)fb_meta[fb_rd].w * (size_t)fb_meta[fb_rd].h * 4;
        if (fb_meta[fb_rd].seq && fb_meta[fb_rd].seq != last_seq && dst_size >= need) {
            memcpy(dst, fb[fb_rd], need);
            if (w) *w = fb_meta[fb_rd].w;
            if (h) *h = fb_meta[fb_rd].h;
            seq = fb_meta[fb_rd].seq;
        }
    }
    pthread_mutex_unlock(&fb_rmx);
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

/* Ritmo de la emulación.
 * 1) Con consumidor de audio vivo (el anillo de la SPU se está leyendo) la emulación se acompasa AL AUDIO: espera mientras
 *    haya más de AUDIO_TARGET frames de audio por delante. El reloj de audio manda, así que no hay deriva entre el reloj de
 *    50 Hz virtual y el real, el anillo nunca desborda y no se descarta ni repite nada. Si la emulación va más lenta que
 *    el audio simplemente no espera (y el consumidor rellena de forma continua, ver spu_pull_rate).
 * 2) Sin consumidor (arranque, audio parado/interrumpido, pausa del motor de audio): reloj de pared a 50 Hz. Un consumidor
 *    se da por muerto si el contador de lectura no avanza en 150 ms (así un audio atascado no congela el juego). */
#define AUDIO_TARGET SPU_AUDIO_TARGET   /* 3 VBlanks PAL de audio por delante (~60 ms): absorbe picos de trabajo de ~1 VBlank */
static uint32_t pace_last_rd;
static uint64_t pace_last_change_ns;
static int audio_live(uint64_t now) {
    uint32_t rd = spu_ring_reads();
    if (rd != pace_last_rd) { pace_last_rd = rd; pace_last_change_ns = now; }
    return pace_last_change_ns && now - pace_last_change_ns < 150000000ull;
}

static void pace(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (atomic_load(&turbo)) { have_deadline = 0; return; }
    uint64_t t0 = mono_ns();
    if (audio_live(t0)) {
        have_deadline = 0;   /* al volver al reloj de pared no se intentan recuperar los VBlanks de la espera */
        uint32_t lvl;
        while ((lvl = spu_ring_level()) > AUDIO_TARGET && atomic_load(&state) == MV_HOST_RUNNING && !atomic_load(&turbo)) {
            long ns = (long)((uint64_t)(lvl - AUDIO_TARGET) * 1000000000ull / SPU_RATE);
            if (ns > 4000000L) ns = 4000000L;
            if (ns < 200000L) ns = 200000L;
            struct timespec s = {0, ns};
            nanosleep(&s, NULL);
            uint64_t t = mono_ns();
            if (!audio_live(t)) { atomic_fetch_add(&st_stall_fallbacks, 1); break; }
        }
        atomic_fetch_add(&st_wait_ns, mono_ns() - t0);
        return;
    }
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
    atomic_fetch_add(&st_wait_ns, mono_ns() - t0);
}

/* ---- Gancho de VBlank (hilo de emulación) ---- */

static uint64_t work_start_ns;   /* fin de la espera del VBlank anterior: lo emulado desde aquí es "trabajo" */

static void on_frame(Cpu *c, uint32_t f) {
    uint64_t t_in = mono_ns();
    atomic_store(&frames, f);
    uint64_t pr = atomic_load_explicit(&pad_req, memory_order_relaxed);
    uint16_t pb = (uint16_t)pr;
    if (hooks.pad) pb = hooks.pad(pb);
    pad_set_state(0, pb, (uint8_t)(pr >> 16), (uint8_t)(pr >> 24), (uint8_t)(pr >> 32), (uint8_t)(pr >> 40));

    {   /* Solo se convierte el vídeo si alguien lo mira (lectura en el último segundo); si no, 1 de cada 10 VBlanks. */
        uint64_t lr = atomic_load_explicit(&fb_read_ns, memory_order_relaxed);
        /* Con signo: si el lector marcó su lectura DESPUÉS de tomar t_in (carrera de microsegundos), la resta sin signo daba ~2^64 y
         * se omitía la publicación de ese VBlank: el visor veía un fotograma repetido (reproducido: 2 de 5200 con un lector a alta
         * frecuencia). */
        if (f % 10 == 0 || (lr ? (int64_t)(t_in - lr) < 1000000000ll : f < 200)) publish_frame();
    }

    uint8_t sm, lg;
    pad_get_rumble(0, &sm, &lg);
    atomic_store(&rumble_pack, (uint16_t)(sm << 8 | lg));

    if (hooks.frame) hooks.frame(c, f);

    {   /* medida de tiempo de trabajo por VBlank (sin esperas) */
        uint64_t w = mono_ns() - (work_start_ns ? work_start_ns : t_in);
        atomic_fetch_add_explicit(&st_work_ns, w, memory_order_relaxed);
        atomic_fetch_add_explicit(&st_work_n, 1, memory_order_relaxed);
        if (w > atomic_load_explicit(&st_work_max_ns, memory_order_relaxed)) atomic_store_explicit(&st_work_max_ns, w, memory_order_relaxed);
    }
    int was_paused = 0;
    if (atomic_load(&state) == MV_HOST_PAUSED) {
        uint64_t tp = mono_ns();
        pthread_mutex_lock(&MX);
        while (atomic_load(&state) == MV_HOST_PAUSED) { was_paused = 1; pthread_cond_wait(&CV, &MX); }
        pthread_mutex_unlock(&MX);
        atomic_fetch_add(&st_pause_ns, mono_ns() - tp);
    }
    if (was_paused) have_deadline = 0;
    pace();
    work_start_ns = mono_ns();
}

static void *emu_main(void *arg) {
    (void)arg;
    FILE *f = fopen(exe_path, "rb");
    static uint8_t buf[EMU_RAM_SIZE + 0x800];
    size_t n = f ? fread(buf, 1, sizeof buf, f) : 0;
    if (f) fclose(f);
    uint32_t pc = 0, gp = 0;
    {   /* validación ya hecha en mv_host_start; aquí solo se cargan los datos */
        uint32_t t, sz;
        memcpy(&pc, buf + 0x10, 4); memcpy(&gp, buf + 0x14, 4);
        memcpy(&t, buf + 0x18, 4); memcpy(&sz, buf + 0x1C, 4);
        memcpy(&emu_ram[t & 0x1FFFFF], buf + 0x800, sz);
        (void)n;
        if (hooks.exe_loaded) hooks.exe_loaded(emu_ram, t, sz);
    }
#ifdef MV_NOGEN
    {   /* sin código generado: el punto de entrada es el de la cabecera y las HLE se enlazan por firmas */
        uint32_t t, sz;
        memcpy(&t, buf + 0x18, 4); memcpy(&sz, buf + 0x1C, 4);
        rt_entry = pc;
        int nb = hle_bind_file(sigs_path, t, sz, pc);
        if (nb < 0) {
            set_error("No se pudo leer el fichero de firmas del SDK.");
            pthread_mutex_lock(&MX); atomic_store(&state, MV_HOST_ERROR); pthread_mutex_unlock(&MX);
            return NULL;
        }
        /* Firmas ausentes o de otro ejecutable: con la mayoría de las HLE sin enlazar el juego correría el código original
         * (accesos a hardware inexistente) y se colgaría a medias. Se tolera alguna suelta (la edición real enlaza todas menos 1). */
        if (nb == 0 || (unsigned)nb * 4u < ((unsigned)nb + hle_bind_missing()) * 3u) {
            fprintf(stderr, "[host] firmas: solo %d funciones HLE enlazadas, %u sin enlazar\n", nb, hle_bind_missing());
            set_error("El ejecutable del disco no coincide con las firmas incluidas en esta compilación. Vuelve a generar la app o revisa que el disco sea el esperado.");
            pthread_mutex_lock(&MX); atomic_store(&state, MV_HOST_ERROR); pthread_mutex_unlock(&MX);
            return NULL;
        }
        if (hle_bind_missing()) fprintf(stderr, "[host] aviso: %u funciones HLE sin enlazar: el juego puede no funcionar\n", hle_bind_missing());
    }
#else
    if (pc != rt_entry) fprintf(stderr, "[host] aviso: pc 0x%08x != entrada recompilada 0x%08x\n", pc, rt_entry);
#endif
    if (hooks.bound) hooks.bound();

    static Cpu c;   /* estático: el vigilante lee sus registros desde otro hilo */
    memset(&c, 0, sizeof c);
    wd_cpu = &c;
    c.r[28] = gp;
    c.r[29] = c.r[30] = 0x801FFFF0u;
    c.r[31] = 0;
    gte_reset();
    gpu_reset();
    if (hooks.start) hooks.start();
    rt_host_frame = on_frame;
    rt_fatal_armed = 1;
    if (setjmp(rt_fatal_jmp)) {
        set_error("El motor se detuvo por un error interno. Consulta el registro (carpeta Logs de la carpeta elegida, o mvision.log en Documents).");
        pthread_mutex_lock(&MX);
        atomic_store(&state, MV_HOST_ERROR);
        pthread_mutex_unlock(&MX);
        return NULL;
    }
    rt_call(&c, pc);
    fprintf(stderr, "[host] la entrada retornó (v0=0x%08x) tras %u frames\n", c.r[2], hle_frame_count());
    set_error("El juego terminó la ejecución.");
    pthread_mutex_lock(&MX);
    atomic_store(&state, MV_HOST_ERROR);
    pthread_mutex_unlock(&MX);
    return NULL;
}

/* ---- Vigilante y latido del registro (hilo propio, no toca nada del emulador salvo lecturas) ---- */

static const char *state_name(int s) {
    return s == MV_HOST_RUNNING ? "marcha" : s == MV_HOST_PAUSED ? "pausa" : s == MV_HOST_ERROR ? "error" : "parado";
}

static void dump_stall(uint32_t f, double secs) {
    fprintf(stderr, "[wd] SIN AVANCE: el hilo de emulación no cambia de VBlank desde hace %.1f s (frame %u, estado %s)\n",
            secs, f, state_name(atomic_load(&state)));
    Cpu *c = wd_cpu;
    uint32_t blocks[64];
    int nb = interp_recent_blocks(blocks, 64);
    if (c) fprintf(stderr, "[wd] regs: pc~0x%08x ra=0x%08x sp=0x%08x gp=0x%08x a0=0x%08x v0=0x%08x\n",
                   nb ? blocks[nb - 1] : 0, c->r[31], c->r[29], c->r[28], c->r[4], c->r[2]);
    fprintf(stderr, "[wd] irq: I_STAT=0x%08x I_MASK=0x%08x ciclos=%llu cd_busy=%d vsync(juego)=%u spu_nivel=%u\n",
            sys_irq_stat(), sys_irq_mask(), (unsigned long long)sys_cycles(), cd_busy(), hle_frame_count(), spu_ring_level());
    fprintf(stderr, "[wd] ultimos %d bloques (el ultimo, el mas reciente):", nb < 32 ? nb : 32);
    for (int i = nb > 32 ? nb - 32 : 0; i < nb; i++) fprintf(stderr, " %08x", blocks[i]);
    fputc('\n', stderr);
    for (int i = nb > 8 ? nb - 8 : 0; i < nb; i++) fprintf(stderr, "[wd]   %08x %s\n", blocks[i], rt_name(blocks[i]));
    cd_dump_state();
    fflush(stderr);
}

static void heartbeat(uint64_t t0, uint32_t f, uint32_t f_prev, double dt) {
    static uint64_t w_prev, wn_prev, wait_prev, pause_prev, ac_prev, af_prev, r0_prev, r1_prev;
    static SpuAudioStats sp_prev;
    SpuAudioStats sp;
    spu_audio_stats(&sp, 1);
    uint64_t w = atomic_load(&st_work_ns), wn = atomic_load(&st_work_n), wx = atomic_exchange(&st_work_max_ns, 0);
    uint64_t wait = atomic_load(&st_wait_ns), pz = atomic_load(&st_pause_ns);
    double work_avg = wn > wn_prev ? (double)(w - w_prev) / (double)(wn - wn_prev) / 1e6 : 0.0;
    double vb_s = (double)(f - f_prev) / dt;
    fprintf(stderr, "[hb] t=%.0fs estado=%s frame=%u vblank/s=%.1f (50=tiempo real) trabajo/frame=%.2f ms (max %.1f) espera-audio=%.0f%% pausa=%.1fs "
                    "| spu prod=%llu cons=%llu (+%llu/+%llu) nivel=%u (min %u, max %u) infra=%u (+%u) sobre=%llu sil=%llu stall-audio=%llu | juego-vsync=%u\n",
            (double)(mono_ns() - t0) / 1e9, state_name(atomic_load(&state)), f, vb_s, work_avg, (double)wx / 1e6,
            100.0 * (double)(wait - wait_prev) / (dt * 1e9), (double)(pz - pause_prev) / 1e9,
            (unsigned long long)sp.produced, (unsigned long long)sp.consumed,
            (unsigned long long)(sp.produced - sp_prev.produced), (unsigned long long)(sp.consumed - sp_prev.consumed),
            sp.level, sp.level_min, sp.level_max, sp.underruns, sp.underruns - sp_prev.underruns,
            (unsigned long long)sp.dropped, (unsigned long long)sp.silence, (unsigned long long)atomic_load(&st_stall_fallbacks), hle_frame_count());
    uint64_t ac = atomic_load(&st_audio_calls), af = atomic_load(&st_audio_frames), amx = atomic_exchange(&st_audio_max_ns, 0);
    uint64_t r0 = atomic_load(&st_render[0]), r1 = atomic_load(&st_render[1]);
    fprintf(stderr, "[hb] audio: callbacks/s=%.1f frames/callback=%.0f max-pull=%.0f us | render: ventana=%.1f/s inmersivo=%.1f/s\n",
            (double)(ac - ac_prev) / dt, ac > ac_prev ? (double)(af - af_prev) / (double)(ac - ac_prev) : 0.0, (double)amx / 1e3,
            (double)(r0 - r0_prev) / dt, (double)(r1 - r1_prev) / dt);
    ac_prev = ac; af_prev = af; r0_prev = r0; r1_prev = r1;
    w_prev = w; wn_prev = wn; wait_prev = wait; pause_prev = pz; sp_prev = sp;
}

static void *mon_main(void *arg) {
    (void)arg;
    uint64_t t0 = mono_ns(), t_hb = t0, t_flush = t0, t_prog = t0, t_dump = 0, t_prev = t0;
    uint32_t f_last = 0, f_hb = 0;
    int err_logged = 0;
    while (!atomic_load(&mon_stop)) {
        struct timespec s = {0, 250000000L};
        nanosleep(&s, NULL);
        uint64_t t = mono_ns();
        uint32_t f = atomic_load(&frames);
        int st = atomic_load(&state);
        /* Si este hilo no corrió durante >1,5 s (app suspendida/en segundo plano, el reloj monótono sigue avanzando) el
         * emulador tampoco pudo avanzar: no es un atasco, se reinicia la cuenta. */
        if (t - t_prev > 1500000000ull) { t_prog = t; t_dump = 0; }
        t_prev = t;
        if (f != f_last || st != MV_HOST_RUNNING) { f_last = f; t_prog = t; t_dump = 0; }
        else if (t - t_prog > 3000000000ull && (!t_dump || t - t_dump > 10000000000ull)) { dump_stall(f, (double)(t - t_prog) / 1e9); t_dump = t; }
        if (st == MV_HOST_ERROR && !err_logged) {
            err_logged = 1;
            fprintf(stderr, "[hb] el motor terminó con error: %s\n", mv_host_error());
            dump_stall(f, 0.0);
        }
        if (t - t_hb >= 5000000000ull) {
            heartbeat(t0, f, f_hb, (double)(t - t_hb) / 1e9);
            f_hb = f; t_hb = t;
        }
        if (t - t_flush >= 1000000000ull) { fflush(stderr); t_flush = t; }
    }
    return NULL;
}

/* ---- API ---- */

/* Monta cada entrada de la lista: carpeta extraída, .cue o .bin (cd_open decide por lo que sea). */
static int mount_discs(const char *list) {
    char *dup = strdup(list);
    if (!dup) return 0;
    char *sv = NULL;
    for (char *e = strtok_r(dup, ";", &sv); e; e = strtok_r(NULL, ";", &sv)) {
        struct stat st;
        if (!*e || stat(e, &st)) { fprintf(stderr, "[host] disco no accesible: '%s'\n", mv_base(e)); continue; }
        cd_open(e);
    }
    free(dup);
    return cd_disc_count();
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
    if (n < 0x800 || memcmp(hdr, EXE_MAGIC, 8)) { set_error("El ejecutable del juego no es válido."); return -1; }
    uint32_t t, sz;
    memcpy(&t, hdr + 0x18, 4); memcpy(&sz, hdr + 0x1C, 4);
    if (total < 0 || sz > (uint32_t)total - 0x800 || (t & 0x1FFFFF) + sz > EMU_RAM_SIZE ||
        sz > EMU_RAM_SIZE) { set_error("La cabecera del ejecutable del juego es inválida."); return -1; }

#ifdef MV_NOGEN
    {
        const char *sp = sigs_path[0] ? sigs_path : getenv("MV_SIGS");
        FILE *sf = sp && *sp ? fopen(sp, "rb") : NULL;
        if (!sf) { set_error("Falta el fichero de firmas del SDK."); return -1; }
        fclose(sf);
        if (sp != sigs_path) snprintf(sigs_path, sizeof sigs_path, "%s", sp);   /* (src == dst sería comportamiento indefinido) */
    }
#endif
    if (!disc_dir || !*disc_dir) { set_error("No se encontró ningún disco."); return -3; }
    if (mount_discs(disc_dir) <= 0) { set_error("No se pudo montar ningún disco."); return -3; }

    if (card_dir && *card_dir) {
        char p[1100];
        snprintf(p, sizeof p, "%s/card0.mcd", card_dir);
        if (card_open(0, p)) fprintf(stderr, "[host] aviso: no se pudo abrir la tarjeta\n");
    }

    for (int i = 0; i < 3; i++) {
        fb[i] = calloc(1, MV_HOST_FB_BYTES);
        if (!fb[i]) { set_error("Sin memoria."); return -4; }
    }
    snprintf(exe_path, sizeof exe_path, "%s", exe);

    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 64u << 20);   /* el código recompilado anida llamadas C */
    pthread_mutex_lock(&MX);
    atomic_store(&state, MV_HOST_RUNNING);
    pthread_mutex_unlock(&MX);
    int r = pthread_create(&thr, &at, emu_main, NULL);
    pthread_attr_destroy(&at);
    if (r) {
        set_error("No se pudo crear el hilo de emulación.");
        pthread_mutex_lock(&MX);
        atomic_store(&state, MV_HOST_ERROR);
        pthread_mutex_unlock(&MX);
        return -5;
    }
    pthread_detach(thr);
    if (!pthread_create(&mon_thr, NULL, mon_main, NULL)) pthread_detach(mon_thr);
    return 0;
}

int mv_host_has_disc_img(void) { return 1; }

int mv_host_scan_discs(const char *dir, char *out, size_t cap) {
    if (!out || !cap) return 0;
    out[0] = 0;
    if (!dir) return 0;
    static DiscEntry ents[16];
    int n = disc_scan_dir(dir, ents, 16), w = 0;
    for (int i = 0; i < n; i++) {
        size_t l = strlen(ents[i].path);
        if ((size_t)w + l + 2 > cap) { n = i; break; }
        w += snprintf(out + w, cap - (size_t)w, "%s%s", i ? ";" : "", ents[i].path);
    }
    return n;
}

int mv_host_extract_exe(const char *disc, const char *outfile) {
    if (!disc || !outfile) return -1;
    struct stat st;
    if (stat(disc, &st)) return -1;
    return disc_extract_dataexe(disc, S_ISDIR(st.st_mode), outfile);
}

MvHostState mv_host_state(void) { return (MvHostState)atomic_load(&state); }

const char *mv_host_error(void) {
    static char copy[512];
    pthread_mutex_lock(&MX);
    memcpy(copy, errmsg, sizeof copy);
    pthread_mutex_unlock(&MX);
    return copy;
}

uint32_t mv_host_frame_count(void) { return atomic_load(&frames); }

/* Hilo de audio de tiempo real: solo atómicos (anillo SPSC), sin candados ni esperas. En pausa o error el anillo se vacía
 * y el consumidor rellena de forma continua hasta decaer a silencio; al reanudar no se reproduce audio viejo (la SPU no avanza durante la pausa). */
int mv_host_audio_pull_rate(int16_t *out, int n, int rate) {
    if (!out || n <= 0) return 0;
    uint64_t t0 = mono_ns();
    int r = spu_pull_rate(out, n, rate);
    uint64_t dt = mono_ns() - t0;
    atomic_fetch_add_explicit(&st_audio_calls, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&st_audio_frames, (uint64_t)n, memory_order_relaxed);
    if (dt > atomic_load_explicit(&st_audio_max_ns, memory_order_relaxed)) atomic_store_explicit(&st_audio_max_ns, dt, memory_order_relaxed);
    return r;
}

int mv_host_audio_pull(int16_t *out, int n) { return mv_host_audio_pull_rate(out, n, 44100); }

void mv_host_note_render(int kind) { atomic_fetch_add_explicit(&st_render[kind ? 1 : 0], 1, memory_order_relaxed); }

void mv_host_pad(uint16_t buttons, uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry) {
    atomic_store_explicit(&pad_req, (uint64_t)buttons | (uint64_t)lx << 16 | (uint64_t)ly << 24 | (uint64_t)rx << 32 | (uint64_t)ry << 40,
                          memory_order_relaxed);
}

void mv_host_rumble(uint8_t *small, uint8_t *large) {
    uint16_t v = atomic_load(&rumble_pack);
    if (small) *small = (uint8_t)(v >> 8);
    if (large) *large = (uint8_t)v;
}

void mv_host_pause(void) {
    pthread_mutex_lock(&MX);
    if (atomic_load(&state) == MV_HOST_RUNNING) atomic_store(&state, MV_HOST_PAUSED);
    pthread_mutex_unlock(&MX);
}

void mv_host_resume(void) {
    pthread_mutex_lock(&MX);
    if (atomic_load(&state) == MV_HOST_PAUSED) { atomic_store(&state, MV_HOST_RUNNING); pthread_cond_broadcast(&CV); }
    pthread_mutex_unlock(&MX);
}

void mv_host_set_turbo(int on) { atomic_store(&turbo, on != 0); }
