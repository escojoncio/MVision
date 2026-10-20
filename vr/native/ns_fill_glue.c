/* ns_fill_glue: ver ns_fill_glue.h. Mismo patrón que ns_cover_glue.c: copia de RAM y lista en el hilo de emulación, un trabajador, sin cola (si está ocupado, se omite la lista;
 * la persistencia del relleno cubre el hueco). */
#include "ns_fill_glue.h"
#include "ns_fill.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FG_MAX_OPS 8192u
#define FG_RAM 0x200000u
#define FG_OUT 6000u

static struct {
    pthread_mutex_t mx; pthread_cond_t cv; pthread_t th;
    int started, init, off, busy, job, snap_valid, reset;
    unsigned every, counter;
    uint8_t *ram, *snap; uint8_t scratch[1024];
    GpuOp *ops; uint32_t nops, frame;
    int have_cam, list_vr; float cam_eye[3], cam_M[9];
    NsEffect *out; uint32_t n_out;      /* resultado vigente (copia) */
    char text[1024]; int text_new; unsigned runs, fails, skipped_busy; uint64_t seen_frame;
} G = {.mx = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER};

static void *worker(void *arg) {
    (void)arg;
    NsFillSession *s = ns_fill_session_create();
    for (;;) {
        pthread_mutex_lock(&G.mx);
        while (!G.job) pthread_cond_wait(&G.cv, &G.mx);
        uint32_t frame = G.frame, nops = G.nops;
        NsFillRunOpts ro; memset(&ro, 0, sizeof ro);
        float ceye[3], cM[9];
        memcpy(ceye, G.cam_eye, sizeof ceye); memcpy(cM, G.cam_M, sizeof cM);
        if (G.have_cam) { ro.list_eye = ceye; ro.list_M = cM; ro.list_vr = G.list_vr; }
        int rst = G.reset; G.reset = 0;
        pthread_mutex_unlock(&G.mx);
        /* G.ram / G.ops / G.scratch son del trabajador mientras job == 1 */
        int rc = -3;
        NsFillStats st; memset(&st, 0, sizeof st);
        if (s) {
            if (rst) ns_fill_reset(ns_fill_session_fill(s));
            rc = ns_fill_session_run(s, G.ram, G.scratch, G.ops, nops, &ro, NULL, frame, &st, NULL);
        }
        pthread_mutex_lock(&G.mx);
        if (rc == 0) {
            uint32_t n = 0; const NsEffect *e = ns_fill_session_effects(s, &n);
            if (n > FG_OUT) n = FG_OUT;
            if (n) memcpy(G.out, e, (size_t)n * sizeof(NsEffect));
            G.n_out = n; G.runs++;
            if (frame - G.seen_frame >= 60u || !G.seen_frame) {   /* ~1 línea por segundo a 60 fotogramas */
                G.seen_frame = frame;
                ns_fill_format(&st, frame, G.text, sizeof G.text); G.text_new = 1;
            }
        } else { G.fails++; G.n_out = 0; }   /* sin escena (menú, vídeo): nada que rellenar */
        G.job = 0; G.busy = 0;
        pthread_mutex_unlock(&G.mx);
    }
    return NULL;
}

static int ensure_started(void) {   /* con G.mx tomado */
    if (!G.init) {
        const char *e = getenv("MV_FILL"), *n = getenv("MV_FILL_N");
        G.every = n ? (unsigned)strtoul(n, NULL, 10) : 1u;
        if (!G.every) G.every = 1u;
        G.init = 1;
        if (e && e[0] == '0') G.off = 1;
    }
    if (G.off) return 0;
    if (!G.ram) {
        G.ram = (uint8_t *)malloc(FG_RAM); G.snap = (uint8_t *)malloc(FG_RAM);
        G.ops = (GpuOp *)malloc((size_t)FG_MAX_OPS * sizeof(GpuOp)); G.out = (NsEffect *)malloc((size_t)FG_OUT * sizeof(NsEffect));
        if (!G.ram || !G.snap || !G.ops || !G.out) { free(G.ram); free(G.snap); free(G.ops); free(G.out); G.ram = G.snap = NULL; G.ops = NULL; G.out = NULL; G.off = 1; return 0; }
    }
    if (!G.started) {
        if (pthread_create(&G.th, NULL, worker, NULL) != 0) { G.off = 1; return 0; }
        pthread_detach(G.th);
        G.started = 1;
    }
    return 1;
}

void ns_fill_glue_post_ik(const uint8_t *ram) {
    if (G.off || !ram) return;
    pthread_mutex_lock(&G.mx);
    if (ensure_started() && !G.busy && !G.snap_valid && (G.counter + 1u) % G.every == 0) { memcpy(G.snap, ram, FG_RAM); G.snap_valid = 1; }
    pthread_mutex_unlock(&G.mx);
}

void ns_fill_glue_submit_cam(const uint8_t *ram, const uint8_t *scratch, const GpuOp *ops, uint32_t nops, uint32_t frame, const float *eye, const float *M, int list_vr) {
    if (G.off || !ram || !ops || !nops || nops > FG_MAX_OPS) return;
    pthread_mutex_lock(&G.mx);
    if (!ensure_started()) { pthread_mutex_unlock(&G.mx); return; }
    unsigned cnt = ++G.counter;
    if (cnt % G.every != 0) { G.snap_valid = 0; pthread_mutex_unlock(&G.mx); return; }
    if (G.busy) { G.skipped_busy++; G.snap_valid = 0; pthread_mutex_unlock(&G.mx); return; }
    int has3d = 0;
    for (uint32_t i = 0; i < nops && !has3d; i++) has3d = ops[i].type == GPU_OP_POLY && ops[i].v[0].vvalid;
    if (!has3d) { G.snap_valid = 0; pthread_mutex_unlock(&G.mx); return; }
    if (G.snap_valid) { uint8_t *t = G.ram; G.ram = G.snap; G.snap = t; }   /* la instantánea con la pose IK pasa al trabajador sin copiarla otra vez */
    else memcpy(G.ram, ram, FG_RAM);
    G.snap_valid = 0;
    if (scratch) memcpy(G.scratch, scratch, sizeof G.scratch); else memset(G.scratch, 0, sizeof G.scratch);
    memcpy(G.ops, ops, (size_t)nops * sizeof(GpuOp));
    G.have_cam = eye && M; G.list_vr = list_vr;
    if (G.have_cam) { memcpy(G.cam_eye, eye, sizeof G.cam_eye); memcpy(G.cam_M, M, sizeof G.cam_M); }
    G.nops = nops; G.frame = frame; G.busy = 1; G.job = 1;
    pthread_cond_signal(&G.cv);
    pthread_mutex_unlock(&G.mx);
}

uint32_t ns_fill_glue_fetch(NsEffect *cap, uint32_t max) {
    uint32_t n = 0;
    if (!cap || !max) return 0;
    pthread_mutex_lock(&G.mx);
    if (!G.off && G.out && G.n_out) { n = G.n_out < max ? G.n_out : max; memcpy(cap, G.out, (size_t)n * sizeof(NsEffect)); }
    pthread_mutex_unlock(&G.mx);
    return n;
}

size_t ns_fill_glue_poll(char *buf, size_t cap) {
    size_t n = 0;
    pthread_mutex_lock(&G.mx);
    if (G.text_new && buf && cap) { n = strlen(G.text); if (n >= cap) n = cap - 1; memcpy(buf, G.text, n); buf[n] = 0; G.text_new = 0; }
    pthread_mutex_unlock(&G.mx);
    return n;
}

void ns_fill_glue_reset(void) { pthread_mutex_lock(&G.mx); G.reset = 1; G.n_out = 0; pthread_mutex_unlock(&G.mx); }
