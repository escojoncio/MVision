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
    private(set) var lastDiag = ""
    private(set) var frameNo = 0
    /// Entorno mínimo visible (suelo en rejilla + panel "cargando") durante los primeros 3 s y siempre que no haya nada que dibujar:
    /// el usuario nunca ve negro absoluto y, en la prueba, se sabe si el compositor muestra nuestros fotogramas. El banco lo desactiva
    /// (`XRFrameEncoder.fallbackDefault = false`) salvo en su caso propio.
    static var fallbackDefault = true
    var fallbackEnabled = XRFrameEncoder.fallbackDefault
    private var fallback: XRFallbackEnv?
    private var axes: XRDebugAxes?
    var bootSeconds: Double = 3          // duración del arranque con entorno mínimo siempre visible
    private var firstEncodeT: CFTimeInterval = 0
    private(set) var projSource = "propia"
    private var projNote = ""

    init?(device: MTLDevice, colorFormat: MTLPixelFormat, depthFormat: MTLPixelFormat, layered: Bool, sampleCount: Int) {
        guard let fp = XRFinalPass(device: device, colorFormat: colorFormat, depthFormat: depthFormat, layered: layered) else { return nil }
        finalPass = fp
        renderer = RGRenderer(device: device, colorFormat: colorFormat, depthFormat: depthFormat,
                              sampleCount: sampleCount, viewCount: 2)
        presenter = XRPresenter(device: device)
        fallback = XRFallbackEnv(device: device)
        axes = XRDebugAxes(device: device)
        targets = XRTargets(device: device)
        self.layered = layered
        self.sampleCount = sampleCount
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
    static func projection(_ e: XREyeView, depthRange r: SIMD2<Float>) -> simd_float4x4 {
        let p = planes(r)
        return XRMath.projection(tangents: e.tangents, near: p.near, far: p.far)
    }

    /// ¿Es `m` una proyección de Z invertida utilizable (la que devuelve `computeProjection` con la convención rightUpBack)? Forma
    /// [sx 0 0 0; 0 sy 0 0; cx cy a -1; 0 0 b 0] con a, b > 0; se comprueba también que sx, sy y el desplazamiento concuerden con las
    /// tangentes de la vista (si no, la del sistema no se usa y se anota el motivo).
    static func systemProjectionIssue(_ m: simd_float4x4, tangents t: SIMD4<Float>) -> String? {
        guard XRMath.isFinite(m) else { return "no finita" }
        let c = m.columns
        if abs(c.2.w + 1) > 1e-3 || abs(c.3.w) > 1e-3 || abs(c.0.y) > 1e-3 || abs(c.0.z) > 1e-3 || abs(c.0.w) > 1e-3
            || abs(c.1.x) > 1e-3 || abs(c.1.z) > 1e-3 || abs(c.1.w) > 1e-3 || abs(c.3.x) > 1e-3 || abs(c.3.y) > 1e-3 { return "forma inesperada" }
        if !(c.2.z > 0 && c.3.z > 0) { return "no es Z invertida (P22=\(c.2.z) P32=\(c.3.z))" }
        let own = XRMath.projection(tangents: t, near: 0.1, far: 1000)
        func close(_ a: Float, _ b: Float) -> Bool { abs(a - b) <= 0.02 * max(abs(b), 0.5) }
        if !(close(c.0.x, own.columns.0.x) && close(c.1.y, own.columns.1.y) && close(c.2.x, own.columns.2.x) && close(c.2.y, own.columns.2.y)) {
            return "P00/P11/P20/P21 no concuerdan con las tangentes"
        }
        return nil
    }

    /// Planos (cercano, lejano) en metros que implica una proyección de Z invertida: a = n/(f-n), b = n f/(f-n).
    static func planes(of m: simd_float4x4) -> (near: Float, far: Float)? {
        let a = m.columns.2.z, b = m.columns.3.z
        guard a > 0, b > 0 else { return nil }
        let n = b / (a + 1), f = n * (a + 1) / a
        return (n.isFinite && f.isFinite && n > 0 && f > n) ? (n, f) : nil
    }

    /// Codifica el fotograma completo en `cb`. `recenterInverse * originFromDevice` = cabeza en el espacio de la escena (recentrado).
    /// Devuelve false si no se pudo (texturas vacías, destinos MSAA).
    func encode(commandBuffer cb: MTLCommandBuffer, gs: MvVrGameState, originFromDevice: simd_float4x4, recenterInverse: simd_float4x4,
                dt: Float, eyes: [XREyeView], depthRange: SIMD2<Float>, colorTextures colorTex: [MTLTexture],
                depthTextures depthTex: [MTLTexture], systemProjections: [simd_float4x4]? = nil,
                debugAxes: [XRAxisFrame] = []) -> Bool {
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
        let ownProjs = projs
        // Proyección: la del sistema (`drawable.computeProjection`, la referencia de Apple, con el rango de profundidad que usa el
        // compositor para reproyectar) si tiene la forma de Z invertida esperada y concuerda con las tangentes; si no, la propia.
        var planesUsed = Self.planes(depthRange)
        projSource = "propia"
        if let sp = systemProjections, sp.count >= viewCount {
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
        var pres = presenter.compose(gs: gs, head: recentered, dt: dt, hud: hud, eyePx: Float(w), p00: projs[0].columns.0.x,
                                     gameView: renderer.gameView, linear: renderer.linearOutput, wrist: wristFrame())
        renderer.drawWorld = pres.drawWorld
        renderer.drawBody = pres.drawBody
        var sceneViews = views
        if let dm = pres.dioramaModel { sceneViews = views.map { $0 * dm } }
        if pres.dioramaModel != nil || !pres.drawWorld { special = false }
        renderer.special = special ? sv : nil
        renderer.suppressFrame = special && sv.kind == Int32(MV_VIEW_SCOPE) && sv.suppress_frame != 0
        renderer.auxRequest = special ? auxView(sv, planesUsed) : nil

        // Arranque a prueba de negro (ver `fallbackEnabled`).
        let nowT0 = CACurrentMediaTime()
        if firstEncodeT == 0 { firstEncodeT = nowT0 }
        var fbReason = ""
        if fallbackEnabled, let fb = fallback {
            let hayPantalla = pres.items.contains(where: { $0.kind == .screen })
            let sinMundo = pres.drawWorld ? !renderer.sceneHas3D : !hayPantalla
            let arranque = nowT0 - firstEncodeT < bootSeconds
            if arranque || sinMundo {
                fbReason = arranque ? "arranque" : "nada que dibujar"
                let extra = fb.items(head: recentered, now: nowT0 - firstEncodeT, frame: frameNo, linear: renderer.linearOutput, waiting: !arranque)
                pres.items.insert(contentsOf: extra, at: 0)
            }
        }
        // Ejes de depuración (opción del menú VR): marcos de los mandos y de las manos del personaje, solo en el mundo.
        if !debugAxes.isEmpty, pres.drawWorld, let ax = axes {
            pres.items.insert(contentsOf: ax.items(frames: debugAxes, linear: renderer.linearOutput), at: 0)
        }
        renderer.encode(commandBuffer: cb, colorTargets: tgt.color, depthTargets: tgt.depth,
                        resolveTargets: tgt.resolve, views: sceneViews, projections: projs,
                        head: recentered, layered: layered)

        finalPass.encode(commandBuffer: cb, colorTextures: colorTex, depthTextures: depthTex,
                         msaaDepth: sampleCount > 1 ? tgt.depth : nil, viewports: eyes.map { $0.viewport },
                         views: views, projections: projs, items: pres.items, viewCount: viewCount)

        // Diagnóstico (1 vez por segundo): qué se decidió y qué se dibujó.
        let now = CACurrentMediaTime()
        if now - diagT >= (now - firstEncodeT < 30 ? 1 : 10) {
            diagT = now
            var d = "xr-diag: modo=\(gs.view_mode) motivo=\(gs.view_reason) cinem=\(gs.cinematic) | " + presenter.diagnostics(items: pres.items)
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
            d += " entorno-minimo=" + (fbReason.isEmpty ? "no" : fbReason)
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
        let proj = XRMath.projection(tangents: SIMD4<Float>(t * aspect, t * aspect, t, t), near: pl.near, far: pl.far)
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

/// Entorno mínimo visible que evita el negro absoluto: un suelo en rejilla tenue (16 x 16 m, ejes de color en el origen recentrado) y un
/// panel de estado que se redibuja cada 0,5 s ("cargando", segundos, número de fotograma). Todo son `XRDrawItem` normales del pase final
/// (sin shaders ni pipelines nuevos): el panel descarta lo transparente, así que la rejilla no tapa nada.
final class XRFallbackEnv {
    private static let pw = 768, ph = 256, fN = 1024
    private let floorTex: MTLTexture
    private let floorSRGB: MTLTexture
    private var panel: [MTLTexture] = []
    private var panelSRGB: [MTLTexture] = []
    private var idx = 0
    private var textKey = -1
    private var anchor: simd_float4x4?
    private var panelY: Float = 1.4

    init?(device: MTLDevice) {
        func make(_ w: Int, _ h: Int, _ label: String) -> (MTLTexture, MTLTexture)? {
            let td = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm, width: w, height: h, mipmapped: false)
            td.usage = [.shaderRead, .pixelFormatView]
            td.storageMode = .shared
            guard let t = device.makeTexture(descriptor: td), let v = t.makeTextureView(pixelFormat: .rgba8Unorm_srgb) else { return nil }
            t.label = label
            return (t, v)
        }
        guard let f = make(Self.fN, Self.fN, "mv.fallback.floor") else { return nil }
        floorTex = f.0; floorSRGB = f.1
        for i in 0..<3 {
            guard let p = make(Self.pw, Self.ph, "mv.fallback.panel\(i)") else { return nil }
            panel.append(p.0); panelSRGB.append(p.1)
        }
        // Rejilla: 64 px por metro, línea cada 0,5 m, desvanecida hacia el borde; ejes: x rojo, z azul (alfa recto, no premultiplicado).
        let n = Self.fN
        var px = [UInt8](repeating: 0, count: n * n * 4)
        let c = Float(n) / 2
        for y in 0..<n {
            for x in 0..<n {
                let r = min(1, hypotf(Float(x) - c, Float(y) - c) / c)
                let fade = powf(max(0, 1 - r), 0.7)
                var col: (Float, Float, Float) = (20, 40, 70), a: Float = 0.05 * fade
                if x % 32 < 2 || y % 32 < 2 { col = (90, 200, 230); a = 0.55 * fade }
                if abs(y - n / 2) < 3 { col = (255, 90, 90); a = 0.9 * fade }
                if abs(x - n / 2) < 3 { col = (90, 120, 255); a = 0.9 * fade }
                let o = (y * n + x) * 4
                px[o] = UInt8(col.0); px[o + 1] = UInt8(col.1); px[o + 2] = UInt8(col.2); px[o + 3] = UInt8(max(0, min(255, a * 255)))
            }
        }
        px.withUnsafeBytes { floorTex.replace(region: MTLRegionMake2D(0, 0, n, n), mipmapLevel: 0, withBytes: $0.baseAddress!, bytesPerRow: n * 4) }
    }

    private func drawPanel(seconds: Double, frame: Int, waiting: Bool) {
        let w = Self.pw, h = Self.ph
        guard let ctx = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w * 4,
                                  space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else { return }
        ctx.setFillColor(CGColor(red: 0.06, green: 0.08, blue: 0.13, alpha: 1))
        ctx.fill(CGRect(x: 0, y: 0, width: w, height: h))
        // marco de cuatro colores (patrón reconocible aunque el texto no se lea)
        let cols: [CGColor] = [CGColor(red: 1, green: 0.2, blue: 0.2, alpha: 1), CGColor(red: 0.2, green: 1, blue: 0.3, alpha: 1),
                               CGColor(red: 0.3, green: 0.4, blue: 1, alpha: 1), CGColor(red: 1, green: 0.9, blue: 0.2, alpha: 1)]
        let t: CGFloat = 8, W = CGFloat(w), H = CGFloat(h)
        let edges = [CGRect(x: 0, y: H - t, width: W, height: t), CGRect(x: W - t, y: 0, width: t, height: H),
                     CGRect(x: 0, y: 0, width: W, height: t), CGRect(x: 0, y: 0, width: t, height: H)]
        for (i, e) in edges.enumerated() { ctx.setFillColor(cols[i]); ctx.fill(e) }
        func line(_ s: String, _ size: CGFloat, _ y: CGFloat, _ color: CGColor) {
            let font = CTFontCreateWithName("Helvetica-Bold" as CFString, size, nil)
            let attrs: [NSAttributedString.Key: Any] = [NSAttributedString.Key(kCTFontAttributeName as String): font,
                                                        NSAttributedString.Key(kCTForegroundColorAttributeName as String): color]
            let l = CTLineCreateWithAttributedString(NSAttributedString(string: s, attributes: attrs))
            ctx.textPosition = CGPoint(x: 36, y: y)
            CTLineDraw(l, ctx)
        }
        let white = CGColor(red: 1, green: 1, blue: 1, alpha: 1), grey = CGColor(red: 0.7, green: 0.75, blue: 0.8, alpha: 1)
        line("MVision", 64, 166, white)
        line(waiting ? String(format: "Esperando imagen del juego... %.0f s", seconds) : String(format: "Cargando... %.1f s", seconds), 38, 102, white)
        line("fotograma \(frame) - el compositor muestra nuestro render", 24, 40, grey)
        guard let data = ctx.data else { return }
        idx = (idx + 1) % panel.count
        panel[idx].replace(region: MTLRegionMake2D(0, 0, w, h), mipmapLevel: 0, withBytes: data, bytesPerRow: w * 4)
    }

    /// Elementos del entorno mínimo (el suelo primero, para que lo demás quede encima). `head` = cabeza en el espacio recentrado.
    func items(head: simd_float4x4, now: Double, frame: Int, linear: Bool, waiting: Bool) -> [XRDrawItem] {
        if let an = anchor {
            // Si el panel queda fuera del campo de visión (recentrado, giro) se recoloca delante.
            let pp = an * SIMD4<Float>(0, panelY, -1.8, 1)
            let d = SIMD3<Float>(pp.x, pp.y, pp.z) - XRMath.position(head)
            let fwd = -SIMD3<Float>(head.columns.2.x, head.columns.2.y, head.columns.2.z)
            if simd_length(d) < 1e-3 || simd_dot(simd_normalize(d), simd_normalize(fwd)) < 0.35 { anchor = nil }
        }
        if anchor == nil {
            let p = XRMath.position(head)
            anchor = XRMath.translation(SIMD3<Float>(p.x, 0, p.z)) * XRMath.rotationY(XRMath.yaw(of: head))
            panelY = max(1.0, p.y - 0.1)
        }
        let key = Int(now * 2) + (waiting ? 1_000_000 : 0)
        if key != textKey { textKey = key; drawPanel(seconds: now, frame: frame, waiting: waiting) }
        var out: [XRDrawItem] = []
        var f = XRDrawItem(kind: .panel, texture: linear ? floorSRGB : floorTex)
        f.model = simd_float4x4(columns: (SIMD4<Float>(1, 0, 0, 0), SIMD4<Float>(0, 0, -1, 0), SIMD4<Float>(0, 1, 0, 0),
                                          SIMD4<Float>(0, 0, 0, 1)))
        f.size = SIMD2<Float>(16, 16)
        f.opacity = 1
        out.append(f)
        var pn = XRDrawItem(kind: .panel, texture: linear ? panelSRGB[idx] : panel[idx])
        pn.model = anchor! * XRMath.translation(SIMD3<Float>(0, panelY, -1.8))
        pn.size = SIMD2<Float>(1.2, 0.4)
        pn.opacity = 1
        out.append(pn)
        return out
    }
}
