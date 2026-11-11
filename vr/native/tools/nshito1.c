/* nshito1: herramienta de escritorio del extractor nativo (HITO 1). Carga un volcado de RAM (fichero .bin de 2 MiB o directorio NSD1 con ram.bin,
 * vram.bin, registros de cálculo...), extrae la escena (ns_extract), la convierte a triángulos (ns_adapt) y la dibuja con el rasterizador de referencia (ns_cpuref):
 *   <nombre>_pano.png     panorama equirectangular 360x180 (def. 4096x2048) desde la cabeza del jugador o la cámara; huecos en magenta
 *   <nombre>_pano_raw.png el mismo sin marcar huecos
 *   <nombre>_cube.png     cubemap en cruz 4x3
 *   <nombre>_game.png     vista con la cámara del juego (eye_inv del canal, H, OFX/OFY de los registros si existen; si no, estimada)
 *   <nombre>_frame.png    (con --frame) el fotograma real, a la misma escala, para comparar
 *   <nombre>_stats.txt    estadísticas (también por la salida estándar)
 * NO ejecuta el juego ni lo simula: sólo lee volcados. No es parte de la app.
 *
 * Uso: nshito1 (--ram f.bin | --dump DIR) [--vram f.bin] [--frame f.ppm] --out DIR --name N [opciones]
 *   --from cam|head|x,y,z     origen del panorama (def. head si hay jugador, si no cam)    --pano WxH (def. 4096x2048)
 *   --yaw g                   rumbo del panorama en grados (def. el de la cámara del juego)
 *   --game-size WxH           tamaño de la vista de juego (def. el del fotograma real si lo hay; si no 640x512)   --frame-ref WxH (def. 320x256)
 *   --ofx n --ofy n           centro de proyección en la pantalla de referencia (def. registros de cálculo del volcado si hay, si no w/2,h/2)
 *   --no-overscan             no aplicar 66/64 a la fila Y de la cámara del motor
 *   --exposure f              sin VRAM: multiplicador del color (def. 1.8)   --no-tint   sin tinte por tpage/clut
 *   --skip-objs a,b,..        direcciones de objetos que no se extraen (sombras que pinta ns_fx)
 *   --no-cull                 la vista del juego NO descarta reversos (por defecto sí, como el motor)
 *   --player-key k            clave de la instancia que hace de jugador (def. heurística: articulada, con luz y ambiente propios, más cercana al rayo de la cámara)
 *   --detect-shadows          omitir además lo que parece sombra (una cara plana translúcida iluminada)
 *   --keep-self               no ocultar el cuerpo propio en el panorama desde la cabeza   --znear n (def. 20)
 *   --only pano,cube,game     seleccionar salidas */
#include "../ns_adapt.h"
#include "../ns_cpuref.h"
#include "../ns_dump.h"
#include "../ns_extract.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int parse_wh(const char *s, int *w, int *h) { return sscanf(s, "%dx%d", w, h) == 2 && *w > 0 && *h > 0 ? 0 : -1; }
static uint8_t *readall(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long l = ftell(f); fseek(f, 0, SEEK_SET);
    if (l < 0) { fclose(f); return NULL; }
    uint8_t *p = (uint8_t *)malloc((size_t)l + 1);
    if (p && fread(p, 1, (size_t)l, f) != (size_t)l) { free(p); p = NULL; }
    fclose(f);
    if (p) *n = (size_t)l;
    return p;
}
static int want(const char *only, const char *k) { return !only || strstr(only, k); }
static FILE *statf;
#define OUT(...) do { printf(__VA_ARGS__); if (statf) fprintf(statf, __VA_ARGS__); } while (0)

/* RAM de la escena: s16/s32 con dirección del motor (sólo en la herramienta, para ojo y cabeza) */
static const uint8_t *g_ram;
static int32_t ramS32(uint32_t a) { int32_t v = 0; a &= 0x1FFFFF; if (a + 4 <= NS_RAM_SIZE) memcpy(&v, g_ram + a, 4); return v; }
static int16_t ramS16(uint32_t a) { int16_t v = 0; a &= 0x1FFFFF; if (a + 2 <= NS_RAM_SIZE) memcpy(&v, g_ram + a, 2); return v; }

static void cross3(const float a[3], const float b[3], float o[3]) { o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0]; }
static void norm3(float v[3]) { float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); if (l > 1e-12f) { v[0] /= l; v[1] /= l; v[2] /= l; } }

/* Pose "a nivel": ojo en pos, mundo con Y ARRIBA (el eje del motor), rumbo yaw (rad) medido sobre el plano XZ desde el ojo del juego. */
static void level_pose(NsRCam *c, const float pos[3], const float fwd_h[3]) {
    float up[3] = {0, 1, 0}, down[3] = {0, -1, 0}, f[3] = {fwd_h[0], 0, fwd_h[2]};
    norm3(f);
    float right[3];
    cross3(down, f, right);                 /* right x down = fwd  =>  right = down x fwd */
    norm3(right);
    (void)up;
    ns_cam_basis(c, pos, right, down, f, 90, 64, 64);
}

/* Caja vertical de mundo de una instancia (8 esquinas de la caja de cada malla visible). Devuelve 0 si no tiene mallas. */
static int inst_y_range(const NsScene *sc, const NsMeshStore *st, const NsInstance *in, float *lo, float *top) {
    *lo = 1e30f; *top = -1e30f;
    for (uint32_t k = 0; k < in->n_pieces; k++) {
        const NsPiece *q = &sc->piece[in->first_piece + k];
        const NsMesh *mm = (q->flags & NS_PF_HIDDEN) ? NULL : ns_store_get(st, q->mesh_id);
        if (!mm) continue;
        for (int c = 0; c < 8; c++) {
            float l[3] = {mm->bbox[(c & 1) ? 3 : 0], mm->bbox[(c & 2) ? 4 : 1], mm->bbox[(c & 4) ? 5 : 2]}, w[3];
            ns_xf_apply(&q->world, l, w);
            if (w[1] > *top) *top = w[1];
            if (w[1] < *lo) *lo = w[1];
        }
    }
    return *top >= *lo;
}

int main(int argc, char **argv) {
    const char *ramf = NULL, *dumpd = NULL, *vramf = NULL, *framef = NULL, *dir = ".", *name = "hito", *from = NULL, *only = NULL;
    int pw = 4096, ph = 2048, gw = 0, gh = 0, rw = 320, rh = 256;
    float ofx = -1, ofy = -1, exposure = 1.8f, znear = 20.0f, yaw_deg = NAN;
    uint32_t skip_list[64]; uint32_t nskip = 0, player_key = 0; int detect_shadows = 0;
    int overscan = 1, no_tint = 0, keep_self = 0, nocull = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
#define NEXT() (i + 1 < argc ? argv[++i] : (fprintf(stderr, "falta valor para %s\n", a), exit(2), (char *)0))
        if (!strcmp(a, "--ram")) ramf = NEXT();
        else if (!strcmp(a, "--dump")) dumpd = NEXT();
        else if (!strcmp(a, "--vram")) vramf = NEXT();
        else if (!strcmp(a, "--frame")) framef = NEXT();
        else if (!strcmp(a, "--out")) dir = NEXT();
        else if (!strcmp(a, "--name")) name = NEXT();
        else if (!strcmp(a, "--from")) from = NEXT();
        else if (!strcmp(a, "--only")) only = NEXT();
        else if (!strcmp(a, "--pano")) { if (parse_wh(NEXT(), &pw, &ph)) return 2; }
        else if (!strcmp(a, "--game-size")) { if (parse_wh(NEXT(), &gw, &gh)) return 2; }
        else if (!strcmp(a, "--frame-ref")) { if (parse_wh(NEXT(), &rw, &rh)) return 2; }
        else if (!strcmp(a, "--ofx")) ofx = (float)atof(NEXT());
        else if (!strcmp(a, "--ofy")) ofy = (float)atof(NEXT());
        else if (!strcmp(a, "--yaw")) yaw_deg = (float)atof(NEXT());
        else if (!strcmp(a, "--exposure")) exposure = (float)atof(NEXT());
        else if (!strcmp(a, "--znear")) znear = (float)atof(NEXT());
        else if (!strcmp(a, "--no-overscan")) overscan = 0;
        else if (!strcmp(a, "--no-tint")) no_tint = 1;
        else if (!strcmp(a, "--keep-self")) keep_self = 1;
        else if (!strcmp(a, "--no-cull")) nocull = 1;
        else if (!strcmp(a, "--player-key")) player_key = (uint32_t)strtoul(NEXT(), NULL, 0);
        else if (!strcmp(a, "--detect-shadows")) detect_shadows = 1;
        else if (!strcmp(a, "--skip-objs")) { for (char *q = (char *)NEXT(); *q && nskip < 64;) { char *e; skip_list[nskip++] = (uint32_t)strtoul(q, &e, 0); if (e == q) return 2; q = *e == ',' ? e + 1 : e; } }
        else { fprintf(stderr, "opción desconocida: %s (véase la cabecera de nshito1.c)\n", a); return 2; }
    }
    if (!ramf && !dumpd) { fprintf(stderr, "uso: nshito1 (--ram f.bin | --dump DIR) [--vram f.bin] [--frame f.ppm] --out DIR --name N (ver cabecera)\n"); return 2; }

    /* ---- carga ---- */
    uint8_t *ram = NULL, *scratch = NULL; uint16_t *vram = NULL; NsDump *nd = NULL;
    float ofx_reg = -1, ofy_reg = -1;
    if (dumpd) {
        nd = ns_dump_open(dumpd);
        if (!nd) { fprintf(stderr, "no se pudo abrir el volcado %s\n", dumpd); return 1; }
        ram = nd->ram; scratch = nd->scratch; vram = nd->vram;
        if (nd->has_gte) {   /* control 24/25 = centro de proyección (16.16) */
            float x = (float)((int32_t)nd->gte[32 + 24]) / 65536.0f, y = (float)((int32_t)nd->gte[32 + 25]) / 65536.0f;
            if (x > 0 && x < 1024 && y > 0 && y < 1024) { ofx_reg = x; ofy_reg = y; }
        }
    } else {
        size_t n = 0;
        ram = readall(ramf, &n);
        if (!ram || n != NS_RAM_SIZE) { fprintf(stderr, "%s: se esperaban 2097152 bytes (RAM del motor)\n", ramf); return 1; }
    }
    g_ram = ram;
    uint16_t *vram_own = NULL;
    if (vramf) {
        size_t n = 0; uint8_t *v = readall(vramf, &n);
        if (!v || n != 1024u * 512u * 2u) { fprintf(stderr, "%s: se esperaba una VRAM de 1 MiB\n", vramf); return 1; }
        vram_own = (uint16_t *)v; vram = vram_own;
    }
    char p[1024];
    snprintf(p, sizeof p, "%s/%s_stats.txt", dir, name);
    statf = fopen(p, "w");

    /* ---- extracción ---- */
    NsScene sc; NsMeshStore *st = ns_store_create();
    if (ns_scene_alloc(&sc, NS_MIN_INST, 8192, NS_MIN_LIGHT, 0) || !st) { fprintf(stderr, "sin memoria\n"); return 1; }
    NsExtractStats es;
    NsExtractOpts xo; memset(&xo, 0, sizeof xo); xo.shadow_objs = skip_list; xo.n_shadow = nskip; xo.detect_shadows = detect_shadows;
    int rc = ns_extract_ex(&sc, st, ram, NS_RAM_SIZE, scratch, scratch ? 1024 : 0, &xo, &es);
    OUT("== %s ==\n", name);
    OUT("extracción: rc=%d valid=0x%x truncated=0x%x cola=%u instancias=%u piezas=%u (ocultas/sin caras=%u, extensiones=%u) luces=%u grupo=0x%x\n", rc, sc.valid, sc.truncated, es.queue_n, es.n_inst, es.n_pieces,
        es.n_hidden_pieces, es.n_ext_pieces, es.n_lights, sc.group_mask);
    OUT("descartes de cola: nulo=%u fuera_de_RAM=%u repetido=%u invisible=%u grupo_inactivo=%u objeto_inconsistente=%u | modelo_inconsistente=%u extension_rota=%u lecturas_fuera_de_rango=%u\n",
        es.drop_null, es.drop_badptr, es.drop_dup, es.drop_hidden_flag, es.drop_group, es.drop_badobj, es.drop_bad_model, es.ext_bad, es.oob_reads);
    OUT("mallas: nuevas=%u (en almacén %u) reutilizadas=%u | caras leídas=%u (cuadriláteros %u, triángulos %u) degeneradas=%u sin_textura=%u versiones_de_color(nuevas/reutilizadas)=%u/%u\n", es.mesh_new,
        ns_store_count(st), es.mesh_ref, es.faces_seen, es.faces_quad, es.faces_tri, es.faces_degenerate, es.faces_no_tex, es.baked_new, es.baked_ref);
    OUT("caras descartadas por motivo: indice_de_vertice_malo=%u | indice_de_normal_malo(sustituido)=%u | soldaduras resueltas=%u sin_resolver=%u\n", es.faces_bad_vindex, es.faces_bad_nindex, es.welded_verts, es.weld_bad);
    for (uint32_t i = 0; i < sc.n_inst && getenv("NS_LIST"); i++) {
        const NsInstance *in = &sc.inst[i];
        OUT("  inst %2u key=%08x piezas=%3u flags=0x%02x luz=%d\n", i, in->key, in->n_pieces, in->flags, in->light);
    }
    if (rc) { OUT("canal principal ilegible: no hay escena\n"); return 1; }

    NsAdaptOpts ao; memset(&ao, 0, sizeof ao);
    ao.vram = vram; ao.exposure = exposure; ao.no_tint = no_tint;

    /* ---- cámara del juego: A, t de eye_inv (canal+0x10), con la escala del motor ---- */
    float A[9], t[3];
    for (int i = 0; i < 9; i++) A[i] = (float)ramS16(0x800B2B7Cu + 0x10u + 2u * (uint32_t)i) / 4096.0f;
    for (int i = 0; i < 3; i++) t[i] = (float)ramS32(0x800B2B7Cu + 0x10u + 0x14u + 4u * (uint32_t)i);
    float H = (float)ramS16(0x800B2B7Cu + 0x50u); if (H <= 0) H = 320;
    if (overscan) { for (int k = 0; k < 3; k++) A[3 + k] *= 66.0f / 64.0f; t[1] *= 66.0f / 64.0f; }
    /* ojo en el mundo = -A^T t con la matriz sin overscan */
    float eye[3] = {sc.cam.eye[0], sc.cam.eye[1], sc.cam.eye[2]};
    /* rumbo de la cámara: proyección horizontal de su eje Z de vista (fila 2 de eye_inv = Z de vista en el mundo) */
    float fwd_h[3] = {(float)ramS16(0x800B2B7Cu + 0x10u + 12u) / 4096.0f, 0, (float)ramS16(0x800B2B7Cu + 0x10u + 16u) / 4096.0f};
    if (fwd_h[0] * fwd_h[0] + fwd_h[2] * fwd_h[2] < 1e-4f) {   /* mira casi vertical: usar la fila Y de vista (abajo) hacia delante en pantalla */
        fwd_h[0] = -(float)ramS16(0x800B2B7Cu + 0x10u + 6u) / 4096.0f; fwd_h[2] = -(float)ramS16(0x800B2B7Cu + 0x10u + 10u) / 4096.0f;
    }
    if (!isnan(yaw_deg)) { fwd_h[0] = sinf(yaw_deg * 3.14159265f / 180.0f); fwd_h[2] = cosf(yaw_deg * 3.14159265f / 180.0f); }

    /* ---- jugador: instancia articulada con luz propia (LIT + OWN_AMBIENT) más cercana al rayo de la cámara, o la indicada con --player-key ---- */
    int player = -1; float head[3] = {0, 0, 0};
    {
        float fw[3] = {(float)ramS16(0x800B2B7Cu + 0x10u + 12u) / 4096.0f, (float)ramS16(0x800B2B7Cu + 0x10u + 14u) / 4096.0f, (float)ramS16(0x800B2B7Cu + 0x10u + 16u) / 4096.0f};
        float best = 1e30f;
        for (uint32_t i = 0; i < sc.n_inst; i++) {
            const NsInstance *in = &sc.inst[i];
            if (player_key) { if (in->key == player_key) { player = (int)i; break; } continue; }
            if (!(in->flags & NS_IF_LIT) || !(in->flags & NS_IF_OWN_AMBIENT) || in->n_pieces < 8) continue;
            const NsPiece *p0 = &sc.piece[in->first_piece];
            float d[3] = {p0->world.m[3] - eye[0], p0->world.m[7] - eye[1], p0->world.m[11] - eye[2]};
            float tpar = d[0] * fw[0] + d[1] * fw[1] + d[2] * fw[2];
            float dn2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2], ylo, ytop;
            int near_eye = inst_y_range(&sc, st, in, &ylo, &ytop) && dn2 <= (ytop - ylo) * (ytop - ylo);   /* la cámara está dentro o pegada a la instancia */
            if (near_eye) { if (dn2 < best) { best = dn2; player = (int)i; } continue; }
            if (tpar <= 0) continue;
            float q[3] = {d[0] - fw[0] * tpar, d[1] - fw[1] * tpar, d[2] - fw[2] * tpar};
            float dist = q[0] * q[0] + q[1] * q[1] + q[2] * q[2];
            if (dist > 0.09f * tpar * tpar) continue;   /* debe quedar cerca del rayo (ángulo < ~17 grados) */
            if (dist < best) { best = dist; player = (int)i; }
        }
        if (player >= 0) {
            const NsInstance *in = &sc.inst[player];
            float lo, top; const NsPiece *pr = &sc.piece[in->first_piece];
            inst_y_range(&sc, st, in, &lo, &top);
            if (top < lo) { top = pr->world.m[7]; lo = top; }
            head[0] = pr->world.m[3]; head[1] = top - 0.08f * (top - lo); head[2] = pr->world.m[11];
            OUT("jugador (heurística): instancia %d key=%08x piezas=%u raíz=(%.0f,%.0f,%.0f) altura=[%.0f,%.0f] origen=(%.0f,%.0f,%.0f)\n", player, in->key, in->n_pieces, pr->world.m[3], pr->world.m[7], pr->world.m[11], lo, top, head[0], head[1], head[2]);
        } else OUT("jugador: no encontrado\n");
    }
    float pos[3]; const char *src = "cam";
    if (from && !strcmp(from, "cam")) { memcpy(pos, eye, sizeof pos); }
    else if (from && strcmp(from, "head")) { if (sscanf(from, "%f,%f,%f", &pos[0], &pos[1], &pos[2]) != 3) { fprintf(stderr, "--from cam|head|x,y,z\n"); return 2; } src = "xyz"; }
    else if (player >= 0) { memcpy(pos, head, sizeof pos); src = "head"; if (!keep_self) ao.skip_key = sc.inst[player].key; }
    else memcpy(pos, eye, sizeof pos);
    OUT("origen del panorama: %s (%.0f,%.0f,%.0f); rumbo (%.3f,%.3f) ; cámara del juego: ojo=(%.0f,%.0f,%.0f) H=%.0f overscan=%s\n", src, pos[0], pos[1], pos[2], fwd_h[0], fwd_h[2], eye[0], eye[1], eye[2], H, overscan ? "66/64" : "no");

    /* ---- triángulos ---- */
    NsRTri *tris = NULL; NsAdaptStats as;
    size_t nt = ns_adapt_scene(&sc, st, &ao, &tris, &as);
    OUT("adaptador: piezas dibujadas=%zu/%zu (ocultas=%zu sin_malla=%zu omitidas_cuerpo_propio=%zu) caras=%zu -> triángulos=%zu | caras: mapa(precalculado)=%zu dinámicas(luz)=%zu neutras=%zu texturizadas=%zu\n",
        as.pieces_drawn, as.pieces_in, as.drop_hidden_piece, as.drop_no_mesh, as.drop_skip_inst, as.faces_in, nt, as.faces_baked, as.faces_dyn, as.faces_flat, as.faces_textured);
    OUT("  cuadriláteros=%zu triángulos=%zu | triángulos descartados por degenerados=%zu | vértices soldados con el padre=%zu\n", as.quads, as.tris, as.drop_degenerate, as.welded_verts);
    OUT("  render: %s\n", vram ? "CON VRAM (texturizado)" : "SIN VRAM: color de vértice x exposición x tinte por tpage/clut (las caras sin textura, grises)");
    /* bbox de la geometría */
    {
        float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
        for (size_t i = 0; i < nt; i++) for (int v = 0; v < 3; v++) for (int k = 0; k < 3; k++) { float x = tris[i].v[v].p[k]; if (x < mn[k]) mn[k] = x; if (x > mx[k]) mx[k] = x; }
        OUT("  caja de la geometría: x[%.0f,%.0f] y[%.0f,%.0f] z[%.0f,%.0f]\n", mn[0], mx[0], mn[1], mx[1], mn[2], mx[2]);
    }
    uint16_t *dvram = vram;
    if (!dvram) dvram = (uint16_t *)calloc(1024 * 512, 2);
    NsRScene rs = {tris, nt, dvram};
    NsRStats rstat;
    uint32_t ropt = 0;

    /* ---- panorama + cubemap ---- */
    if (want(only, "pano") || want(only, "cube")) {
        NsRCam pose; level_pose(&pose, pos, fwd_h);
        int n = pw / 4; if (n < 16) n = 16;
        NsRBuf faces[6];
        for (int f = 0; f < 6; f++) { ns_buf_alloc(&faces[f], n, n); ns_buf_clear(&faces[f], 0, 0, 0); }
        ns_render_cube(&rs, &pose, n, znear, ropt, faces, &rstat);
        OUT("cubemap 6x%d: %.0f ms, triángulos dibujados(suma de caras)=%zu, huecos medios=%.2f%%\n", n, rstat.ms, rstat.tris_drawn, rstat.hole_frac * 100);
        if (want(only, "cube")) {
            uint8_t *cr = (uint8_t *)malloc((size_t)4 * n * 3 * n * 3);
            ns_cube_cross_rgb8(faces, 0xFF00FF, 1, cr);
            snprintf(p, sizeof p, "%s/%s_cube.png", dir, name); ns_write_png(p, 4 * n, 3 * n, cr); free(cr);
        }
        if (want(only, "pano")) {
            NsRBuf pano; ns_buf_alloc(&pano, pw, ph);
            ns_cube_to_equirect(faces, &pose, &pano);
            uint8_t *rgb = (uint8_t *)malloc((size_t)pw * ph * 3);
            ns_buf_to_rgb8(&pano, rgb, 1, 0xFF00FF); snprintf(p, sizeof p, "%s/%s_pano.png", dir, name); ns_write_png(p, pw, ph, rgb);
            ns_buf_to_rgb8(&pano, rgb, 1, ((uint32_t)sc.bg_rgb[0] << 16) | ((uint32_t)sc.bg_rgb[1] << 8) | (uint32_t)sc.bg_rgb[2]); snprintf(p, sizeof p, "%s/%s_pano_raw.png", dir, name); ns_write_png(p, pw, ph, rgb);
            size_t holes = 0, hup = 0, hdn = 0, nup = 0, ndn = 0;
            for (int j = 0; j < ph; j++) for (int i = 0; i < pw; i++) {
                int h = pano.iz[(size_t)j * pw + i] == 0;
                holes += (size_t)h; if (j < ph / 2) { nup++; hup += (size_t)h; } else { ndn++; hdn += (size_t)h; }
            }
            OUT("panorama %dx%d: huecos magenta=%.2f%% (mitad superior %.2f%%, mitad inferior %.2f%%)\n", pw, ph, 100.0 * (double)holes / ((double)pw * ph), 100.0 * (double)hup / (double)nup, 100.0 * (double)hdn / (double)ndn);
            /* franjas de latitud de 15 grados: dónde faltan datos */
            OUT("  huecos por franja de latitud (de arriba a abajo, 15 grados):");
            for (int b = 0; b < 12; b++) {
                size_t hh = 0, tot = 0;
                for (int j = b * ph / 12; j < (b + 1) * ph / 12; j++) for (int i = 0; i < pw; i++) { tot++; hh += pano.iz[(size_t)j * pw + i] == 0; }
                OUT(" %.0f%%", 100.0 * (double)hh / (double)tot);
            }
            OUT("\n");
            free(rgb); ns_buf_free(&pano);
        }
        for (int f = 0; f < 6; f++) ns_buf_free(&faces[f]);
    }

    /* ---- vista con la cámara del juego ---- */
    if (want(only, "game")) {
        int fw = 0, fh = 0; uint8_t *frame = framef ? ns_read_ppm(framef, &fw, &fh) : NULL;
        if (framef && !frame) fprintf(stderr, "no se pudo leer %s\n", framef);
        if (!gw) { if (frame) { gw = fw; gh = fh; } else { gw = 640; gh = 512; } }
        /* referencia: la pantalla de juego (rw x rh); un fotograma más ancho (p. ej. 368) tiene el mismo alto y se centra: se escala por el alto */
        float sx = (float)gw / (float)(frame ? fw : rw), sy = (float)gh / (float)(frame ? fh : rh);
        (void)sx; (void)sy;
        float cx = ofx >= 0 ? ofx : (ofx_reg >= 0 ? ofx_reg : (float)(frame ? fw : rw) * 0.5f), cy = ofy >= 0 ? ofy : (ofy_reg >= 0 ? ofy_reg : (float)(frame ? fh : rh) * 0.5f);
        NsRCam c; ns_cam_game(&c, A, t, H, cx, cy, frame ? fw : rw, frame ? fh : rh, gw, gh); c.znear = 1.0f; if (getenv("NSHITO_RECHAZO")) c.zreject = 0.5f * H + 4.0f;   /* regla del motor con SU cámara (cinemáticas): fuera los polígonos con algún vértice sin división fiable */
        NsRBuf b; ns_buf_alloc(&b, gw, gh); ns_buf_clear(&b, 0, 0, 0);
        NsAdaptOpts go = ao; go.skip_key = 0; if (!nocull) go.cull_eye = eye;
        NsRTri *gt = NULL; NsAdaptStats gas; size_t gn = ns_adapt_scene(&sc, st, &go, &gt, &gas);
        NsRScene gs = {gt, gn, dvram};
        ns_render_view(&gs, &c, ropt, &b, &rstat);
        uint8_t *rgb = (uint8_t *)malloc((size_t)gw * gh * 3); ns_buf_to_rgb8(&b, rgb, 0, 0);
        snprintf(p, sizeof p, "%s/%s_game.png", dir, name); ns_write_png(p, gw, gh, rgb);
        OUT("vista del juego: caras descartadas por reverso=%zu\n", gas.drop_backface);
        OUT("vista del juego %dx%d (ref %dx%d, centro %.1f,%.1f, H=%.0f): %.0f ms, triángulos dibujados=%zu, píxeles sin geometría=%.2f%% (el fondo negro; fuera de campo y huecos del juego)\n", gw, gh, frame ? fw : rw, frame ? fh : rh, cx, cy, H, rstat.ms, rstat.tris_drawn, rstat.hole_frac * 100);
        if (frame) {   /* copia del fotograma real a la misma escala (vecino más próximo) */
            uint8_t *fr = (uint8_t *)malloc((size_t)gw * gh * 3);
            for (int y = 0; y < gh; y++) for (int x = 0; x < gw; x++) memcpy(&fr[((size_t)y * gw + x) * 3], &frame[((size_t)(y * fh / gh) * fw + (size_t)(x * fw / gw)) * 3], 3);
            snprintf(p, sizeof p, "%s/%s_frame.png", dir, name); ns_write_png(p, gw, gh, fr); free(fr);
        }
        free(rgb); ns_buf_free(&b); free(gt); free(frame);
    }
    if (statf) fclose(statf);
    free(tris);
    ns_scene_free(&sc); ns_store_destroy(st);
    if (nd) ns_dump_close(nd); else free(ram);
    if (!vram) free(dvram);
    free(vram_own);
    return 0;
}
