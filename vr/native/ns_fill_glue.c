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
#define FG_STALE 20u   /* fotogramas sin ejecución tras los que el resultado deja de servirse */
#define FG_COV_MIN 8u   /* triángulos 3D (sin pose) mínimos para dar una medida de cobertura */
#define FG_KEEP 5u     /* fotogramas que se conserva el resultado anterior ante un fallo puntual del trabajador */

/* a - b en fotogramas; devuelve UINT32_MAX si el contador retrocedió (carga de partida): el resultado es de otra época */
static uint32_t fdiff(uint32_t a, uint32_t b) { return a >= b ? a - b : 0xFFFFFFFFu; }

static struct {
    pthread_mutex_t mx; pthread_cond_t cv; pthread_t th;
    int started, init, off, busy, job, snap_valid, reset;
    unsigned every, counter;
    uint8_t *ram, *snap; uint8_t scratch[1024];
    GpuOp *ops; uint32_t nops, frame;
    int have_cam, list_vr; float cam_eye[3], cam_M[9];
    NsEffect *out; uint32_t n_out;      /* resultado vigente (copia) */
    uint32_t res_frame, sub_frame, rgen;   /* fotograma del último resultado / de la última lista recibida / generación de reset */
    uint32_t cov_frame; int cov_pct, cov_valid;   /* última medida de cobertura (nativo + relleno, sin pose): fotograma de la lista y % */
    char text[1024]; int text_new; unsigned runs, fails, skipped_busy; uint64_t seen_frame;
} G = {.mx = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER};

/* Cobertura que vería el usuario dentro de la vista del juego: lo que cubre el nativo MÁS lo que rellena ns_fill (ns_fill_run: (MATCH+SPLIT+SUBDIV+LOCKED+rellenados) / (tris - pose)).
 * Con menos de FG_COV_MIN triángulos sin pose no hay escena que medir (cover_pct vale 100 sin denominador): no cuenta. Con G.mx tomado. */
static void fill_cover_note(const NsFillStats *st, uint32_t frame) {
    uint32_t pose = st->cls[NF_POSE_PLAYER] + st->cls[NF_POSE_OTHER] + st->cls[NF_SKIP_NEAR];   /* como el denominador de ns_fill_run: lo pegado a la cámara tampoco cuenta */
    if (st->tris < pose + FG_COV_MIN) return;
    int pct = (int)(st->cover_pct + 0.5f);
    G.cov_pct = pct < 0 ? 0 : pct > 100 ? 100 : pct; G.cov_frame = frame; G.cov_valid = 1;
}

static void cover_clear(void);

static void *worker(void *arg) {
    (void)arg;
    NsFillSession *s = ns_fill_session_create();
    uint32_t zlast_g = 0, zlast_e = 0; int zlast_ok = 0;
    for (;;) {
        pthread_mutex_lock(&G.mx);
        while (!G.job) pthread_cond_wait(&G.cv, &G.mx);
        uint32_t frame = G.frame, nops = G.nops;
        NsFillRunOpts ro; memset(&ro, 0, sizeof ro);
        float ceye[3], cM[9];
        memcpy(ceye, G.cam_eye, sizeof ceye); memcpy(cM, G.cam_M, sizeof cM);
        if (G.have_cam) { ro.list_eye = ceye; ro.list_M = cM; ro.list_vr = G.list_vr; }
        int rst = G.reset; G.reset = 0;
        uint32_t rgen = G.rgen;
        pthread_mutex_unlock(&G.mx);
        /* G.ram / G.ops / G.scratch son del trabajador mientras job == 1 */
        int rc = -3;
        NsFillStats st; memset(&st, 0, sizeof st);
        if (s) {
            if (rst) ns_fill_reset(ns_fill_session_fill(s));
            rc = ns_fill_session_run(s, G.ram, G.scratch, G.ops, nops, &ro, NULL, frame, &st, NULL);
        }
        uint32_t zg = 0, ep = 0; int haveZone = s && ns_fill_session_zone(s, &zg, &ep) == 0;
        int zchg = rc == 0 && haveZone && zlast_ok && (zg != zlast_g || ep != zlast_e);   /* cambio de zona o de época del extractor */
        if (rc == 0 && haveZone) { zlast_g = zg; zlast_e = ep; zlast_ok = 1; }
        if (zchg) ns_fill_reset(ns_fill_session_fill(s));
        pthread_mutex_lock(&G.mx);
        if (rc == 0 && (rgen == G.rgen || zchg)) fill_cover_note(&st, frame);   /* una medida de la escena anterior (reset pedido durante la ejecución) no repone la que el reset borró; con zona nueva la lista ya es la de la zona nueva */
        if (rgen != G.rgen || zchg) { G.n_out = 0; if (zchg) G.rgen++; }   /* reset pedido durante la ejecución, o zona nueva: el resultado es de la escena anterior */
        else if (rc == 0) {
            uint32_t n = 0; const NsEffect *e = ns_fill_session_effects(s, &n);
            if (n > FG_OUT) n = FG_OUT;
            if (n) memcpy(G.out, e, (size_t)n * sizeof(NsEffect));
            G.n_out = n; G.runs++; G.res_frame = frame;
            if (frame - G.seen_frame >= 60u || !G.seen_frame) {   /* ~1 línea por segundo a 60 fotogramas */
                G.seen_frame = frame;
                ns_fill_format(&st, frame, G.text, sizeof G.text); G.text_new = 1;
            }
        } else {   /* fallo puntual: se conserva el resultado anterior unos pocos fotogramas; después, sin escena (menú, vídeo): nada que rellenar */
            G.fails++;
            if (fdiff(frame, G.res_frame) > FG_KEEP) G.n_out = 0;
        }
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
        if (e && e[0] == '0') { G.off = 1; cover_clear(); }   /* MV_FILL=0: ninguna medida de relleno */
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
    if (G.off || !ram || !ops || nops > FG_MAX_OPS) return;
    pthread_mutex_lock(&G.mx);
    if (!ensure_started()) { pthread_mutex_unlock(&G.mx); return; }
    G.sub_frame = frame;
    if (!nops) { G.reset = 1; G.rgen++; G.n_out = 0; G.snap_valid = 0; pthread_mutex_unlock(&G.mx); return; }   /* lista vacía = sin 3D */
    unsigned cnt = ++G.counter;
    if (cnt % G.every != 0) { G.snap_valid = 0; pthread_mutex_unlock(&G.mx); return; }
    if (G.busy) { G.skipped_busy++; G.snap_valid = 0; pthread_mutex_unlock(&G.mx); return; }
    int has3d = 0;
    for (uint32_t i = 0; i < nops && !has3d; i++) has3d = ops[i].type == GPU_OP_POLY && ops[i].v[0].vvalid;
    if (!has3d) { G.snap_valid = 0; G.reset = 1; G.rgen++; G.n_out = 0; pthread_mutex_unlock(&G.mx); return; }   /* pantalla 2D: la persistencia no vale para la siguiente escena */
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
    if (!G.off && G.out && G.n_out && fdiff(G.sub_frame, G.res_frame) <= FG_STALE) { n = G.n_out < max ? G.n_out : max; memcpy(cap, G.out, (size_t)n * sizeof(NsEffect)); }
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

int ns_fill_glue_last(uint32_t *frame, int *pct) {
    pthread_mutex_lock(&G.mx);
    int ok = !G.off && G.cov_valid;
    if (ok) { if (frame) *frame = G.cov_frame; if (pct) *pct = G.cov_pct; }
    pthread_mutex_unlock(&G.mx);
    return ok;
}

/* Cambio de escena / de clase de vista: la medida de cobertura es de la escena anterior y no debe sobrevivir (la siguiente la repone) */
static void cover_clear(void) { G.cov_valid = 0; G.cov_frame = 0; G.cov_pct = 0; }
void ns_fill_glue_reset(void) { pthread_mutex_lock(&G.mx); G.reset = 1; G.rgen++; G.n_out = 0; cover_clear(); pthread_mutex_unlock(&G.mx); }

#ifdef NS_FILL_GLUE_TEST
/* Solo pruebas: registra una medida de cobertura como la haría el trabajador tras una ejecución. */
void ns_fill_glue_test_cover(const NsFillStats *st, uint32_t frame) { pthread_mutex_lock(&G.mx); fill_cover_note(st, frame); pthread_mutex_unlock(&G.mx); }
/* Solo pruebas: coloca un resultado vigente a mano (sin escena real) para comprobar la caducidad y los vaciados. */
void ns_fill_glue_test_set(const NsEffect *e, uint32_t n, uint32_t res_frame, uint32_t sub_frame) {
    pthread_mutex_lock(&G.mx);
    if (ensure_started() && n <= FG_OUT) { if (n) memcpy(G.out, e, (size_t)n * sizeof(NsEffect)); G.n_out = n; G.res_frame = res_frame; G.sub_frame = sub_frame; }
    pthread_mutex_unlock(&G.mx);
}
#endif
