import GameController

/// Mando extendido estándar y DualSense -> bits del mando PS1 (activo-alto).
/// SELECT 0, L3 1, R3 2, START 3, ARRIBA 4, DERECHA 5, ABAJO 6, IZQUIERDA 7, L2 8, R2 9, L1 10, R1 11,
/// TRIANGULO 12, CIRCULO 13, CRUZ 14, CUADRADO 15.
final class InputManager {
    static let shared = InputManager()
    private var observers: [NSObjectProtocol] = []
    private var current: GCExtendedGamepad?
    private let lock = NSLock()
    private var suspended = false

    /// Con el espacio inmersivo abierto manda VRInput (escribe en mv_host_pad): este gestor calla y suelta todo.
    /// Al cerrarlo se reanuda con el estado actual del mando.
    func setSuspended(_ on: Bool) {
        lock.lock()
        suspended = on
        let g = current
        lock.unlock()
        if on { releaseAll() } else if let g { push(g) }
    }

    func start() {
        let nc = NotificationCenter.default
        observers.append(nc.addObserver(forName: .GCControllerDidConnect, object: nil, queue: .main) { [weak self] n in
            if let c = n.object as? GCController { self?.attach(c) }
        })
        observers.append(nc.addObserver(forName: .GCControllerDidDisconnect, object: nil, queue: .main) { [weak self] _ in
            self?.releaseAll()
        })
        for c in GCController.controllers() { attach(c) }
    }

    private func attach(_ c: GCController) {
        Storage.log("mando: \(c.vendorName ?? "?") (\(c.productCategory))")
        guard let gp = c.extendedGamepad else { return }
        lock.lock(); current = gp; lock.unlock()
        gp.valueChangedHandler = { [weak self] pad, _ in self?.push(pad) }
        push(gp)
    }

    func releaseAll() {
        mv_host_pad(0, 0x80, 0x80, 0x80, 0x80)
    }

    private static func axis(_ v: Float, invert: Bool) -> UInt8 {
        var x = abs(v) < 0.08 ? 0 : max(-1, min(1, v))
        if invert { x = -x }
        return UInt8(max(0, min(255, Int((x + 1) * 127.5 + 0.5))))
    }

    private func push(_ g: GCExtendedGamepad) {
        lock.lock(); let off = suspended; lock.unlock()
        if off { return }
        var b: UInt16 = 0
        func set(_ bit: Int, _ on: Bool) { if on { b |= UInt16(1) << UInt16(bit) } }
        set(0, g.buttonOptions?.isPressed ?? false)
        if let ds = g as? GCDualSenseGamepad { set(0, ds.touchpadButton.isPressed) }
        set(1, g.leftThumbstickButton?.isPressed ?? false)
        set(2, g.rightThumbstickButton?.isPressed ?? false)
        set(3, g.buttonMenu.isPressed)
        set(4, g.dpad.up.isPressed)
        set(5, g.dpad.right.isPressed)
        set(6, g.dpad.down.isPressed)
        set(7, g.dpad.left.isPressed)
        set(8, g.leftTrigger.isPressed)
        set(9, g.rightTrigger.isPressed)
        set(10, g.leftShoulder.isPressed)
        set(11, g.rightShoulder.isPressed)
        set(12, g.buttonY.isPressed)   // triángulo
        set(13, g.buttonB.isPressed)   // círculo
        set(14, g.buttonA.isPressed)   // cruz
        set(15, g.buttonX.isPressed)   // cuadrado
        mv_host_pad(b,
                    Self.axis(g.leftThumbstick.xAxis.value, invert: false),
                    Self.axis(g.leftThumbstick.yAxis.value, invert: true),
                    Self.axis(g.rightThumbstick.xAxis.value, invert: false),
                    Self.axis(g.rightThumbstick.yAxis.value, invert: true))
    }
}
