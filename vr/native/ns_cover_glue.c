/* ns_cover_glue: comprobación de COBERTURA en la app, en tiempo real y sin coste para el hilo de emulación (ver ns_cover.h).
 * Cada MV_COVER_N fotogramas (def. 25; 0 = apagado) el hilo de emulación copia una instantánea de la RAM, los 1 KiB rápidos y la lista de la GPU recién cerrada
 * (unos 2 MiB, ~0,3 ms) y despierta a UN hilo trabajador que extrae la escena con su propio almacén de mallas, la empareja con la lista (ns_cover_run) y deja el
 * texto del registro; si el trabajador sigue ocupado, el fotograma se omite (nunca hay cola). vrhost lo imprime una vez por segundo (ns_cover_glue_poll).
 * RAM de la instantánea: la de la IK de los brazos (vrbody_apply ya aplicó la pose; ns_cover_glue_post_ik la copia en el fotograma anterior a la entrega de la lista)
 * si hay capa de cuerpo; si no, la del propio gancho de fin de fotograma. vrhost tiene definiciones débiles vacías de estas tres funciones (como ns_extract_register). */
#include "ns_cover.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COVER_MAX_OPS 8192u
#define COVER_RAM 0x200000u

static struct {
    pthread_mutex_t mx;
    pthread_cond_t cv;
    pthread_t th;
    int started, init, off;
    unsigned every, counter;
    uint8_t *ram, *snap;
    uint8_t scratch[1024], snap_scratch[1024];
    GpuOp *ops;
    uint32_t nops, frame, snap_counter;
    int snap_valid, job, busy;
    int have_cam; float cam_eye[3], cam_M[9];   /* cámara con la que se emitió la lista de esta tarea */
    char text[8192];
    int text_new;
    unsigned skipped_busy, runs, fails;
    uint32_t last_frame; int last_pct, last_valid;   /* última medida numérica: fotograma de la lista y cobertura en % redondeada */
} G = {.mx = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER};

static void *worker(void *arg) {
    (void)arg;
    NsCover *c = ns_cover_create();
    for (;;) {
        pthread_mutex_lock(&G.mx);
        while (!G.job) pthread_cond_wait(&G.cv, &G.mx);
        uint32_t frame = G.frame, nops = G.nops;
        int jcam = G.have_cam; float ceye[3], cM[9];
        memcpy(ceye, G.cam_eye, sizeof ceye); memcpy(cM, G.cam_M, sizeof cM);
        pthread_mutex_unlock(&G.mx);
        /* G.ram / G.ops / G.scratch solo los toca el hilo de emulación mientras busy == 0; con job == 1 son del trabajador */
        char buf[sizeof G.text];
        buf[0] = 0;
        int rc = c ? -1 : -3;
        NsCoverResult *r = (NsCoverResult *)malloc(sizeof *r);
        if (c && r) {
            NsCoverCfg cfg;
            ns_cover_cfg_default(&cfg);
            if (jcam) { cfg.list_eye = ceye; cfg.list_M = cM; }
            rc = ns_cover_run(c, G.ram, G.scratch, G.ops, nops, &cfg, r);
            if (rc == 0) ns_cover_format(r, frame, buf, sizeof buf);
        }
        int pct = (rc == 0 && r) ? (int)(r->cover_pct + 0.5f) : 0;
        free(r);
        pthread_mutex_lock(&G.mx);
        if (rc == 0) {
            memcpy(G.text, buf, sizeof G.text); G.text_new = 1; G.runs++;
            G.last_frame = frame; G.last_valid = 1;
            G.last_pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
        } else G.fails++;   /* rc != 0: sin escena (menú, vídeo): nada que decir */
        G.job = 0; G.busy = 0;
        pthread_mutex_unlock(&G.mx);
    }
    return NULL;
}

static int ensure_started(void) {   /* con G.mx tomado */
    if (!G.init) {
        const char *e = getenv("MV_COVER_N");
        G.every = e ? (unsigned)strtoul(e, NULL, 10) : 25u;
        G.init = 1;
        if (!G.every) G.off = 1;
    }
    if (G.off) return 0;
    if (!G.ram) {
        G.ram = (uint8_t *)malloc(COVER_RAM);
        G.snap = (uint8_t *)malloc(COVER_RAM);
        G.ops = (GpuOp *)malloc((size_t)COVER_MAX_OPS * sizeof(GpuOp));
        if (!G.ram || !G.snap || !G.ops) { free(G.ram); free(G.snap); free(G.ops); G.ram = G.snap = NULL; G.ops = NULL; G.off = 1; return 0; }
    }
    if (!G.started) {
        if (pthread_create(&G.th, NULL, worker, NULL) != 0) { G.off = 1; return 0; }
        pthread_detach(G.th);
        G.started = 1;
    }
    return 1;
}

/* Hilo de emulación, tras vrbody_apply: copia la RAM con la pose IK del fotograma anterior a la entrega de la lista. */
void ns_cover_glue_post_ik(const uint8_t *ram) {
    if (G.off || !ram) return;
    pthread_mutex_lock(&G.mx);
    if (ensure_started() && !G.busy && !G.snap_valid && G.every && (G.counter + 1u) % G.every == 0) {
        memcpy(G.snap, ram, COVER_RAM);
        G.snap_counter = G.counter; G.snap_valid = 1;
    }
    pthread_mutex_unlock(&G.mx);
}

/* Hilo de emulación, al cerrarse una lista de la GPU. Barato salvo 1 de cada N llamadas. */
void ns_cover_glue_submit_cam(const uint8_t *ram, const uint8_t *scratch, const GpuOp *ops, uint32_t nops, uint32_t frame, const float *eye, const float *M) {
    if (G.off || !ram || !ops || !nops || nops > COVER_MAX_OPS) return;
    pthread_mutex_lock(&G.mx);
    if (!ensure_started() || !G.every) { pthread_mutex_unlock(&G.mx); return; }
    unsigned cnt = ++G.counter;
    if (cnt % G.every != 0) {
        if (G.snap_valid && cnt - G.snap_counter > 3u) G.snap_valid = 0;   /* instantánea caducada */
        pthread_mutex_unlock(&G.mx);
        return;
    }
    if (G.busy) { G.skipped_busy++; G.snap_valid = 0; pthread_mutex_unlock(&G.mx); return; }
    int has3d = 0;
    for (uint32_t i = 0; i < nops && !has3d; i++) has3d = ops[i].type == GPU_OP_POLY && ops[i].v[0].vvalid;
    if (!has3d) { G.snap_valid = 0; pthread_mutex_unlock(&G.mx); return; }
    if (G.snap_valid && cnt - G.snap_counter <= 3u) memcpy(G.ram, G.snap, COVER_RAM);
    else memcpy(G.ram, ram, COVER_RAM);
    G.snap_valid = 0;
    if (scratch) memcpy(G.scratch, scratch, sizeof G.scratch); else memset(G.scratch, 0, sizeof G.scratch);
    memcpy(G.ops, ops, (size_t)nops * sizeof(GpuOp));
    G.have_cam = eye && M;
    if (G.have_cam) { memcpy(G.cam_eye, eye, sizeof G.cam_eye); memcpy(G.cam_M, M, sizeof G.cam_M); }
    G.nops = nops; G.frame = frame; G.busy = 1; G.job = 1;
    pthread_cond_signal(&G.cv);
    pthread_mutex_unlock(&G.mx);
}

void ns_cover_glue_submit(const uint8_t *ram, const uint8_t *scratch, const GpuOp *ops, uint32_t nops, uint32_t frame) { ns_cover_glue_submit_cam(ram, scratch, ops, nops, frame, NULL, NULL); }

/* Cualquier hilo: copia el último texto de registro aún no leído (cabe en cap) y devuelve su longitud; 0 si no hay nada nuevo. */
size_t ns_cover_glue_poll(char *buf, size_t cap) {
    size_t n = 0;
    pthread_mutex_lock(&G.mx);
    if (G.text_new && buf && cap) {
        n = strlen(G.text);
        if (n >= cap) n = cap - 1;
        memcpy(buf, G.text, n);
        buf[n] = 0;
        G.text_new = 0;
    }
    pthread_mutex_unlock(&G.mx);
    return n;
}

/* Cualquier hilo: última medida de cobertura (fotograma de la lista y % de la lista de la GPU que representa la escena nativa). 0 si aún no hay ninguna. */
int ns_cover_glue_last(uint32_t *frame, int *pct) {
    pthread_mutex_lock(&G.mx);
    int ok = G.last_valid;
    if (ok) { if (frame) *frame = G.last_frame; if (pct) *pct = G.last_pct; }
    pthread_mutex_unlock(&G.mx);
    return ok;
}
