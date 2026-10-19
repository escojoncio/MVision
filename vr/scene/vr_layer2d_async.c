#include "vr_layer2d_async.h"
#include "vr_layer2d.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NJOB 16
#define L2A_STALE 3
#define SLOW_MS 12.0
#define FAST_MS 3.0
#define RECOVER 240

typedef struct {
    GpuDrawList l;                 /* ops/data/vram_init apuntan a la memoria propia de abajo */
    GpuOp *ops; size_t ops_cap;
    uint16_t *data; size_t data_cap;
    uint16_t *vram; int has_vram;
    uint8_t *opd; size_t opd_cap; int has_opd;
    int full, reset;
    uint32_t lseq;                 /* nº de envío */
} Job;

typedef struct { uint8_t *px; size_t cap; int w, h, full; uint32_t seq; } Out;

struct VrL2Async {
    pthread_t th; int started;
    pthread_mutex_t mx; pthread_cond_t cv_job, cv_idle;
    Job job[NJOB]; int head, count; int busy;   /* cola circular */
    int quit;
    VrLayer2D *L;
    _Atomic int target, smooth;
    int cur, fast;
    Out out[3]; int pub, held, have_pub;        /* índice publicado / en uso por el consumidor (-1) */
    uint32_t sub_seq, pub_sub_seq, acq_seq;     /* envíos / envío al que corresponde lo publicado / último adquirido */
    double work_ms, submit_max;
};

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
        int later = A->count > 1;          /* hay listas más nuevas esperando: de esta solo se aplican los eventos */
        A->busy = 1;
        int wi = 0, scale = A->cur;
        while (wi == A->pub || wi == A->held) wi++;   /* búfer libre (de 3, con 2 ocupados como máximo) */
        pthread_mutex_unlock(&A->mx);

        if (j->reset) vl2d_reset(A->L);
        int draw = j->has_opd && !later;
        vl2d_set_options(A->L, scale, atomic_load(&A->smooth));
        memset(&sc, 0, sizeof sc);
        if (draw) { sc.op_dest = j->opd; sc.n_op_dest = j->l.nops; }
        double t0 = now_ms();
        int have = vl2d_run(A->L, &j->l, draw ? &sc : NULL, j->full);
        double ms = now_ms() - t0;
        int w = 0, h = 0;
        const uint8_t *px = have ? vl2d_rgba(A->L, &w, &h) : NULL;
        Out *o = &A->out[wi];
        int ok = 0;
        if (px && w > 0 && h > 0 && grow((void **)&o->px, &o->cap, (size_t)w * h * 4, 1)) {
            memcpy(o->px, px, (size_t)w * h * 4);
            o->w = w; o->h = h; o->full = j->full; ok = 1;
        }

        pthread_mutex_lock(&A->mx);
        if (draw) {
            A->work_ms = ms;
            if (ms > SLOW_MS && A->cur > 1) { A->cur--; A->fast = 0; }
            else if (ms < FAST_MS && A->cur < atomic_load(&A->target) && ms * (double)((A->cur + 1) * (A->cur + 1)) / (double)(A->cur * A->cur) < SLOW_MS * 0.8) {   /* el coste crece con la escala al cuadrado: solo se sube si cabría */ if (++A->fast >= RECOVER) { A->cur++; A->fast = 0; } }
            else A->fast = 0;
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
    atomic_store(&A->target, 4); atomic_store(&A->smooth, VL_SMOOTH_SHARP);
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
    for (int i = 0; i < NJOB; i++) { free(A->job[i].ops); free(A->job[i].data); free(A->job[i].vram); free(A->job[i].opd); }
    for (int i = 0; i < 3; i++) free(A->out[i].px);
    vl2d_free(A->L);
    pthread_mutex_destroy(&A->mx); pthread_cond_destroy(&A->cv_job); pthread_cond_destroy(&A->cv_idle);
    free(A);
}

void vl2a_options(VrL2Async *A, int scale, int smooth) {
    if (!A) return;
    if (scale < 1) scale = 1;
    if (scale > 6) scale = 6;
    int old = atomic_exchange(&A->target, scale);
    if (old != scale) { pthread_mutex_lock(&A->mx); A->cur = scale; A->fast = 0; pthread_mutex_unlock(&A->mx); }
    atomic_store(&A->smooth, smooth < 0 ? 0 : smooth > 2 ? 2 : smooth);
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
             grow((void **)&j->opd, &j->opd_cap, l->nops, 1) && (!l->vram_init || (j->vram || (j->vram = malloc(sizeof(uint16_t) * 1024 * 512))));
    if (ok) {
        j->l = *l;
        if (l->nops) memcpy(j->ops, l->ops, l->nops * sizeof(GpuOp));
        if (l->ndata) memcpy(j->data, l->data, (size_t)l->ndata * 2);
        j->l.ops = j->ops; j->l.data = j->data;
        j->l.cap_ops = j->l.cap_data = 0; j->l.vram_after = NULL;
        j->has_vram = l->vram_init != NULL;
        if (j->has_vram) { memcpy(j->vram, l->vram_init, sizeof(uint16_t) * 1024 * 512); j->l.vram_init = j->vram; } else j->l.vram_init = NULL;
        j->has_opd = op_dest != NULL;
        if (op_dest && l->nops) memcpy(j->opd, op_dest, l->nops);
        j->full = full; j->reset = reset; j->lseq = ++A->sub_seq;
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
    if (!A->have_pub || A->pub < 0 || A->sub_seq - A->pub_sub_seq > L2A_STALE) { A->held = -1; pthread_mutex_unlock(&A->mx); return -1; }
    int r = 0;
    if (A->held != A->pub || A->out[A->pub].seq != A->acq_seq) {
        A->held = A->pub; A->acq_seq = A->out[A->pub].seq; r = 1;
    }
    const Out *o = &A->out[A->held];
    if (px) *px = o->px;
    if (w) *w = o->w;
    if (h) *h = o->h;
    if (full) *full = o->full;
    if (seq) *seq = o->seq;
    pthread_mutex_unlock(&A->mx);
    return r;
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

void vl2a_flush(VrL2Async *A) {
    if (!A) return;
    pthread_mutex_lock(&A->mx);
    while (A->count || A->busy) pthread_cond_wait(&A->cv_idle, &A->mx);
    pthread_mutex_unlock(&A->mx);
}
