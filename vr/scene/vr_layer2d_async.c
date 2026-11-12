#include "vr_layer2d_async.h"
#include "vr_layer2d.h"
#include "../host/vr_view.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NJOB 16
#define L2A_STALE 12   /* listas sin capa nueva antes de dejar de mostrarla (~0,5 s a 25 listas/s). Mejor un HUD algo viejo que volver al shader antiguo, que rompía los textos */
#define SLOW_MS 12.0
#define RECOVER 15
#define AVG_N 8

/* Escala adaptativa: baja si la media móvil de los últimos AVG_N trabajos pasa de SLOW_MS; sube tras RECOVER trabajos seguidos en que la escala siguiente cabría con esa media (medido en visor: 3,5 ms a escala 1 y
 * 8,3 ms a escala 3, el coste crece ~lineal con la escala). La media evita que un pico aislado (vista radio/códec, marco 384x256) baje la escala o reinicie la recuperación. */
int vl2a_scale_step(int cur, int target, double ms, Vl2aScale *st) {
    st->ms[st->i] = ms;
    st->i = (st->i + 1) % AVG_N;
    if (st->n < AVG_N) st->n++;
    double sum = 0;
    for (int k = 0; k < st->n; k++) sum += st->ms[k];
    double avg = sum / st->n;
    if (st->n == AVG_N && avg > SLOW_MS && cur > 1) { memset(st, 0, sizeof *st); return cur - 1; }
    if (cur < target && avg * (double)(cur + 1) / (double)cur < SLOW_MS * 0.8) {
        if (++st->fast >= RECOVER) { memset(st, 0, sizeof *st); return cur + 1; }
    } else st->fast = 0;
    return cur;
}

typedef struct {
    GpuDrawList l;                 /* ops/data/vram_init apuntan a la memoria propia de abajo */
    GpuOp *ops; size_t ops_cap;
    uint16_t *data; size_t data_cap;
    uint16_t *vram; int has_vram;
    uint16_t *vend; int has_vend;   /* VRAM real al cerrar la lista (texturas dibujadas durante ella) */
    uint8_t *opd; size_t opd_cap; int has_opd;
    int full, reset, view;         /* view: etiqueta de vista al encolar (vl2a_set_view) */
    uint32_t lseq;                 /* nº de envío */
} Job;

typedef struct { uint8_t *px; size_t cap; int w, h, full, mode, view; int win[6]; uint32_t seq; } Out;   /* mode: VL_* con que se dibujó; view: etiqueta de vista del trabajo */

struct VrL2Async {
    pthread_t th; int started;
    pthread_mutex_t mx; pthread_cond_t cv_job, cv_idle;
    Job job[NJOB]; int head, count; int busy;   /* cola circular */
    int quit;
    VrLayer2D *L;
    _Atomic int target, smooth, view_tag, native;   /* native: se publica la capa empaquetada para el visor (vl2d_pack) */
    int cur; Vl2aScale scst;
    Out out[3]; int pub, held, have_pub;        /* índice publicado / en uso por el consumidor (-1) */
    uint32_t sub_seq, pub_sub_seq, acq_seq;     /* envíos / envío al que corresponde lo publicado / último adquirido */
    double work_ms, submit_max;
    uint32_t n_sub, n_skip, n_run, n_neg, n_pub;   /* contadores del log [capa2d] */
    VrL2Info info;
};

static _Atomic int g_split;   /* ver vl2a_set_split: 0 = capa 2D completa, sin trocear (defecto) */
void vl2a_set_split(int on) { atomic_store(&g_split, on != 0); }
int vl2a_split(void) { return atomic_load(&g_split); }

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec * 1e-6; }

static int grow(void **p, size_t *cap, size_t need, size_t esz) {
    if (need <= *cap) return 1;
    size_t n = need + need / 2 + 64;
    void *q = realloc(*p, n * esz);
    if (!q) return 0;
    *p = q; *cap = n;
    return 1;
}

static void *worker(void *arg) {
    VrL2Async *A = arg;
    static __thread VrScene sc;
    pthread_mutex_lock(&A->mx);
    for (;;) {
        while (!A->count && !A->quit) pthread_cond_wait(&A->cv_job, &A->mx);
        if (!A->count && A->quit) break;
        Job *j = &A->job[A->head];
        int later = A->count > 1;          /* hay listas más nuevas esperando: si esta trae la VRAM real de partida no hace falta ni aplicarla (la siguiente trae la suya); si no, solo se aplican sus eventos */
        int skip = later && A->job[(A->head + 1) % NJOB].has_vram;   /* la siguiente trae su propia VRAM de partida: esta no hace falta */
        A->busy = 1;
        int wi = 0, scale = A->cur;
        while (wi == A->pub || wi == A->held) wi++;   /* búfer libre (de 3, con 2 ocupados como máximo) */
        pthread_mutex_unlock(&A->mx);

        if (j->reset) vl2d_reset(A->L);
        int draw = (j->has_opd || j->full) && !later;   /* fotograma completo: no necesita la clasificación de la escena (capa exacta aunque vr_scene_build fallase) */
        vl2d_set_options(A->L, scale, atomic_load(&A->smooth));
        memset(&sc, 0, sizeof sc);
        if (draw && j->has_opd) { sc.op_dest = j->opd; sc.n_op_dest = j->l.nops; }
        double t0 = now_ms();
        if (skip) A->n_skip++;   /* (contador leído bajo el mutex en vl2a_report; incremento sin él: solo informativo) */
        int have = skip ? 0 : vl2d_run(A->L, &j->l, draw && j->has_opd ? &sc : NULL, j->full);
        double ms = now_ms() - t0;
        VrL2Info inf; vl2d_info(A->L, &inf);
        int w = 0, h = 0;
        const uint8_t *px = have ? vl2d_rgba(A->L, &w, &h) : NULL;
        int planes = 1;
        const int pack = px && atomic_load(&A->native);
        if (pack && !vl2d_pack_size(A->L, &w, &h, &planes)) px = NULL;
        Out *o = &A->out[wi];
        int ok = 0;
        if (px && w > 0 && h > 0 && grow((void **)&o->px, &o->cap, (size_t)w * h * planes * 4, 1)) {
            if (pack) vl2d_pack(A->L, o->px); else memcpy(o->px, px, (size_t)w * h * 4);
            if (!vl2d_window(A->L, o->win)) memset(o->win, 0, sizeof o->win);
            o->w = w; o->h = h * planes; o->full = j->full == VL_FULL; o->mode = j->full; o->view = j->view; ok = 1;
        }

        pthread_mutex_lock(&A->mx);
        if (!skip) { A->n_run++; if (draw) A->info = inf; }
        if (draw) {
            if (inf.verify && inf.v_ms > 0 && inf.v_ms < ms) ms -= inf.v_ms;   /* la verificación (pantalla 3D del motor) no cuenta para la escala adaptativa: el panel se ve igual de nítido */
            A->work_ms = ms;
            A->cur = vl2a_scale_step(A->cur, atomic_load(&A->target), ms, &A->scst);
            if (ok) A->n_pub++;
            if (ok) { o->seq = j->lseq; A->pub = wi; A->have_pub = 1; A->pub_sub_seq = j->lseq; }
            else A->have_pub = 0;           /* lista sin capa (o fallo): el renderer usa el shader */
        }
        A->head = (A->head + 1) % NJOB; A->count--; A->busy = 0;
        pthread_cond_broadcast(&A->cv_idle);
    }
    pthread_mutex_unlock(&A->mx);
    return NULL;
}

VrL2Async *vl2a_new(void) {
    VrL2Async *A = calloc(1, sizeof *A);
    if (!A) return NULL;
    pthread_mutex_init(&A->mx, NULL); pthread_cond_init(&A->cv_job, NULL); pthread_cond_init(&A->cv_idle, NULL);
    A->L = vl2d_new();
    A->pub = -1; A->held = -1; A->cur = 4;
    atomic_store(&A->target, 4); atomic_store(&A->smooth, VL_SMOOTH_ORIGINAL);
    if (!A->L || pthread_create(&A->th, NULL, worker, A) != 0) { vl2a_free(A); return NULL; }
    A->started = 1;
    return A;
}

void vl2a_free(VrL2Async *A) {
    if (!A) return;
    if (A->started) {
        pthread_mutex_lock(&A->mx); A->quit = 1; pthread_cond_broadcast(&A->cv_job); pthread_mutex_unlock(&A->mx);
        pthread_join(A->th, NULL);
    }
    for (int i = 0; i < NJOB; i++) { free(A->job[i].ops); free(A->job[i].data); free(A->job[i].vram); free(A->job[i].vend); free(A->job[i].opd); }
    for (int i = 0; i < 3; i++) free(A->out[i].px);
    vl2d_free(A->L);
    pthread_mutex_destroy(&A->mx); pthread_cond_destroy(&A->cv_job); pthread_cond_destroy(&A->cv_idle);
    free(A);
}

static void set_options(VrL2Async *A, int scale, int smooth) {
    if (scale < 1) scale = 1;
    if (scale > 6) scale = 6;
    int old = atomic_exchange(&A->target, scale);
    if (old != scale) { pthread_mutex_lock(&A->mx); A->cur = scale; memset(&A->scst, 0, sizeof A->scst); pthread_mutex_unlock(&A->mx); }
    atomic_store(&A->smooth, smooth < 0 ? 0 : smooth > 2 ? 2 : smooth);
}

void vl2a_options(VrL2Async *A, int scale, int smooth) { if (A) { set_options(A, scale, smooth); atomic_store(&A->native, 0); } }
void vl2a_options_native(VrL2Async *A) { if (A) { set_options(A, 1, VL_SMOOTH_ORIGINAL); atomic_store(&A->native, 1); } }

void vl2a_set_view(VrL2Async *A, int view) { if (A) atomic_store(&A->view_tag, view); }

int vl2a_layer_accept(int mode, int layer_view, int cur_view, int overlay) {
    if ((layer_view >> 8) != (cur_view >> 8)) return 0;
    return !overlay || mode == VL_PANEL;
}

int vl2a_submit(VrL2Async *A, const GpuDrawList *l, const uint8_t *op_dest, int full, int reset) {
    if (!A || !l) return 0;
    double t0 = now_ms();
    pthread_mutex_lock(&A->mx);
    /* Cola llena (la trabajadora va muy por detrás): se espera a que libere un hueco; los trabajos viejos solo aplican eventos, así que es raro y breve. */
    while (A->count >= NJOB && !A->quit) pthread_cond_wait(&A->cv_idle, &A->mx);
    Job *j = &A->job[(A->head + A->count) % NJOB];
    /* El trabajo en curso es el de `head`; el hueco nuevo nunca coincide con él (count < NJOB). */
    int ok = grow((void **)&j->ops, &j->ops_cap, l->nops, sizeof(GpuOp)) && grow((void **)&j->data, &j->data_cap, l->ndata, 2) &&
             grow((void **)&j->opd, &j->opd_cap, l->nops, 1) && ((!l->vram_init && !l->vram_start) || (j->vram || (j->vram = malloc(sizeof(uint16_t) * 1024 * 512)))) &&
             (!l->vram_end || j->vend || (j->vend = malloc(sizeof(uint16_t) * 1024 * 512)));
    if (ok) {
        j->l = *l;
        if (l->nops) memcpy(j->ops, l->ops, l->nops * sizeof(GpuOp));
        if (l->ndata) memcpy(j->data, l->data, (size_t)l->ndata * 2);
        j->l.ops = j->ops; j->l.data = j->data;
        j->l.cap_ops = j->l.cap_data = 0; j->l.vram_after = NULL;
        /* VRAM de partida: la real al abrir la lista (vram_start, cada lista) o la instantánea de la primera/resincronización (vram_init). Una sola copia de 1 MB en el hilo de emulación. */
        const uint16_t *v0 = l->vram_start ? l->vram_start : l->vram_init;
        j->has_vram = v0 != NULL;
        if (j->has_vram) memcpy(j->vram, v0, sizeof(uint16_t) * 1024 * 512);
        j->l.vram_init = NULL; j->l.vram_start = j->has_vram ? j->vram : NULL;
        j->has_vend = l->vram_end != NULL;
        if (j->has_vend) memcpy(j->vend, l->vram_end, sizeof(uint16_t) * 1024 * 512);
        j->l.vram_end = j->has_vend ? j->vend : NULL;
        j->has_opd = op_dest != NULL;
        if (op_dest && l->nops) memcpy(j->opd, op_dest, l->nops);
        j->full = full; j->reset = reset; j->view = atomic_load(&A->view_tag); j->lseq = ++A->sub_seq; A->n_sub++;
        A->count++;
        pthread_cond_signal(&A->cv_job);
    }
    pthread_mutex_unlock(&A->mx);
    double ms = now_ms() - t0;
    if (ms > A->submit_max) A->submit_max = ms;
    return ok;
}

int vl2a_acquire(VrL2Async *A, const uint8_t **px, int *w, int *h, int *full, uint32_t *seq) {
    if (!A) return -1;
    pthread_mutex_lock(&A->mx);
    if (!A->have_pub || A->pub < 0 || A->sub_seq - A->pub_sub_seq > L2A_STALE) { A->held = -1; A->acq_seq = 0; A->n_neg++; pthread_mutex_unlock(&A->mx); return -1; }
    int r = 0;
    A->held = A->pub;
    if (A->out[A->pub].seq != A->acq_seq) { A->acq_seq = A->out[A->pub].seq; r = 1; }   /* nueva solo si lo publicado cambió desde la última adquisición (vl2a_release no la borra) */
    const Out *o = &A->out[A->held];
    if (px) *px = o->px;
    if (w) *w = o->w;
    if (h) *h = o->h;
    if (full) *full = o->full;
    if (seq) *seq = o->seq;
    pthread_mutex_unlock(&A->mx);
    return r;
}

int vl2a_acquire_window(VrL2Async *A, int win[6]) {
    if (!A || !win) return 0;
    pthread_mutex_lock(&A->mx);
    const int ok = A->held >= 0 && A->out[A->held].win[0] > 0;
    if (ok) memcpy(win, A->out[A->held].win, sizeof A->out[A->held].win);
    pthread_mutex_unlock(&A->mx);
    return ok;
}

int vl2a_acquire_info(VrL2Async *A, int *mode, int *view) {
    if (!A) return 0;
    pthread_mutex_lock(&A->mx);
    int ok = A->held >= 0;
    if (ok) { if (mode) *mode = A->out[A->held].mode; if (view) *view = A->out[A->held].view; }
    pthread_mutex_unlock(&A->mx);
    return ok;
}

void vl2a_release(VrL2Async *A) {
    if (!A) return;
    pthread_mutex_lock(&A->mx); A->held = -1; pthread_mutex_unlock(&A->mx);
}

void vl2a_stats(VrL2Async *A, int *scale, double *work_ms, double *submit_ms_max, uint32_t *lag) {
    if (!A) return;
    pthread_mutex_lock(&A->mx);
    if (scale) *scale = A->cur;
    if (work_ms) *work_ms = A->work_ms;
    if (submit_ms_max) { *submit_ms_max = A->submit_max; A->submit_max = 0; }
    if (lag) *lag = A->have_pub ? A->sub_seq - A->pub_sub_seq : 0;
    pthread_mutex_unlock(&A->mx);
}

int vl2a_report(VrL2Async *A, char *buf, int n) {
    if (!A || !buf || n <= 0) return 0;
    pthread_mutex_lock(&A->mx);
    static const char *sm[3] = {"original", "nitido", "suave"};
    int sd = A->info.smooth < 0 || A->info.smooth > 2 ? 0 : A->info.smooth;
    char vbuf[300];   /* panel sobre la pantalla 3D del motor: píxeles del panel / distintos del emulador (tras componer sobre el mundo reproducido), sustituidos, huecos del modelo fuera del panel y 2D perdidas */
    if (A->info.verify) snprintf(vbuf, sizeof vbuf, "si panel_px=%d distintos=%d dif_malos=%.3f sustituidos=%d(resta=%d transparentes=%d) mundo_distinto_fuera=%d perdidas_2d=%d coste=%.1fms", A->info.v_cov, A->info.v_bad, A->info.v_mean_milli / 1000.0, A->info.v_patched, A->info.v_sub, A->info.v_transp, A->info.v_out, A->info.v_lost2d, A->info.v_ms);
    else snprintf(vbuf, sizeof vbuf, "%s", A->info.verify_req ? "pedida_no_aplicable(sin vram_end, 24 bits, escala>1 o sin clasificacion)" : "no");
    int r = snprintf(buf, (size_t)n,
        "[capa2d] escala=%d(raster %d, objetivo %d) retraso=%u envios=%u descartes=%u ejecutados=%u publicadas=%u caidas_shader(-1)=%u trabajador=%.1fms suavizado=%s vram_start=%s vram_end=%s marco=%dx%d aw=%d pad_l=%d %s prims=%d mascara=%d solo_vram=%d fidelidad=%d/%d dif=%.3f respaldo_emulador=%s | verificacion=%s",
        A->cur, A->info.raster, atomic_load(&A->target), A->have_pub ? A->sub_seq - A->pub_sub_seq : 0, A->n_sub, A->n_skip, A->n_run, A->n_pub, A->n_neg, A->work_ms, sm[sd],
        A->info.used_start ? "si" : "no", A->info.used_end ? "si" : "no", A->info.fw, A->info.h, A->info.aw, A->info.pad, A->info.full ? "completa" : "panel", A->info.prims, A->info.mask_prims,
        A->info.mirror_prims, A->info.fid_n - A->info.fid_bad, A->info.fid_n, A->info.fid_mean_milli / 1000.0, A->info.fallback ? "SI" : "no", vbuf);
    A->n_sub = A->n_skip = A->n_run = A->n_neg = A->n_pub = 0;
    pthread_mutex_unlock(&A->mx);
    return r;
}

void vl2a_flush(VrL2Async *A) {
    if (!A) return;
    pthread_mutex_lock(&A->mx);
    while (A->count || A->busy) pthread_cond_wait(&A->cv_idle, &A->mx);
    pthread_mutex_unlock(&A->mx);
}
