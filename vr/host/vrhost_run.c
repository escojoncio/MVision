/* Prueba del host completo (runtime + capa VR) con datos locales: arranca mv_host_start en MV_NOGEN, hace de "hilo de render"
 * (publica la pose de la cabeza y pide escenas con mv_host_scene) y de "hilo de entrada" (guion de mando por mv_host_pad), y
 * comprueba el contrato de las escenas. No lleva datos del juego; los lee de las rutas que se le pasan.
 *   vrhost_run <exe> <disco> [frames] [--pose fija|barrido] [--no-scene]
 *   vrhost_run --mkbin <carpeta_disco> <salida.bin>   escribe una imagen MODE2/2352 del disco sintetizado de una carpeta
 * MV_SIGS = fichero de firmas (tools/sig/hlesigs.py). Código de salida 0 = todo bien. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "bridge.h"
#include "cdrom.h"
#include "gpu.h"
#include "host.h"
#include "vr_camera.h"
#include "vrhost.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "FALLO %d: ", __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); fails++; } } while (0)

static int mkbin(const char *dir, const char *out) {
    if (!cd_open(dir)) { fprintf(stderr, "sin disco en %s\n", dir); return 1; }
    uint8_t s[2340];
    cd_read_sector(16, s, 0);
    uint32_t total = 0;
    uint8_t pvd[2048];
    cd_read_sector(16, pvd, 0);
    memcpy(&total, pvd + 80, 4);
    total -= 150;
    FILE *f = fopen(out, "wb");
    if (!f) { perror(out); return 1; }
    static const uint8_t sync[12] = {0, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 0};
    for (uint32_t l = 0; l < total; l++) {
        cd_read_sector(l, s, 1);
        fwrite(sync, 1, 12, f);
        fwrite(s, 1, 2340, f);
    }
    fclose(f);
    fprintf(stderr, "imagen: %u sectores -> %s\n", total, out);
    return 0;
}

typedef struct { uint32_t start, dur, every; uint16_t mask; } PadEv;
static const PadEv PAD[] = {{120, 6, 0, 1u << 3}, {400, 6, 0, 1u << 3}, {0, 6, 97, 1u << 13}, {0, 6, 211, 1u << 7}};
static uint16_t pad_at(uint32_t f) {
    uint16_t b = 0;
    for (unsigned i = 0; i < sizeof PAD / sizeof PAD[0]; i++) {
        const PadEv *p = &PAD[i];
        if (p->every) { if (f >= p->every && f % p->every < p->dur) b |= p->mask; }
        else if (f >= p->start && f < p->start + p->dur) b |= p->mask;
    }
    return b;
}

static void head_pose(float yaw_deg, float out[16]) {
    float y = yaw_deg * 3.14159265f / 180.0f, c = cosf(y), s = sinf(y);
    float m[16] = {c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, 0, 1.65f, 0, 1};
    memcpy(out, m, sizeof m);
}

int main(int argc, char **argv) {
    if (argc >= 4 && !strcmp(argv[1], "--mkbin")) return mkbin(argv[2], argv[3]);
    if (argc < 3) { fprintf(stderr, "uso: %s <exe> <disco> [frames] [--pose fija|barrido] [--no-scene]\n", argv[0]); return 1; }
    const char *exe = argv[1], *disc = argv[2];
    uint32_t nframes = argc > 3 && argv[3][0] != '-' ? (uint32_t)strtoul(argv[3], 0, 0) : 7000;
    int sweep = 1, scenes_on = 1;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--pose") && i + 1 < argc) sweep = !strcmp(argv[++i], "barrido");
        else if (!strcmp(argv[i], "--no-scene")) scenes_on = 0;
    }
    const char *sigs = getenv("MV_SIGS");
    if (sigs) mv_host_set_sigs(sigs);
    mkdir("build-host", 0755);
    mkdir("build-host/card", 0755);
    mv_vrhost_install();
    mv_host_set_turbo(1);
    int r = mv_host_start(exe, disc, "build-host/card");
    if (r) { fprintf(stderr, "mv_host_start = %d: %s\n", r, mv_host_error()); return 1; }

    uint32_t last_f = 0xFFFFFFFFu, last_idx = 0;
    int have_idx = 0;
    uint32_t n_scenes = 0, n_3d = 0, n_wv = 0, n_game3d = 0, first_game3d = 0, n_resync = 0, n_gap_no_init = 0, n_ev_bad = 0;
    uint32_t n_vtx = 0, max_vtx = 0, n_tri3d_game = 0;
    float yaw = 0;
    uint32_t cam_f = 0;
    for (;;) {
        uint32_t f = mv_host_frame_count();
        MvHostState st = mv_host_state();
        if (st == MV_HOST_ERROR) { fprintf(stderr, "error del motor en el frame %u: %s\n", f, mv_host_error()); fails++; break; }
        if (f >= nframes) break;
        if (f != last_f) {
            last_f = f;
            mv_host_pad(pad_at(f), 128, 128, 128, 128);
            if (sweep && f > 3000) yaw = 60.0f * sinf((float)(f - 3000) * 0.02f);
            MvVrGameState gs;
            mv_vrhost_game_state(&gs);
            if (gs.cam_active && !cam_f) cam_f = f;
            if (f % 1000 == 0) {
                MvVrHostStats s; mv_vrhost_stats(&s);
                fprintf(stderr, "[run] f=%u partida=%d control=%d cam=%d escenas=%u (3D %u, mundo %u) esperas=%u\n", f, gs.game, gs.in_control, gs.cam_active, s.built, s.has3d, s.world_valid, s.full_waits);
            }
        }
        float m[16];
        head_pose(yaw, m);
        mv_vr_set_head(m, 0.0);
        int got = 0;
        if (scenes_on) for (int k = 0; k < 4; k++) {
            VrScene sc;
            if (!mv_host_scene(&sc)) break;
            got++;
            n_scenes++;
            CHECK(sc.src != NULL, "escena sin lista");
            if (!sc.src) continue;
            uint32_t idx = sc.src->index;
            if (have_idx && idx != last_idx + 1) {
                n_resync++;
                if (!sc.src->vram_init) n_gap_no_init++;   /* salto de lista sin instantánea de VRAM: el consumidor divergiría */
            }
            if (!have_idx && !sc.src->vram_init) CHECK(0, "la primera lista no trae vram_init");
            have_idx = 1; last_idx = idx;
            if (sc.has_3d) n_3d++;
            if (sc.world_valid) n_wv++;
            if (sc.has_3d && sc.world_valid) { n_game3d++; if (!first_game3d) first_game3d = sc.src->host_frame; n_tri3d_game += sc.stats.n_tris3d; }
            n_vtx += sc.nv; if (sc.nv > max_vtx) max_vtx = sc.nv;
            uint32_t prev = 0;
            for (uint32_t i = 0; i < sc.nb; i++) {
                const VrBatch *b = &sc.b[i];
                if (b->ev_end < prev || b->ev_end > sc.src->nops || (uint64_t)b->first + b->count > sc.nv) n_ev_bad++;
                prev = b->ev_end;
            }
            for (uint32_t i = 0; i < sc.src->nops; i++) {
                int t = sc.src->ops[i].type;
                CHECK(t == GPU_OP_FILL || t == GPU_OP_VCOPY || t == GPU_OP_UPLOAD, "op no-evento en la lista compacta (tipo %d)", t);
                if (t == GPU_OP_UPLOAD) CHECK(sc.src->ops[i].data_off <= sc.src->ndata, "data_off fuera de datos");
            }
        }
        if (!got) usleep(300);
    }
    mv_host_pause();
    usleep(200000);
    MvVrHostStats s; mv_vrhost_stats(&s);
    VrCamStats cs; vrcam_stats(&cs);
    MvVrGameState gs; mv_vrhost_game_state(&gs);
    BridgeState *bs = malloc(sizeof *bs);
    int hb = mv_host_bridge(bs);
    fprintf(stderr, "[run] FIN f=%u estado=%d\n", mv_host_frame_count(), (int)mv_host_state());
    fprintf(stderr, "[run] escenas entregadas %u (ensambladas %u): con 3D %u, mundo válido %u, 3D+mundo (partida) %u (primera en frame %u, %u triángulos 3D en total)\n",
            n_scenes, s.built, n_3d, n_wv, n_game3d, first_game3d, n_tri3d_game);
    fprintf(stderr, "[run] vértices por escena: media %u, máx %u; lista con salto: %u (sin vram_init: %u); batches con ev_end inválido: %u\n",
            n_scenes ? n_vtx / n_scenes : 0, max_vtx, n_resync, n_gap_no_init, n_ev_bad);
    fprintf(stderr, "[run] ensamblado: media %.3f ms, máx %.3f ms; esperas de cola %u; encendidos de captura %u; registro de cámara hallado %u (distinto del puente: %u) / ausente %u\n",
            s.build_ms_avg, s.build_ms_max, s.full_waits, s.cap_resyncs, s.cam_rec_found, s.cam_rec_differs, s.cam_rec_missing);
    fprintf(stderr, "[run] cámara VR: parche+HLE %s; primera activación en frame %u; construcciones %u (VR %u), activaciones %u, desactivaciones %u, recentrados %u, cabeza perdida %u\n",
            s.cam_ok ? "sí" : "NO", cam_f, cs.builds, cs.vr_builds, cs.activations, cs.deactivations, cs.recenters, cs.head_misses);
    fprintf(stderr, "[run] puente: publicado=%d modo=%s partida=%d control=%d postura=%d\n", hb, hb ? bridge_mode_name(bs) : "-", gs.game, gs.in_control, gs.posture);
    free(bs);

    if (scenes_on) {
        CHECK(s.built > 100, "muy pocas escenas ensambladas (%u)", s.built);
        CHECK(n_game3d > 50, "muy pocas escenas con 3D y mundo válido (%u)", n_game3d);
        CHECK(n_gap_no_init == 0, "saltos de lista sin vram_init: %u", n_gap_no_init);
        CHECK(n_ev_bad == 0, "batches con ev_end o rango inválido: %u", n_ev_bad);
        CHECK(s.cam_ok, "la cámara VR no se activó en el host (parche/HLE)");
        CHECK(cs.vr_builds > 100, "la cámara VR casi no construyó cámaras (%u)", cs.vr_builds);
    }
    fprintf(stderr, fails ? "vrhost_run: %d FALLOS\n" : "vrhost_run: OK\n", fails);
    return fails ? 1 : 0;
}
