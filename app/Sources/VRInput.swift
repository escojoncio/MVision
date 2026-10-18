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
    var allowProne = true
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
        if has("prone") { s.allowProne = d.bool(forKey: prefix + "prone") }
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
        d.set(allowProne, forKey: Self.prefix + "prone")
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

    /// Categoría de producto de GameController de un Sense (valor de GCProductCategorySpatialController, escrito a mano).
    private static let spatialCategory = "Spatial Controller"

    private let lock = NSLock()
    private var senseLeft: GCController?
    private var senseRight: GCController?
    private var pad: GCController?
    private var observers: [NSObjectProtocol] = []
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
        c.crouch_drop_m = settings.crouchDrop
        c.prone_drop_m = settings.proneDrop
        vr_input_set_cfg(&state, &c)
        if applyHeight, settings.headHeight > 0 { vr_input_set_height(&state, settings.headHeight) }
    }

    // MARK: - Conexión

    /// Empieza a vigilar mandos. Hilo principal.
    func start() {
        guard !started else { return }
        started = true
        let nc = NotificationCenter.default
        observers.append(nc.addObserver(forName: .GCControllerDidConnect, object: nil, queue: .main) { [weak self] _ in
            self?.choose()
        })
        observers.append(nc.addObserver(forName: .GCControllerDidDisconnect, object: nil, queue: .main) { [weak self] _ in
            self?.choose()
        })
        GCController.startWirelessControllerDiscovery {}
        choose()
    }

    private static func isSense(_ c: GCController) -> Bool {
        if c.productCategory == spatialCategory { return true }
        // Respaldo si la cadena de categoría cambia: los Sense se anuncian como "...VR2 Sense Controller...".
        let n = (c.vendorName ?? "").lowercased()
        return n.contains("vr2")   // ojo: "dualsense" contiene "sense"; no usar solo eso
    }

    private static func isLeftSense(_ c: GCController) -> Bool {
        let n = (c.vendorName ?? "").lowercased()
        return n.contains("left") || n.contains("(l)") || n.hasSuffix(" l")
    }

    private func choose() {
        let all = GCController.controllers()
        let senses = all.filter(Self.isSense)
        var left: GCController?
        var right: GCController?
        for s in senses {
            if Self.isLeftSense(s) { if left == nil { left = s } else if right == nil { right = s } }
            else { if right == nil { right = s } else if left == nil { left = s } }
        }
        let pads = all.filter { $0.extendedGamepad != nil && !Self.isSense($0) }
        let chosenPad = pads.first(where: { $0.extendedGamepad is GCDualSenseGamepad }) ?? pads.first
        lock.lock()
        senseLeft = left
        senseRight = right
        pad = (left == nil && right == nil) ? chosenPad : nil
        pulsers = [:]
        lock.unlock()
        for s in senses {
            for (_, b) in s.physicalInputProfile.buttons where b.isBoundToSystemGesture {
                b.preferredSystemGestureState = .disabled
            }
        }
        if let p = chosenPad {
            for (_, b) in p.physicalInputProfile.buttons where b.isBoundToSystemGesture {
                b.preferredSystemGestureState = .disabled
            }
        }
        VRInputPose.shared.track([left, right].compactMap { $0 })
        Storage.log("control VR: \(senses.isEmpty ? (chosenPad?.vendorName ?? "sin mando") : "Sense x\(senses.count)")")
        if senses.isEmpty && chosenPad == nil { releaseAll() }
    }

    func releaseAll() {
        mv_host_pad(0, 0x80, 0x80, 0x80, 0x80)
    }

    // MARK: - Lectura

    private static func button(_ c: GCController?, _ names: [String]) -> (pressed: Bool, value: Float) {
        guard let profile = c?.physicalInputProfile else { return (false, 0) }
        var best: Float = 0
        var pressed = false
        for n in names {
            if let b = profile.buttons[n] {
                best = max(best, b.value)
                pressed = pressed || b.isPressed
            }
        }
        return (pressed, best)
    }

    private static func stick(_ c: GCController?, _ names: [String]) -> (Float, Float) {
        guard let profile = c?.physicalInputProfile else { return (0, 0) }
        for n in names {
            if let d = profile.dpads[n] { return (d.xAxis.value, d.yAxis.value) }
        }
        return (0, 0)
    }

    /// Un Sense -> mano. Nombres tomados del motor propio: cada mando tiene "Thumbstick", cara baja/alta, etc.
    private static func senseHand(_ c: GCController?, left: Bool, pose: VRInputPose.Pose) -> vri_hand_t {
        var h = vri_hand_t()
        guard let c else { return h }
        let a = GCInputButtonA, b = GCInputButtonB, x = GCInputButtonX, y = GCInputButtonY
        let (sx, sy) = stick(c, ["Thumbstick", left ? GCInputLeftThumbstick : GCInputRightThumbstick])
        h.stick_x = sx
        h.stick_y = sy
        let trig = button(c, [left ? GCInputLeftTrigger : GCInputRightTrigger, "Trigger", left ? "Left Trigger" : "Right Trigger"])
        let grip = button(c, ["Grip Button", left ? GCInputLeftShoulder : GCInputRightShoulder, left ? "Left Shoulder" : "Right Shoulder"])
        h.trigger = trig.pressed ? max(trig.value, 0.6) : trig.value
        h.grip = grip.pressed ? max(grip.value, 0.6) : grip.value
        var bits: UInt32 = 0
        if button(c, left ? [x, a] : [a, x]).pressed { bits |= UInt32(VRI_H_LOW) }
        if button(c, left ? [y, b] : [b, y]).pressed { bits |= UInt32(VRI_H_UP) }
        if button(c, ["Thumbstick Button", left ? GCInputLeftThumbstickButton : GCInputRightThumbstickButton]).pressed {
            bits |= UInt32(VRI_H_STICK)
        }
        let menu = left ? [GCInputButtonShare, GCInputButtonMenu, GCInputButtonOptions]
                        : [GCInputButtonOptions, GCInputButtonMenu]
        if button(c, menu).pressed { bits |= UInt32(VRI_H_MENU) }
        h.buttons = bits
        h.tracked = pose.tracked
        h.pos = (pose.position.x, pose.position.y, pose.position.z)
        h.quat = (pose.rotation.x, pose.rotation.y, pose.rotation.z, pose.rotation.w)
        return h
    }

    /// DualSense / extendido -> bits PS1 como en Input.swift. Sticks con Y arriba (C invierte).
    private static func fill(pad g: GCExtendedGamepad, _ f: inout vr_input_frame_t) {
        var b: UInt16 = 0
        func set(_ bit: Int, _ on: Bool) { if on { b |= UInt16(1) << UInt16(bit) } }
        set(0, g.buttonOptions?.isPressed ?? false)
        if let ds = g as? GCDualSenseGamepad { set(0, ds.touchpadButton.isPressed) }
        set(1, g.leftThumbstickButton?.isPressed ?? false)
        set(2, g.rightThumbstickButton?.isPressed ?? false)
        set(3, g.buttonMenu.isPressed)
        set(4, g.dpad.up.isPressed); set(5, g.dpad.right.isPressed)
        set(6, g.dpad.down.isPressed); set(7, g.dpad.left.isPressed)
        set(8, g.leftTrigger.isPressed); set(9, g.rightTrigger.isPressed)
        set(10, g.leftShoulder.isPressed); set(11, g.rightShoulder.isPressed)
        set(12, g.buttonY.isPressed); set(13, g.buttonB.isPressed)
        set(14, g.buttonA.isPressed); set(15, g.buttonX.isPressed)
        f.pad_buttons = b
        f.pad_lx = g.leftThumbstick.xAxis.value
        f.pad_ly = g.leftThumbstick.yAxis.value
        f.pad_rx = g.rightThumbstick.xAxis.value
        f.pad_ry = g.rightThumbstick.yAxis.value
    }

    /// Un fotograma: lee, procesa y envía al juego. `head` = transformación cabeza->origen (deviceAnchor) o nil.
    /// `gamePosture` del puente RAM (0 de pie, 1 agachado, 2 tumbado, -1 desconocida). Devuelve el pad enviado.
    @discardableResult
    func frame(head: simd_float4x4?, dt: Float, gamePosture: Int32 = -1, menu: Bool = false) -> vr_pad_t {
        lock.lock()
        let left = senseLeft, right = senseRight, gamepad = pad
        lock.unlock()

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
        if left != nil || right != nil {
            mode = VRI_MODE_SENSE
            let hl = Self.senseHand(left, left: true, pose: VRInputPose.shared.pose(hand: 0))
            let hr = Self.senseHand(right, left: false, pose: VRInputPose.shared.pose(hand: 1))
            f.hand = (hl, hr)
        } else if let g = gamepad?.extendedGamepad {
            Self.fill(pad: g, &f)
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

        mv_host_pad(out.buttons, out.lx, out.ly, out.rx, out.ry)
        if turnDeg != 0 { mv_vr_turn(-turnDeg) }
        forwardRumble()
        return out
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
        if senseLeft != nil || senseRight != nil {
            if let c = side == 0 ? senseLeft : senseRight { targets.append((c, .default, "\(ObjectIdentifier(c).hashValue)-d")) }
        } else if let c = pad, let h = c.haptics {
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
