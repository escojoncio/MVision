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
#include "bridge_addrs.h"
#include "sys.h"
#include "vr_body.h"
#include "vr_camera.h"
#include "vr_fire.h"
#include "vr_reload.h"
#include "vrgame.h"
#include "vrhost.h"
#include "hud_emu.h"

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
    int vr_auto = 0;              /* --auto: recarga automática del arma VR y el botón de soltar cargador pulsado cuando está vacía */
    int vr_inv = 0;               /* --inv: inventario de prueba (pistola equipada con munición) en la copia de prueba, SOLO para pruebas */
    uint32_t inv_f = 0;
    int vr_hands = 0;             /* --vr: publica manos sintéticas (brazos, radar, arma, gatillo) como haría la app */
    uint32_t n_radar = 0;
    uint32_t scene_from = 0;   /* --scene-from F: el consumidor de escenas aparece en el frame F (captura encendida tarde) */
    const char *hud_dir = NULL;   /* --hud DIR F1,F2,..: vuelca (PPM) el HUD que dibujaría Metal (emulado en CPU) de la primera escena con frame >= Fi */
    uint32_t hud_at[16]; int n_hud = 0;
    static uint16_t evram[1024 * 512];   /* VRAM del renderer: solo eventos */
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--scene-from") && i + 1 < argc) { scene_from = (uint32_t)strtoul(argv[++i], 0, 0); continue; }
        if (!strcmp(argv[i], "--hud") && i + 2 < argc) {
            hud_dir = argv[++i];
            char *dup = strdup(argv[++i]), *sv = NULL;
            for (char *t = strtok_r(dup, ",", &sv); t && n_hud < 16; t = strtok_r(NULL, ",", &sv)) hud_at[n_hud++] = (uint32_t)strtoul(t, 0, 0);
            free(dup);
            mkdir(hud_dir, 0755);
            continue;
        }
        if (!strcmp(argv[i], "--pose") && i + 1 < argc) sweep = !strcmp(argv[++i], "barrido");
        else if (!strcmp(argv[i], "--no-scene")) scenes_on = 0;
        else if (!strcmp(argv[i], "--vr")) vr_hands = 1;
        else if (!strcmp(argv[i], "--inv")) vr_inv = 1;
        else if (!strcmp(argv[i], "--auto")) vr_auto = 1;
    }
    const char *sigs = getenv("MV_SIGS");
    if (sigs) mv_host_set_sigs(sigs);
    mkdir("build-host", 0755);
    mkdir("build-host/card", 0755);
    mv_vrhost_install();
    if (vr_auto) mv_vrgame_set_options(1, 2);
    mv_host_set_turbo(getenv("MV_PACED") ? 0 : 1);   /* MV_PACED=1: al ritmo real (50 Hz) como en el visor */
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
            if (vr_hands) {   /* mandos Sense sintéticos: derecho con el gatillo a pulsos (tras 150 frames de cámara VR), izquierdo quieto */
                MvVrHand h;
                memset(&h, 0, sizeof h);
                h.valid = 1; h.rot[3] = 1;
                h.pos[0] = 0.16f; h.pos[1] = 1.30f; h.pos[2] = -0.36f;
                int fire = cam_f && f > cam_f + 150 && (f - cam_f) % 120 < 8;
                h.trigger = fire ? 1.0f : 0.0f;
                if (vr_auto) {
                    MvVrGameStatus g2; mv_vrgame_status(&g2);
                    h.up = g2.active && g2.mag_inserted && !g2.mag_rounds && !g2.chambered && (f % 200) < 8;
                }
                mv_vrgame_set_hand(1, &h);
                h.pos[0] = -0.20f; h.pos[1] = 1.22f; h.pos[2] = -0.30f; h.trigger = 0; h.up = 0;
                mv_vrgame_set_hand(0, &h);
            }
            if (vr_inv && cam_f && !inv_f && f > cam_f + 60) {   /* con el motor en pausa: arma 0 equipada y munición en las variables de partida */
                inv_f = f;
                mv_host_pause();
                usleep(300000);
                static const int ammo[10] = {60, 120, 6, 6, 4, 4, 4, 3, 3, 20}, mx[10] = {200, 400, 10, 10, 10, 10, 10, 10, 10, 100};
                for (int i = 0; i < 10; i++) {
                    *(int16_t *)(psx_ram + ((BA_VARS + 2u * (BRIDGE_V_WEAPON_AMMO + i)) & 0x1FFFFF)) = (int16_t)ammo[i];
                    *(int16_t *)(psx_ram + ((BA_VARS + 2u * (BRIDGE_V_WEAPON_MAX + i)) & 0x1FFFFF)) = (int16_t)mx[i];
                }
                *(int16_t *)(psx_ram + ((BA_VARS + 2u * BRIDGE_V_WEAPON) & 0x1FFFFF)) = 0;
                mv_host_resume();
            }
            if (vr_hands && f % 500 == 0 && cam_f) {
                MvVrGameStatus gst; mv_vrgame_status(&gst);
                fprintf(stderr, "[vr] f=%u arma: carga=%d activa=%d cargador=%d recámara=%d dentro=%d | juego mag=%d reserva=%d recarga=%d disparos=%u brazos=%u arma_juego=%d\n", f, gst.loader, gst.active, gst.mag_rounds,
                        gst.chambered, gst.mag_inserted, gst.game_mag, gst.game_reserve, gst.reload_state, gst.shots, gst.arms_driven, gs.weapon);
            }
            if (f % 1000 == 0) {
                MvVrHostStats s; mv_vrhost_stats(&s);
                fprintf(stderr, "[run] f=%u partida=%d control=%d cam=%d escenas=%u (3D %u, mundo %u) esperas=%u\n", f, gs.game, gs.in_control, gs.cam_active, s.built, s.has3d, s.world_valid, s.full_waits);
            }
        }
        float m[16];
        head_pose(yaw, m);
        mv_vr_set_head(m, 0.0);
        int got = 0;
        if (scenes_on && f >= scene_from) for (int k = 0; k < 4; k++) {
            VrScene sc;
            if (!mv_host_scene(&sc)) break;
            got++;
            n_scenes++;
            { VrRadarScene rs; if (mv_host_radar(&rs)) n_radar++; }
            CHECK(sc.src != NULL, "escena sin lista");
            if (!sc.src) continue;
            if (hud_dir) {   /* igual que PS1Renderer: instantánea completa si la trae y eventos en orden */
                if (sc.src->vram_init) memcpy(evram, sc.src->vram_init, sizeof evram);
                int want = 0;
                for (int q = 0; q < n_hud; q++) if (hud_at[q] && sc.src->host_frame >= hud_at[q]) { hud_at[q] = 0; want = 1; }
                if (want) {
                    HudImg im = hud_img_new(sc.src->disp.w, sc.src->disp.h);
                    hud_emu_run(&sc, evram, &im, sc.src->disp.x, sc.src->disp.y);
                    uint8_t *rgb = malloc((size_t)im.w * im.h * 3);
                    hud_emu_to_rgb(&im, rgb);
                    char p[512];
                    snprintf(p, sizeof p, "%s/hud_%05u.ppm", hud_dir, sc.src->host_frame);
                    FILE *pf = fopen(p, "wb");
                    if (pf) { fprintf(pf, "P6\n%d %d\n255\n", im.w, im.h); fwrite(rgb, 1, (size_t)im.w * im.h * 3, pf); fclose(pf); }
                    fprintf(stderr, "[hud] frame %u lista %u -> %s (%d batches, %u vértices; tris 3D %u, 2D %u, polis 3D %u, 2D %u, rects %u)\n", sc.src->host_frame, sc.src->index, p, (int)sc.nb, sc.nv,
                            sc.stats.n_tris3d, sc.stats.n_tris2d, sc.stats.n_poly3d, sc.stats.n_poly2d, sc.stats.n_rect);
                    if (getenv("HUD_VERTS")) {   /* vértices de los batches kind 2 */
                        for (uint32_t bi = 0; bi < sc.nb; bi++) {
                            const VrBatch *b = &sc.b[bi];
                            fprintf(stderr, "  batch %u kind %u blend 0x%02x first %u count %u ev_end %u\n", bi, b->kind, b->blend, b->first, b->count, b->ev_end);
                            for (uint32_t j = 0; j < b->count && j < 12; j += 3) {
                                const VrVtx *v = &sc.v[b->first + j];
                                fprintf(stderr, "    tri pos(%.0f,%.0f) uv(%.0f,%.0f) rgba %08x clut %04x tpage %04x flags %04x pad %u\n", v->pos[0], v->pos[1], v->uv[0], v->uv[1], v->rgba, v->clut, v->tpage, v->flags, v->pad);
                            }
                        }
                    }
                    free(rgb); hud_img_free(&im);
                } else hud_emu_run(&sc, evram, NULL, 0, 0);
            }
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
    {
        VrBodyStats bst; vrbody_stats(&bst);
        VrFireStats fst; vrfire_stats(&fst);
        VrReloadStats rst; vrreload_stats(&rst);
        MvVrGameStatus gst; mv_vrgame_status(&gst);
        MvVrForearm fa;
        int fl = mv_vrgame_forearm(0, &fa);
        fprintf(stderr, "[run] cuerpo: apply %u, brazos %u, restore %u, pendiente %d, saltos (sin vista %u, modelo %u, fuera %u, sin manos %u); antebrazo izq. %s\n", bst.applies, bst.arms_driven, bst.restores,
                vrbody_pending(), bst.skip_noview, bst.skip_model, bst.skip_offscreen, bst.skip_nohands, fl ? "IK" : "no");
        fprintf(stderr, "[run] radar: escenas con radar %u de %u\n", n_radar, n_scenes);
        fprintf(stderr, "[run] arma VR: carga=%d disparos VR %u; juego: peticiones %u, sustituidas %u, pasadas %u, suprimidas %u, caducadas %u, pulsaciones %u; recarga: secuencias %u ok %u fallos %u\n", gst.loader, gst.shots,
                fst.requests, fst.subst, fst.passed, fst.skipped, fst.expired, fst.presses, rst.sequences, rst.ok, rst.fails);
        if (vr_hands) {
            CHECK(bst.arms_driven > 0 && vrbody_pending() == 0, "la IK no movió brazos o quedó sin restaurar");
            CHECK(n_radar > 0, "ninguna escena trajo radar");
        }
    }

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
