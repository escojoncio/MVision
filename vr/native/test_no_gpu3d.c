/* Regresión de código del 3D: UN SOLO MODO. El mundo lo dibuja SOLO el render nativo (escena leída de la RAM); la lista de la GPU queda para el 2D. El camino gpuMode de RenderRG es código muerto sin acceso
 * (no hay selector de modo; MV_RENDER solo acepta «nativo»). Lee los fuentes (Swift / Metal / C del host) y comprueba que el camino nativo está cableado de punta a punta y que el
 * código muerto del modo gpu sigue sin acceso. Se ejecuta desde la raíz del repositorio (make -f vr/native/fx.mk test). */
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
    CHECK(strstr(rg, "private(set) var gpuMode = false") != NULL && count(rg, "stereoHealthy") == 0, "RenderRG arranca en nativo (gpuMode = false) y no hay red de seguridad stereoHealthy");
    CHECK(strstr(rg, "\"vr_vs_scene_amp\"") != NULL && strstr(rg, "\"vr_vs_scene\"") != NULL, "pipelines de escena de la lista de la GPU (con y sin amplificación)");
    CHECK(strstr(rg, "d1.depthCompareFunction = .greaterEqual; d1.isDepthWriteEnabled = true") != NULL && strstr(rg, "d2.depthCompareFunction = .greaterEqual; d2.isDepthWriteEnabled = false") != NULL,
          "opaco: prueba + escritura de profundidad; semitransparente: solo prueba (Z invertida)");
    CHECK(count(rg, ".cullMode") == 0 && count(rg, "setCullMode(.none)") >= 2, "sin descarte de reversos (la PS1 no lo tiene)");
    CHECK(count(rg, "drawPrimitives") == 6, "RenderRG.swift: HUD (encodeHud), su paso final, el 2D dentro del estéreo (encodeStereo2D), drawGPU y el post-proceso (estéreo + ojo suelto) dibujan primitivas (hay %d)", count(rg, "drawPrimitives"));
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
    CHECK(strstr(rg, "if gpuMode { nativeDraw = false; nativeDrawnLast = false }") != NULL, "modo gpu (diagnóstico muerto): no se prepara el render nativo");
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
    CHECK(strstr(rg, "n.cinemaGameView = (engineView && Self.viewOK(gameView))") != NULL && count(rg, "n.sceneUnfit") == 0, "cámara del juego solo en cinemáticas del motor (el mundo con cámara VR no la usa aunque la escena no sea apta)");
    CHECK(count(xp, "gameCamScene") == 0 && count(xp, "target = .stereo") == 1, "XRPresent: la pantalla 3D solo por cinemática del motor (view_kind 2), nunca por una escena no apta en el mundo");
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
    CHECK(strstr(vh, "#define MV_RENDER_DEFAULT_NATIVE 1") != NULL, "el modo por defecto es el nativo");
    CHECK(strstr(vh, "int mv_vrhost_render_mode(void) { return atomic_load(&render_req) ? 1 : 0; }") != NULL, "mv_vrhost_render_mode: 0 = gpu, 1 = nativo");
    CHECK(strstr(vh, "int mv_vrhost_render_only_native(void) { return 0; }") != NULL, "no existe «solo nativo»");
    CHECK(strstr(vh, "!strcmp(e, \"nativo\")") != NULL && strstr(vh, "!strcmp(e, \"gpu\")") == NULL && strstr(vh, "ignorado: el único modo es nativo") != NULL, "MV_RENDER=nativo se acepta; cualquier otro valor se ignora (no hay modo gpu)");
    CHECK(strstr(vh, "if (atomic_load(&render_req) && atomic_load(&renderer_ready)) return 1;") != NULL, "la captura nativa solo corre en el modo nativo (ns_gate)");
    CHECK(strstr(vh, "return native_mode() && r && r->active && !cine_cand") != NULL, "el modo gpu no marca batches para el nativo");
    /* B6: el recentrado reinicia el seguimiento de efectos (el espacio de cabeza salta); hold_near 15 cm */
    { char *vi = slurp("app/Sources/VRInput.swift", 1);
      CHECK(vi && count(vi, "ns_gpu_fx_track_request_reset") == 0 && count(nr, "fxTrack") == 0 && count(nr, "fxFollow") == 0, "sin seguimiento de efectos (los efectos de tamaño de mundo no se siguen a la cabeza)");
      CHECK(strstr(nr, "prm.drop_inside_m = 0.15") != NULL && count(nr, "hold_near_m") == 0 && count(nr, "comfort") == 0, "efectos: se descarta lo que cae a menos de 15 cm de la cabeza (sin retención ni confort)");
      free(vi); }
    /* 7) la colisión de la cabeza: escena nativa en el modo nativo, lista de la GPU en el modo gpu (sin captura nativa) */
    CHECK(strstr(vh, "if (!native_mode()) vrhc_feed_scene(&hm, &sc") != NULL, "modo gpu: la colisión de la cabeza se alimenta de la lista de la GPU");
    /* 7b) cercanía al ojo, vista y registro del modo gpu */
    CHECK(strstr(vh, "if (!native_mode()) { o.eye_near_m = GPU3D_NEAR_EYE_M; o.line3d_px = 1.0f; o.coplanar_m = 0.005f; o.rect3d = 1; o.billboard = 1; }") != NULL && strstr(vh, "#define GPU3D_NEAR_EYE_M 0.10f") != NULL, "modo gpu: nada a menos de 0,10 m del ojo (vr_scene eye_near_m)");
    CHECK(count(vh, "vin.gpu3d") == 0 && count(vh, "VRV_K_BACKUP") == 0 && count(vh, "ns_fill_glue") == 0, "vrhost: sin vin.gpu3d, sin respaldo, sin relleno (la medida de cobertura ns_cover_glue sigue como diagnóstico)");
    CHECK(strstr(vh, "int keep3b = vrhost_keep_filter(native_mode(), cine_cand,") != NULL, "modo gpu: fundidos y destellos al panel 2D (vrhost_keep_filter decide qué filtro se conserva)");
    CHECK(strstr(vh, "gpu_capture_hiprec(1)") != NULL && strstr(vh, "if (cap_on && !gte_hp_enabled()) gpu_capture_hiprec(1);") != NULL, "vértices de vista de la GTE en alta precisión siempre que haya captura");
    CHECK(strstr(vh, "\"[gpu3d] fotograma=%u") != NULL && strstr(vh, "gpu3d_log();") != NULL && strstr(vh, "gpu3d_note(&sc, r);") != NULL, "registro [gpu3d] una vez por segundo");
    CHECK(strstr(vh, "post=%u drop_ambig=%u drop_partial=%u drop_incoh=%u reconstruidos=%u drop_huge=%u drop_nan=%u n_rect=%u n_rect_semi_tex=%u n_hw_cull=%u n_rect_hp=%u n_rect3d=%u n_billboard=%u | render=%s") != NULL && strstr(vh, "g3.post += sc->b[i].kind == 3") != NULL, "registro [gpu3d]: lotes de filtro de pantalla (post=N)");
    { char *vw = slurp("vr/host/vr_view.c", 1), *vs = slurp("vr/scene/vr_scene.c", 1);
      CHECK(vw && count(vw, "VRV_K_BACKUP") == 0 && count(vw, "vrview_native_trusted") == 0 && count(vw, "vrview_cover_feed") == 0, "vr_view: sin respaldo, sin cobertura (una escena 3D es siempre ENGINE)");
      CHECK(vw && count(vw, "gpu3d") == 0, "vr_view: sin atajo gpu3d");
      CHECK(vw && strstr(vw, "\"[vista] cambio:") != NULL, "vr_view: registro [vista] de cada cambio con su motivo");
      CHECK(vs && strstr(vs, "o.eye_near_m > 0") != NULL && strstr(vs, "stats.n_near_eye++") != NULL, "vr_scene: descarte por cercanía al ojo y recorte del plano cercano");
      free(vw); free(vs); }

    /* 8) cine: fmv / motor / respaldo (la lista de la GPU no se dibuja en 3D en ninguno; el respaldo es la imagen de la VRAM en la pantalla de cine plana) */
    CHECK(strstr(vh, "vrview_cine_describe(&view, &vin") != NULL && strstr(vh, "mdec_out_words_total()") != NULL, "vrhost registra [cine] tipo=... y lee el decodificador de vídeo");
    CHECK(count(vh, "vrview_cover_feed") == 0 && count(vh, "cover_new") == 0, "vrhost no alimenta la vista con ninguna cobertura");
    CHECK(strstr(vh, "vrview_uploads_cover(") != NULL, "vídeo por subidas a pantalla completa (tiras incluidas)");
    CHECK(strstr(vh, "view.kind == VRV_K_UI || view.kind == VRV_K_VIDEO") != NULL, "la capa 2D entera acompaña a la UI y al vídeo");
    CHECK(count(xp, "engineHealthy") == 0 && count(xp, "engineBackup") == 0 && count(xp, "tipo=respaldo") == 0 && count(xe, "engineHealthy") == 0, "XRPresent no cae a ninguna imagen plana si el nativo no dibuja");
    CHECK(strstr(xp, "XRCinemaLayout.legible(reason: Int32(gs.view_reason), kind: gs.view_kind)") != NULL, "el vídeo y el respaldo se presentan grandes, planos y nítidos");
    CHECK(count(nr, "scenery_stale") == 0 && count(nr, "ns_fill") == 0 && count(nr, "fillBuf") == 0, "NativeRenderer: sin relleno ni escenario obsoleto en el mundo");
    CHECK(strstr(nr, "[nativo] escena_ausente=") != NULL && strstr(nr, "sceneAbsent += 1") != NULL, "escena nativa ausente: se conserva la última válida y se registra [nativo] escena_ausente=N");
    CHECK(count(xe, "worldNearM") == 0 && strstr(xe, "if dg.projMode == 1 { projNote") != NULL && strstr(xe, "projSource = \"sistema\"") != NULL && strstr(xe, "nearOverride: dg.nearPlane))") != NULL, "proyeccion del sistema (drawable, con su depthRange) por defecto; sin plano cercano propio");
    CHECK(count(nr, "setCullMode(.none)") >= 2 && count(nr, "setCullMode(.back)") == 0 && count(nr, "setCullMode(.front)") == 0, "sin descarte de caras en el mundo ni en los recubrimientos");
    { char *yml = slurp("app/project.yml", 0); CHECK(yml && count(yml, "MTL_FAST_MATH") == 0 && count(yml, "MTL_PRESERVE_INVARIANCE") == 0, "sin ajustes globales de Metal (fast-math / invariancia)"); free(yml); }
    CHECK(strstr(rg, "if stereoRequest == nil { stereoRendered = false;") != NULL, "la imagen de la pantalla 3D no sobrevive a la escena (caché)");
    {   /* en cine plano (clases 0, 3 y 4) la imagen sale de la VRAM, sin capa 2D duplicada encima ni mundo: el bloque de la pantalla de cine solo añade UN elemento */
        int items = count_range(xp, "if shown == .cinema, let c = cinema, let tex = c.texture(srgb: linear) {", "if shown == .stereo, Self.gameViewOK(gameView) {", "r.items.append(");
        CHECK(items == 1, "la pantalla de cine añade un único elemento (la imagen de la VRAM) (%d)", items);
    }
    {   /* tamaño de las ventanas 2D en el visor: no depende de la escala de raster de la capa (c.width = lógico x escala) y el aspecto es el real de la capa (no 3/4 fijo) */
        CHECK(count(xp, "Float(c.width) / ppm") == 0 && strstr(xp, "XRCinemaLayout.snappedWidth(W, logicalWidth: c.w, pixelsPerMeter: ppm)") != NULL, "pantalla de cine: escalado entero sobre el ancho LÓGICO (c.w), no el de la textura");
        CHECK(strstr(xp, "it.size = SIMD2<Float>(W, W * XRCinemaLayout.aspect(width: c.width, height: c.height))") != NULL, "pantalla de cine: aspecto real de la imagen");
        int o75 = count_range(xp, "if split && overlay2D, let h = hud, !menuPending {", "if split && shown != .cinema && shown != .stereo && cfg.hudPanels", "0.75");
        CHECK(o75 == 0 && strstr(xp, "XRMenuPanelLayout.handSize(uv: hudMenuUV, aspect: la)") != NULL, "ventana 2D del selector: tamaño fijo por densidad y aspecto real de la capa, sin 0,75 (%d)", o75);
    }
    {   /* selector sin rect asentado: el panel de la mano no muestra nada y no hay paneles de HUD mientras dura la ventana; antebrazo de reserva con el dorso real */
        CHECK(strstr(xp, "let menuPending = split && overlay2D && gs.view_reason == 10 && hudMenuUV == nil") != NULL && strstr(xp, "cfg.hudPanels && !overlay2D, let h = hud {") != NULL && strstr(xp, "if menuPending && i != 0 { continue }") == NULL,
              "selector sin rect asentado (solo con el troceo del 2D): la mano no dibuja la capa entera y con una ventana 2D abierta no se dibuja ningún panel del HUD");
        char *vg = slurp("vr/host/vrgame.c", 1);
        CHECK(vg && count(vg, "vrradar_dorsal_from_grip(") == 2 && count(vg, "bc->hand_rot_offset[i]") == 1 && count(vg, "bc ? bc->hand_rot_offset[side]") == 1, "antebrazo: la ruta viva y la de reserva usan el mismo dorso real (vrradar_dorsal_from_grip + hand_rot_offset)");
        free(vg);
    }
    {   /* 2D SIN TROCEAR (opción 1): todo el 2D del juego es UNA capa completa en UN panel plano. El troceo (regiones del HUD, ventana del menú en la mano, recorte de la capa del selector, radar aparte) está
         * desactivado tras un interruptor global APAGADO por defecto (vl2a_set_split / XRLayer2D.split): apagado, ninguna de esas rutas dibuja ni calcula nada. */
        char *xm = slurp("app/Sources/XRMath.swift", 1), *xh = slurp("app/Sources/XRHud.swift", 1), *rp = slurp("app/Sources/RadarPanel.swift", 1), *la = slurp("vr/scene/vr_layer2d_async.c", 1), *lh = slurp("vr/scene/vr_layer2d_async.h", 1), *vhh = slurp("vr/host/vrhost.h", 1);
        CHECK(xm && xh && lh && vhh && la, "fuentes del 2D sin trocear");
        if (xm && xh && lh && vhh && la) {
            /* interruptor: una sola fuente (C), apagado por defecto, y los tres lados lo leen */
            CHECK(strstr(la, "static _Atomic int g_split;") != NULL && strstr(la, "int vl2a_split(void) { return atomic_load(&g_split); }") != NULL && strstr(lh, "void vl2a_set_split(int on);") != NULL, "interruptor del troceo: _Atomic global de vr_layer2d_async, a cero por defecto");
            CHECK(strstr(vh, "{ const char *e = getenv(\"MV_L2D_SPLIT\"); vl2a_set_split(e && e[0] == '1'); }") != NULL && strstr(vh, "int mv_vrhost_l2d_split(void) { return vl2a_split(); }") != NULL && strstr(vhh, "int mv_vrhost_l2d_split(void);") != NULL,
                  "vrhost: el troceo solo se enciende con MV_L2D_SPLIT=1 y se expone al visor (mv_vrhost_l2d_split)");
            CHECK(strstr(xh, "static var split: Bool { mv_vrhost_l2d_split() != 0 }") != NULL && strstr(xh, "enum XRLayer2D {") != NULL && count(xm, "XRLayer2D") == 0 && count(xm, "mv_vrhost") == 0,
                  "XRLayer2D.split (XRHud.swift) lee el interruptor del host; XRMath.swift (lo compila también bench/build_native.sh sin el host) no depende de él");
            /* C: ventana del menú (rect), filtro de créditos por región y radar aparte */
            CHECK(strstr(la, "const int menu_win = atomic_load(&g_split) && (j->view & 255) == VRV_R_SELECTOR;") != NULL && strstr(la, "vl2d_set_menu(A->L, menu_win);") != NULL && count(la, "VRV_R_SELECTOR") == 1,
                  "vl2a: sin troceo el trabajador nunca activa la ventana del selector (sin rect de menú ni créditos/vida omitidos de la capa)");
            CHECK(strstr(vh, "if (vl2a_split() && vrradar_split(l, &rsplit) == 0 && rsplit.present) {") != NULL && count(vh, "vrradar_split(") == 1, "vrhost: sin troceo el radar no se separa del HUD (la escena y el quad de muñeca no existen)");
            CHECK(strstr(vh, "view.kind == VRV_K_UI || view.kind == VRV_K_VIDEO ? VL_FULL : view.kind == VRV_K_ENGINE ? VL_PANEL_VERIFIED : VL_PANEL") != NULL, "códec, créditos, vídeo (VL_FULL) y cinemáticas (VL_PANEL_VERIFIED) siguen igual: capa completa");
        }
        if (xm && rp) {
            /* visor: ruta de 5b (mano), 6 (regiones del HUD y muñeca) y radar apagadas; 6b = un solo panel con la capa entera */
            CHECK(strstr(xp, "let split = XRLayer2D.split") != NULL && strstr(xp, "if split && overlay2D, let h = hud, !menuPending {") != NULL && strstr(xp, "if split && shown != .cinema && shown != .stereo && cfg.hudPanels && !overlay2D, let h = hud {") != NULL,
                  "XRPresent: panel de la mano (5b) y paneles de regiones/muñeca (6) solo con el troceo");
            int n6b = count_range(xp, "if !split && shown != .cinema && shown != .stereo, let h = hud {", "if pending { pendVeil = min(1", "r.items.append(");
            CHECK(n6b == 1 && count_range(xp, "if !split && shown != .cinema && shown != .stereo, let h = hud {", "if pending { pendVeil = min(1", "XRFramedLayout.fullUV") == 1
                  && count_range(xp, "if !split && shown != .cinema && shown != .stereo, let h = hud {", "if pending { pendVeil = min(1", "uvrect = uv") == 1 && count_range(xp, "if !split && shown != .cinema && shown != .stereo, let h = hud {", "if pending { pendVeil = min(1", "XR2DFilter.current.panelMode") == 1,
                  "XRPresent 6b: UN solo panel con la capa entera (uv 0,0,1,1), geometría XRFramedLayout y muestreo XR2DFilter (%d elementos)", n6b);
            CHECK(strstr(xm, "static let fullUV = SIMD4<Float>(0, 0, 1, 1)") != NULL, "XRFramedLayout.fullUV: capa entera");
            /* la capa 2D del mundo no arrastra 3D: en el modo nativo el host activa drop_world2d (polígonos sin vista dentro del pase del mundo no van al panel; vr_scene.h) */
            CHECK(strstr(vh, "if (native_mode() && view.kind == VRV_K_WORLD && view.reason != VRV_R_SELECTOR && view.reason != VRV_R_PAUSE_WORLD) o.drop_world2d = 1;") != NULL, "vrhost: modo nativo en vista mundo sin selector ni pausa -> opts.drop_world2d (la capa 2D no lleva 3D sin resolver)");
            /* 6b sin trocear: «Paneles del HUD» (hudPanels) NO lo oculta (vida, radar y créditos se ven siempre); con ventana 2D abierta (selector/pausa) va a 0,9 m centrado en la mirada fijada al abrirse */
            CHECK(count_range(xp, "if !split && shown != .cinema && shown != .stereo, let h = hud {", "if pending { pendVeil = min(1", "cfg.hudPanels") == 0, "XRPresent 6b: sin trocear el panel único no depende de cfg.hudPanels");
            CHECK(strstr(xp, "if !overlay2D { overlayAim = nil } else if overlayAim == nil { overlayAim = (hy, XRFollow.pitch(of: head)) }") != NULL
                  && count_range(xp, "if !split && shown != .cinema && shown != .stereo, let h = hud {", "if pending { pendVeil = min(1", "XRFramedLayout.overlayModel(position: hp, yaw: aim.yaw, pitch: aim.pitch)") == 1
                  && count_range(xp, "if !split && shown != .cinema && shown != .stereo, let h = hud {", "if pending { pendVeil = min(1", "XRFramedLayout.overlayScale") == 1
                  && strstr(xm, "static let overlayDistance: Float = 0.9") != NULL && strstr(xm, "static func overlayModel(position p: SIMD3<Float>, yaw: Float, pitch: Float) -> simd_float4x4 {") != NULL,
                  "XRPresent 6b: con ventana 2D abierta el panel va a 0,9 m, centrado en el rumbo y cabeceo de la cabeza al abrirse (fijos mientras dure)");
            CHECK(strstr(xp, "hudPanels=\\(cfg.hudPanels) hud!=nil=\\(hudSeen)") != NULL && strstr(xp, "hudSeen = hud != nil") != NULL, "xr-diag items: hudPanels y hud!=nil");
            /* menú VR: sin troceo se ocultan las opciones que solo afectan al troceo (HUD, tamaño, muñeca) y `sel` indexa la lista visible */
            CHECK(strstr(xp, "private var mainItems: [XRMenuItem] {") != NULL && strstr(xp, "let splitOnly: [XRMenuItem] = [.hud, .hudScale, .wristHud, .wristScale, .wristSide]") != NULL && strstr(xp, "XRMenuItem.allCases.filter { !splitOnly.contains($0) }") != NULL
                  && strstr(xp, "return mainItems.map { it -> (String, String) in") != NULL && strstr(xp, "let it = main[sel]") != NULL && strstr(xp, "XRMenuItem(rawValue: sel)") == NULL,
                  "menú VR: opciones del troceo ocultas sin XRLayer2D.split (filas, selección y activación sobre mainItems)");
            /* cinemáticas del motor: pantalla de cine 3D ESTÉREO (IPD real) por defecto; «screen» (mono) solo como opción; un «screen» guardado migra una vez a «stereo» */
            CHECK(strstr(xp, "var cutscene: Cutscene = .stereo") != NULL && strstr(xp, "else { c.cutscene = .stereo }") != NULL && strstr(xp, "else { c.cutscene = .screen }") == NULL, "XRViewConfig: el defecto de las cinemáticas del motor es .stereo");
            CHECK(strstr(xp, "if !UserDefaults.standard.bool(forKey: \"xr.view.cutscene3.toStereo\") {") != NULL && strstr(xp, "string(forKey: \"xr.view.cutscene3\") == \"screen\" { UserDefaults.standard.set(Cutscene.stereo.rawValue, forKey: \"xr.view.cutscene3\") }") != NULL
                  && strstr(xp, "xr.view.cutscene3.migrated") == NULL, "XRViewConfig: migración única «screen» -> «stereo» con su marca (sin la antigua stereo -> screen)");
            CHECK(strstr(xp, "let mono = cfg.cutscene == .screen") != NULL && strstr(xp, "ipd: mono ? 0 : ipd") != NULL, "XRPresent 4b: el IPD 0 (mono) solo con la opción .screen");
            CHECK(count(rp, "XRLayer2D.split") == 5 && strstr(rp, "guard XRLayer2D.split else { quadValid = false; glow = 0; visibility = 0; return }") != NULL, "RadarPanel: ingest, renderTexture, updatePose, wristFrame y encodeQuad apagados sin troceo (%d)", count(rp, "XRLayer2D.split"));
            CHECK(strstr(rg, "if XRLayer2D.split && lMode == 0 && mv_vrhost_hud2d_menu_rect(&menuUVBuf) != 0 {") != NULL && strstr(rg, "} else if XRLayer2D.split && lMode == 0 && (lView & 255) == 10 && hudMenuUV != nil {") != NULL,
                  "RenderRG: sin troceo no hay rect de la ventana del menú (hudMenuUV siempre nil)");
        }
        free(xm); free(xh); free(rp); free(la); free(lh); free(vhh);
    }
    printf("test_no_gpu3d: %d comprobaciones, %d fallos\n", checks, fails);
    if (!fails) printf("TODO OK\n");
    return fails ? 1 : 0;
}
