/* Regresión de código: el 3D del mundo y de los efectos sale SOLO del render nativo; la lista de la GPU no se dibuja jamás en 3D.
 * Lee los fuentes (Swift / Metal / C del host) y comprueba que no queda ningún camino de dibujo 3D por GPU ni modo «gpu», y que el camino nativo está cableado
 * (efectos de pantalla, repetir el último válido, cinemáticas con el render nativo). Se ejecuta desde la raíz del repositorio (make -f vr/native/fx.mk test). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FALLO: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* Lee un fichero entero y le quita los comentarios de línea (// ...) para no confundir texto explicativo con código. */
static char *slurp(const char *path, int strip) {
    FILE *f = fopen(path, "rb");
    if (!f) { printf("FALLO: no se pudo abrir %s\n", path); fails++; return NULL; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
    fclose(f); b[n] = 0;
    if (strip) {
        char *w = b; int instr = 0;
        for (char *r = b; *r; r++) {
            if (*r == '"' && (r == b || r[-1] != '\\')) instr = !instr;
            if (!instr && r[0] == '/' && r[1] == '/') { while (*r && *r != '\n') r++; if (!*r) break; }
            *w++ = *r;
        }
        *w = 0;
    }
    return b;
}
static int count(const char *h, const char *needle) {
    int n = 0; size_t l = strlen(needle);
    for (const char *p = h; (p = strstr(p, needle)); p += l) n++;
    return n;
}
static int count_range(const char *h, const char *a, const char *b, const char *needle) {   /* apariciones de needle entre a y b (b = NULL: hasta el final) */
    const char *s = strstr(h, a); if (!s) return -1;
    const char *e = b ? strstr(s, b) : s + strlen(s); if (!e) return -1;
    int n = 0; size_t l = strlen(needle);
    for (const char *p = s; (p = strstr(p, needle)) && p < e; p += l) n++;
    return n;
}

int main(void) {
    char *rg = slurp("app/Sources/RenderRG.swift", 1);
    char *nr = slurp("app/Sources/Native/NativeRenderer.swift", 1);
    char *sh = slurp("app/Sources/Native/NativeShaders.metal", 1);
    char *vh = slurp("vr/host/vrhost.c", 1);
    char *xp = slurp("app/Sources/XRPresent.swift", 1);
    char *gm = slurp("app/Sources/GameModel.swift", 1);
    char *xe = slurp("app/Sources/XRFrameEncoder.swift", 1);
    if (!rg || !nr || !sh || !vh || !xp || !gm || !xe) return 1;

    /* 1) RenderRG: nada del dibujo 3D por la lista de la GPU */
    const char *forbidden[] = { "dsOpaque", "dsSemi", "skip_if_native", "nativeSkip", "decidedSkip", "nativeOnly", "nativeUse", "nativeFailed", "fxConfirmed", "VrFilter",
                                "storeFilter", "planPost", "encodePost", "postPipeline", "semiDepthBias", "scanlineGain", "render_mode", "render_only_native", "vr_vs_scene", "vr_vs_scene_amp",
                                ".decide(", "VR_SKIP_" };
    for (size_t i = 0; i < sizeof forbidden / sizeof *forbidden; i++) CHECK(count(rg, forbidden[i]) == 0, "RenderRG.swift conserva «%s» (camino de 3D por GPU)", forbidden[i]);
    CHECK(strstr(rg, "guard k.hud") != NULL, "pipeline(_:) debe construir SOLO pipelines de HUD (guard k.hud)");
    CHECK(count(rg, "drawPrimitives") == 2, "RenderRG.swift: solo el HUD (encodeHud) y su paso final dibujan primitivas (hay %d)", count(rg, "drawPrimitives"));
    int inHud = count_range(rg, "private func encodeHud", "private func encodeScene(", "drawPrimitives");
    CHECK(inHud == 2, "todos los drawPrimitives de RenderRG.swift deben estar en encodeHud/finishHud (dentro: %d)", inHud);
    CHECK(count_range(rg, "private func encodeScene(", NULL, "drawPrimitives") == 0, "encodeScene no dibuja primitivas de la lista de la GPU");
    CHECK(count_range(rg, "private func encodeScene(", NULL, "n.encode(encoder") == 1, "encodeScene dibuja el mundo SOLO con NativeRenderer.encode");
    /* 2) los lotes de 3D de la lista de la GPU no se dibujan: solo se cuentan */
    CHECK(strstr(rg, "gpu3DTris") != NULL, "el 3D de la lista se registra (gpu3DTris) pero no se dibuja");
    CHECK(count_range(rg, "private func encodeHud", "private func encodeScene(", "batches[$0].kind == 2") >= 1, "el HUD filtra los lotes kind 2");
    /* 3) presentación: repetir el último válido o negro; nunca GPU */
    CHECK(strstr(rg, "n.present(ready:") != NULL, "prepareNative usa ns_present (repetir último válido / negro)");
    CHECK(strstr(rg, "NS_PR_BLACK") != NULL, "el modo negro de la presentación está cableado");
    CHECK(strstr(nr, "ns_present_step") != NULL, "NativeRenderer.present llama a ns_present_step");
    /* 4) efectos de pantalla nativos: sombreador, recubrimientos, agua en el mundo */
    CHECK(strstr(nr, "ns_screen_build") != NULL && strstr(nr, "drawOverlays") != NULL, "efectos de pantalla: ns_screen_build y drawOverlays");
    CHECK(strstr(sh, "ns_overlay_fs") != NULL || strstr(sh, "ns_ov_fs") != NULL || strstr(sh, "overlay") != NULL, "sombreador de recubrimientos de pantalla");
    CHECK(strstr(sh, "water") != NULL, "sombreador del agua en el mundo");
    CHECK(strstr(nr, "ns_gpu_semi_merge") != NULL, "una sola pasada semitransparente con mallas y efectos intercalados");
    CHECK(strstr(rg, "screenEffectsEnabled") != NULL && strstr(rg, "suppressRing") != NULL, "RenderRG fija los interruptores de efectos de pantalla");
    /* 5) cinemáticas con el render nativo, en estéreo, en la textura de la pantalla 3D */
    CHECK(strstr(nr, "ns_cinema_to_vr") != NULL && strstr(nr, "cinemaGameView") != NULL, "NativeRenderer coloca la escena desde la cámara del juego (ns_cinema)");
    CHECK(count_range(rg, "private func encodeStereo", "private func encodeAux", "cams: cams") >= 1, "encodeStereo dibuja con el render nativo (cams)");
    CHECK(strstr(rg, "n.cinemaGameView = ((engineView || n.sceneUnfit)") != NULL, "cámara del juego en cinemáticas del motor Y en escenas no aptas para primera persona");
    CHECK(strstr(xp, "gameCamScene") != NULL && strstr(xp, "target = .stereo") != NULL, "XRPresent pide la pantalla 3D con una escena válida no apta (nunca negro)");
    CHECK(strstr(rg, "n.camH") != NULL && strstr(rg, "160 / n.camH") != NULL, "la pantalla 3D usa la H real de la escena (tangentes 160/H, 120/H) y OFX/OFY");
    CHECK(count(sh, "ns_vs_overlay(") == 1, "ningún vertex llama a otro vertex (helper ns_overlay_vertex)");
    CHECK(strstr(sh, "ns_overlay_vertex") != NULL, "helper de vértice de los recubrimientos");
    CHECK(strstr(nr, "if r.kind != 0 && !drawFx { continue }") != NULL, "con los efectos no permitidos se omiten los tramos de efectos (continue), no se pierde el fotograma");
    CHECK(strstr(nr, "serial &- lastForceGc >= 10") != NULL, "barrido forzado de mallas como mucho cada 10 fotogramas");
    CHECK(strstr(xe, "renderer.stereoImage") != NULL, "la imagen estéreo llega a la pantalla 3D");
    /* 6) sin modo «gpu» en el menú, el modelo ni el host */
    CHECK(count(xp, "renderMode") == 0 && count(gm, "renderMode") == 0, "el menú VR y el modelo ya no tienen modo de render");
    CHECK(count(xp, "XRMenuItem.render") == 0, "sin elemento de menú de render");
    CHECK(strstr(vh, "int mv_vrhost_render_mode(void) { return 1; }") != NULL, "mv_vrhost_render_mode devuelve siempre «nativo»");
    CHECK(strstr(vh, "int mv_vrhost_render_only_native(void) { return 0; }") != NULL, "no existe «solo nativo»");
    CHECK(count(vh, "!strcmp(e, \"gpu\")") == 0 && count(vh, "atomic_store(&render_req, 0)") == 0, "MV_RENDER=gpu no cambia el modo");
    /* 7) la colisión de la cabeza usa solo la escena nativa (sin listas de la GPU) */
    CHECK(count(vh, "vrhc_feed_scene(") == 0 || count(vh, "vrhc_feed_scene(&") == 0, "la colisión de la cabeza no se alimenta de la lista de la GPU");
    /* 8) cine: fmv / motor / respaldo (la lista de la GPU no se dibuja en 3D en ninguno; el respaldo es la imagen de la VRAM en la pantalla de cine plana) */
    CHECK(strstr(vh, "vrview_cine_describe(&view, &vin") != NULL && strstr(vh, "mdec_out_words_total()") != NULL, "vrhost registra [cine] tipo=... y lee el decodificador de vídeo");
    CHECK(strstr(vh, "ns_cover_glue_last(&cf, &cp)") != NULL && strstr(vh, "vin.cover_new") != NULL, "vrhost alimenta la vista con la cobertura medida del render nativo");
    CHECK(strstr(vh, "vrview_uploads_cover(") != NULL, "vídeo por subidas a pantalla completa (tiras incluidas)");
    CHECK(strstr(vh, "view.kind == VRV_K_UI || view.kind == VRV_K_VIDEO || view.kind == VRV_K_BACKUP") != NULL, "la capa 2D entera acompaña al respaldo");
    CHECK(strstr(xp, "engineHealthy") != NULL && strstr(xp, "engineBackup") != NULL && strstr(xp, "tipo=respaldo") != NULL, "XRPresent cae al respaldo si el nativo no dibuja la pantalla 3D");
    CHECK(strstr(xp, "XRCinemaLayout.legible(reason: Int32(gs.view_reason), kind: gs.view_kind)") != NULL, "el vídeo y el respaldo se presentan grandes, planos y nítidos");
    CHECK(strstr(xe, "engineHealthy: renderer.stereoHealthy") != NULL && strstr(rg, "stereoHealthy = ready && stereoWanted") != NULL, "salud de la pantalla 3D desde el renderer");
    CHECK(strstr(rg, "if stereoRequest == nil { stereoRendered = false }") != NULL, "la imagen de la pantalla 3D no sobrevive a la escena (caché)");
    {   /* en cine plano (clases 0, 3 y 4) la imagen sale de la VRAM, sin capa 2D duplicada encima ni mundo: el bloque de la pantalla de cine solo añade UN elemento */
        int items = count_range(xp, "if shown == .cinema, let c = cinema, let tex = c.texture(srgb: linear) {", "if shown == .stereo, Self.gameViewOK(gameView) {", "r.items.append(");
        CHECK(items == 1, "la pantalla de cine añade un único elemento (la imagen de la VRAM) (%d)", items);
    }
    printf("test_no_gpu3d: %d comprobaciones, %d fallos\n", checks, fails);
    if (!fails) printf("TODO OK\n");
    return fails ? 1 : 0;
}
