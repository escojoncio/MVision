import Foundation
import Metal
import simd
import QuartzCore

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
    private(set) var lastDiag = ""
    private(set) var frameNo = 0

    init?(device: MTLDevice, colorFormat: MTLPixelFormat, depthFormat: MTLPixelFormat, layered: Bool, sampleCount: Int) {
        guard let fp = XRFinalPass(device: device, colorFormat: colorFormat, depthFormat: depthFormat, layered: layered) else { return nil }
        finalPass = fp
        renderer = RGRenderer(device: device, colorFormat: colorFormat, depthFormat: depthFormat,
                              sampleCount: sampleCount, viewCount: 2)
        presenter = XRPresenter(device: device)
        targets = XRTargets(device: device)
        self.layered = layered
        self.sampleCount = sampleCount
    }

    /// Proyección de Z invertida del ojo (a partir de las tangentes y el rango de profundidad del sistema, en metros).
    static func projection(_ e: XREyeView, depthRange r: SIMD2<Float>) -> simd_float4x4 {
        XRMath.projection(tangents: e.tangents, near: min(r.x, r.y), far: max(r.x, r.y))
    }

    /// Codifica el fotograma completo en `cb`. `recenterInverse * originFromDevice` = cabeza en el espacio de la escena (recentrado).
    /// Devuelve false si no se pudo (texturas vacías, destinos MSAA).
    func encode(commandBuffer cb: MTLCommandBuffer, gs: MvVrGameState, originFromDevice: simd_float4x4, recenterInverse: simd_float4x4,
                dt: Float, eyes: [XREyeView], depthRange: SIMD2<Float>, colorTextures colorTex: [MTLTexture],
                depthTextures depthTex: [MTLTexture]) -> Bool {
        guard eyes.count >= viewCount, !colorTex.isEmpty, !depthTex.isEmpty else { return false }
        frameNo += 1
        let recentered = recenterInverse * originFromDevice

        // Vistas y proyecciones por ojo.
        var views: [simd_float4x4] = []
        var projs: [simd_float4x4] = []
        for i in 0..<viewCount {
            views.append((recenterInverse * originFromDevice * eyes[i].transform).inverse)
            projs.append(Self.projection(eyes[i], depthRange: depthRange))
        }

        let w = colorTex[0].width, h = colorTex[0].height
        guard let tgt = targets.get(width: w, height: h, layered: layered, views: viewCount,
                                    samples: sampleCount, colorFormat: colorTex[0].pixelFormat, depthFormat: depthTex[0].pixelFormat,
                                    drawableColor: colorTex, drawableDepth: depthTex) else { return false }

        // Con destino sRGB/lineal el panel se muestrea por la vista sRGB (decodifica a lineal al leer).
        var hud: MTLTexture? = renderer.linearOutput ? renderer.hudTextureSRGB : renderer.hudTexture
        if hud != nil && hud!.textureType != .type2D { hud = nil }
        // Vista especial del arma (mira óptica, visor antiaéreo, pantalla del misil): la publica vr/host por VBlank.
        var sv = MvVrSpecialView()
        var special = mv_vrgame_special_view(&sv) != 0
        if special && sv.hide_hud != 0 { hud = nil }          // el panel plano del HUD sobra mientras se mira por la mira / el visor
        // Presentación: mundo en primera persona, pantalla de cine o diorama, paneles del HUD, velo de transición y menú.
        let pres = presenter.compose(gs: gs, head: recentered, dt: dt, hud: hud, eyePx: Float(w), p00: projs[0].columns.0.x,
                                     gameView: renderer.gameView, linear: renderer.linearOutput, wrist: wristFrame())
        renderer.drawWorld = pres.drawWorld
        renderer.drawBody = pres.drawBody
        var sceneViews = views
        if let dm = pres.dioramaModel { sceneViews = views.map { $0 * dm } }
        if pres.dioramaModel != nil || !pres.drawWorld { special = false }
        renderer.special = special ? sv : nil
        renderer.suppressFrame = special && sv.kind == Int32(MV_VIEW_SCOPE) && sv.suppress_frame != 0
        renderer.auxRequest = special ? auxView(sv, depthRange) : nil

        renderer.encode(commandBuffer: cb, colorTargets: tgt.color, depthTargets: tgt.depth,
                        resolveTargets: tgt.resolve, views: sceneViews, projections: projs,
                        head: recentered, layered: layered)

        finalPass.encode(commandBuffer: cb, colorTextures: colorTex, depthTextures: depthTex,
                         msaaDepth: sampleCount > 1 ? tgt.depth : nil, viewports: eyes.map { $0.viewport },
                         views: views, projections: projs, items: pres.items, viewCount: viewCount)

        // Diagnóstico (1 vez por segundo): qué se decidió y qué se dibujó.
        let now = CACurrentMediaTime()
        if now - diagT >= 1 {
            diagT = now
            var d = "xr-diag: modo=\(gs.view_mode) motivo=\(gs.view_reason) cinem=\(gs.cinematic) | " + presenter.diagnostics(items: pres.items)
            d += " | escena: " + renderer.diagnostics()
            let c = colorTex[0], z = depthTex[0]
            d += " | color: tipo=\(c.textureType.rawValue) fmt=\(c.pixelFormat.rawValue) \(c.width)x\(c.height)x\(c.arrayLength) n=\(colorTex.count) | prof: fmt=\(z.pixelFormat.rawValue) n=\(depthTex.count)"
            d += String(format: " | rango=(%.3f, %.3f) tan0=(%.2f %.2f %.2f %.2f) tan1=(%.2f %.2f %.2f %.2f)", depthRange.x, depthRange.y,
                        eyes[0].tangents.x, eyes[0].tangents.y, eyes[0].tangents.z, eyes[0].tangents.w,
                        eyes[1].tangents.x, eyes[1].tangents.y, eyes[1].tangents.z, eyes[1].tangents.w)
            d += String(format: " vp0=(%.0f %.0f %.0fx%.0f) cabeza=(%.2f %.2f %.2f)", eyes[0].viewport.originX, eyes[0].viewport.originY,
                        eyes[0].viewport.width, eyes[0].viewport.height, recentered.columns.3.x, recentered.columns.3.y, recentered.columns.3.z)
            d += " pase final: " + finalPass.lastCounts
            lastDiag = d
            log(d)
        }
        return true
    }

    /// Marco del radar de muñeca para los paneles del HUD del brazo (nil si el radar no tiene pose válida).
    private func wristFrame() -> XRPresenter.Wrist? {
        guard let f = renderer.radar?.wristFrame else { return nil }
        return XRPresenter.Wrist(center: f.center, right: f.right, up: f.up, halfW: f.halfW, halfH: f.halfH)
    }

    /// Segunda vista de la escena pedida por el arma (cámara del visor / del misil): matrices en el espacio recentrado. nil si no hace falta renderizarla.
    private func auxView(_ sv: MvVrSpecialView, _ depthRange: SIMD2<Float>) -> RGRenderer.AuxView? {
        let wantsAux = sv.kind == Int32(MV_VIEW_SCOPE) || (sv.kind == Int32(MV_VIEW_MISSILE) && sv.ended == 0)
        guard wantsAux, sv.cam_valid != 0, sv.tex_w >= 16, sv.tex_h >= 16, sv.tex_w <= 2048, sv.tex_h <= 2048 else { return nil }
        let vals = [sv.cam_pos.0, sv.cam_pos.1, sv.cam_pos.2, sv.cam_rot.0, sv.cam_rot.1, sv.cam_rot.2, sv.cam_rot.3, sv.cam_fov_y]
        guard vals.allSatisfy({ $0.isFinite }), sv.cam_fov_y > 0.005, sv.cam_fov_y < 2.5 else { return nil }
        let q = simd_quatf(ix: sv.cam_rot.0, iy: sv.cam_rot.1, iz: sv.cam_rot.2, r: sv.cam_rot.3).normalized
        var cam = simd_float4x4(q)
        cam.columns.3 = simd_float4(sv.cam_pos.0, sv.cam_pos.1, sv.cam_pos.2, 1)
        let t = tan(sv.cam_fov_y * 0.5)
        let aspect = Float(sv.tex_w) / Float(sv.tex_h)
        let proj = XRMath.projection(tangents: SIMD4<Float>(t * aspect, t * aspect, t, t), near: min(depthRange.x, depthRange.y),
                                     far: max(depthRange.x, depthRange.y))
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
