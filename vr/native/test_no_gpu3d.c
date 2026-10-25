/* Regresión de código de los DOS modos de 3D: gpu (DEFECTO; el 3D sale de la lista de la GPU con los vértices de vista de la GTE) y nativo (MV_RENDER=nativo; escena leída de la RAM).
 * Lee los fuentes (Swift / Metal / C del host) y comprueba que el camino gpu está cableado de punta a punta (modo por defecto, dibujo de lotes 3D con profundidad, 2D al panel,
 * cercanía al ojo, vista ENGINE directa, registro) y que el camino nativo sigue intacto (efectos de pantalla, repetir el último válido, cinemáticas). Antes de k-pgxp esta prueba
 * exigía lo contrario (solo nativo, sin modo gpu). Se ejecuta desde la raíz del repositorio (make -f vr/native/fx.mk test). */
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

    /* 1) RenderRG: el modo gpu dibuja los lotes 3D; nada de la maquinaria antigua de saltar lotes ante el nativo (el post-proceso por ojo SÍ existe, solo en el modo gpu: ver 2b) */
    const char *forbidden[] = { "skip_if_native", "nativeSkip", "decidedSkip", "nativeOnly", "nativeUse", "nativeFailed", "fxConfirmed",
                                "scanlineGain", "render_only_native", ".decide(", "VR_SKIP_" };
    for (size_t i = 0; i < sizeof forbidden / sizeof *forbidden; i++) CHECK(count(rg, forbidden[i]) == 0, "RenderRG.swift conserva «%s» (maquinaria antigua de sustitución por el nativo)", forbidden[i]);
    CHECK(count(rg, "guard k.hud") == 0, "pipeline(_:) construye también los pipelines de escena (sin guard k.hud)");
    CHECK(strstr(rg, "mv_vrhost_render_mode() == 0") != NULL && strstr(rg, "private(set) var gpuMode = true") != NULL, "RenderRG lee el modo (0 = gpu) y arranca en gpu");
    CHECK(strstr(rg, "\"vr_vs_scene_amp\"") != NULL && strstr(rg, "\"vr_vs_scene\"") != NULL, "pipelines de escena de la lista de la GPU (con y sin amplificación)");
    CHECK(strstr(rg, "d1.depthCompareFunction = .greaterEqual; d1.isDepthWriteEnabled = true") != NULL && strstr(rg, "d2.depthCompareFunction = .greaterEqual; d2.isDepthWriteEnabled = false") != NULL,
          "opaco: prueba + escritura de profundidad; semitransparente: solo prueba (Z invertida)");
    CHECK(count(rg, ".cullMode") == 0 && count(rg, "setCullMode(.none)") >= 2, "sin descarte de reversos (la PS1 no lo tiene)");
    CHECK(count(rg, "drawPrimitives") == 5, "RenderRG.swift: HUD (encodeHud), su paso final, drawGPU y el post-proceso (estéreo + ojo suelto) dibujan primitivas (hay %d)", count(rg, "drawPrimitives"));
    CHECK(count_range(rg, "private func encodePost(", "private func encodeVram(", "drawPrimitives") == 2, "encodePost: un triángulo a pantalla completa por ojo (estéreo con instancias / ojo suelto)");
    CHECK(count_range(rg, "private func encodeHud", "private func encodeScene(", "drawPrimitives") == 2, "dos drawPrimitives en encodeHud/finishHud");
    CHECK(count_range(rg, "private func encodeScene(", "private func drawGPU(", "drawPrimitives") == 0, "encodeScene no dibuja primitivas directamente (delega en drawGPU)");
    CHECK(count_range(rg, "private func drawGPU(", NULL, "drawPrimitives") == 1, "drawGPU es el único dibujo de la lista de la GPU en 3D");
    CHECK(count_range(rg, "private func encodeScene(", NULL, "n.encode(encoder") == 1, "encodeScene dibuja el mundo nativo SOLO con NativeRenderer.encode");
    CHECK(count_range(rg, "private func drawGPU(", NULL, "batches[i]") >= 1 && strstr(rg, "batches[$0].kind != 2") != NULL, "drawGPU dibuja los lotes 3D (kind 0/1) en el orden de envío");
    CHECK(count_range(rg, "private func drawGPU(", NULL, "dsOpaque") >= 1 && count_range(rg, "private func drawGPU(", NULL, "dsSemi") >= 1, "drawGPU fija el estado de profundidad por clase");
    CHECK(count_range(rg, "private func encodeScene(", "private func drawGPU(", "gpuRange != nil") == 1, "encodeScene: el modo gpu sale antes del camino nativo");
    CHECK(strstr(rg, "if gpuMode { encodeStereoGPU(") != NULL && strstr(rg, "if gpuMode {\n            guard haveScene, has3D, !batches.isEmpty, ensureAux") != NULL, "pantalla 3D y segunda vista también desde la lista de la GPU");
    CHECK(strstr(rg, "encodeScenePasses(commandBuffer, range: g.0..<g.1, first: firstG, last: lastG") != NULL, "modo gpu: un pase por grupo de eventos de VRAM (primero limpia, último resuelve)");
    CHECK(strstr(rg, "if gpuMode { nativeDraw = false; nativeDrawnLast = false; stereoHealthy = true }") != NULL, "modo gpu: no se prepara el render nativo");
    /* 2) el 3D de la lista se cuenta y el HUD solo toma kind 2 */
    CHECK(strstr(rg, "gpu3DTris") != NULL, "el 3D de la lista se registra (gpu3DTris)");
    CHECK(count_range(rg, "private func encodeHud", "private func encodeScene(", "batches[$0].kind == 2") >= 1, "el HUD filtra los lotes kind 2");
    /* 2b) filtro de pantalla (clase 3: bajo el agua) en el modo gpu: las tiras no se dibujan, la escena de los ojos pasa por un post-proceso por ojo con VrScene.filter */
    CHECK(strstr(rg, "private func storeFilter(_ f: VrFilter)") != NULL && strstr(rg, "guard gpuMode, screenFilterEnabled, f.active != 0 else { return }") != NULL && strstr(rg, "        storeFilter(sc.filter)\n") != NULL, "VrScene.filter se resume al guardar la escena (solo modo gpu)");
    CHECK(strstr(rg, "if kind == 3 { continue }") != NULL, "los lotes kind 3 no se dibujan como geometría ni HUD");
    CHECK(strstr(rg, "let plan = gpuMode ? planPost(colorTargets: colorTargets, resolveTargets: resolveTargets, layered: layered) : nil") != NULL, "el plan del post-proceso solo existe en el modo gpu");
    CHECK(strstr(rg, "colorTargets: sceneColor, depthTargets: depthTargets,") != NULL && strstr(rg, "resolveTargets: sceneResolve, views: sceneV,") != NULL, "el pase gpu de los ojos escribe en las texturas del plan");
    CHECK(strstr(rg, "if let p = plan { encodePost(commandBuffer, p, projections: projections, layered: layered) }") != NULL, "el post-proceso se codifica tras el último grupo (radar y arma incluidos)");
    CHECK(strstr(rg, "guard fx.needsPost, screenFilterEnabled, drawWorld, drawEyes, drawBody, haveScene, has3D, viewportOverride == nil, rasterizationRateMap == nil else { return nil }") != NULL, "el post-proceso solo se hace con mundo y ojos dibujados");
    CHECK(strstr(rg, "\"vr_fs_post_layered\" : \"vr_fs_post\"") != NULL && strstr(rg, "\"vr_vs_post_layered\" : \"vr_vs_post\"") != NULL, "pipelines del post-proceso (con y sin amplificación)");
    CHECK(strstr(rg, "post=\\(statPost)") != NULL, "el registro de la escena cuenta los pases de post-proceso");
    { char *ms = slurp("render/metal/vr_scene.metal", 1);
      CHECK(ms && strstr(ms, "fragment float4 vr_fs_post(") != NULL && strstr(ms, "fragment float4 vr_fs_post_layered(") != NULL, "sombreadores del post-proceso en vr_scene.metal");
      free(ms); }

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
    CHECK(strstr(rg, "n.camH") != NULL && strstr(rg, "160 / n.camH") != NULL, "la pantalla 3D nativa usa la H real de la escena (tangentes 160/H, 120/H) y OFX/OFY");
    CHECK(strstr(rg, "if !gpuMode, let n = native, n.camH > 8 {") != NULL, "la pantalla 3D del modo gpu usa las tangentes de la petición");
    CHECK(count(sh, "ns_vs_overlay(") == 1, "ningún vertex llama a otro vertex (helper ns_overlay_vertex)");
    CHECK(strstr(sh, "ns_overlay_vertex") != NULL, "helper de vértice de los recubrimientos");
    CHECK(strstr(nr, "if r.kind != 0 && !drawFx { continue }") != NULL, "con los efectos no permitidos se omiten los tramos de efectos (continue), no se pierde el fotograma");
    CHECK(strstr(nr, "serial &- lastForceGc >= 10") != NULL, "barrido forzado de mallas como mucho cada 10 fotogramas");
    CHECK(strstr(xe, "renderer.stereoImage") != NULL, "la imagen estéreo llega a la pantalla 3D");
    /* 6) modo de render: gpu por defecto, nativo con MV_RENDER=nativo; sin menú ni ajuste guardado (el modo no se cambia en caliente desde la interfaz) */
    CHECK(count(xp, "renderMode") == 0 && count(gm, "renderMode") == 0, "el menú VR y el modelo no tienen modo de render");
    CHECK(count(xp, "XRMenuItem.render") == 0, "sin elemento de menú de render");
    CHECK(strstr(vh, "#define MV_RENDER_DEFAULT_NATIVE 0") != NULL, "el modo por defecto es gpu");
    CHECK(strstr(vh, "int mv_vrhost_render_mode(void) { return atomic_load(&render_req) ? 1 : 0; }") != NULL, "mv_vrhost_render_mode: 0 = gpu, 1 = nativo");
    CHECK(strstr(vh, "int mv_vrhost_render_only_native(void) { return 0; }") != NULL, "no existe «solo nativo»");
    CHECK(strstr(vh, "!strcmp(e, \"gpu\")") != NULL && strstr(vh, "!strcmp(e, \"nativo\")") != NULL && count(vh, "solo-nativo\")") == 0, "MV_RENDER admite gpu y nativo (no solo-nativo)");
    CHECK(strstr(vh, "if (atomic_load(&render_req) && atomic_load(&renderer_ready)) return 1;") != NULL, "la captura nativa solo corre en el modo nativo (ns_gate)");
    CHECK(strstr(vh, "return native_mode() && r && r->active && !cine_cand") != NULL, "el modo gpu no marca batches para el nativo");
    /* B6: el recentrado reinicia el seguimiento de efectos (el espacio de cabeza salta); hold_near 15 cm */
    { char *vi = slurp("app/Sources/VRInput.swift", 1);
      CHECK(vi && strstr(vi, "ns_gpu_fx_track_request_reset()") != NULL, "VRInput.recenter reinicia el seguimiento de efectos");
      CHECK(strstr(nr, "prm.hold_near_m = 0.15") != NULL, "retencion de cercanos a 15 cm");
      free(vi); }
    /* 7) la colisión de la cabeza: escena nativa en el modo nativo, lista de la GPU en el modo gpu (sin captura nativa) */
    CHECK(strstr(vh, "if (!native_mode()) vrhc_feed_scene(&hm, &sc") != NULL, "modo gpu: la colisión de la cabeza se alimenta de la lista de la GPU");
    /* 7b) cercanía al ojo, vista y registro del modo gpu */
    CHECK(strstr(vh, "if (!native_mode()) o.eye_near_m = GPU3D_NEAR_EYE_M;") != NULL && strstr(vh, "#define GPU3D_NEAR_EYE_M 0.25f") != NULL, "modo gpu: nada a menos de 0,25 m del ojo (vr_scene eye_near_m)");
    CHECK(strstr(vh, "vin.gpu3d = (uint8_t)!native_mode();") != NULL, "la vista recibe el modo gpu (ENGINE directo, sin respaldo)");
    CHECK(strstr(vh, "if (!native_mode()) keep3b = 0;") != NULL, "modo gpu: fundidos y destellos al panel 2D");
    CHECK(strstr(vh, "gpu_capture_hiprec(1)") != NULL && strstr(vh, "if (cap_on && !gte_hp_enabled()) gpu_capture_hiprec(1);") != NULL, "vértices de vista de la GTE en alta precisión siempre que haya captura");
    CHECK(strstr(vh, "\"[gpu3d] fotograma=%u") != NULL && strstr(vh, "gpu3d_log();") != NULL && strstr(vh, "gpu3d_note(&sc, r);") != NULL, "registro [gpu3d] una vez por segundo");
    CHECK(strstr(vh, "post=%u | render=%s") != NULL && strstr(vh, "g3.post += sc->b[i].kind == 3") != NULL, "registro [gpu3d]: lotes de filtro de pantalla (post=N)");
    { char *vw = slurp("vr/host/vr_view.c", 1), *vs = slurp("vr/scene/vr_scene.c", 1);
      CHECK(vw && strstr(vw, "return in->gpu3d || vrview_native_trusted(in) ? VRV_K_ENGINE : VRV_K_BACKUP;") != NULL, "vr_view: en el modo gpu una escena 3D es ENGINE sin mirar la cobertura nativa");
      CHECK(vw && strstr(vw, "(in && in->gpu3d ? 1 : v->to_engine)") != NULL, "vr_view: entrada a ENGINE sin histéresis en el modo gpu");
      CHECK(vw && strstr(vw, "\"[vista] cambio:") != NULL, "vr_view: registro [vista] de cada cambio con su motivo");
      CHECK(vs && strstr(vs, "o.eye_near_m > 0") != NULL && strstr(vs, "stats.n_near_eye++") != NULL, "vr_scene: descarte por cercanía al ojo y recorte del plano cercano");
      free(vw); free(vs); }

    /* 8) cine: fmv / motor / respaldo (la lista de la GPU no se dibuja en 3D en ninguno; el respaldo es la imagen de la VRAM en la pantalla de cine plana) */
    CHECK(strstr(vh, "vrview_cine_describe(&view, &vin") != NULL && strstr(vh, "mdec_out_words_total()") != NULL, "vrhost registra [cine] tipo=... y lee el decodificador de vídeo");
    CHECK(strstr(vh, "ns_cover_glue_last(&cf, &cp)") != NULL && strstr(vh, "ns_fill_glue_last(&ff, &fp)") != NULL && strstr(vh, "vrview_cover_feed(&vin") != NULL, "vrhost alimenta la vista con la cobertura medida del render nativo");
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
