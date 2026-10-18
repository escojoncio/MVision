import Foundation
import GameController
import UIKit

/// Única fuente de verdad de los mandos y único punto que escribe en `mv_host_pad`.
///
/// - Detecta mando normal (DualSense, DualShock, extendido) y mandos de seguimiento (Sense) a la vez.
/// - Lee SIEMPRE por sondeo del estado completo (sin handlers): cada llamada a `sample()` devuelve el estado actual.
/// - Si hay los dos tipos, manda el que se haya usado más recientemente (por defecto, los Sense).
/// - `write(_:from:)` es el único camino hacia `mv_host_pad`: el espacio inmersivo (`.vr`) tiene prioridad y la
///   ventana (`.window`) solo escribe si no hay inmersivo o si este lleva >0,25 s sin fotogramas.
/// - Diagnóstico a stderr (va al log): mandos detectados con categoría/perfil/elementos, botones pulsados al
///   cambiar (con el nombre real de cada elemento), cambios de foco y bits enviados al juego.
final class ControllerHub: @unchecked Sendable {
    static let shared = ControllerHub()

    enum Source { case window, vr }
    enum Kind: Int { case none = 0, gamepad = 1, sense = 2 }

    /// Mando normal ya en los bits del mando del juego (activo-alto, bitacora §14); sticks -1..1 con Y arriba.
    struct PadSnapshot {
        var buttons: UInt16 = 0
        var lx: Float = 0, ly: Float = 0, rx: Float = 0, ry: Float = 0
        var active: Bool { buttons != 0 || abs(lx) > 0.5 || abs(ly) > 0.5 || abs(rx) > 0.5 || abs(ry) > 0.5 }
    }

    /// Un Sense (una mano). Sticks -1..1 con Y arriba; gatillo y grip 0..1.
    struct HandSnapshot {
        var present = false
        var stickX: Float = 0, stickY: Float = 0
        var trigger: Float = 0, grip: Float = 0
        var low = false, up = false, stickClick = false, menu = false
        var active: Bool {
            low || up || stickClick || menu || trigger > 0.5 || grip > 0.5 || abs(stickX) > 0.5 || abs(stickY) > 0.5
        }
    }

    struct Sample {
        var kind: Kind = .none
        var pad = PadSnapshot()
        var left = HandSnapshot()      // Sense izquierdo
        var right = HandSnapshot()     // Sense derecho
    }

    // Bits del mando del juego.
    private static let bit: (select: UInt16, l3: UInt16, r3: UInt16, start: UInt16, up: UInt16, right: UInt16,
                             down: UInt16, left: UInt16, l2: UInt16, r2: UInt16, l1: UInt16, r1: UInt16,
                             tri: UInt16, cir: UInt16, cro: UInt16, squ: UInt16) =
        (1 << 0, 1 << 1, 1 << 2, 1 << 3, 1 << 4, 1 << 5, 1 << 6, 1 << 7, 1 << 8, 1 << 9, 1 << 10, 1 << 11,
         1 << 12, 1 << 13, 1 << 14, 1 << 15)
    private static let bitNames = ["SELECT", "L3", "R3", "START", "ARRIBA", "DERECHA", "ABAJO", "IZQUIERDA",
                                   "L2", "R2", "L1", "R1", "TRIANGULO", "CIRCULO", "CRUZ", "CUADRADO"]

    private enum Role { case trigger, grip, low, up, stickClick, menu }

    /// Elementos de un Sense clasificados por función (referencias vivas del perfil físico).
    private struct HandMap {
        var trigger: [GCControllerButtonInput] = []
        var grip: [GCControllerButtonInput] = []
        var low: [GCControllerButtonInput] = []
        var up: [GCControllerButtonInput] = []
        var stickClick: [GCControllerButtonInput] = []
        var menu: [GCControllerButtonInput] = []
        var stick: GCControllerDirectionPad?
    }

    private let lock = NSLock()
    private var started = false
    private var observers: [NSObjectProtocol] = []
    private var gamepad: GCController?
    private var senseL: GCController?
    private var senseR: GCController?
    private var maps: [ObjectIdentifier: HandMap] = [:]
    private var lastPadActive: TimeInterval = 0
    private var lastSenseActive: TimeInterval = 0
    private var vrActive = false
    private var lastVRWrite: TimeInterval = 0
    private var lastWritten: UInt16 = 0xFFFF
    private var lastSig: [ObjectIdentifier: String] = [:]
    private var lastSigLog: [ObjectIdentifier: TimeInterval] = [:]
    private var lastDiag: [ObjectIdentifier: TimeInterval] = [:]
    private var logCount = 0
    private var eventCount = 0
    private var lastBeat: TimeInterval = 0
    private var lastEvent: TimeInterval = 0
    private var lastKind: Kind = .none
    private var focusChanges = 0          // cambios de GCController.current (solo se cuentan; el sistema los alterna entre los dos Sense)
    private var focusLogged = false
    private var appActive: Bool?
    private var vrWrites = 0, windowWrites = 0, samples = 0
    private var lastMuestra: TimeInterval = 0
    private var pollTimer: DispatchSourceTimer?
    private let pollQueue = DispatchQueue(label: "mv.hub.poll", qos: .userInteractive)
    /// Serializa TODA lectura/recorrido de perfiles de GCController (la documentación de GameController solo garantiza
    /// acceso desde cualquier hilo si no es simultáneo). Orden de bloqueo: `ioLock` primero, `lock` después; nunca al revés.
    private let ioLock = NSLock()
    private var lastSampleT: TimeInterval = 0

    /// Aviso (hilo principal) tras cada cambio de mandos conectados.
    var onChange: (() -> Void)?

    private init() {}

    // MARK: - Estado accesible

    var gamepadController: GCController? { lock.withLock { gamepad } }
    var senseLeft: GCController? { lock.withLock { senseL } }
    var senseRight: GCController? { lock.withLock { senseR } }
    var senseControllers: [GCController] { lock.withLock { [senseL, senseR].compactMap { $0 } } }

    // MARK: - Registro

    private var budgetWindow: TimeInterval = 0
    private var budgetUsed = 0

    /// Línea de registro. Tope total de 1500 y, además, un máximo de 25 líneas por segundo (un botón con ruido no
    /// puede agotar el tope y dejar sin registro lo que ocurra después).
    private func dlog(_ s: String) {
        let now = ProcessInfo.processInfo.systemUptime
        lock.lock()
        defer { lock.unlock() }
        guard logCount < 4000 else { return }
        if now - budgetWindow >= 1 { budgetWindow = now; budgetUsed = 0 }
        guard budgetUsed < 25 else { return }
        budgetUsed += 1
        logCount += 1
        fputs("[pad] \(s)\n", stderr)
    }

    // MARK: - Arranque y detección

    /// Empieza a vigilar mandos. Idempotente. Hilo principal.
    func start() {
        lock.lock()
        if started { lock.unlock(); return }
        started = true
        lock.unlock()
        // En visionOS los mandos solo envían eventos a la app con foco; con el espacio inmersivo abierto la ventana
        // pasa a segundo plano, así que se piden también los eventos de fondo (como hace el motor propio).
        GCController.shouldMonitorBackgroundEvents = true
        let nc = NotificationCenter.default
        observers.append(nc.addObserver(forName: .GCControllerDidConnect, object: nil, queue: .main) { [weak self] n in
            self?.dlog("conectado: \((n.object as? GCController)?.vendorName ?? "?")")
            self?.choose()
        })
        observers.append(nc.addObserver(forName: .GCControllerDidDisconnect, object: nil, queue: .main) { [weak self] n in
            self?.dlog("desconectado: \((n.object as? GCController)?.vendorName ?? "?")")
            self?.choose()
        })
        // Foco: si los eventos no llegan, estas líneas dicen si la app estaba activa y qué mando era el actual.
        // Con mandos espaciales el sistema alterna `current` entre los dos continuamente: no tiene ningún efecto en la
        // lectura y NO se registra cada vez (inundaba el registro y agotaba su tope por segundo); se cuenta y se
        // informa en el latido, y solo la primera vez deja una línea.
        let onFocus: (Notification) -> Void = { [weak self] n in
            guard let self else { return }
            self.lock.lock()
            self.focusChanges += 1
            let first = !self.focusLogged
            self.focusLogged = true
            self.lock.unlock()
            if first { self.dlog("foco: primer cambio de mando actual (\((n.object as? GCController)?.vendorName ?? "?")); el resto se cuenta en el latido") }
        }
        observers.append(nc.addObserver(forName: .GCControllerDidBecomeCurrent, object: nil, queue: nil, using: onFocus))
        observers.append(nc.addObserver(forName: .GCControllerDidStopBeingCurrent, object: nil, queue: nil) { [weak self] _ in
            guard let self else { return }
            self.lock.lock(); self.focusChanges += 1; self.lock.unlock()
        })
        observers.append(nc.addObserver(forName: UIApplication.didBecomeActiveNotification, object: nil, queue: .main) { [weak self] _ in
            guard let self else { return }
            self.lock.lock(); self.appActive = true; self.lock.unlock()
            self.dlog("app activa")
        })
        observers.append(nc.addObserver(forName: UIApplication.willResignActiveNotification, object: nil, queue: .main) { [weak self] _ in
            guard let self else { return }
            self.lock.lock(); self.appActive = false; self.lock.unlock()
            self.dlog("app pierde actividad")
        })
        let active = UIApplication.shared.applicationState == .active
        lock.lock(); appActive = active; lock.unlock()
        dlog("inicio (shouldMonitorBackgroundEvents=\(GCController.shouldMonitorBackgroundEvents))")
        GCController.startWirelessControllerDiscovery {}
        choose()
        startPolling()
    }

    /// Sondeo propio del hub (50 Hz): mantiene vivos diagnóstico, muestras y latido aunque ni la ventana ni el bucle
    /// inmersivo estén llamando a `sample()` en ese momento. Es de solo lectura: no toca `lastWritten` ni ningún estado
    /// de `write()`; se salta el ciclo si otro hilo acaba de leer o está leyendo (no compite con la lectura del juego).
    private func startPolling() {
        let t = DispatchSource.makeTimerSource(queue: pollQueue)
        t.schedule(deadline: .now() + .milliseconds(100), repeating: .milliseconds(20), leeway: .milliseconds(2))
        t.setEventHandler { [weak self] in self?.pollOnce() }
        pollTimer = t
        t.resume()
    }

    private func pollOnce() {
        let now = ProcessInfo.processInfo.systemUptime
        lock.lock()
        let recent = now - lastSampleT < 0.015
        lock.unlock()
        guard !recent, ioLock.try() else { return }
        defer { ioLock.unlock() }
        _ = sampleLocked()
    }

    /// Categoría de producto de un Sense (valor de GCProductCategorySpatialController, escrito a mano).
    private static let spatialCategory = "Spatial Controller"

    static func isSense(_ c: GCController) -> Bool {
        if c.productCategory == spatialCategory { return true }
        let cat = c.productCategory.lowercased()
        let n = (c.vendorName ?? "").lowercased()
        // "dualsense" contiene "sense": solo "vr2" o "spatial" identifican a un Sense.
        return cat.contains("spatial") || n.contains("vr2")
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
        let chosenPad = pads.first(where: { $0.extendedGamepad is GCDualSenseGamepad })
            ?? pads.first(where: { $0.extendedGamepad is GCDualShockGamepad })
            ?? pads.first

        // Los botones ligados a gestos del sistema (PS, Create, panel táctil) pasan a ser del juego.
        ioLock.lock()
        for c in all {
            for (_, b) in c.physicalInputProfile.buttons where b.isBoundToSystemGesture {
                b.preferredSystemGestureState = .disabled
            }
        }
        var newMaps: [ObjectIdentifier: HandMap] = [:]
        for c in [left, right].compactMap({ $0 }) { newMaps[ObjectIdentifier(c)] = Self.buildMap(c) }
        ioLock.unlock()

        lock.lock()
        gamepad = chosenPad
        senseL = left
        senseR = right
        maps = newMaps
        lastSig = [:]
        lock.unlock()

        if all.isEmpty { dlog("sin mandos conectados") }
        ioLock.lock()
        for c in all { describe(c, isLeft: c === left, isRight: c === right, chosenPad: c === chosenPad) }
        ioLock.unlock()
        onChange?()
    }

    /// Registra todo lo que se sabe de un mando para poder corregir nombres sin otra build.
    private func describe(_ c: GCController, isLeft: Bool, isRight: Bool, chosenPad: Bool) {
        let role = isLeft ? "Sense izquierdo" : isRight ? "Sense derecho" : chosenPad ? "mando elegido" : "ignorado"
        let eg = c.extendedGamepad.map { String(describing: type(of: $0)) } ?? "nil"
        let profile = c.physicalInputProfile
        dlog("mando [\(role)] nombre=\(c.vendorName ?? "?") categoria=\(c.productCategory) extendedGamepad=\(eg) perfil=\(String(describing: type(of: profile)))")
        func names<T: GCControllerElement>(_ d: [String: T]) -> String {
            var seen = Set<ObjectIdentifier>()
            var out: [String] = []
            for (k, e) in d where seen.insert(ObjectIdentifier(e)).inserted {
                out.append(e.aliases.union([k]).sorted().joined(separator: "|"))
            }
            return out.sorted().joined(separator: ", ")
        }
        dlog("  botones: \(names(profile.buttons))")
        dlog("  dpads: \(names(profile.dpads))")
        dlog("  ejes: \(names(profile.axes))")
        if let h = c.haptics { dlog("  hapticos: \(h.supportedLocalities.map { $0.rawValue }.sorted().joined(separator: ", "))") }
        if isLeft || isRight {
            let m = Self.buildMap(c)
            func n(_ a: [GCControllerButtonInput]) -> Int { a.count }
            dlog("  clasificacion: gatillo=\(n(m.trigger)) grip=\(n(m.grip)) inferior=\(n(m.low)) superior=\(n(m.up)) clicStick=\(n(m.stickClick)) menu=\(n(m.menu)) stick=\(m.stick != nil)")
        }
    }

    // MARK: - Clasificación de elementos de un Sense

    private static func role(of name: String) -> Role? {
        let k = name.lowercased()
        // Sensores capacitivos (dedo apoyado): no son pulsaciones.
        if k.contains("touch") || k.contains("proximity") || k.contains("capacitive") { return nil }
        if k.contains("stick") {
            return (k.contains("button") || k.contains("click") || k.contains("press")) ? .stickClick : nil
        }
        if k.contains("trigger") { return .trigger }
        if k.contains("grip") || k.contains("shoulder") || k.contains("squeeze") { return .grip }
        if k.contains("menu") || k.contains("options") || k.contains("share") || k.contains("create") { return .menu }
        let f = k.hasPrefix("button ") ? String(k.dropFirst(7)) : k
        switch f {
        case "a", "x": return .low      // cara inferior: A (derecho) / X (izquierdo)
        case "b", "y": return .up       // cara superior: B (derecho) / Y (izquierdo)
        default: return nil
        }
    }

    private static func buildMap(_ c: GCController) -> HandMap {
        let profile = c.physicalInputProfile
        var m = HandMap()
        var seen = Set<ObjectIdentifier>()
        for (key, b) in profile.buttons where seen.insert(ObjectIdentifier(b)).inserted {
            let names = b.aliases.union([key]).sorted()
            guard let r = names.lazy.compactMap({ role(of: $0) }).first else { continue }
            switch r {
            case .trigger: m.trigger.append(b)
            case .grip: m.grip.append(b)
            case .low: m.low.append(b)
            case .up: m.up.append(b)
            case .stickClick: m.stickClick.append(b)
            case .menu: m.menu.append(b)
            }
        }
        for (key, d) in profile.dpads {
            if d.aliases.union([key]).contains(where: { $0.lowercased().contains("stick") }) { m.stick = d; break }
        }
        if m.stick == nil, profile.dpads.count == 1 { m.stick = profile.dpads.first?.value }
        return m
    }

    // MARK: - Lectura (sondeo)

    private static func on(_ b: GCControllerButtonInput?) -> Bool {
        guard let b else { return false }
        return b.isPressed || b.value > 0.5
    }

    private func readPad(_ c: GCController?) -> PadSnapshot? {
        guard let g = c?.extendedGamepad else { return nil }
        let k = Self.bit
        var b: UInt16 = 0
        func set(_ bit: UInt16, _ v: Bool) { if v { b |= bit } }
        set(k.select, Self.on(g.buttonOptions))
        if let ds = g as? GCDualSenseGamepad { set(k.select, Self.on(ds.touchpadButton)) }
        else if let d4 = g as? GCDualShockGamepad { set(k.select, Self.on(d4.touchpadButton)) }
        set(k.l3, Self.on(g.leftThumbstickButton))
        set(k.r3, Self.on(g.rightThumbstickButton))
        set(k.start, Self.on(g.buttonMenu))
        set(k.up, Self.on(g.dpad.up)); set(k.right, Self.on(g.dpad.right))
        set(k.down, Self.on(g.dpad.down)); set(k.left, Self.on(g.dpad.left))
        set(k.l2, Self.on(g.leftTrigger)); set(k.r2, Self.on(g.rightTrigger))
        set(k.l1, Self.on(g.leftShoulder)); set(k.r1, Self.on(g.rightShoulder))
        set(k.tri, Self.on(g.buttonY)); set(k.cir, Self.on(g.buttonB))
        set(k.cro, Self.on(g.buttonA)); set(k.squ, Self.on(g.buttonX))
        return PadSnapshot(buttons: b,
                           lx: g.leftThumbstick.xAxis.value, ly: g.leftThumbstick.yAxis.value,
                           rx: g.rightThumbstick.xAxis.value, ry: g.rightThumbstick.yAxis.value)
    }

    /// Lectura por `GCController.input` (GCControllerLiveInput, visionOS 1+/iOS 17+) con los alias oficiales de los
    /// mandos espaciales. DESACTIVADA por defecto: se activa con la clave `mv.hub.liveInput` (UserDefaults, Bool) y, aunque
    /// esté activa, si el perfil no entrega ningún elemento se cae a la lectura clásica. Siempre se registra en `muestra`.
    private static let useLiveInput = UserDefaults.standard.bool(forKey: "mv.hub.liveInput")

    private static func readHandLive(_ c: GCController) -> HandSnapshot? {
        let input = c.input
        func button(_ name: String) -> (any GCButtonElement)? { input[name] as? (any GCButtonElement) }
        let a = button(GCInputButtonA), b = button(GCInputButtonB)
        let grip = button(GCInputGripButton), trig = button(GCInputTrigger)
        let menu = button(GCInputButtonMenu), click = button(GCInputThumbstick)
        let pad = input[GCInputThumbstick] as? (any GCDirectionPadElement)
        if a == nil && b == nil && grip == nil && trig == nil && menu == nil && pad == nil { return nil }
        func pressed(_ e: (any GCButtonElement)?) -> Bool { e?.pressedInput.isPressed ?? false }
        func analog(_ e: (any GCButtonElement)?) -> Float {
            guard let p = e?.pressedInput else { return 0 }
            return p.isPressed ? max(p.value, 0.6) : p.value
        }
        var h = HandSnapshot()
        h.present = true
        h.trigger = analog(trig)
        h.grip = analog(grip)
        h.low = pressed(a)
        h.up = pressed(b)
        h.menu = pressed(menu)
        h.stickClick = pressed(click)
        if let pad {
            h.stickX = (pad.xAxis as? any GCLinearInput)?.value ?? 0
            h.stickY = (pad.yAxis as? any GCLinearInput)?.value ?? 0
        }
        return h
    }

    private func readHand(_ c: GCController?) -> HandSnapshot {
        var h = HandSnapshot()
        guard let c else { return h }
        if Self.useLiveInput, let live = Self.readHandLive(c) { return live }
        lock.lock()
        let m = maps[ObjectIdentifier(c)]
        lock.unlock()
        guard let m else { return h }
        func strongest(_ a: [GCControllerButtonInput]) -> Float {
            a.reduce(Float(0)) { max($0, $1.isPressed ? max($1.value, 0.6) : $1.value) }
        }
        h.present = true
        h.trigger = strongest(m.trigger)
        h.grip = strongest(m.grip)
        h.low = m.low.contains { $0.isPressed }
        h.up = m.up.contains { $0.isPressed }
        h.stickClick = m.stickClick.contains { $0.isPressed }
        h.menu = m.menu.contains { $0.isPressed }
        if let s = m.stick { h.stickX = s.xAxis.value; h.stickY = s.yAxis.value }
        return h
    }

    /// Estado actual de todo. Barato; se puede llamar cada fotograma desde cualquier hilo (las lecturas se serializan).
    func sample() -> Sample {
        ioLock.lock()
        defer { ioLock.unlock() }
        return sampleLocked()
    }

    /// Cuerpo de `sample()`; exige `ioLock` tomado. No modifica nada de lo que usa `write()`.
    private func sampleLocked() -> Sample {
        let now = ProcessInfo.processInfo.systemUptime
        lock.lock()
        let gp = gamepad, l = senseL, r = senseR
        lock.unlock()
        var s = Sample()
        if let p = readPad(gp) { s.pad = p }
        s.left = readHand(l)
        s.right = readHand(r)
        let hasPad = gp?.extendedGamepad != nil
        let hasSense = l != nil || r != nil
        let padActive = hasPad && s.pad.active
        let senseActive = s.left.active || s.right.active

        lock.lock()
        samples += 1
        lastSampleT = now
        if padActive { lastPadActive = now }
        if senseActive { lastSenseActive = now }
        if !hasPad && !hasSense { s.kind = .none }
        else if !hasSense { s.kind = .gamepad }
        else if !hasPad { s.kind = .sense }
        else { s.kind = lastPadActive > lastSenseActive ? .gamepad : .sense }
        let kindChanged = s.kind != lastKind
        lastKind = s.kind
        lock.unlock()
        if kindChanged { dlog("fuente de entrada: \(s.kind == .gamepad ? "mando normal" : s.kind == .sense ? "Sense" : "ninguna")") }

        diagnose(gp, "mando", now)
        diagnose(l, "Sense I", now)
        diagnose(r, "Sense D", now)
        muestra(l, r, now)
        heartbeat(now, s)
        return s
    }

    // MARK: - Diagnóstico

    private func diagnose(_ c: GCController?, _ label: String, _ now: TimeInterval) {
        guard let c else { return }
        let id = ObjectIdentifier(c)
        // El diagnóstico recorre todo el perfil: a lo sumo cada 20 ms por mando (la lectura del juego no depende de esto).
        lock.lock()
        let due = now - (lastDiag[id] ?? 0) >= 0.02
        if due { lastDiag[id] = now }
        lock.unlock()
        guard due else { return }
        let profile = c.physicalInputProfile
        var seen = Set<ObjectIdentifier>()
        var parts: [String] = []
        for (key, b) in profile.buttons where (b.isPressed || b.value > 0.5) && seen.insert(ObjectIdentifier(b)).inserted {
            parts.append(b.aliases.union([key]).min() ?? key)
        }
        func q(_ v: Float) -> Int { v > 0.5 ? 1 : (v < -0.5 ? -1 : 0) }
        var dseen = Set<ObjectIdentifier>()
        for (key, d) in profile.dpads where dseen.insert(ObjectIdentifier(d)).inserted {
            let x = q(d.xAxis.value), y = q(d.yAxis.value)
            if x != 0 || y != 0 { parts.append("\(d.aliases.union([key]).min() ?? key)(\(x),\(y))") }
        }
        parts.sort()
        let sig = parts.joined(separator: ", ")
        lock.lock()
        let prev = lastSig[id] ?? ""
        let lastT = lastSigLog[id] ?? 0
        let changed = sig != prev && now - lastT >= 0.03    // limita el ritmo; el cambio pendiente se registra después
        if changed {
            lastSig[id] = sig
            lastSigLog[id] = now
            eventCount += 1
            lastEvent = now
        }
        lock.unlock()
        if changed { dlog("entrada \(label): [\(sig)]") }
    }

    /// Valores crudos de los Sense, 1 vez por segundo, para saber si el sistema entrega datos aunque nada llegue al juego.
    private func muestra(_ l: GCController?, _ r: GCController?, _ now: TimeInterval) {
        lock.lock()
        // Reserva de tope: las muestras dejan de salir al llegar a 3000 líneas para no agotar el registro de eventos.
        let due = now - lastMuestra >= 1 && logCount < 3000
        if due { lastMuestra = now }
        let act = appActive
        lock.unlock()
        guard due else { return }
        func f(_ v: Float) -> String { String(format: "%.2f", v) }
        func line(_ label: String, _ c: GCController?) {
            guard let c else { dlog("muestra \(label): sin mando"); return }
            let p = c.physicalInputProfile
            func btn(_ n: String) -> GCControllerButtonInput? { p.buttons[n] }
            let st = p.dpads["Thumbstick"]
            let cur = GCController.current === c ? 1 : 0
            let a: String = act.map { $0 ? "1" : "0" } ?? "?"
            let gat: Float = btn("Trigger")?.value ?? -1
            let grip: Float = btn("Grip")?.value ?? -1
            let bA: Float = btn("Button A")?.value ?? -1
            let bB: Float = btn("Button B")?.value ?? -1
            let menu: Float = btn("Button Menu")?.value ?? -1
            let clic: Float = btn("Thumbstick Button")?.value ?? -1
            let sx: Float = st?.xAxis.value ?? -9
            let sy: Float = st?.yAxis.value ?? -9
            let bg = GCController.shouldMonitorBackgroundEvents ? 1 : 0
            var out = "muestra \(label): gat=\(f(gat)) grip=\(f(grip)) A=\(f(bA)) B=\(f(bB)) menu=\(f(menu)) clic=\(f(clic))"
            out += " stick=(\(f(sx)),\(f(sy))) actual=\(cur) appActiva=\(a) fondo=\(bg)"
            if let v = Self.readHandLive(c) {
                out += " | input: gat=\(f(v.trigger)) grip=\(f(v.grip)) A=\(v.low ? 1 : 0) B=\(v.up ? 1 : 0) menu=\(v.menu ? 1 : 0) clic=\(v.stickClick ? 1 : 0) stick=(\(f(v.stickX)),\(f(v.stickY)))"
            } else {
                out += " | input: sin datos"
            }
            dlog(out)
        }
        line("Sense I", l)
        line("Sense D", r)
    }

    private func heartbeat(_ now: TimeInterval, _ s: Sample) {
        lock.lock()
        if lastBeat == 0 { lastBeat = now }
        let due = now - lastBeat >= 10
        let n = eventCount, since = lastEvent == 0 ? -1 : now - lastEvent
        let fc = focusChanges, vw = vrWrites, ww = windowWrites, sm = samples
        if due { lastBeat = now; eventCount = 0; focusChanges = 0; vrWrites = 0; windowWrites = 0; samples = 0 }
        lock.unlock()
        guard due else { return }
        let ago = since < 0 ? "nunca" : String(format: "hace %.1f s", since)
        dlog("latido: fuente=\(s.kind) cambios en 10 s=\(n) ultimo=\(ago) cambiosFoco=\(fc) lecturas=\(sm) escriturasVR=\(vw) escriturasVentana=\(ww) mandoBits=\(String(format: "0x%04X", s.pad.buttons))")
    }

    // MARK: - Escritura hacia el juego

    /// El espacio inmersivo entra/sale: mientras está activo y emite fotogramas, la ventana no escribe.
    func setVRActive(_ on: Bool) {
        lock.lock()
        vrActive = on
        if !on { lastVRWrite = 0 }
        lock.unlock()
        dlog("entrada VR \(on ? "activa" : "inactiva")")
    }

    /// ¿Debe escribir ahora la ventana? (sin inmersivo, o inmersivo sin fotogramas desde hace >0,25 s)
    func windowMayWrite() -> Bool {
        let now = ProcessInfo.processInfo.systemUptime
        lock.lock()
        defer { lock.unlock() }
        return !vrActive || now - lastVRWrite > 0.25
    }

    func write(buttons: UInt16, lx: UInt8, ly: UInt8, rx: UInt8, ry: UInt8, from: Source) {
        let now = ProcessInfo.processInfo.systemUptime
        lock.lock()
        switch from {
        case .vr: lastVRWrite = now; vrWrites += 1
        case .window:
            if vrActive && now - lastVRWrite <= 0.25 { lock.unlock(); return }
            windowWrites += 1
        }
        let changed = buttons != lastWritten
        lastWritten = buttons
        lock.unlock()
        mv_host_pad(buttons, lx, ly, rx, ry)
        if changed {
            var names: [String] = []
            for (i, n) in Self.bitNames.enumerated() where buttons & (UInt16(1) << UInt16(i)) != 0 { names.append(n) }
            dlog("al juego (\(from == .vr ? "VR" : "ventana")): \(String(format: "0x%04X", buttons)) [\(names.joined(separator: " "))]")
        }
    }

    func releaseAll(from: Source) {
        write(buttons: 0, lx: 0x80, ly: 0x80, rx: 0x80, ry: 0x80, from: from)
    }

    // MARK: - Mando plano (ventana y respaldo)

    private static func axis(_ v: Float, invert: Bool) -> UInt8 {
        var x = abs(v) < 0.08 ? 0 : max(-1, min(1, v))
        if invert { x = -x }
        return UInt8(max(0, min(255, Int((x + 1) * 127.5 + 0.5))))
    }

    /// Estado -> bytes de `mv_host_pad` sin locomoción VR (mando normal tal cual; Sense con el mapa por defecto).
    func plainPad(_ s: Sample) -> (UInt16, UInt8, UInt8, UInt8, UInt8) {
        let k = Self.bit
        var b: UInt16 = 0
        var lx: Float = 0, ly: Float = 0, rx: Float = 0, ry: Float = 0
        switch s.kind {
        case .none:
            break
        case .gamepad:
            b = s.pad.buttons
            lx = s.pad.lx; ly = s.pad.ly; rx = s.pad.rx; ry = s.pad.ry
        case .sense:
            func set(_ bit: UInt16, _ v: Bool) { if v { b |= bit } }
            set(k.squ, s.right.trigger > 0.55); set(k.cir, s.left.trigger > 0.55)
            set(k.r1, s.right.grip > 0.55); set(k.l1, s.left.grip > 0.55)
            set(k.cro, s.right.low); set(k.tri, s.right.up)
            set(k.l2, s.left.low); set(k.r2, s.left.up)
            set(k.r3, s.right.stickClick); set(k.l3, s.left.stickClick)
            set(k.start, s.right.menu); set(k.select, s.left.menu)
            lx = s.left.stickX; ly = s.left.stickY; rx = s.right.stickX; ry = s.right.stickY
            set(k.up, ly > 0.55); set(k.down, ly < -0.55); set(k.right, lx > 0.55); set(k.left, lx < -0.55)
        }
        return (b, Self.axis(lx, invert: false), Self.axis(ly, invert: true),
                Self.axis(rx, invert: false), Self.axis(ry, invert: true))
    }
}
