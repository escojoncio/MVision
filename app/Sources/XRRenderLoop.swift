import Foundation
import CompositorServices
import Metal
import simd
import ARKit
import QuartzCore

/// Bucle de render inmersivo en hilo propio (nunca MainActor).
///
/// Por frame: queryNextFrame -> startUpdate/endUpdate -> predictTiming -> espera a optimalInputTime ->
/// startSubmission -> queryDrawable -> pose prevista (ARKit) en presentationTime -> RGRenderer.encode
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

    private var encoder: XRFrameEncoder!                       // escena + pantalla de cine/HUD/menú (XRFrameEncoder.swift, compartido con el banco de CI)
    private var presenter: XRPresenter { encoder.presenter }   // pantalla de cine, paneles del HUD, menú VR (XRPresent.swift)
    private var wasBlocked = false
    private var diagT: CFTimeInterval = 0
    private static let errLock = NSLock()
    private static var lastErrLog: CFTimeInterval = 0
    private var trackedFrames = 0, untrackedFrames = 0
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
        guard let enc = XRFrameEncoder(device: device, colorFormat: colorFormat, depthFormat: depthFormat, layered: layered,
                                       sampleCount: sampleCount) else {
            Storage.log("xr: no se pudieron crear los pipelines del pase final")
            finish()
            return
        }
        enc.log = { Storage.log($0) }
        enc.presenter.onRecenter = { XRControl.shared.requestRecenter() }
        enc.presenter.onExit = { Task { @MainActor in await XRSession.shared.exitVR() } }
        encoder = enc

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
        if tracked { trackedFrames += 1 } else { untrackedFrames += 1 }
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

        // Texturas, vistas por ojo y codificación (XRFrameEncoder: mismo código que el banco de render de CI).
        let eyes: [XREyeView] = (0..<viewCount).map { i in
            let v = drawable.views[i]
            return XREyeView(transform: v.transform, tangents: v.tangents, viewport: v.textureMap.viewport)
        }
        guard let cb = queue.makeCommandBuffer() else { return }
        cb.label = "mv.xr.frame"
        // Una vez por segundo se compara nuestra proyección con la del sistema (solo para el registro).
        var sysProj: [simd_float4x4]?
        if nowT - diagT >= 1, #available(visionOS 2.0, *) {
            sysProj = (0..<viewCount).map { drawable.computeProjection(viewIndex: $0) }
        }
        let ok = encoder.encode(commandBuffer: cb, gs: gs, originFromDevice: originFromDevice, recenterInverse: recenterInverse, dt: dt,
                                eyes: eyes, depthRange: drawable.depthRange, colorTextures: drawable.colorTextures,
                                depthTextures: drawable.depthTextures, systemProjections: sysProj)
        if !ok { Storage.log("xr: el codificador no pudo preparar el fotograma (texturas del drawable vacías o destinos MSAA)") }

        if nowT - diagT >= 1 {
            diagT = nowT
            Storage.log("xr-diag: seguimiento ok=\(trackedFrames) sin=\(untrackedFrames) ancla=\(anchor == nil ? "nil" : (anchor!.isTracked ? "rastreada" : "no rastreada")) recentrado=\(needsRecenter ? "pendiente" : "hecho") frame=\(encoder.frameNo) origenDispositivo=(\(String(format: "%.2f %.2f %.2f", originFromDevice.columns.3.x, originFromDevice.columns.3.y, originFromDevice.columns.3.z))) rumboDispositivo=\(String(format: "%.0f", XRMath.yaw(of: originFromDevice) * 180 / .pi))grados mapasTasa=\(drawable.rasterizationRateMaps.count) objetivo=\(drawable.target) vistas=\(drawable.views.count) uso(color0)=\(drawable.colorTextures.first?.usage.rawValue ?? 0) almacenamiento=\(drawable.colorTextures.first?.storageMode.rawValue ?? 99) pasoDeFotograma=\(Int(1000 * dt)) ms")
            trackedFrames = 0; untrackedFrames = 0
        }

        drawable.encodePresent(commandBuffer: cb)
        // Errores de GPU (un command buffer con error no presenta el fotograma = negro): se registran, como mucho uno por segundo.
        cb.addCompletedHandler { buf in
            if let e = buf.error {
                let t = CACurrentMediaTime()
                XRRenderLoop.errLock.lock()
                let go = t - XRRenderLoop.lastErrLog >= 1
                if go { XRRenderLoop.lastErrLog = t }
                XRRenderLoop.errLock.unlock()
                if go { Storage.log("xr: ERROR DE GPU en el command buffer (estado \(buf.status.rawValue)): \(e)") }
            }
        }
        cb.commit()
    }

    /// Coloca el radar de muñeca en el antebrazo izquierdo: pose tras la IK del cuerpo; sin ella, el agarre del mando izquierdo.
    private func updateRadarPose(head: simd_float4x4) {
        guard let radar = encoder.renderer.radar else { return }
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
