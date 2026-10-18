import Foundation

/// Mando de la ventana (fuera del espacio inmersivo y como respaldo de este). Ya no lee ni detecta nada por su cuenta:
/// sondea `ControllerHub` a ~120 Hz y escribe el estado completo en el juego a través del propio hub, que es el único
/// punto que llama a `mv_host_pad`. Mientras el espacio inmersivo emite fotogramas manda `VRInput` (el hub descarta
/// las escrituras de la ventana); si el inmersivo lleva >0,25 s sin fotogramas, vuelve a escribir esta capa.
///
/// Bits del mando del juego, activo-alto: SELECT 0, L3 1, R3 2, START 3, ARRIBA 4, DERECHA 5, ABAJO 6, IZQUIERDA 7, L2 8, R2 9,
/// L1 10, R1 11, TRIANGULO 12, CIRCULO 13, CRUZ 14, CUADRADO 15.
final class InputManager {
    static let shared = InputManager()
    private let hub = ControllerHub.shared
    private let queue = DispatchQueue(label: "mv.input", qos: .userInteractive)
    private var timer: DispatchSourceTimer?

    /// El espacio inmersivo entra (true) o sale (false): decide quién escribe en el juego.
    func setSuspended(_ on: Bool) {
        hub.setVRActive(on)
    }

    func start() {
        hub.start()
        guard timer == nil else { return }
        let t = DispatchSource.makeTimerSource(queue: queue)
        t.schedule(deadline: .now(), repeating: .milliseconds(8), leeway: .milliseconds(1))
        t.setEventHandler { [weak self] in self?.tick() }
        timer = t
        t.resume()
    }

    private func tick() {
        guard hub.windowMayWrite() else { return }
        let s = hub.sample()
        let (b, lx, ly, rx, ry) = hub.plainPad(s)
        hub.write(buttons: b, lx: lx, ly: ly, rx: rx, ry: ry, from: .window)
    }

    func releaseAll() {
        hub.releaseAll(from: .window)
    }
}
