#define _GNU_SOURCE
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
      CHECK(ms && strstr(ms, "fragment float4 vr_fs_wave(") != NULL && strstr(ms, "fragment float4 vr_fs_wave_layered(") != NULL, "sombreadores de la onda de pantalla (con y sin capas) en vr_scene.metal");
      free(ms); }
    /* 2c) onda del agua del render nativo: post-proceso de PANTALLA por ojo (no por textura): ns_fs no desplaza uv, NativeRenderer solo da los parámetros y RenderRG codifica el pase tras la escena (antes de los paneles 2D) */
    CHECK(count(sh, "water_wave") == 0 && count(sh, "dfdx") == 0, "ns_fs: sin onda por textura (ni water_wave ni dfdx)");
    CHECK(count(nr, "u.water_wave") == 0 && strstr(nr, "var waveParams: SIMD4<Float>?") != NULL, "NativeRenderer: la onda no va en los uniformes del mundo; waveParams la ofrece al post-proceso");
    CHECK(strstr(rg, "let wavePlan = gpuMode ? nil : planWave(colorTargets: colorTargets, resolveTargets: resolveTargets, layered: layered)") != NULL
          && strstr(rg, "if let w = wavePlan { encodePost(commandBuffer, w.plan, projections: projections, layered: layered, wave: w.wave) }") != NULL
          && count_range(rg, "private func planWave(", "private func encodePost(", "viewportOverride == nil, rasterizationRateMap == nil") == 1
          && count_range(rg, "private func planWave(", "private func encodePost(", "n.waveParams") == 1, "RenderRG: plan y pase de la onda de pantalla solo en el modo nativo, sin viewport propio ni mapa de tasas");
    CHECK(count_range(rg, "private func encodeStereo(", "private func encodeStereoGPU(", "n.waveParams") == 1
          && count_range(rg, "private func encodeStereo(", "private func encodeStereoGPU(", "warpOnly: true, wave:") == 1,
          "encodeStereo nativo: onda de agua de la cinemática (waveParams + encodePost wave)");

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
    CHECK(strstr(xe, "(gs.fx.binocular != 0 || (vrWeapon.active == 0 && vrWeapon.game_weapon == 9))") != NULL && strstr(xe, "gs.fx.binocular != 0 || vrWeapon.active == 0)") == NULL,
          "XRFrameEncoder: el disco de mira de reserva (sight.c sin scope.c) solo con el PSG1 equipado en el juego y sin arma VR mostrada; al desequipar el PSG1 (o con otra arma / sin arma) no sale la mirilla");
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
    CHECK(count(vh, "vin.gpu3d") == 0 && count(vh, "VRV_K_BACKUP") == 0, "vrhost: sin vin.gpu3d y sin respaldo (la medida de cobertura ns_cover_glue sigue como diagnóstico)");
    /* relleno por la verdad del motor: retirado de la app; el anfitrión ya no lo alimenta ni lo consulta (competía por el cerrojo de ns_fx con el hilo de emulación) */
    CHECK(count(vh, "ns_fill_glue") == 0 && count(vh, "ns_fill_gate") == 0, "vrhost: el relleno está apagado del todo (ni instantáneas, ni trabajador, ni línea [relleno])");
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
    /* el relleno por la verdad del motor ya no tiene ruta de dibujo en la app (medido: solo aportaba efectos flotantes y su «ausente» era desfase de cámara): ni recogida en el renderer ni ajuste en el menú; el diagnóstico `[relleno]` del anfitrión sigue */
    CHECK(count(nr, "scenery_stale") == 0 && count(nr, "ns_fill_glue") == 0 && count(nr, "fillBuf") == 0, "NativeRenderer: sin recogida del relleno");
    CHECK(count(xp, "fill3D") == 0 && count(xp, "ns_fill_glue") == 0 && count(xp, "Relleno en pantalla 3D") == 0, "XRPresent: sin ajuste «Relleno en pantalla 3D»");
    /* pantalla 3D con la cámara del motor: reglas de descarte de ns_gpu_engine_tri y tres interruptores de diagnóstico en los ajustes, encendidos de serie y persistentes */
    CHECK(strstr(sh, "const float3 zc = max(zv, float3(U.misc.y));") != NULL && strstr(sh, "const uint eng_off = uint(U.misc.z + 0.5);") != NULL &&
          strstr(sh, "rescue = zmax >= U.cam_x.w && (!rev || front);") != NULL && strstr(sh, "back = !rescue;") != NULL && strstr(sh, "if (rescue) cf |= NSG_CF_WINCUT;") != NULL &&
          strstr(sh, "if (ez > 0.0 && abs(U.cam_x.w * dot(U.cam_x.xyz, er)) <= NSG_ENG_WIN_X * ez && abs(U.cam_y.w * dot(U.cam_y.xyz, er)) <= NSG_ENG_WIN_Y * ez) discard_fragment();") != NULL,
          "sombreador: regla del motor por triángulo entero (ajuste al plano H/2); lo rescatado (no cercano, anverso desde un ojo de la vista) solo se dibuja fuera de la ventana del motor");
    CHECK(strstr(nr, "let engOff = ns_gpu_engine_off()") != NULL && strstr(nr, "cullBack ? 0.5 * camH : 0, Float(engOff), Self.tex3DMMPX ? 1 : 0)") != NULL && strstr(nr, "(engOff & UInt32(NSG_ENG_OFF_BACK)) != 0 ? 0 : Float(NSG_CULL_ALL)") != NULL,
          "NativeRenderer: las reglas apagadas llegan al sombreador (misc.z y cull_eye.w)");
    CHECK(strstr(xp, "var engBehind = true") != NULL && strstr(xp, "var engSize = true") != NULL && strstr(xp, "var engBack = true") != NULL &&
          strstr(xp, "c.engBehind = b(\"xr.eng.behind\", true)") != NULL && strstr(xp, "d.set(engSize, forKey: \"xr.eng.size\")") != NULL && strstr(xp, "d.set(engBack, forKey: \"xr.eng.back\")") != NULL,
          "XRViewConfig: descartes de la pantalla 3D encendidos de serie y persistentes");
    CHECK(strstr(xp, "ns_gpu_engine_set_off(m)") != NULL && strstr(xp, "if !size { m |= UInt32(NSG_ENG_OFF_SIZE) }") != NULL && strstr(xp, "applyHands(c.newHands); applyDiscards(behind: c.engBehind, size: c.engSize, back: c.engBack, split: c.engSplit)") != NULL && count(xp, "XRViewConfig.applyDiscards(behind: cfg.engBehind, size: cfg.engSize, back: cfg.engBack, split: cfg.engSplit); cfg.save()") == 4 &&
          strstr(xp, "(\"Descarte por detrás (3D)\", yn(cfg.engBehind))") != NULL && strstr(xp, "(\"Tope de tamaño (3D)\", yn(cfg.engSize))") != NULL && strstr(xp, "(\"Reversos (3D)\", yn(cfg.engBack))") != NULL,
          "los interruptores aplican la máscara al arrancar y al conmutar, y tienen etiqueta en el menú");
    /* «Caras cercanas (3D)»: Original (de serie: lo que el motor original parte se dibuja con su geometría real) / Recortadas; persistente; el sombreador lo decide con NSG_ENG_OFF_SPLIT */
    CHECK(strstr(xp, "var engSplit = true") != NULL && strstr(xp, "c.engSplit = b(\"xr.eng.split\", true)") != NULL && strstr(xp, "d.set(engSplit, forKey: \"xr.eng.split\")") != NULL &&
          strstr(xp, "if !split { m |= UInt32(NSG_ENG_OFF_SPLIT) }") != NULL && strstr(xp, "(\"Caras cercanas (3D)\", cfg.engSplit ? \"Original\" : \"Recortadas\")") != NULL, "XRViewConfig.engSplit: «Original» de serie, persistente y con etiqueta");
    CHECK(strstr(sh, "(P.flags & NSG_PF_NOSPLIT) == 0u && (eng_off & uint(NSG_ENG_OFF_SPLIT)) == 0u && !rev && (cull || min(min(zv.x, zv.y), zv.z) < U.misc.y)") != NULL &&
          strstr(sh, "abs((sx.y - sx.x) * (sy.z - sy.x) - (sx.z - sx.x) * (sy.y - sy.x)) > NSG_DIV_AREA;") != NULL && strstr(sh, "} else if (cull) {") != NULL, "sombreador: cara de un objeto sin GBOUND que el motor original parte: geometría real");
    /* el rescate del reverso lo decide cada vista con SU ojo (con un pase por ojo, el del pase), no «algún ojo» */
    CHECK(strstr(sh, "const bool front = sense * dot(v0 - U.eye[amp].xyz, nvr) > 0.0;") != NULL && count(sh, "U.eye[1].xyz, nvr)") == 0, "sombreador: el rescate del reverso usa el ojo de la vista");
    /* la cota de paralaje de la pantalla 3D se pide en todas las compilaciones (los bancos traen la cabecera y un sustituto: 0 = sin cota) */
    {
        char *bh = slurp("bench/NativeBridge.h", 1), *fs = slurp("bench/c/fill_stub.c", 1), *ah = slurp("app/Sources/Bridging-Header.h", 1);
        const char *zs = strstr(nr, "cineFilter.z_static = 0.001 * ns_solid_view_near(sc, store)");
        CHECK(zs != NULL && zs - nr > 40 && !memmem(zs - 40, 40, "#if", 3), "NativeRenderer: la cota de paralaje no depende de MV_BENCH");
        CHECK(zs != NULL && zs - nr > 11 && !memcmp(zs - 11, "if fresh { ", 11), "NativeRenderer: la cota de paralaje se calcula solo con escena nueva");
        CHECK(bh && fs && ah && strstr(bh, "#include \"ns_solid.h\"") != NULL && strstr(ah, "#include \"ns_solid.h\"") != NULL &&
              strstr(fs, "float ns_solid_view_near(const NsScene *sc, const NsMeshStore *st) { (void)sc; (void)st; return 0.0f; }") != NULL, "los dos bancos declaran y definen ns_solid_view_near");
        free(bh); free(fs); free(ah);
    }
    /* fuera de la pantalla 3D la medida de ventana del trabajador de cobertura se tira: al entrar no se vuelca `nativo_tapa` con una medida vieja */
    CHECK(strstr(vh, "else { disc.have_prev = 0; disc.k = 0; disc.f0 = (uint32_t)hle_frame_count(); }") != NULL &&
          strstr(vh, "const int hw = ns_cover_glue_win(win, &wf) && (int32_t)(wf - disc.f0) > 0;") != NULL, "vrhost: las medidas de ventana de listas anteriores a la entrada en ENGINE se descartan");
    {   /* la página más larga del menú (principal o Diagnóstico) cabe: 40 px por fila desde y = 106 y el pie de ayuda 56 px por encima del borde */
        const char *m = strstr(xp, "private static let menuW = 1024, menuH = ");
        int mh = m ? atoi(m + strlen("private static let menuW = 1024, menuH = ")) : 0, rows = 0;
        const char *names[2] = { "enum XRMenuItem: Int, CaseIterable {", "enum XRDiagItem: Int, CaseIterable {" };
        int per[2] = { 0, 0 };
        for (int k = 0; k < 2; k++) {
            const char *e = strstr(xp, names[k]), *q = e ? strstr(e, "case ") : NULL, *end = q ? strchr(q, '\n') : NULL;
            if (q && end) { per[k] = 1; for (; q < end; q++) per[k] += *q == ','; }
            if (per[k] > rows) rows = per[k];
        }
        CHECK(per[0] > 0 && per[1] > 0 && mh - 56 >= 106 + 40 * rows + 18, "menú: principal %d filas, Diagnóstico %d filas, %d px", per[0], per[1], mh);
        /* «Filtro de texturas 3D» ya no existe en el menú ni se guarda: el filtro MMPX de las texturas 3D queda siempre apagado */
        CHECK(strstr(nr, "static var tex3DMMPX = false") != NULL && count(xp, "tex3D") == 0 && count(xp, "Filtro de texturas 3D") == 0 && count(xp, "xr.view.tex3d") == 0, "texturas 3D: sin ajuste en el menú, apagado fijo");
        /* esos siete ajustes viven solo en la página de Diagnóstico (no en el menú principal) */
        { const char *de = strstr(xp, "enum XRDiagItem: Int, CaseIterable {"), *dl = de ? strchr(de, '\n') : NULL, *dn = dl ? strchr(dl + 1, '\n') : NULL;
          const char *me = strstr(xp, "enum XRMenuItem: Int, CaseIterable {"), *ml = me ? strchr(me, '\n') : NULL, *mn = ml ? strchr(ml + 1, '\n') : NULL;
          const char *k[7] = { "engBehind", "engSize", "engBack", "engSplit", "exact2D", "integerScale", "debugAxes" };
          int in_diag = 0, in_main = 0;
          for (int i = 0; i < 7; i++) {
              const char *p = de ? strstr(de, k[i]) : NULL; in_diag += p && dn && p < dn;
              p = me ? strstr(me, k[i]) : NULL; in_main += p && mn && p < mn;
          }
          CHECK(in_diag == 7 && in_main == 0, "menú: los 7 ajustes 3D/2D están en Diagnóstico y no en el principal (%d/%d)", in_diag, in_main); }
    }
    CHECK(strstr(nr, "[nativo] escena_ausente=") != NULL && strstr(nr, "sceneAbsent += 1") != NULL, "escena nativa ausente: se conserva la última válida y se registra [nativo] escena_ausente=N");
    CHECK(count(xe, "worldNearM") == 0 && strstr(xe, "if dg.projMode == 1 { projNote") != NULL && strstr(xe, "projSource = \"sistema\"") != NULL && strstr(xe, "nearOverride: dg.nearPlane))") != NULL, "proyeccion del sistema (drawable, con su depthRange) por defecto; sin plano cercano propio");
    CHECK(count(nr, "setCullMode(.none)") >= 2 && count(nr, "setCullMode(.back)") == 0 && count(nr, "setCullMode(.front)") == 0, "sin descarte de caras en el mundo ni en los recubrimientos");
    { char *yml = slurp("app/project.yml", 0); CHECK(yml && count(yml, "MTL_FAST_MATH") == 0 && count(yml, "MTL_PRESERVE_INVARIANCE") == 0, "sin ajustes globales de Metal (fast-math / invariancia)"); free(yml); }
    CHECK(strstr(rg, "if stereoRequest == nil { stereoRendered = false;") != NULL, "la imagen de la pantalla 3D no sobrevive a la escena (caché)");
    {   /* en cine plano (clases 0, 3 y 4) la imagen sale de la VRAM, sin capa 2D duplicada encima ni mundo: el bloque de la pantalla de cine solo añade UN elemento */
        int items = count_range(xp, "if shown == .cinema, !nativeOnly, let c = cinema, let tex = c.texture(srgb: linear) {", "if shown == .stereo, Self.gameViewOK(gameView) {", "r.items.append(");
        CHECK(items == 1, "la pantalla de cine añade un único elemento (la imagen de la VRAM) (%d)", items);
        /* cinemática del motor (view_kind 2): la pantalla de cine NUNCA enseña la imagen de PS1 (ni en el fundido de entrada, con la pantalla aún en .cinema, ni si falta la cámara del juego): solo el render nativo; si no está listo, negro.
         * Solo la opción explícita «Pantalla de cine» (cutscene == .cinema, imagen plana) la conserva. */
        CHECK(strstr(xp, "let nativeOnly = gs.view_kind == 2 && cfg.cutscene != .cinema") != NULL, "XRPresent: en cinemática del motor la pantalla de cine plana (imagen de la VRAM) no se dibuja salvo la opción «Pantalla de cine»");
    }
    {   /* tamaño de las ventanas 2D en el visor: no depende de la escala de raster de la capa (c.width = lógico x escala) y el aspecto es el real de la capa (no 3/4 fijo) */
        CHECK(count(xp, "Float(c.width) / ppm") == 0 && strstr(xp, "XRCinemaLayout.snappedWidth(W, logicalWidth: c.w, pixelsPerMeter: ppm)") != NULL, "pantalla de cine: escalado entero sobre el ancho LÓGICO (c.w), no el de la textura");
        CHECK(strstr(xp, "it.size = SIMD2<Float>(W, W * XRCinemaLayout.aspect(width: c.width, height: c.height))") != NULL, "pantalla de cine: aspecto real de la imagen");
        int o75 = count_range(xp, "if split && overlay2D, let h = hud {", "if split && shown != .cinema && shown != .stereo && cfg.hudPanels", "0.75");
        CHECK(o75 == 0 && strstr(xp, "XRMenuPanelLayout.handSize(uv: hudMenuUV, aspect: la)") != NULL, "ventana 2D del selector: tamaño fijo por densidad y aspecto real de la capa, sin 0,75 (%d)", o75);
    }
    {   /* selector sin rect asentado: el panel de la mano no muestra nada y no hay paneles de HUD mientras dura la ventana; antebrazo de reserva con el dorso real */
        CHECK(strstr(xp, "menuPending") == NULL && strstr(xp, "cfg.hudPanels && !overlay2D, let h = hud {") != NULL && strstr(xp, "if menuPending && i != 0 { continue }") == NULL,
              "sin rect de menú (la ventana es la capa WINDOW): sin menuPending; con una ventana 2D abierta no se dibuja ningún panel del HUD troceado");
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
            CHECK(strstr(la, "vl2d_set_menu") == NULL && strstr(la, "vl2d_hide_credits") == NULL && strstr(la, "menu_win") == NULL && strstr(la, "menu_rect") == NULL,
                  "vl2a: sin heurísticas de menú / créditos en la capa (la ventana y los créditos se separan por procedencia, vr_prov.h)");
            CHECK(strstr(vh, "if (vl2a_split() && vrradar_split(l, &rsplit) == 0 && rsplit.present) {") != NULL && count(vh, "vrradar_split(") == 1, "vrhost: sin troceo el radar no se separa del HUD (la escena y el quad de muñeca no existen)");
            CHECK(strstr(vh, "view.kind == VRV_K_UI || view.kind == VRV_K_VIDEO ? VL_FULL : view.kind == VRV_K_ENGINE ? VL_PANEL_VERIFIED : VL_PANEL") != NULL, "códec, créditos, vídeo (VL_FULL) y cinemáticas (VL_PANEL_VERIFIED) siguen igual: capa completa");
        }
        if (xm && rp) {
            /* visor: ruta de 5b (mano), 6 (regiones del HUD y muñeca) y radar apagadas; 6b = un solo panel con la capa entera */
            CHECK(strstr(xp, "let split = XRLayer2D.split") != NULL && strstr(xp, "if split && overlay2D, let h = hud {") != NULL && strstr(xp, "if split && shown != .cinema && shown != .stereo && cfg.hudPanels && !overlay2D, let h = hud {") != NULL,
                  "XRPresent: panel de la mano (5b) y paneles de regiones/muñeca (6) solo con el troceo");
            int n6b = count_range(xp, "if !split && shown != .cinema && shown != .stereo, let h = hud {", "if !split && shown != .cinema && shown != .stereo && overlay2D, let wh = hudWindow {", "r.items.append(");
            CHECK(n6b == 1 && count_range(xp, "if !split && shown != .cinema && shown != .stereo, let h = hud {", "if !split && shown != .cinema && shown != .stereo && overlay2D, let wh = hudWindow {", "XRFramedLayout.fullUV") == 1
                  && count_range(xp, "if !split && shown != .cinema && shown != .stereo, let h = hud {", "if !split && shown != .cinema && shown != .stereo && overlay2D, let wh = hudWindow {", "uvrect = uv") == 1 && count_range(xp, "if !split && shown != .cinema && shown != .stereo, let h = hud {", "if !split && shown != .cinema && shown != .stereo && overlay2D, let wh = hudWindow {", "XR2DFilter.current.panelMode") == 1,
                  "XRPresent 6b: UN solo panel con la capa entera (uv 0,0,1,1), sin tiras ni recortes, geometría XRFramedLayout y muestreo XR2DFilter (%d elementos)", n6b);
            CHECK(strstr(xm, "static let fullUV = SIMD4<Float>(0, 0, 1, 1)") != NULL, "XRFramedLayout.fullUV: capa entera");
            /* la capa 2D del mundo y de las cinemáticas del motor no arrastra 3D: en el modo nativo el host activa drop_world2d (polígonos sin vista dentro del pase 3D no van al panel; vr_scene.h; regla en vrhost_drop_world2d, probada en vr/host/test_view.c) */
            CHECK(strstr(vh, "if (vrhost_drop_world2d(native_mode(), view.kind, view.reason)) o.drop_world2d = 1;") != NULL, "vrhost: modo nativo en vista mundo (sin selector ni pausa) o cinemática del motor -> opts.drop_world2d (la capa 2D no lleva 3D sin resolver)");
            /* 6b sin trocear: «Paneles del HUD» (hudPanels) NO lo oculta (vida, radar y créditos se ven siempre); con ventana 2D abierta (selector/pausa) va a 0,9 m centrado en la mirada fijada al abrirse */
            CHECK(count_range(xp, "if !split && shown != .cinema && shown != .stereo, let h = hud {", "if pending || blackout || worldDue { pendVeil = min(1", "cfg.hudPanels") == 0, "XRPresent 6b: sin trocear el panel único no depende de cfg.hudPanels");
            /* fundido a negro de carga (vr_view.h, VrView.black -> MvVrGameState.view_black): la app sube el velo con él, no lo baja mientras la pantalla mostrada no sea la pedida (sin destello de la
             * vista retenida al cambiar de pantalla) y cambia de pantalla en cuanto está en negro; la escena de una zona nueva espera a su VRAM hasta 20 fotogramas (6 se agotaban siempre) */
            CHECK(strstr(xp, "let blackout = gs.view_black != 0") != NULL
                  && strstr(xp, "let worldDue = gs.view_mode != 1 && gs.view_reason == 0") != NULL
                  && strstr(xp, "if pending || blackout || worldDue { pendVeil = min(1, pendVeil + d / 0.15) } else if target == shown { pendVeil = max(0, pendVeil - d / 0.28) }") != NULL,
                  "XRPresent: el negro de carga sube el velo y solo baja con la pantalla pedida ya mostrada");
            CHECK(strstr(xp, "if veil >= 1 || pendVeil >= 1 { veil = 1; shown = target; needAnchor = true }") != NULL, "XRPresent: con el velo de carga en negro el cambio de pantalla es inmediato");
            CHECK(strstr(vhh, "int32_t view_black;") != NULL && strstr(vh, "g.view_black = view.black;") != NULL, "vrhost publica el negro de la vista");
            CHECK(strstr(nr, "static let syncWaitMax = 20") != NULL, "NativeRenderer: la escena de una época nueva espera a su VRAM hasta 20 fotogramas");
            CHECK(strstr(xp, "overlayAim") == NULL && strstr(xp, "overlayModel") == NULL && strstr(xp, "overlayDistance") == NULL && strstr(xp, "overlayScale") == NULL
                  && strstr(xm, "overlayDistance") == NULL && strstr(xm, "overlayModel") == NULL && strstr(xm, "overlayScale") == NULL,
                  "XRPresent 6b/6c: sin overlayAim/overlayModel/overlayDistance/overlayScale: nada se acerca ni se centra en la mirada al abrir L2/R2");
            /* el HUD (6b: vida, vida del enemigo, oxigeno, radar) NO depende de overlay2D: no se mueve, escala ni cambia de profundidad al abrir o cerrar los menus */
            CHECK(count_range(xp, "if !split && shown != .cinema && shown != .stereo, let h = hud {", "if !split && shown != .cinema && shown != .stereo && overlay2D, let wh = hudWindow {", "overlay2D") == 0
                  && count_range(xp, "if !split && shown != .cinema && shown != .stereo, let h = hud {", "if !split && shown != .cinema && shown != .stereo && overlay2D, let wh = hudWindow {", "XRFramedLayout.distance") == 2,
                  "XRPresent 6b: el HUD va siempre a XRFramedLayout.distance con el rumbo hudYaw, sin depender de overlay2D");
            /* 6c: la ventana del selector/armas/pausa va en el MISMO plano fijo que el HUD (misma distancia, rumbo y tamano): L2 y R2 siempre a la misma profundidad, abriendo y cerrando */
            CHECK(count_range(xp, "if !split && shown != .cinema && shown != .stereo && overlay2D, let wh = hudWindow {", "if !split && shown != .cinema && shown != .stereo, let fh = hudFull {", "XRMath.rotationY(hudYaw) * XRMath.translation(SIMD3<Float>(c.x, c.y, -XRFramedLayout.distance))") == 1
                  && count_range(xp, "if !split && shown != .cinema && shown != .stereo && overlay2D, let wh = hudWindow {", "if !split && shown != .cinema && shown != .stereo, let fh = hudFull {", "it.distance = XRFramedLayout.distance") == 1
                  && count_range(xp, "if !split && shown != .cinema && shown != .stereo && overlay2D, let wh = hudWindow {", "if !split && shown != .cinema && shown != .stereo, let fh = hudFull {", "r.items.append(") == 1,
                  "XRPresent 6c: la ventana 2D va al mismo plano fijo que el HUD (XRFramedLayout.distance, hudYaw): menus L2 y R2 a igual profundidad siempre");
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
            CHECK(strstr(rg, "mv_vrhost_hud2d_menu_rect") == NULL && strstr(rg, "menuUVBuf") == NULL && strstr(rg, "hudMenuUV = nil ") != NULL,
                  "RenderRG: sin rect de la ventana del menú (hudMenuUV siempre nil; la ventana es la capa WINDOW)");
            CHECK(strstr(rg, "private func takeAuxLayer(_ k: Int, _ commandBuffer: MTLCommandBuffer) {") != NULL && strstr(rg, "takeAuxLayer(1, commandBuffer); takeAuxLayer(2, commandBuffer)") != NULL && count_range(rg, "private func takeAuxLayer(", "private struct BoxMaskIn", "drawPrimitives") == 0
                  && strstr(rg, "u.vfx = warpOnly ? SIMD4<Float>(0, 0, 0, 0) : gameFx") != NULL,
                  "RenderRG: capas WINDOW y FULL (takeAuxLayer) y viñeta / bandas del juego en el pase de post existente, sin drawPrimitives nuevos");
        }
        free(xm); free(xh); free(rp); free(la); free(lh); free(vhh);
    }
    {   /* vistas especiales del arma (HUD propio por capa y recorte del misil): el marco y la vista 4:3 se convierten a uv de la textura del HUD en Swift (frameUV, buffer 3) y el sombreador los usa; SpcDrawU sigue en 192 bytes */
        char *rw = slurp("app/Sources/RenderWeapon.swift", 1), *wm = slurp("render/metal/vr_weapon.metal", 0);
        CHECK(rw && wm, "fuentes de las vistas especiales del arma");
        if (rw && wm) {
            CHECK(strstr(rw, "MemoryLayout<SpcDrawU>.stride != 192") != NULL && strstr(rw, "MemoryLayout<SpcHudU>.stride != 48") != NULL && strstr(rw, "w.frameUV(") != NULL && strstr(rw, "sv.kind == Int32(MV_VIEW_MISSILE) || sv.kind == Int32(MV_VIEW_AA_SCREEN)") != NULL
                  && strstr(rw, "encoder.setFragmentBytes(&hd, length: MemoryLayout<SpcHudU>.stride, index: 3)") != NULL && strstr(rg, "hudPremultiplied: hudHQ, weaponHud: auxTexture[3], hudWin: hudWin,") != NULL,
                  "RenderWeapon: marco y vista 4:3 en uv de la textura (frameUV del hudWin de RGRenderer) por el buffer 3; layouts vigilados (192 / 48)");
            CHECK(strstr(wm, "#define VR_SPC_BUF_HUD 3") != NULL && strstr(wm, "constant VrSpcHud &hd [[buffer(VR_SPC_BUF_HUD)]]") != NULL
                  && count(wm, "vr_spc_hud_keep") == 0 && count(wm, "vr_spc_hud_icon(") == 0
                  && count(wm, "float2 hu = hd.view.xy + (float2(d.uv.x, d.uv.y) + q * float2(d.uv.z, d.uv.w)) * hd.view.zw;") == 2 && strstr(wm, "(ambas 4:3)") == NULL,
                  "vr_weapon.metal: sin máscara por posición (la textura 1 es la capa WEAPON_HUD); el recorte del misil / AA sobre la vista 4:3 de la textura; modos 0 y 1 sin cambios");
        }
        if (rw && wm) CHECK(strstr(rw, "mv_vrgame_special_sync(&synced)") != NULL && strstr(rw, "hd.nik = simd_float4(") != NULL && strstr(wm, "float4 nik;") != NULL && strstr(wm, "vr_spc_dot(rgb, r, ain") != NULL
                            && count(wm, "vr_spc_dot(rgb, r, ain") == 2 && strstr(wm, "float3(1.0, 0.08, 0.05)") != NULL && strstr(wm, "float3(0.2, 1.0, 0.25)") != NULL,
                            "LED: sync con la mano del modelo, HUD propio del Nikita (nik) y puntos de mira (rojo Stinger, verde Nikita)");
        if (wm) CHECK(count(wm, "vr_spc_dot(rgb, r, ain, 0.0065 / max(0.5 * (1.0 - 2.0 * d.p1.y), 0.05)") == 2 && strstr(wm, "0.0125") == NULL,
                      "LED: punto de mira PEQUEÑO e igual en Stinger y Nikita (radio 0,65 %% de la altura de la pantalla)");
        free(rw); free(wm);
    }
    {   /* HUD del jugador con Stinger / Nikita: el panel plano 2D (radar, vida, alerta...) SIGUE como con cualquier arma, ENTERO y sin recortes por posición. El HUD propio del arma (stnsight.c, sgtrect3.c, rmissile.c) es la clase
         * WEAPON_HUD por PROCEDENCIA (vr_prov.h): capa 2D propia (índice 3) que el panel plano no incluye y que solo leen las pantallas LED; hide_hud solo con la pantalla grande del misil ante la cara (taparía la cámara) y los prismáticos.
         * Fallan si se vuelve a ocultar el radar / el panel con el arma equipada, a recortar el panel por rectángulos, o si la LED vuelve a leer la capa del HUD del jugador. */
        char *xm = slurp("app/Sources/XRMath.swift", 1), *vg = slurp("vr/host/vrgame.c", 1), *wm = slurp("render/metal/vr_weapon.metal", 0), *pr = slurp("vr/scene/vr_prov.c", 1),
             *pru = slurp("vr/scene/vr_prov_rules.c", 1), *rw2 = slurp("app/Sources/RenderWeapon.swift", 1);
        if (xm && vg && wm && pr && pru && rw2) {
            CHECK(count(rg, "hide_hud") == 0 && count_range(rg, "if last && drawBody {", "weapon?.sampleCount", "radar?.encodeQuad(") == 1,
                  "RenderRG: el radar VR (encodeQuad) se dibuja siempre en el pase que resuelve el MSAA, sin condición por hide_hud / vista especial");
            CHECK(count(vg, "sv.hide_hud = 0;") == 3 && count(vg, "sv.hide_hud = 1;") == 1 && count(vg, "route != GLED_STANDBY;") == 0 && count(vg, "sv.hide_hud = sg.active") == 0,
                  "vrgame.c: hide_hud = 0 con el Stinger (LED y respaldo) y la pantalla LED del Nikita; 1 solo con la pantalla grande del misil (%d / %d)", count(vg, "sv.hide_hud = 0;"), count(vg, "sv.hide_hud = 1;"));
            CHECK(count(xe, "if special && sv.hide_hud != 0 { hud = nil }") == 1 && count(xe, "weaponHudMask") == 0 && count(xp, "weaponHudMask") == 0 && count(xp, "strips") == 0 && count(xm, "strips") == 0 && count(xm, "weaponHudMask") == 0,
                  "XRFrameEncoder / XRPresent / XRMath: el panel plano solo se quita con hide_hud (pantalla grande / prismáticos); sin weaponHudMask ni tiras (el HUD del arma ya no llega a esa capa)");
            CHECK(count(wm, "vr_spc_hud_keep") == 0 && strstr(wm, "p.x >= 14.0 && p.x <= 60.0") == NULL && strstr(wm, "p.x >= 112.0 && p.x <= 176.0") == NULL && strstr(xp, "r(14, 44, 60, 212)") == NULL && strstr(xp, "r(112, 96, 176, 120)") == NULL,
                  "vr_weapon.metal / XRPresent.swift: sin la lista blanca ni los rectángulos del HUD del arma (recorte por posición)");
            CHECK(count(pr, "case VPC_WEAPON_HUD: return VPC_L_WEAPON;") == 1 && count(pru, "static const char *const N_WEAPON[] = {\"stnsight.c\", \"sgtrect3.c\", \"rmissile.c\", NULL};") == 1
                  && count(pru, "if (vpc_is_weapon_hud(e)) return VPC_WEAPON_HUD;") == 1 && count_range(pru, "if (vpc_is_weapon_hud(e)) return VPC_WEAPON_HUD;", "has_name(e, N_SCOPE)", "return VPC_WEAPON_HUD") == 1,
                  "vr_prov: stnsight.c / sgtrect3.c / rmissile.c = WEAPON_HUD por procedencia (antes de la regla de mira, que también casaba con «stnsight.c»), capa propia VPC_L_WEAPON");
            CHECK(count(rg, "takeAuxLayer(3, commandBuffer)") == 1 && count(rg, "weaponHud: auxTexture[3]") == 1 && count(xe, "auxDiag[3]") == 1,
                  "RenderRG: la capa 3 (HUD del arma) se toma como las demás auxiliares y solo se pasa a encodeSpecial; XRFrameEncoder no la dibuja como panel (layerPlanes solo con hud, ventana, completa)");
            CHECK(count_range(xe, "var layerPlanes", "finalPass.layerPlanes = layerPlanes", "auxTexture[3]") == 0 && count_range(xe, "var layerPlanes", "finalPass.layerPlanes = layerPlanes", "auxPlanes[3]") == 0
                  && strstr(rw2, "let hud: MTLTexture? = weaponLayer ? (hudWin != nil ? weaponHud : nil) : hudIn") != NULL
                  && strstr(rw2, "let weaponLayer = sv.kind == Int32(MV_VIEW_AA_SCREEN) || sv.kind == Int32(MV_VIEW_MISSILE) || (sv.kind == Int32(MV_VIEW_AA_SIGHT) && sv.round != 2)") != NULL,
                  "encodeSpecial: las LED (Stinger / Nikita) y el visor de reserva del Stinger leen la capa WEAPON_HUD en vez de la del HUD; los prismáticos (round 2) siguen con la del HUD");
            CHECK(count(xm, "MV_VIEW_") == 0,
                  "XRMath.swift no usa símbolos de vrgame.h (MV_VIEW_*): bench/build_native.sh lo compila solo con NativeBridge.h");
        }
        free(xm); free(vg); free(wm); free(pr); free(pru); free(rw2);
    }
    printf("test_no_gpu3d: %d comprobaciones, %d fallos\n", checks, fails);
    if (!fails) printf("TODO OK\n");
    return fails ? 1 : 0;
}
