import CoreHaptics
import Foundation
import GameController
import simd

/// Ajustes de control VR persistentes (UserDefaults, claves "vri.*").
struct VRInputSettings: Equatable {
    var smoothTurn = false            // false: giro por pasos
    var snapDegrees: Float = 30       // 30 o 45
    var smoothDegreesPerSecond: Float = 120
    var vignette: Float = 0.6         // 0..1
    var locomotionFromHand = false    // false: relativa a la cabeza
    var rightHanded = true            // mano dominante
    var weaponReload = 0              // recarga del arma VR: 0 manual, 1 simplificada, 2 automática
    var allowProne = false
    var physicalPosture = 0           // postura por altura de la cabeza: 0 desactivada (defecto; el botón del juego manda), 1 solo para bajar, 2 completa
    var headHeight: Float = 0         // altura de pie calibrada en metros; 0 = autocalibrar
    var crouchDrop: Float = 0.35
    var proneDrop: Float = 0.80

    private static let prefix = "vri."

    static func load(_ d: UserDefaults = .standard) -> VRInputSettings {
        var s = VRInputSettings()
        func has(_ k: String) -> Bool { d.object(forKey: prefix + k) != nil }
        if has("smooth") { s.smoothTurn = d.bool(forKey: prefix + "smooth") }
        if has("snap") { s.snapDegrees = d.float(forKey: prefix + "snap") }
        if has("smoothDps") { s.smoothDegreesPerSecond = d.float(forKey: prefix + "smoothDps") }
        if has("vignette") { s.vignette = d.float(forKey: prefix + "vignette") }
        if has("locoHand") { s.locomotionFromHand = d.bool(forKey: prefix + "locoHand") }
        if has("rightHanded") { s.rightHanded = d.bool(forKey: prefix + "rightHanded") }
        if has("wreload") { s.weaponReload = min(max(d.integer(forKey: prefix + "wreload"), 0), 2) }
        if has("prone") { s.allowProne = d.bool(forKey: prefix + "prone") }
        if has("posture") { s.physicalPosture = min(max(d.integer(forKey: prefix + "posture"), 0), 2) }
        if has("height") { s.headHeight = d.float(forKey: prefix + "height") }
        if has("crouch") { s.crouchDrop = d.float(forKey: prefix + "crouch") }
        if has("proneDrop") { s.proneDrop = d.float(forKey: prefix + "proneDrop") }
        return s
    }

    func save(_ d: UserDefaults = .standard) {
        d.set(smoothTurn, forKey: Self.prefix + "smooth")
        d.set(snapDegrees, forKey: Self.prefix + "snap")
        d.set(smoothDegreesPerSecond, forKey: Self.prefix + "smoothDps")
        d.set(vignette, forKey: Self.prefix + "vignette")
        d.set(locomotionFromHand, forKey: Self.prefix + "locoHand")
        d.set(rightHanded, forKey: Self.prefix + "rightHanded")
        d.set(weaponReload, forKey: Self.prefix + "wreload")
        d.set(allowProne, forKey: Self.prefix + "prone")
        d.set(physicalPosture, forKey: Self.prefix + "posture")
        d.set(headHeight, forKey: Self.prefix + "height")
        d.set(crouchDrop, forKey: Self.prefix + "crouch")
        d.set(proneDrop, forKey: Self.prefix + "proneDrop")
    }
}

/// Lee mandos (PS VR2 Sense, DualSense / extendido), rellena el estado C cada fotograma, llama a vr_input y envía
/// el resultado a `mv_host_pad`. Debe usarse EN LUGAR de `InputManager` (ambos escriben en `mv_host_pad`).
/// Uso: `VRInput.shared.start()` una vez (hilo principal) y, en cada fotograma de render,
/// `VRInput.shared.frame(head: deviceAnchorTransform, dt: dt, gamePosture: ..., menu: ...)`.
final class VRInput: @unchecked Sendable {
    static let shared = VRInput()

    private let lock = NSLock()
    private let hub = ControllerHub.shared
    private var state = vr_input_t()
    private var started = false
    private var lastRumbleSmall: UInt8 = 0
    private var lastRumbleLarge: UInt8 = 0
    private var lastTurnYaw: Float = 0     // body_yaw ya enviado a la cámara (para reenviar solo el incremento)
    private var pulsers: [String: HapticPulse] = [:]
    private var lastPulse: [String: Double] = [:]

    private(set) var settings = VRInputSettings.load()

    // Salidas para la capa de cámara y el renderer (valores del último fotograma).
    private var _bodyYaw: Float = 0
    private var _vignette: Float = 0
    private var _snapCount: UInt32 = 0
    private var _posture: Int32 = 0
    var bodyYaw: Float { lock.withLock { _bodyYaw } }
    var vignette: Float { lock.withLock { _vignette } }
    var snapCount: UInt32 { lock.withLock { _snapCount } }
    /// Postura deseada por la cabeza: 0 de pie, 1 agachado, 2 tumbado.
    var posture: Int32 { lock.withLock { _posture } }

    private init() {
        vr_input_init(&state)
        applySettingsLocked(applyHeight: true)
    }

    // MARK: - Ajustes

    func update(_ change: (inout VRInputSettings) -> Void) {
        lock.lock()
        var s = settings
        let oldHeight = s.headHeight
        change(&s)
        settings = s
        // La altura guardada solo se reaplica si el cambio la toca; si no, pisaría una recalibración pendiente.
        applySettingsLocked(applyHeight: s.headHeight != oldHeight)
        lock.unlock()
        s.save()
    }

    /// Toma la altura de cabeza del próximo fotograma como "de pie" (y la guarda al calibrarse).
    func recalibrateHeight() {
        lock.withLock { vr_input_recalibrate(&state) }
    }

    func resetBodyYaw(_ radians: Float = 0) {
        lock.withLock {
            vr_input_reset_body_yaw(&state, radians)
            lastTurnYaw = radians
        }
    }

    /// Recentrado del espacio: el giro acumulado del cuerpo vuelve a 0 y la capa de cámara recentra su propio rumbo.
    func recenter() {
        resetBodyYaw(0)
        mv_vr_recenter()
    }

    /// Rumbo del mundo (cabeza+cuerpo) al que corresponde "stick arriba" del juego.
    func setGameUpHeading(_ radians: Float) {
        lock.withLock { vr_input_set_game_up(&state, radians) }
    }

    private func applySettingsLocked(applyHeight: Bool) {
        var c = state.cfg
        c.turn_mode = settings.smoothTurn ? VRI_TURN_SMOOTH : VRI_TURN_SNAP
        c.snap_deg = settings.snapDegrees
        c.smooth_dps = settings.smoothDegreesPerSecond
        c.vignette = settings.vignette
        c.loco_ref = settings.locomotionFromHand ? VRI_LOCO_HAND : VRI_LOCO_HEAD
        c.dominant_hand = settings.rightHanded ? 1 : 0
        c.allow_prone = settings.allowProne
        c.posture_mode = Int32(settings.physicalPosture)
        c.crouch_drop_m = settings.crouchDrop
        c.prone_drop_m = settings.proneDrop
        vr_input_set_cfg(&state, &c)
        mv_vrgame_set_options(settings.rightHanded ? 1 : 0, Int32(settings.weaponReload))   // mano y recarga del arma VR
        if applyHeight, settings.headHeight > 0 { vr_input_set_height(&state, settings.headHeight) }
    }

    // MARK: - Conexión

    /// Empieza a vigilar mandos (la detección y la lectura viven en ControllerHub). Hilo principal.
    func start() {
        guard !started else { return }
        started = true
        hub.onChange = { [weak self] in self?.controllersChanged() }
        hub.start()
        controllersChanged()
    }

    /// Conexión tardía incluida: cada alta/baja de un mando vuelve a pedir el seguimiento del conjunto actual.
    private func controllersChanged() {
        lock.withLock { pulsers = [:] }
        VRInputPose.shared.track(hub.senseControllers)
        if hub.senseControllers.isEmpty && hub.gamepadController == nil { releaseAll() }
    }

    func releaseAll() {
        hub.releaseAll(from: .vr)
    }

    // MARK: - Lectura

    private static func hand(_ s: ControllerHub.HandSnapshot, pose: VRInputPose.Pose) -> vri_hand_t {
        var h = vri_hand_t()
        h.stick_x = s.stickX
        h.stick_y = s.stickY
        h.trigger = s.trigger
        h.grip = s.grip
        var bits: UInt32 = 0
        if s.low { bits |= UInt32(VRI_H_LOW) }
        if s.up { bits |= UInt32(VRI_H_UP) }
        if s.stickClick { bits |= UInt32(VRI_H_STICK) }
        if s.menu { bits |= UInt32(VRI_H_MENU) }
        h.buttons = bits
        h.tracked = pose.tracked
        h.pos = (pose.position.x, pose.position.y, pose.position.z)
        h.quat = (pose.rotation.x, pose.rotation.y, pose.rotation.z, pose.rotation.w)
        return h
    }

    /// Pose del mando en el espacio recentrado (el mismo en que llega la cabeza): `recenter` = inversa del origen de recentrado.
    private static func recentered(_ p: VRInputPose.Pose, _ recenter: simd_float4x4) -> VRInputPose.Pose {
        guard p.tracked else { return p }
        var m = simd_matrix4x4(simd_quatf(vector: p.rotation))
        m.columns.3 = SIMD4<Float>(p.position.x, p.position.y, p.position.z, 1)
        let r = recenter * m
        let q = simd_quatf(simd_float3x3(
            SIMD3<Float>(r.columns.0.x, r.columns.0.y, r.columns.0.z),
            SIMD3<Float>(r.columns.1.x, r.columns.1.y, r.columns.1.z),
            SIMD3<Float>(r.columns.2.x, r.columns.2.y, r.columns.2.z)))
        var out = p
        out.position = SIMD3<Float>(r.columns.3.x, r.columns.3.y, r.columns.3.z)
        out.rotation = q.vector
        return out
    }

    /// Entrega el mando (pose recentrada y botones) a la capa de jugabilidad en C (brazos, radar de muñeca, arma VR).
    private static func publish(_ s: ControllerHub.HandSnapshot, _ pose: VRInputPose.Pose, side: Int32) {
        var h = MvVrHand()
        h.valid = (s.present && pose.tracked) ? 1 : 0
        h.pos = (pose.position.x, pose.position.y, pose.position.z)
        h.rot = (pose.rotation.x, pose.rotation.y, pose.rotation.z, pose.rotation.w)
        h.trigger = s.trigger
        h.grip = s.grip
        h.low = s.low ? 1 : 0
        h.up = s.up ? 1 : 0
        h.stick = s.stickClick ? 1 : 0
        mv_vrgame_set_hand(side, &h)
    }

    /// Un fotograma: lee, procesa y envía al juego. `head` = transformación cabeza->origen (deviceAnchor) o nil.
    /// `gamePosture` del puente RAM (0 de pie, 1 agachado, 2 tumbado, -1 desconocida). Devuelve el pad enviado.
    @discardableResult
    func frame(head: simd_float4x4?, dt: Float, gamePosture: Int32 = -1, menu: Bool = false,
               recenter: simd_float4x4 = matrix_identity_float4x4) -> vr_pad_t {
        let sample = hub.sample()

        var f = vr_input_frame_t()
        f.dt = dt
        f.game_posture = gamePosture
        f.menu = menu
        if let m = head {
            f.head_valid = true
            f.head_pos = (m.columns.3.x, m.columns.3.y, m.columns.3.z)
            let q = simd_quatf(simd_float3x3(
                SIMD3<Float>(m.columns.0.x, m.columns.0.y, m.columns.0.z),
                SIMD3<Float>(m.columns.1.x, m.columns.1.y, m.columns.1.z),
                SIMD3<Float>(m.columns.2.x, m.columns.2.y, m.columns.2.z)))
            f.head_quat = (q.vector.x, q.vector.y, q.vector.z, q.vector.w)
        }
        var mode = VRI_MODE_PAD
        switch sample.kind {
        case .sense:
            mode = VRI_MODE_SENSE
            // Poses de los mandos en el espacio recentrado (como la cabeza) y entrega a la capa de jugabilidad (por mano física).
            let pl = Self.recentered(VRInputPose.shared.pose(hand: 0), recenter)
            let pr = Self.recentered(VRInputPose.shared.pose(hand: 1), recenter)
            Self.publish(sample.left, pl, side: 0)
            Self.publish(sample.right, pr, side: 1)
            var l = sample.left, r = sample.right
            // Con un solo Sense: su stick pasa a ser el de mover (el de girar queda quieto; se gira con el cuerpo).
            // En los menús ya confirman tanto su gatillo como su botón inferior (mapa de menús de vr_input).
            if l.present != r.present {
                let dominantRight = lock.withLock { settings.rightHanded }
                var lone = l.present ? l : r
                var other = ControllerHub.HandSnapshot()
                other.present = true
                other.stickX = lone.stickX; other.stickY = lone.stickY
                lone.stickX = 0; lone.stickY = 0
                // El mando solo hace de mano dominante; la otra mano es la sintética.
                if dominantRight { l = other; r = lone } else { l = lone; r = other }
            }
            f.hand = (Self.hand(l, pose: pl), Self.hand(r, pose: pr))
        case .gamepad:
            mv_vrgame_clear_hands()
            f.pad_buttons = sample.pad.buttons
            f.pad_lx = sample.pad.lx
            f.pad_ly = sample.pad.ly
            f.pad_rx = sample.pad.rx
            f.pad_ry = sample.pad.ry
        case .none:
            mv_vrgame_clear_hands()
        }

        lock.lock()
        // "Stick arriba" del juego = hacia donde mira la cabeza (cabeza + giro del cuerpo): la cámara VR del motor
        // sigue ese rumbo, así que el stick de mover (relativo a la cabeza) llega al juego como "adelante".
        if head != nil {
            let h = withUnsafePointer(to: &f.head_quat) {
                $0.withMemoryRebound(to: Float.self, capacity: 4) { vr_input_quat_heading($0) }
            }
            vr_input_set_game_up(&state, h + vr_input_body_yaw(&state))
        }
        if state.cfg.mode != mode {
            var c = state.cfg
            c.mode = mode
            vr_input_set_cfg(&state, &c)
        }
        var out = vr_pad_t()
        var turnDeg: Float = 0
        vr_input_update(&state, &f, &out)
        _bodyYaw = vr_input_body_yaw(&state)
        _vignette = vr_input_vignette(&state)
        _snapCount = vr_input_snap_count(&state)
        _posture = Int32(vr_input_posture(&state))
        // Giro del cuerpo -> cámara del motor: incremento envuelto a (-pi, pi]; vr_input (+ = derecha), cámara (+ = izquierda).
        var dyaw = _bodyYaw - lastTurnYaw
        while dyaw > Float.pi { dyaw -= 2 * Float.pi }
        while dyaw <= -Float.pi { dyaw += 2 * Float.pi }
        lastTurnYaw = _bodyYaw
        turnDeg = dyaw * 180 / Float.pi
        // Guardar la altura calibrada para la próxima sesión si cambió de forma apreciable.
        let h = vr_input_height(&state)
        let saveHeight = h > 0 && abs(h - settings.headHeight) > 0.03
        if saveHeight { settings.headHeight = h }
        let toSave = settings
        lock.unlock()
        if saveHeight { toSave.save() }

        hub.write(buttons: out.buttons, lx: out.lx, ly: out.ly, rx: out.rx, ry: out.ry, from: .vr)
        if turnDeg != 0 { mv_vr_turn(-turnDeg) }
        forwardRumble()
        return out
    }

    /// Botones de los dos Sense con la tabla de MENÚS de vr_input (sin estado): la usa la ventana, que no conoce el juego.
    func plainSenseButtons(left: ControllerHub.HandSnapshot, right: ControllerHub.HandSnapshot) -> UInt16 {
        let hands: [vri_hand_t] = [Self.hand(left, pose: VRInputPose.Pose()), Self.hand(right, pose: VRInputPose.Pose())]
        var cfg = lock.withLock { state.cfg }
        return hands.withUnsafeBufferPointer { vr_input_sense_plain(&cfg, $0.baseAddress) }
    }

    // MARK: - Hápticos

    /// Vibración del mando: `small` = motor agudo (derecha), `large` = grave (izquierda), 0..1.
    /// Se llama cada fotograma con el nivel actual (se limita a ~12 Hz por motor); 0 no hace nada.
    func rumble(_ small: Float, _ large: Float) {
        pulse(side: 1, amplitude: small)
        pulse(side: 0, amplitude: large)
    }

    private func forwardRumble() {
        var s: UInt8 = 0, l: UInt8 = 0
        mv_host_rumble(&s, &l)
        if s != 0 || l != 0 || lastRumbleSmall != 0 || lastRumbleLarge != 0 {
            rumble(Float(s) / 255, Float(l) / 255)
        }
        lastRumbleSmall = s
        lastRumbleLarge = l
    }

    private func pulse(side: Int, amplitude: Float) {
        guard amplitude > 0.01 else { return }
        let now = CFAbsoluteTimeGetCurrent()
        lock.lock()
        // Objetivos: Sense -> cada mando su lado; mando normal -> localidades de empuñadura o por defecto.
        var targets: [(GCController, GCHapticsLocality, String)] = []
        let sl = hub.senseLeft, sr = hub.senseRight, gp = hub.gamepadController
        if sl != nil || sr != nil {
            if let c = side == 0 ? sl : sr { targets.append((c, .default, "\(ObjectIdentifier(c).hashValue)-d")) }
        } else if let c = gp, let h = c.haptics {
            let loc: GCHapticsLocality
            if h.supportedLocalities.contains(side == 0 ? .leftHandle : .rightHandle) {
                loc = side == 0 ? .leftHandle : .rightHandle
            } else {
                loc = .default
            }
            targets.append((c, loc, "\(ObjectIdentifier(c).hashValue)-\(loc.rawValue)"))
        }
        var run: [(HapticPulse)] = []
        for (c, loc, key) in targets {
            if let last = lastPulse[key], now - last < 0.08 { continue }
            lastPulse[key] = now
            if pulsers[key] == nil, let h = c.haptics, let p = HapticPulse(haptics: h, locality: loc) { pulsers[key] = p }
            if let p = pulsers[key] { run.append(p) }
        }
        lock.unlock()
        for p in run { p.pulse(amplitude: amplitude, seconds: 0.12) }
    }
}

/// Un motor del mando como motor CoreHaptics que toca un evento continuo por pulso.
final class HapticPulse: @unchecked Sendable {
    private var engine: CHHapticEngine?
    private let haptics: GCDeviceHaptics
    private let locality: GCHapticsLocality
    private let lock = NSLock()

    init?(haptics: GCDeviceHaptics, locality: GCHapticsLocality) {
        self.haptics = haptics
        self.locality = locality
        guard makeEngine() else { return nil }
    }

    private func makeEngine() -> Bool {
        guard let e = haptics.createEngine(withLocality: locality) else { return false }
        e.stoppedHandler = { [weak self] _ in
            guard let self else { return }
            self.lock.lock()
            self.engine = nil
            self.lock.unlock()
        }
        e.resetHandler = { [weak self] in
            guard let self else { return }
            self.lock.lock()
            try? self.engine?.start()
            self.lock.unlock()
        }
        do { try e.start() } catch { return false }
        engine = e
        return true
    }

    func pulse(amplitude: Float, seconds: Float) {
        let v = min(max(amplitude, 0), 1)
        guard v > 0 else { return }
        lock.lock()
        defer { lock.unlock() }
        if engine == nil, !makeEngine() { return }
        guard let engine else { return }
        do {
            let ev = CHHapticEvent(eventType: .hapticContinuous,
                                   parameters: [CHHapticEventParameter(parameterID: .hapticIntensity, value: v),
                                                CHHapticEventParameter(parameterID: .hapticSharpness, value: 0.5)],
                                   relativeTime: 0, duration: TimeInterval(max(seconds, 0.02)))
            let pattern = try CHHapticPattern(events: [ev], parameters: [])
            try engine.makePlayer(with: pattern).start(atTime: CHHapticTimeImmediate)
        } catch {
            // Un pulso perdido no importa.
        }
    }
}
