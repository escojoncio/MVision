import Foundation
import Metal
import simd
import QuartzCore
import CoreGraphics
import CoreText

// Codificación de un fotograma inmersivo SIN depender de CompositorServices: el bucle (XRRenderLoop, app) y el banco de render de CI
// (bench/, macOS) le pasan las mismas estructuras planas. Así lo que se prueba en CI es exactamente el código que corre en el visor:
// vistas/proyecciones por ojo, presentación (mundo / cine / diorama), pase de escena (RGRenderer) y pase final (XRFinalPass).

/// Una vista (ojo) del drawable: equivale a `LayerRenderer.Drawable.View` + `textureMap.viewport`.
struct XREyeView {
    var transform: simd_float4x4        // dispositivo <- ojo (convención de CompositorServices: view.transform)
    var tangents: SIMD4<Float>          // izquierda, derecha, arriba, abajo (positivas)
    var viewport: MTLViewport
}

final class XRFrameEncoder {
    let renderer: RGRenderer
    let presenter: XRPresenter
    private let finalPass: XRFinalPass
    private var targets: XRTargets
    let layered: Bool
    let sampleCount: Int
    let viewCount = 2
    /// Destino del registro de diagnóstico (1 vez por segundo). La app lo apunta a Storage.log; el banco, a la salida estándar.
    var log: (String) -> Void = { _ in }
    private var diagT: CFTimeInterval = 0
    private var cine3dT: CFTimeInterval = 0
    private(set) var lastDiag = ""
    private(set) var frameNo = 0
    private var axes: XRDebugAxes?
    private var firstEncodeT: CFTimeInterval = 0
    private(set) var projSource = "propia"
    private var projNote = ""
    /// Fluidez de la locomoción (vr/host/vr_smooth.h, bitácora): corrección rígida del mundo por fotograma de render (giro del stick pedido y aún no visto en la escena
    /// + traslación del personaje extrapolada). Solo mueve la geometría del mundo; el arma, las manos, el radar y los paneles van en el espacio del usuario.
    var smoothMotion = true
    /// Viñeta de confort 0..1 (la calcula vr_input: crece al girar con el stick / moverse); 0 = sin viñeta. La app la fija cada fotograma.
    var comfortVignette: Float = 0
    private(set) var lastSmooth = VrSmCorr()
    /// Lectura sin bloqueo de unos píxeles del drawable final (diagnóstico, 1 vez por segundo).
    /// Distancia (m) a la que se rellena la profundidad de los píxeles sin geometría opaca (ver XRDiag.fillDepth).
    static let fillDepthMeters: Float = 50
    private var probe: XRPixelProbe?
    private var probeT: CFTimeInterval = 0
    private var stereoProbe: XRStereoProbe?
    private var stereoProbeT: CFTimeInterval = 0

    init?(device: MTLDevice, colorFormat: MTLPixelFormat, depthFormat: MTLPixelFormat, layered: Bool, sampleCount: Int) {
        guard let fp = XRFinalPass(device: device, colorFormat: colorFormat, depthFormat: depthFormat, layered: layered) else { return nil }
        finalPass = fp
        renderer = RGRenderer(device: device, colorFormat: colorFormat, depthFormat: depthFormat,
                              sampleCount: sampleCount, viewCount: 2)
        presenter = XRPresenter(device: device)
        axes = XRDebugAxes(device: device)
        targets = XRTargets(device: device)
        probe = XRPixelProbe(device: device)
        stereoProbe = XRStereoProbe(device: device)
        self.layered = layered
        self.sampleCount = sampleCount
    }

    /// Corrección de fluidez (matriz 4x4 columna-mayor de C) como simd_float4x4.
    static func matrix(_ c: VrSmCorr) -> simd_float4x4 {
        let v: [Float] = withUnsafeBytes(of: c.m) { Array($0.bindMemory(to: Float.self)) }
        guard v.count >= 16 else { return matrix_identity_float4x4 }
        return simd_float4x4(columns: (SIMD4<Float>(v[0], v[1], v[2], v[3]), SIMD4<Float>(v[4], v[5], v[6], v[7]),
                                       SIMD4<Float>(v[8], v[9], v[10], v[11]), SIMD4<Float>(v[12], v[13], v[14], v[15])))
    }

    /// Planos cercano y lejano (m) a partir de `drawable.depthRange` (el sistema da lejos/cerca en metros; el orden no importa). Un rango degenerado
    /// (no finito, cercano < 1 cm o lejano < 4x el cercano, p. ej. (0,0) o (1,0) = valores de profundidad en vez de distancias) haría la proyección
    /// NaN o la dejaría sin plano lejano útil y todo saldría negro: en ese caso se usa 0,1-1000 m (el banco de CI lo prueba con rangos degenerados).
    static func planes(_ r: SIMD2<Float>) -> (near: Float, far: Float, valid: Bool) {
        let n = min(r.x, r.y), f = max(r.x, r.y)
        if r.x.isFinite && r.y.isFinite && n >= 0.01 && f >= n * 4 { return (n, f, true) }
        return (0.1, 1000, false)
    }

    /// Proyección de Z invertida del ojo (a partir de las tangentes y el rango de profundidad del sistema, en metros).
    static func projection(_ e: XREyeView, depthRange r: SIMD2<Float>, nearOverride: Float = 0) -> simd_float4x4 {
        let p = planes(r)
        // Diagnóstico: plano cercano fijado (el lejano se conserva, con al menos 4x el cercano para que la proyección sea válida).
        let n = nearOverride > 0 ? nearOverride : p.near
        let f = nearOverride > 0 ? max(p.far, n * 4) : p.far
        return XRMath.projection(tangents: e.tangents, near: n, far: f)
    }

    /// ¿Es `m` una proyección de Z invertida utilizable (la que devuelve `computeProjection` con la convención rightUpBack)? Forma
    /// [sx 0 0 0; 0 sy 0 0; cx cy a -1; 0 0 b 0] con a, b > 0; se comprueba también que sx, sy y el desplazamiento concuerden con las
    /// tangentes de la vista (si no, la del sistema no se usa y se anota el motivo).
    static func systemProjectionIssue(_ m: simd_float4x4, tangents t: SIMD4<Float>) -> String? {
        guard XRMath.isFinite(m) else { return "no finita" }
        let c = m.columns
        if abs(c.2.w + 1) > 1e-3 || abs(c.3.w) > 1e-3 || abs(c.0.y) > 1e-3 || abs(c.0.z) > 1e-3 || abs(c.0.w) > 1e-3
            || abs(c.1.x) > 1e-3 || abs(c.1.z) > 1e-3 || abs(c.1.w) > 1e-3 || abs(c.3.x) > 1e-3 || abs(c.3.y) > 1e-3 { return "forma inesperada" }
        // Z invertida: P32 > 0 y P22 >= 0. P22 = 0 es el caso del plano lejano INFINITO (el que da el compositor: P22=0, P32=cercano).
        if !(c.2.z >= -1e-6 && c.3.z > 0) { return "no es Z invertida (P22=\(c.2.z) P32=\(c.3.z))" }
        let own = XRMath.projection(tangents: t, near: 0.1, far: 1000)
        func close(_ a: Float, _ b: Float) -> Bool { abs(a - b) <= 0.02 * max(abs(b), 0.5) }
        if !(close(c.0.x, own.columns.0.x) && close(c.1.y, own.columns.1.y) && close(c.2.x, own.columns.2.x) && close(c.2.y, own.columns.2.y)) {
            return "P00/P11/P20/P21 no concuerdan con las tangentes"
        }
        return nil
    }

    /// Planos (cercano, lejano) en metros que implica una proyección de Z invertida: a = n/(f-n), b = n f/(f-n).
    /// Con a ~ 0 (P22 = 0) el plano lejano es INFINITO (f = +inf, n = b): es lo que devuelve `computeProjection` en el visor.
    static func planes(of m: simd_float4x4) -> (near: Float, far: Float)? {
        let a = m.columns.2.z, b = m.columns.3.z
        guard a >= -1e-6, b > 0 else { return nil }
        if a < 1e-6 { return b >= 0.001 ? (b, Float.infinity) : nil }
        let n = b / (a + 1), f = n * (a + 1) / a
        return (n.isFinite && f.isFinite && n > 0 && f > n) ? (n, f) : nil
    }

    /// Codifica el fotograma completo en `cb`. `recenterInverse * originFromDevice` = cabeza en el espacio de la escena (recentrado).
    /// Devuelve false si no se pudo (texturas vacías, destinos MSAA).
    func encode(commandBuffer cb: MTLCommandBuffer, gs: MvVrGameState, originFromDevice: simd_float4x4, recenterInverse: simd_float4x4,
                dt: Float, eyes: [XREyeView], depthRange: SIMD2<Float>, colorTextures colorTex: [MTLTexture],
                depthTextures depthTex: [MTLTexture], systemProjections: [simd_float4x4]? = nil,
                debugAxes: [XRAxisFrame] = [], rateMapInfo: String = "") -> Bool {
        guard eyes.count >= viewCount, !colorTex.isEmpty, !depthTex.isEmpty else { return false }
        frameNo += 1
        let recentered = recenterInverse * originFromDevice

        // Interruptores de diagnóstico (submenú «Diagnóstico»; XRDiag): se leen en cada fotograma, así que valen en vivo.
        let dg = XRDiag.shared.settings
        let effSamples = (dg.msaa1 || sampleCount <= 1) ? 1 : sampleCount      // MSAA 1x: se dibuja directo en el drawable, sin resolve
        renderer.sampleCount = effSamples
        renderer.backgroundColor = dg.magentaClear ? MTLClearColor(red: 1, green: 0, blue: 1, alpha: 1) : MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
        renderer.diagNoZ = dg.noZTest
        renderer.diagSplitMeters = dg.splitMeters
        renderer.screenFilterEnabled = !dg.noLayers      // el filtro de pantalla (tinte, recubrimientos, máscara de visión) es una capa final

        // Vistas y proyecciones por ojo.
        var views: [simd_float4x4] = []
        var projs: [simd_float4x4] = []
        for i in 0..<viewCount {
            views.append((recenterInverse * originFromDevice * eyes[i].transform).inverse)
            projs.append(Self.projection(eyes[i], depthRange: depthRange, nearOverride: dg.nearPlane))
        }
        let ownProjs = projs
        // Proyección: la del sistema (`drawable.computeProjection`, la referencia de Apple, con el rango de profundidad que usa el
        // compositor para reproyectar) si tiene la forma de Z invertida esperada y concuerda con las tangentes; si no, la propia.
        var planesUsed = Self.planes(depthRange)
        if dg.nearPlane > 0 { planesUsed = (dg.nearPlane, max(planesUsed.far, dg.nearPlane * 4), planesUsed.valid) }
        projSource = "propia"
        if dg.projMode == 1 { projNote = "forzada la propia (diagnostico)" }
        else if dg.nearPlane > 0 { projNote = "plano cercano fijado: se usa la propia (diagnostico)" }
        else if let sp = systemProjections, sp.count >= viewCount {
            let issue = (0..<viewCount).compactMap { Self.systemProjectionIssue(sp[$0], tangents: eyes[$0].tangents) }.first
            if issue == nil, let pl = Self.planes(of: sp[0]) {
                projs = Array(sp.prefix(viewCount))
                planesUsed = (pl.near, pl.far, true)
                projSource = "sistema"
                projNote = ""
            } else { projNote = issue ?? "planos no deducibles" }
        }

        let w = colorTex[0].width, h = colorTex[0].height
        guard let tgt = targets.get(width: w, height: h, layered: layered, views: viewCount,
                                    samples: effSamples, colorFormat: colorTex[0].pixelFormat, depthFormat: depthTex[0].pixelFormat,
                                    drawableColor: colorTex, drawableDepth: depthTex) else { return false }

        // Con destino sRGB/lineal el panel se muestrea por la vista sRGB (decodifica a lineal al leer).
        // La vista vigente antes de leer el panel 2D: una capa del anillo de otra vista (créditos al abrir el menú) se descarta ya en este fotograma.
        renderer.layerView = (Int32(gs.view_kind) << 8) | Int32(gs.view_reason & 255)
        renderer.layerOverlay = gs.view_mode == 1 && XRMenuPanelLayout.isOverlay(reason: gs.view_reason)
        var hud: MTLTexture? = renderer.linearOutput ? renderer.hudTextureSRGB : renderer.hudTexture
        if hud != nil && hud!.textureType != .type2D { hud = nil }
        // Vista especial del arma (mira óptica, visor antiaéreo, pantalla del misil): la publica vr/host por VBlank.
        var sv = MvVrSpecialView()
        var special = mv_vrgame_special_view(&sv) != 0
        // Prismáticos (scope.c / sight.c con el juego en vista de mundo). Con scope.c en la lista (gs.fx.binocular): el mundo nativo se ve en ESTÉREO con el aumento en los dos ojos (RGRenderer.worldZoom, sin segunda
        // vista ni lente) y el marco del propio juego (rejilla, retícula, textos, tinte verdoso, radar: capa del HUD) va en un rectángulo fijo a la cabeza ante los ojos, con el mismo camino que el visor antiaéreo
        // (MV_VIEW_AA_SIGHT, sin panel plano del HUD). Sin scope.c (sight.c solo): disco con visión estrecha, como antes.
        var worldZoom: Float = 1
        if !special && gs.view_kind == 1 && gs.fx.scope_on != 0 {
            if gs.fx.binocular != 0 {
                let hl = renderer.hudLogical
                worldZoom = Self.binocularZoom(gs.fx.scope_zoom)
                sv = Self.binocularFrame(head: recentered, aspect: hl.y > 0 ? hl.x / hl.y : 4.0 / 3.0)
            } else {
                sv = Self.binocularView(head: recentered, zoom: gs.fx.scope_zoom)
            }
            special = true
        }
        if special && sv.hide_hud != 0 { hud = nil }          // el panel plano del HUD sobra mientras se mira por la mira / el visor
        // Presentación: mundo en primera persona, pantalla de cine o diorama, paneles del HUD, velo de transición y menú.
        var ipd = simd_length(XRMath.position(eyes[0].transform) - XRMath.position(eyes[1].transform))
        if !(ipd > 0.05 && ipd < 0.08) { ipd = 0.063 }
        presenter.hqFull = renderer.hqFull
        // Capas 2D por procedencia: ventana del selector / pausa (1) y pantalla completa (2), de la vista vigente; el HUD sigue siendo `hud`.
        var hudWindow: MTLTexture? = renderer.linearOutput ? renderer.auxTextureSRGB[1] : renderer.auxTexture[1]
        if hudWindow != nil && hudWindow!.textureType != .type2D { hudWindow = nil }
        var hudFull: MTLTexture? = renderer.linearOutput ? renderer.auxTextureSRGB[2] : renderer.auxTexture[2]
        if hudFull != nil && hudFull!.textureType != .type2D { hudFull = nil }
        renderer.gameFx = gs.view_kind == 1 ? SIMD4<Float>(gs.fx.vignette, gs.fx.vign_radius, gs.fx.letterbox, 0) : SIMD4<Float>(0, 0, 0, 0)
        var pres = presenter.compose(gs: gs, head: recentered, dt: dt, hud: hud, eyePx: Float(w), p00: projs[0].columns.0.x,
                                     gameView: renderer.gameView, linear: renderer.linearOutput, wrist: wristFrame(), ipd: ipd, shot: renderer.sceneShot,
                                     stereo2DInside: renderer.stereo2DInside, hudMenuUV: renderer.hudMenuUV, hudLogical: renderer.hudLogical, hudWindow: hudWindow, hudFull: hudFull)
        presenter.encodeUpscale(cb)
        if dg.noLayers {
            // Sin capas finales: ni velo (transición, fundido de corte), ni viñeta, ni pantalla de cine / pantalla 3D; solo el mundo, el HUD 2D y los textos (menú).
            pres.items.removeAll { $0.kind == .veil || $0.kind == .vignette || $0.kind == .screen || $0.kind == .screen3d }
        }
        renderer.screenMode = pres.stereo != nil || !pres.drawWorld
        renderer.engineView = gs.view_kind == 2          // cinemática del motor: el render nativo coloca la escena desde la cámara del juego
        renderer.drawWorld = pres.drawWorld
        renderer.drawBody = pres.drawBody && !dg.noLayers      // radar y arma en la mano se dibujan encima del mundo, dentro del pase de escena
        renderer.drawEyes = pres.drawEyes
        renderer.stereoRequest = pres.stereo
        if let sr = pres.stereo { renderer.prepareStereo(width: sr.width, height: sr.height) }
        var sceneViews = views
        if let dm = pres.dioramaModel { sceneViews = views.map { $0 * dm } }
        // Fluidez: P' = m P en el espacio del usuario -> vista' = vista * m (solo el pase de la escena; el pase final conserva `views`).
        // La corrección se evalúa DENTRO de renderer.encode, justo después de tomar las escenas nuevas (mv_host_scene empuja su muestra al
        // suavizador): así corresponde a la geometría que se dibuja en este mismo fotograma.
        var corr = VrSmCorr()
        if smoothMotion && presenter.cfg.smoothMotion && pres.drawWorld && pres.dioramaModel == nil && pres.stereo == nil {
            let base = sceneViews
            renderer.viewsHook = {
                mv_vrhost_smooth(&corr)
                guard corr.active != 0 else { return base }
                let m = Self.matrix(corr)
                if XRMath.isFinite(m) { return base.map { $0 * m } }
                corr.active = 0
                return base
            }
        } else { renderer.viewsHook = nil }
        if pres.dioramaModel != nil || !pres.drawWorld || pres.stereo != nil { special = false }
        renderer.special = special ? sv : nil
        renderer.worldZoom = special ? worldZoom : 1
        renderer.suppressFrame = special && sv.kind == Int32(MV_VIEW_SCOPE) && sv.suppress_frame != 0
        renderer.auxRequest = special ? auxView(sv, planesUsed) : nil

        // Sin entorno visible de arranque (antes: rejilla + panel "cargando"): si no hay nada que dibujar el ojo queda en negro limpio. Solo se
        // registra el motivo para el diagnóstico.
        let nowT0 = CACurrentMediaTime()
        if firstEncodeT == 0 { firstEncodeT = nowT0 }
        var fbReason = ""
        do {
            let hayPantalla = pres.items.contains(where: { $0.kind == .screen })
            let sinMundo = pres.stereo != nil ? false : (pres.drawWorld ? !renderer.sceneHas3D : !hayPantalla)
            if sinMundo { fbReason = nowT0 - firstEncodeT < 3 ? "arranque" : "nada que dibujar" }
        }
        // Ejes de depuración (opción del menú VR): marcos de los mandos y de las manos del personaje, solo en el mundo.
        if !debugAxes.isEmpty, pres.drawWorld, !dg.noLayers, let ax = axes {
            pres.items.insert(contentsOf: ax.items(frames: debugAxes, linear: renderer.linearOutput), at: 0)
        }
        // Viñeta de confort: oscurece la periferia al girar / moverse (solo en el mundo en primera persona). Va debajo del velo y del menú.
        if comfortVignette > 0.01 && !dg.noLayers && pres.drawWorld && pres.dioramaModel == nil && pres.stereo == nil {
            var vg = XRDrawItem(kind: .vignette, texture: nil)
            vg.opacity = min(max(comfortVignette, 0), 1)
            vg.vig = (0..<2).map { i -> SIMD4<Float> in
                let e = eyes[min(i, eyes.count - 1)], t = e.tangents, vp = e.viewport
                let tw = max(t.x + t.y, 1e-3), th = max(t.z + t.w, 1e-3)
                // centro óptico en píxeles del destino (x der, y abajo) y tangentes por píxel
                return SIMD4<Float>(Float(vp.originX) + Float(vp.width) * t.x / tw, Float(vp.originY) + Float(vp.height) * t.z / th,
                                    tw / max(Float(vp.width), 1), th / max(Float(vp.height), 1))
            }
            let at = pres.items.firstIndex(where: { $0.kind == .veil || $0.kind == .menu }) ?? pres.items.count
            pres.items.insert(vg, at: at)
        }
        renderer.encode(commandBuffer: cb, colorTargets: tgt.color, depthTargets: tgt.depth,
                        resolveTargets: tgt.resolve, views: sceneViews, projections: projs,
                        head: recentered, layered: layered)
        renderer.viewsHook = nil
        lastSmooth = corr

        if pres.stereo != nil {   // la imagen estéreo y el tinte/recubrimientos de la escena se conocen tras renderer.encode
            let fxs = renderer.screenFx
            for i in pres.items.indices where pres.items[i].kind == .screen3d {
                pres.items[i].texture = renderer.stereoImage
                pres.items[i].tint = fxs.tint
                pres.items[i].covers = fxs.covers
                pres.items[i].storedLinear = renderer.stereoStoredLinear
                pres.items[i].outLinear = renderer.linearOutput
            }
        }
        // Diagnóstico de la pantalla 3D: rejilla 16x16 de su textura (ojo 0), 1 vez cada 2 s, sin bloquear (blit a un búfer compartido; ver XRStereoProbe).
        if pres.stereo != nil, let img = renderer.stereoImage, let sp = stereoProbe {
            let nowS = CACurrentMediaTime()
            if nowS - stereoProbeT >= 2 { stereoProbeT = nowS; sp.encode(commandBuffer: cb, texture: img) }
        }
        // [cine3d] (1/s, solo mientras la vista es una cinemática del motor o se muestra la pantalla 3D): por qué se ve (o no) la pantalla en vez del mundo VR.
        if gs.view_kind == 2 || presenter.shown == .stereo {
            let nowC = CACurrentMediaTime()
            if nowC - cine3dT >= 1 {
                cine3dT = nowC
                let s3 = pres.items.first(where: { $0.kind == .screen3d })
                let t3 = s3 == nil ? "sin-item" : (s3!.texture.map { "\($0.width)x\($0.height)x\($0.arrayLength)" } ?? "nil")
                log("[cine3d] clase=\(gs.view_kind) motivo=\(gs.view_reason) cutscene=\(presenter.cfg.cutscene.rawValue) mostrando=\(presenter.shown) velo=\(String(format: "%.2f", presenter.veil)) veloPend=\(String(format: "%.2f", presenter.pendVeil)) pantalla3d=\(t3) " + renderer.cine3dDiag() + " | pantalla3d-pixeles: " + (stereoProbe?.last ?? "n/d"))
            }
        }
        finalPass.encode(commandBuffer: cb, colorTextures: colorTex, depthTextures: depthTex,
                         msaaDepth: effSamples > 1 ? tgt.depth : nil, viewports: eyes.map { $0.viewport },
                         views: views, projections: projs, items: pres.items, viewCount: viewCount,
                         fillDepth: (dg.fillDepth || dg.flatDepth) ? XRMath.depthValue(projection: projs[0], meters: Self.fillDepthMeters) : nil, flatDepth: dg.flatDepth)

        // Muestra de píxeles del color FINAL (tras el pase final, antes de presentar): 1 vez por segundo, un blit a un búfer pequeño; el resultado llega en el manejador de finalización.
        let nowP = CACurrentMediaTime()
        if let pr = probe, nowP - probeT >= 1 {
            probeT = nowP
            pr.encode(commandBuffer: cb, colorTextures: colorTex, depthTextures: depthTex, layered: layered, eyes: eyes, viewCount: viewCount)
        }

        // Diagnóstico (1 vez por segundo): qué se decidió y qué se dibujó.
        let now = CACurrentMediaTime()
        if now - diagT >= 1 {      // 1/s siempre mientras dure la depuración (antes pasaba a 1 cada 10 s tras 30 s)
            diagT = now
            var d = "xr-diag: modo=\(gs.view_mode) motivo=\(gs.view_reason) clase=\(gs.view_kind) negro=\(gs.view_black) plano=\(gs.cine_shot) cortes=\(gs.cine_cuts) cubiertas=\(gs.cine_cover) cinem=\(gs.cinematic) estereo=\(renderer.stereoRendered) | " + presenter.diagnostics(items: pres.items)
            d += " | escena: " + renderer.diagnostics()
            let c = colorTex[0], z = depthTex[0]
            d += " | color: tipo=\(c.textureType.rawValue) fmt=\(c.pixelFormat.rawValue) \(c.width)x\(c.height)x\(c.arrayLength) n=\(colorTex.count) | prof: fmt=\(z.pixelFormat.rawValue) n=\(depthTex.count)"
            let pl = Self.planes(depthRange)
            d += String(format: " | rango=(%.3f, %.3f) valido=%d usado=(%.3f, %.3f) tan0=(%.2f %.2f %.2f %.2f) tan1=(%.2f %.2f %.2f %.2f)", depthRange.x, depthRange.y, Int32(pl.valid ? 1 : 0), pl.near, pl.far,
                        eyes[0].tangents.x, eyes[0].tangents.y, eyes[0].tangents.z, eyes[0].tangents.w,
                        eyes[1].tangents.x, eyes[1].tangents.y, eyes[1].tangents.z, eyes[1].tangents.w)
            d += String(format: " vp0=(%.0f %.0f %.0fx%.0f) cabeza=(%.2f %.2f %.2f)", eyes[0].viewport.originX, eyes[0].viewport.originY,
                        eyes[0].viewport.width, eyes[0].viewport.height, recentered.columns.3.x, recentered.columns.3.y, recentered.columns.3.z)
            d += " pase final: " + finalPass.lastCounts
            // Proyección propia vs la del sistema (computeProjection): P00 P11 P20 P21 P22 P32 por ojo.
            func pm(_ m: simd_float4x4) -> String {
                String(format: "[%.3f %.3f %.3f %.3f %.3f %.3f]", m.columns.0.x, m.columns.1.y, m.columns.2.x, m.columns.2.y, m.columns.2.z, m.columns.3.z)
            }
            d += " | proyeccion usada=\(projSource)" + (projNote.isEmpty ? "" : " (sistema descartada: \(projNote))")
            d += String(format: " planos=(%.3f, %.3f)", planesUsed.near, planesUsed.far)
            d += " P propia=" + ownProjs.map(pm).joined(separator: " ")
            if let sp = systemProjections { d += " P sistema=" + sp.map(pm).joined(separator: " ") }
            d += " sin-contenido=" + (fbReason.isEmpty ? "no" : fbReason)
            var ss = VrSmoothStats()
            mv_vrhost_smooth_stats(&ss)
            d += String(format: " | fluidez: " + (smoothMotion ? "si" : "no") + " escenas/s=%.1f rho=%.3f reinicios=%u activa=%d giro=%.2fgr traslacion=%.0fmm err-pred-max=%.0fmm vineta=%.2f",
                        ss.period_s > 0 ? 1 / ss.period_s : 0, ss.rho, ss.resets, Int32(corr.active), corr.delta_deg,
                        simd_length(SIMD3<Float>(corr.shift_m.0, corr.shift_m.1, corr.shift_m.2)) * 1000, ss.max_err_m * 1000, comfortVignette)
            lastDiag = d
            log(d)
            if gs.view_mode == 0 && gs.view_reason == 3 { let cs = presenter.cineSrc; log(renderer.radioSrcLine(hqActive: cs.hq, xbrActive: cs.xbr)) }   // radio (códec): ¿capa 2D fiel o fotograma del emulador?
            if let nd = renderer.nativeDiagLine() { log(nd) }
            if let nl = renderer.nativeNetLine() { log(nl) }
            // Línea de diagnóstico del hueco: interruptores, triángulos largos / que cruzan el plano cercano en cada ruta, mapa de tasa, viewport y color final muestreado.
            var vpTxt: [String] = []
            for i in 0..<min(eyes.count, 2) {
                let v = eyes[i].viewport, t = colorTex[min(i, colorTex.count - 1)]
                let same = Int(v.originX) == 0 && Int(v.originY) == 0 && Int(v.width) == t.width && Int(v.height) == t.height
                if same {
                    vpTxt.append("ojo\(i)=textura completa")
                } else {
                    let geo = String(format: "(%.0f,%.0f %.0fx%.0f) textura=%ldx%ld DISTINTO", v.originX, v.originY, v.width, v.height, t.width, t.height)
                    vpTxt.append("ojo\(i)=" + geo)
                }
            }
            let c0 = colorTex[0]
            let nearTxt = String(format: "%.3f", planesUsed.near)
            var dl = "diag-huecos: " + XRDiag.summary(dg)
            dl += " | msaa-efectivo=\(effSamples) proyeccion=\(projSource) cercano-usado=\(nearTxt)m"
            dl += " | " + renderer.diagLine(near: planesUsed.near, bigEdge: 12)
            let rm: String = rateMapInfo.isEmpty ? "n/d" : rateMapInfo
            let rmNote: String = dg.ignoreRateMap ? "; interruptor «sin mapa» activo: sin efecto" : ""
            dl += " | mapa-tasa: " + rm + " (la app no asigna ningun mapa al pase" + rmNote + ")"
            dl += " | viewport vs textura: " + vpTxt.joined(separator: ", ") + " (el pase de escena usa la textura completa)"
            dl += " | drawable: fmt=\(c0.pixelFormat.rawValue) soloFramebuffer=\(c0.isFramebufferOnly) uso=\(c0.usage.rawValue)"
            let px: String = probe?.last ?? "n/d"
            dl += " | color final (3 columnas x 3 filas al 35/50/65 % y 60/75/90 % de cada ojo): " + px
            log(dl)
        }
        return true
    }

    /// Marco del radar de muñeca para los paneles del HUD del brazo (nil si el radar no tiene pose válida).
    private func wristFrame() -> XRPresenter.Wrist? {
        guard let f = renderer.radar?.wristFrame else { return nil }
        return XRPresenter.Wrist(center: f.center, right: f.right, up: f.up, halfW: f.halfW, halfH: f.halfH)
    }

    /// Segunda vista de la escena pedida por el arma (cámara del visor / del misil): matrices en el espacio recentrado. nil si no hace falta renderizarla.
    /// Aumento de los prismáticos: el del juego (H de la pantalla / 160) acotado a 2...8.
    private static func binocularZoom(_ zoom: Float) -> Float { min(max(zoom.isFinite ? zoom : 2, 2), 8) }

    /// Marco de los prismáticos del juego (scope.c en la lista): rectángulo con la proporción `aspect` de la capa del HUD, fijo a la cabeza a 1 m y de 58 grados de alto, que dibuja la capa del HUD del juego
    /// en vivo (MV_VIEW_AA_SIGHT: textura entera, sin premultiplicar) por encima del mundo ampliado; el exterior del rectángulo se tapa con negro (borde de la pantalla del original). Es geometría real, así que se ve a profundidad correcta en estéreo; sin prueba de profundidad (nada lo tapa).
    private static func binocularFrame(head: simd_float4x4, aspect: Float) -> MvVrSpecialView {
        var sv = MvVrSpecialView()
        let q = simd_quatf(head).normalized
        let dist: Float = 1
        let h: Float = 2 * dist * tan(Float(58) * Float.pi / 360)
        let asp: Float = aspect.isFinite && aspect > 0.5 && aspect < 3 ? aspect : 4.0 / 3.0
        let p = head * simd_float4(0, 0, -dist, 1)
        sv.kind = Int32(MV_VIEW_AA_SIGHT)
        sv.blend = 1
        sv.surf_pos = (p.x, p.y, p.z)
        sv.surf_rot = (q.imag.x, q.imag.y, q.imag.z, q.real)
        sv.surf_w = h * asp; sv.surf_h = h
        sv.uv = (0, 0, 1, 1)
        sv.hide_hud = 1
        sv.round = 2                 // marca "prismáticos": encodeSpecial tapa con negro todo lo que queda fuera del rectángulo del HUD
        return sv
    }

    /// Mira de visor sin prismáticos (sight.c sin scope.c): lente de disco fija a la cabeza (a 0,45 m, 0,34 m de diámetro) que muestra la escena desde la cabeza con el campo estrechado por el aumento (mínimo 2).
    private static func binocularView(head: simd_float4x4, zoom: Float) -> MvVrSpecialView {
        var sv = MvVrSpecialView()
        let q = simd_quatf(head).normalized
        let z = binocularZoom(zoom)
        let dist: Float = 0.45
        let h: Float = 0.34
        let p = head * simd_float4(0, 0, -dist, 1)
        sv.kind = Int32(MV_VIEW_SCOPE)
        sv.blend = 1
        sv.cam_valid = 1
        sv.cam_pos = (head.columns.3.x, head.columns.3.y, head.columns.3.z)
        sv.cam_rot = (q.imag.x, q.imag.y, q.imag.z, q.real)
        sv.cam_fov_y = 2 * atan((h * 0.5 / dist) / z)
        sv.tex_w = 384
        sv.tex_h = 384
        sv.surf_pos = (p.x, p.y, p.z)
        sv.surf_rot = (q.imag.x, q.imag.y, q.imag.z, q.real)
        sv.surf_w = h; sv.surf_h = h
        sv.round = 1
        sv.hide_hud = 0
        sv.zoom = z
        return sv
    }

    private func auxView(_ sv: MvVrSpecialView, _ pl: (near: Float, far: Float, valid: Bool)) -> RGRenderer.AuxView? {
        let wantsAux = sv.kind == Int32(MV_VIEW_SCOPE) || (sv.kind == Int32(MV_VIEW_MISSILE) && sv.ended == 0)
        guard wantsAux, sv.cam_valid != 0, sv.tex_w >= 16, sv.tex_h >= 16, sv.tex_w <= 2048, sv.tex_h <= 2048 else { return nil }
        let vals = [sv.cam_pos.0, sv.cam_pos.1, sv.cam_pos.2, sv.cam_rot.0, sv.cam_rot.1, sv.cam_rot.2, sv.cam_rot.3, sv.cam_fov_y]
        guard vals.allSatisfy({ $0.isFinite }), sv.cam_fov_y > 0.005, sv.cam_fov_y < 2.5 else { return nil }
        let q = simd_quatf(ix: sv.cam_rot.0, iy: sv.cam_rot.1, iz: sv.cam_rot.2, r: sv.cam_rot.3).normalized
        var cam = simd_float4x4(q)
        cam.columns.3 = simd_float4(sv.cam_pos.0, sv.cam_pos.1, sv.cam_pos.2, 1)
        let t = tan(sv.cam_fov_y * 0.5)
        let aspect = Float(sv.tex_w) / Float(sv.tex_h)
        let proj = XRMath.projection(tangents: SIMD4<Float>(t * aspect, t * aspect, t, t), near: pl.near, far: pl.far.isFinite ? pl.far : 1000)
        return RGRenderer.AuxView(view: cam.inverse, proj: proj, width: Int(sv.tex_w), height: Int(sv.tex_h),
                                   pos: simd_float4(sv.cam_pos.0, sv.cam_pos.1, sv.cam_pos.2, 1))
    }
}

/// Texturas MSAA propias (color + profundidad) y destinos de resolve (texturas del drawable).
final class XRTargets {
    struct Set {
        var color: [MTLTexture]
        var depth: [MTLTexture]
        var resolve: [MTLTexture]
    }
    private let device: MTLDevice
    private var key = ""
    private var cached: Set?
    init(device: MTLDevice) { self.device = device }

    func get(width: Int, height: Int, layered: Bool, views: Int, samples: Int,
             colorFormat: MTLPixelFormat, depthFormat: MTLPixelFormat,
             drawableColor: [MTLTexture], drawableDepth: [MTLTexture]) -> Set? {
        if samples <= 1 {
            // Sin MSAA: se dibuja directo en el drawable (el renderer no hace resolve).
            return Set(color: drawableColor, depth: drawableDepth, resolve: [])
        }
        let k = "\(width)x\(height) \(layered) \(views) \(samples) \(colorFormat.rawValue) \(depthFormat.rawValue)"
        if k != key || cached == nil {
            var c: [MTLTexture] = [], d: [MTLTexture] = []
            let n = layered ? 1 : views
            for i in 0..<n {
                guard let ct = make(width, height, layered, views, samples, colorFormat, "mv.xr.msaa.color\(i)"),
                      let dt = make(width, height, layered, views, samples, depthFormat, "mv.xr.msaa.depth\(i)") else { return nil }
                c.append(ct); d.append(dt)
            }
            cached = Set(color: c, depth: d, resolve: [])
            key = k
        }
        guard var s = cached else { return nil }
        s.resolve = drawableColor      // layered: [array]; dedicated: una por vista
        return s
    }

    private func make(_ w: Int, _ h: Int, _ layered: Bool, _ views: Int, _ samples: Int,
                      _ fmt: MTLPixelFormat, _ label: String) -> MTLTexture? {
        let d = MTLTextureDescriptor()
        d.pixelFormat = fmt
        d.width = w; d.height = h
        d.sampleCount = samples
        if layered {
            d.textureType = .type2DMultisampleArray
            d.arrayLength = views
        } else {
            d.textureType = .type2DMultisample
        }
        d.usage = [.renderTarget, .shaderRead]
        d.storageMode = .private
        let t = device.makeTexture(descriptor: d)
        t?.label = label
        return t
    }
}


/// Lectura SIN BLOQUEO de unos píxeles del drawable final: un blit de 1x1 por muestra a un búfer compartido pequeño, codificado en el mismo command buffer del fotograma
/// (tras el pase final); el contenido se decodifica en el manejador de finalización, sin esperar en el hilo de render. Muestras por ojo: 3 columnas (35/50/65 % del
/// ancho del viewport) x 3 filas (60/75/90 % del alto), es decir, el centro-inferior de la imagen donde aparece el hueco.
final class XRPixelProbe {
    static let cols: [Float] = [0.35, 0.5, 0.65]
    static let rows: [Float] = [0.60, 0.75, 0.90]
    static let perEye = 9
    private static let slot = 16                 // bytes por muestra en el búfer
    private let buf: MTLBuffer
    private let lock = NSLock()
    private var text = "sin muestra aun"
    private var busy = false

    init?(device: MTLDevice) {
        guard let b = device.makeBuffer(length: 4 * XRPixelProbe.perEye * XRPixelProbe.slot, options: .storageModeShared) else { return nil }
        b.label = "mv.xr.probe"
        buf = b
    }

    /// Último resultado (de hace como mucho un par de segundos).
    var last: String { lock.lock(); defer { lock.unlock() }; return text }

    private func set(_ t: String) { lock.lock(); text = t; busy = false; lock.unlock() }

    static func half(_ h: UInt16) -> Float {
        let sign: Float = (h & 0x8000) != 0 ? -1 : 1
        let e = Int((h >> 10) & 0x1F), m = Float(h & 0x3FF)
        if e == 0 { return sign * m * powf(2, -24) }
        if e == 31 { return m == 0 ? sign * Float.infinity : Float.nan }
        return sign * (1 + m / 1024) * powf(2, Float(e - 15))
    }

    func encode(commandBuffer cb: MTLCommandBuffer, colorTextures: [MTLTexture], depthTextures: [MTLTexture] = [], layered: Bool, eyes: [XREyeView], viewCount: Int) {
        lock.lock()
        if busy { lock.unlock(); return }
        lock.unlock()
        guard let t0 = colorTextures.first else { return }
        let fmt = t0.pixelFormat
        let bpp: Int
        switch fmt {
        case .bgra8Unorm, .bgra8Unorm_srgb, .rgba8Unorm, .rgba8Unorm_srgb: bpp = 4
        case .rgba16Float: bpp = 8
        default: set("formato \(fmt.rawValue) sin decodificar"); return
        }
        if t0.isFramebufferOnly { set("el drawable es solo-framebuffer: no se puede leer"); return }
        guard let blit = cb.makeBlitCommandEncoder() else { return }
        blit.label = "mv.xr.probe"
        let n = min(viewCount, eyes.count, 2)
        var targets: [(tex: MTLTexture, slice: Int)] = []
        for e in 0..<n {
            if layered { targets.append((t0, e)) }
            else if e < colorTextures.count { targets.append((colorTextures[e], 0)) }
        }
        // Profundidad en los mismos puntos (solo depth32Float y legible): segunda mitad del búfer. Sin ella el registro dice d=n/d.
        var depthTargets: [(tex: MTLTexture, slice: Int)] = []
        for e in 0..<n {
            if layered, let d0 = depthTextures.first { depthTargets.append((d0, e)) }
            else if e < depthTextures.count { depthTargets.append((depthTextures[e], 0)) }
        }
        let depthOK = depthTargets.count == targets.count && depthTargets.allSatisfy { $0.tex.pixelFormat == .depth32Float && !$0.tex.isFramebufferOnly }
        for (e, tg) in targets.enumerated() {
            let vp = eyes[e].viewport
            var k = 0
            for r in Self.rows {
                for c in Self.cols {
                    let x = min(max(Int(vp.originX + Double(c) * vp.width), 0), tg.tex.width - 1)
                    let y = min(max(Int(vp.originY + Double(r) * vp.height), 0), tg.tex.height - 1)
                    blit.copy(from: tg.tex, sourceSlice: tg.slice, sourceLevel: 0, sourceOrigin: MTLOrigin(x: x, y: y, z: 0), sourceSize: MTLSize(width: 1, height: 1, depth: 1),
                              to: buf, destinationOffset: (e * Self.perEye + k) * Self.slot, destinationBytesPerRow: bpp, destinationBytesPerImage: bpp)
                    if depthOK {
                        let dt = depthTargets[e]
                        let dx = min(max(x, 0), dt.tex.width - 1), dy = min(max(y, 0), dt.tex.height - 1)
                        blit.copy(from: dt.tex, sourceSlice: dt.slice, sourceLevel: 0, sourceOrigin: MTLOrigin(x: dx, y: dy, z: 0), sourceSize: MTLSize(width: 1, height: 1, depth: 1),
                                  to: buf, destinationOffset: (2 * Self.perEye + e * Self.perEye + k) * Self.slot, destinationBytesPerRow: 4, destinationBytesPerImage: 4, options: [])
                    }
                    k += 1
                }
            }
        }
        blit.endEncoding()
        lock.lock(); busy = true; lock.unlock()
        let count = targets.count
        let fmtCopy = fmt
        let depthOK2 = depthOK
        cb.addCompletedHandler { [weak self] _ in
            guard let self = self else { return }
            let base = self.buf.contents()
            var out: [String] = []
            var black = 0, total = 0
            for e in 0..<count {
                var parts: [String] = []
                for k in 0..<Self.perEye {
                    let p = base + (e * Self.perEye + k) * Self.slot
                    let r: Float, g: Float, b: Float, al: Float
                    switch fmtCopy {
                    case .bgra8Unorm, .bgra8Unorm_srgb:
                        let q = p.assumingMemoryBound(to: UInt8.self); r = Float(q[2]); g = Float(q[1]); b = Float(q[0]); al = Float(q[3])
                    case .rgba8Unorm, .rgba8Unorm_srgb:
                        let q = p.assumingMemoryBound(to: UInt8.self); r = Float(q[0]); g = Float(q[1]); b = Float(q[2]); al = Float(q[3])
                    default:
                        let q = p.assumingMemoryBound(to: UInt16.self)
                        r = Self.half(q[0]) * 255; g = Self.half(q[1]) * 255; b = Self.half(q[2]) * 255; al = Self.half(q[3]) * 255
                    }
                    total += 1
                    if r == 0 && g == 0 && b == 0 { black += 1 }
                    parts.append(String(format: "%.0f,%.0f,%.0f/a%.0f", r, g, b, al))   // /a = alfa del drawable (255 = opaco)
                }
                out.append("ojo\(e): " + parts.joined(separator: " "))
                if depthOK2 {
                    var dparts: [String] = []
                    for k in 0..<Self.perEye {
                        let p = base + (2 * Self.perEye + e * Self.perEye + k) * Self.slot
                        dparts.append(String(format: "%.5f", p.assumingMemoryBound(to: Float.self).pointee))
                    }
                    out.append("prof-ojo\(e) (0 = borrado/infinito, near/dist): " + dparts.joined(separator: " "))
                } else if e == 0 { out.append("prof=n/d") }
            }
            self.set(out.joined(separator: " | ") + " | negro-puro(0,0,0)=\(black)/\(total)")
        }
    }
}

/// Lectura SIN BLOQUEO de la textura de la pantalla 3D (diagnóstico [cine3d]): rejilla de 16x16 muestras de la capa del ojo 0 (un blit de 1x1 por muestra a un búfer compartido, en el command buffer del
/// fotograma); la luminancia media, la máxima y el % de muestras no negras se calculan en el manejador de finalización, sin esperar en el hilo de render.
final class XRStereoProbe {
    private static let n = 16
    private static let slot = 8                  // bytes por muestra en el búfer (cabe rgba16Float)
    private let buf: MTLBuffer
    private let lock = NSLock()
    private var text = "sin muestra aun"
    private var busy = false
    private var busyT: CFTimeInterval = 0       // si el command buffer no llega a enviarse el manejador no corre: pasados 4 s se libera `busy`

    init?(device: MTLDevice) {
        guard let b = device.makeBuffer(length: Self.n * Self.n * Self.slot, options: .storageModeShared) else { return nil }
        b.label = "mv.xr.probe3d"
        buf = b
    }

    var last: String { lock.lock(); defer { lock.unlock() }; return text }
    private func set(_ t: String, busy b: Bool = false) { lock.lock(); text = t; busy = b; busyT = CACurrentMediaTime(); lock.unlock() }

    func encode(commandBuffer cb: MTLCommandBuffer, texture t: MTLTexture) {
        lock.lock(); let skip = busy && CACurrentMediaTime() - busyT < 4; lock.unlock()
        if skip { return }
        let bpp: Int
        switch t.pixelFormat {
        case .bgra8Unorm, .bgra8Unorm_srgb, .rgba8Unorm, .rgba8Unorm_srgb: bpp = 4
        case .rgba16Float: bpp = 8
        default: set("formato \(t.pixelFormat.rawValue) sin decodificar"); return
        }
        guard t.sampleCount == 1, !t.isFramebufferOnly, t.width > 0, t.height > 0, let blit = cb.makeBlitCommandEncoder() else { return }
        blit.label = "mv.xr.probe3d"
        let n = Self.n
        for j in 0..<n {
            for i in 0..<n {
                let x = min(Int((Float(i) + 0.5) / Float(n) * Float(t.width)), t.width - 1), y = min(Int((Float(j) + 0.5) / Float(n) * Float(t.height)), t.height - 1)
                blit.copy(from: t, sourceSlice: 0, sourceLevel: 0, sourceOrigin: MTLOrigin(x: x, y: y, z: 0), sourceSize: MTLSize(width: 1, height: 1, depth: 1),
                          to: buf, destinationOffset: (j * n + i) * Self.slot, destinationBytesPerRow: bpp, destinationBytesPerImage: bpp)
            }
        }
        blit.endEncoding()
        set(last, busy: true)
        let fmt = t.pixelFormat
        cb.addCompletedHandler { [weak self] _ in
            guard let self = self else { return }
            let base = self.buf.contents()
            var sum: Float = 0, mx: Float = 0, lit = 0, valid = 0
            for k in 0..<(n * n) {
                let p = base + k * Self.slot
                let r: Float, g: Float, b: Float
                switch fmt {
                case .bgra8Unorm, .bgra8Unorm_srgb:
                    let q = p.assumingMemoryBound(to: UInt8.self); r = Float(q[2]); g = Float(q[1]); b = Float(q[0])
                case .rgba8Unorm, .rgba8Unorm_srgb:
                    let q = p.assumingMemoryBound(to: UInt8.self); r = Float(q[0]); g = Float(q[1]); b = Float(q[2])
                default:
                    let q = p.assumingMemoryBound(to: UInt16.self)
                    let hr = XRPixelProbe.half(q[0]), hg = XRPixelProbe.half(q[1]), hb = XRPixelProbe.half(q[2])
                    if !(hr.isFinite && hg.isFinite && hb.isFinite) { continue }     // NaN / Inf: no cuentan
                    r = min(max(hr, 0), 1) * 255; g = min(max(hg, 0), 1) * 255; b = min(max(hb, 0), 1) * 255
                }
                valid += 1
                let l = 0.2126 * r + 0.7152 * g + 0.0722 * b
                sum += l; mx = max(mx, l)
                if r > 0 || g > 0 || b > 0 { lit += 1 }
            }
            self.set("ojo0 rejilla \(n)x\(n): " + String(format: "luminancia media=%.1f max=%.1f (0-255) no_negro=%.0f%%", sum / Float(max(valid, 1)), mx, 100 * Float(lit) / Float(max(valid, 1))) + " validas=\(valid)/\(n * n)")
        }
    }
}
