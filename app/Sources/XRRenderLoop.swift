import Foundation
import CompositorServices
import Metal
import simd
import ARKit
import QuartzCore

/// Bucle de render inmersivo en hilo propio (nunca MainActor).
///
/// Por frame: queryNextFrame -> startUpdate/endUpdate -> predictTiming -> espera a optimalInputTime ->
/// startSubmission -> queryDrawable -> pose prevista (ARKit) en presentationTime -> PS1Renderer.encode
/// (a texturas MSAA propias con resolve a la textura del drawable) -> pase final (resolución de profundidad
/// MSAA + panel 2D) -> encodePresent -> commit -> endSubmission.
///
/// Espacio de la escena = "origen recentrado": metros, Y arriba, suelo en y = 0, -Z = delante al recentrar.
/// Se obtiene de la pose cruda de ARKit multiplicando por inverse(recenterOrigin).
final class XRRenderLoop {
    private let layerRenderer: LayerRenderer
    private let onEnded: (XRRenderLoop) -> Void
    private let device: MTLDevice
    private let queue: MTLCommandQueue
    private let arSession = ARKitSession()
    private let worldTracking = WorldTrackingProvider()
    private var thread: Thread?
    private let stopLock = NSLock()
    private var stopFlag = false

    private var renderer: PS1Renderer!
    private var finalPass: XRFinalPass!
    private var presenter: XRPresenter!                        // pantalla de cine, paneles del HUD, menú VR (XRPresent.swift)
    private var wasBlocked = false
    private var targets: XRTargets
    private let colorFormat: MTLPixelFormat
    private let depthFormat: MTLPixelFormat
    private let layered: Bool
    private let sampleCount: Int
    private let viewCount = 2

    // Recentrado (solo se toca desde el hilo de render).
    private var recenterOrigin = matrix_identity_float4x4        // originFromRecentered
    private var recenterInverse = matrix_identity_float4x4
    private var needsRecenter = true                             // entrar = recentrar en el primer frame con tracking
    private var lastOriginFromDevice = XRMath.translation(SIMD3<Float>(0, 1.6, 0))
    private var everTracked = false

    // Pinza mantenida (gesto de recentrado). Escrito por onSpatialEvent (otro hilo).
    private let gestureLock = NSLock()
    private var pinchSince: [AnyHashable: CFTimeInterval] = [:]
    private var pinchFired = false

    private var wasPaused = false
    private var lastFrameTime: CFTimeInterval = 0

    init?(layerRenderer: LayerRenderer, onEnded: @escaping (XRRenderLoop) -> Void) {
        let dev = layerRenderer.device      // el dispositivo del compositor (el de las texturas del drawable)
        guard let q = dev.makeCommandQueue() else { return nil }
        self.layerRenderer = layerRenderer
        self.onEnded = onEnded
        self.device = dev
        self.queue = q
        q.label = "mv.xr.queue"
        let cfg = layerRenderer.configuration
        colorFormat = cfg.colorFormat
        depthFormat = cfg.depthFormat
        layered = cfg.layout == .layered
        var s = XRSettings.msaa
        while s > 1 && !dev.supportsTextureSampleCount(s) { s /= 2 }
        sampleCount = s
        targets = XRTargets(device: dev)
    }

    func start() {
        let t = Thread { [weak self] in self?.run() }
        t.name = "mv.xr.render"
        t.qualityOfService = .userInteractive
        t.stackSize = 1 << 20
        thread = t
        t.start()
    }

    func stop() { stopLock.lock(); stopFlag = true; stopLock.unlock() }
    private var stopped: Bool { stopLock.lock(); defer { stopLock.unlock() }; return stopFlag }

    // MARK: hilo

    private func run() {
        renderer = PS1Renderer(device: device, colorFormat: colorFormat, depthFormat: depthFormat,
                               sampleCount: sampleCount, viewCount: viewCount)
        guard let fp = XRFinalPass(device: device, colorFormat: colorFormat, depthFormat: depthFormat, layered: layered) else {
            Storage.log("xr: no se pudieron crear los pipelines del pase final")
            finish()
            return
        }
        finalPass = fp
        let pr = XRPresenter(device: device)
        pr.onRecenter = { XRControl.shared.requestRecenter() }
        pr.onExit = { Task { @MainActor in await XRSession.shared.exitVR() } }
        presenter = pr

        if WorldTrackingProvider.isSupported {
            let s = arSession, p = worldTracking
            Task.detached { do { try await s.run([p]) } catch { Storage.log("xr: ARKitSession.run falló: \(error)") } }
        } else {
            Storage.log("xr: WorldTrackingProvider no disponible; pose fija")
        }

        installRecenterGesture()
        Storage.log("xr: bucle iniciado (layout \(layered ? "layered" : "dedicated"), msaa \(sampleCount), color \(colorFormat.rawValue))")

        loop: while !stopped {
            switch layerRenderer.state {
            case .paused:
                if !wasPaused { wasPaused = true; XRControl.shared.notifyPaused(true) }
                layerRenderer.waitUntilRunning()
            case .running:
                if wasPaused { wasPaused = false; XRControl.shared.notifyPaused(false) }
                autoreleasepool { renderFrame() }
            case .invalidated:
                break loop
            @unknown default:
                break loop
            }
        }
        if wasPaused { XRControl.shared.notifyPaused(false) }
        finish()
    }

    private func finish() {
        arSession.stop()
        layerRenderer.onSpatialEvent = { _ in }
        mv_vr_clear_head()      // sin cabeza publicada la cámara del motor vuelve a la del juego
        Storage.log("xr: bucle terminado")
        onEnded(self)
    }

    // MARK: frame

    private func renderFrame() {
        guard let frame = layerRenderer.queryNextFrame() else { return }

        frame.startUpdate()
        if XRControl.shared.takeRecenterRequest() { needsRecenter = true }
        if consumeLongPinch() { needsRecenter = true }
        frame.endUpdate()

        guard let timing = frame.predictTiming() else { return }
        LayerRenderer.Clock().wait(until: timing.optimalInputTime)

        frame.startSubmission()
        defer { frame.endSubmission() }

        let drawable: LayerRenderer.Drawable?
        drawable = frame.queryDrawable()
        guard let drawable = drawable, drawable.views.count >= viewCount else { return }
        mv_host_note_render(1)
        // NO se toca drawable.depthRange: cp_drawable_set_depth_range aborta (__BUG_IN_CLIENT__) en visionOS 27 al llamarlo
        // sobre el drawable ya consultado. La proyección (projection(_:_:)) lee el rango del sistema, así que es coherente.

        // Pose en el instante de presentación previsto.
        let d = LayerRenderer.Clock.Instant.epoch.duration(to: drawable.frameTiming.presentationTime)
        let seconds = Double(d.components.seconds) + Double(d.components.attoseconds) * 1e-18
        let anchor = worldTracking.queryDeviceAnchor(atTimestamp: seconds)
        drawable.deviceAnchor = anchor

        var tracked = false
        var originFromDevice = lastOriginFromDevice
        if let a = anchor, a.isTracked, XRMath.isFinite(a.originFromAnchorTransform) {
            originFromDevice = a.originFromAnchorTransform
            lastOriginFromDevice = originFromDevice
            tracked = true
            everTracked = true
        }
        if needsRecenter && tracked {
            applyRecenter(originFromDevice)
            needsRecenter = false
            presenter.reanchor()
            VRInput.shared.recenter()      // giro del cuerpo a 0 y recentrado del rumbo de la cámara del motor
        }

        let recentered = recenterInverse * originFromDevice
        if tracked || !everTracked {
            let f = XRMath.floats(recentered)
            f.withUnsafeBufferPointer { mv_vr_set_head($0.baseAddress, seconds) }
        }

        // Entrada VR -> mando del juego (en lugar de InputManager mientras el espacio está abierto).
        let nowT = CACurrentMediaTime()
        let dt = lastFrameTime > 0 ? Float(min(max(nowT - lastFrameTime, 0.001), 0.1)) : Float(1.0 / 90.0)
        lastFrameTime = nowT
        var gs = MvVrGameState()
        mv_vrhost_game_state(&gs)
        // Menú VR (mantener el botón de menú izquierdo): mientras está abierto el juego no recibe entrada.
        let blocked = presenter.updateInput(ControllerHub.shared.sample(), dt: dt, head: recentered)
        if blocked {
            if !wasBlocked { VRInput.shared.releaseAll() }
            ControllerHub.shared.releaseAll(from: .vr)
        } else {
            VRInput.shared.frame(head: tracked || !everTracked ? recentered : nil, dt: dt,
                                 gamePosture: gs.posture, menu: gs.menu != 0, recenter: recenterInverse)
        }
        wasBlocked = blocked
        // Hápticos del arma VR (disparo, recarga) y pose del radar de muñeca (antebrazo izquierdo tras la IK de vr/body).
        var hapticL: Float = 0, hapticR: Float = 0
        mv_vrgame_take_haptics(&hapticL, &hapticR)
        if hapticL > 0 || hapticR > 0 { VRInput.shared.rumble(hapticR, hapticL) }
        updateRadarPose(head: recentered)

        // Vistas y proyecciones por ojo.
        var views: [simd_float4x4] = []
        var projs: [simd_float4x4] = []
        for i in 0..<viewCount {
            let v = drawable.views[i]
            let viewFromRecentered = (recenterInverse * originFromDevice * v.transform).inverse
            views.append(viewFromRecentered)
            projs.append(projection(drawable, i))
        }

        // Texturas.
        let colorTex = drawable.colorTextures
        let depthTex = drawable.depthTextures
        guard !colorTex.isEmpty, !depthTex.isEmpty else { return }
        let w = colorTex[0].width, h = colorTex[0].height
        guard let tgt = targets.get(width: w, height: h, layered: layered, views: viewCount,
                                    samples: sampleCount, colorFormat: colorFormat, depthFormat: depthFormat,
                                    drawableColor: colorTex, drawableDepth: depthTex) else { return }

        guard let cb = queue.makeCommandBuffer() else { return }
        cb.label = "mv.xr.frame"

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
        renderer.auxRequest = special ? auxView(sv, drawable) : nil

        renderer.encode(commandBuffer: cb, colorTargets: tgt.color, depthTargets: tgt.depth,
                        resolveTargets: tgt.resolve, views: sceneViews, projections: projs,
                        head: recentered, layered: layered)

        let viewports = (0..<viewCount).map { drawable.views[$0].textureMap.viewport }
        finalPass.encode(commandBuffer: cb, colorTextures: colorTex, depthTextures: depthTex,
                         msaaDepth: sampleCount > 1 ? tgt.depth : nil, viewports: viewports,
                         views: views, projections: projs, items: pres.items, viewCount: viewCount)

        drawable.encodePresent(commandBuffer: cb)
        cb.commit()
    }

    /// Coloca el radar de muñeca en el antebrazo izquierdo: pose tras la IK del cuerpo; sin ella, el agarre del mando izquierdo.
    private func updateRadarPose(head: simd_float4x4) {
        guard let radar = renderer.radar else { return }
        var fa = MvVrForearm()
        var forearm: (elbow: SIMD3<Float>, wrist: SIMD3<Float>, dorsal: SIMD3<Float>)?
        if mv_vrgame_forearm(0, &fa) != 0 {
            forearm = (SIMD3<Float>(fa.elbow.0, fa.elbow.1, fa.elbow.2),
                       SIMD3<Float>(fa.wrist.0, fa.wrist.1, fa.wrist.2),
                       SIMD3<Float>(fa.dorsal.0, fa.dorsal.1, fa.dorsal.2))
        }
        var grip: simd_float4x4?
        var g = [Float](repeating: 0, count: 16)
        if mv_vrgame_hand_matrix(0, &g) != 0 {
            grip = simd_float4x4(columns: (SIMD4<Float>(g[0], g[1], g[2], g[3]), SIMD4<Float>(g[4], g[5], g[6], g[7]),
                                           SIMD4<Float>(g[8], g[9], g[10], g[11]), SIMD4<Float>(g[12], g[13], g[14], g[15])))
        }
        let fwd = simd_normalize(-SIMD3<Float>(head.columns.2.x, head.columns.2.y, head.columns.2.z))
        radar.updatePose(forearm: forearm, leftGrip: grip,
                         headPosition: SIMD3<Float>(head.columns.3.x, head.columns.3.y, head.columns.3.z), headForward: fwd)
    }

    /// Marco del radar de muñeca para los paneles del HUD del brazo (nil si el radar no tiene pose válida).
    private func wristFrame() -> XRPresenter.Wrist? {
        guard let f = renderer.radar?.wristFrame else { return nil }
        return XRPresenter.Wrist(center: f.center, right: f.right, up: f.up, halfW: f.halfW, halfH: f.halfH)
    }

    /// Segunda vista de la escena pedida por el arma (cámara del visor / del misil): matrices en el espacio recentrado. nil si no hace falta renderizarla.
    private func auxView(_ sv: MvVrSpecialView, _ drawable: LayerRenderer.Drawable) -> PS1Renderer.AuxView? {
        let wantsAux = sv.kind == Int32(MV_VIEW_SCOPE) || (sv.kind == Int32(MV_VIEW_MISSILE) && sv.ended == 0)
        guard wantsAux, sv.cam_valid != 0, sv.tex_w >= 16, sv.tex_h >= 16, sv.tex_w <= 2048, sv.tex_h <= 2048 else { return nil }
        let vals = [sv.cam_pos.0, sv.cam_pos.1, sv.cam_pos.2, sv.cam_rot.0, sv.cam_rot.1, sv.cam_rot.2, sv.cam_rot.3, sv.cam_fov_y]
        guard vals.allSatisfy({ $0.isFinite }), sv.cam_fov_y > 0.005, sv.cam_fov_y < 2.5 else { return nil }
        let q = simd_quatf(ix: sv.cam_rot.0, iy: sv.cam_rot.1, iz: sv.cam_rot.2, r: sv.cam_rot.3).normalized
        var cam = simd_float4x4(q)
        cam.columns.3 = simd_float4(sv.cam_pos.0, sv.cam_pos.1, sv.cam_pos.2, 1)
        let t = tan(sv.cam_fov_y * 0.5)
        let aspect = Float(sv.tex_w) / Float(sv.tex_h)
        let r = drawable.depthRange
        let proj = XRMath.projection(tangents: SIMD4<Float>(t * aspect, t * aspect, t, t), near: min(r.x, r.y), far: max(r.x, r.y))
        return PS1Renderer.AuxView(view: cam.inverse, proj: proj, width: Int(sv.tex_w), height: Int(sv.tex_h),
                                   pos: simd_float4(sv.cam_pos.0, sv.cam_pos.1, sv.cam_pos.2, 1))
    }

    /// Proyección de Z invertida del ojo i (a partir de las tangentes de la vista).
    private func projection(_ drawable: LayerRenderer.Drawable, _ i: Int) -> simd_float4x4 {
        let r = drawable.depthRange
        let near = min(r.x, r.y), far = max(r.x, r.y)
        return XRMath.projection(tangents: drawable.views[i].tangents, near: near, far: far)
    }

    private func applyRecenter(_ originFromDevice: simd_float4x4) {
        let p = XRMath.position(originFromDevice)
        recenterOrigin = XRMath.translation(SIMD3<Float>(p.x, 0, p.z)) * XRMath.rotationY(XRMath.yaw(of: originFromDevice))
        recenterInverse = recenterOrigin.inverse
    }

    // MARK: gesto de recentrado

    private func installRecenterGesture() {
        guard XRSettings.recenterGesture else { return }
        layerRenderer.onSpatialEvent = { [weak self] events in
            guard let self = self else { return }
            let now = CACurrentMediaTime()
            self.gestureLock.lock()
            for e in events {
                switch e.phase {
                case .active:
                    if self.pinchSince[e.id] == nil { self.pinchSince[e.id] = now }
                default:
                    self.pinchSince[e.id] = nil
                    if self.pinchSince.isEmpty { self.pinchFired = false }
                }
            }
            self.gestureLock.unlock()
        }
    }

    /// true una vez cuando una pinza lleva activa >= 1,2 s (se rearma al soltar todas).
    private func consumeLongPinch() -> Bool {
        gestureLock.lock(); defer { gestureLock.unlock() }
        guard !pinchFired, let t = pinchSince.values.min(), CACurrentMediaTime() - t >= 1.2 else { return false }
        pinchFired = true
        return true
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
