import SwiftUI

enum GamePhase: Equatable {
    case loading
    case running
    case noDisc
    case failed(String)
}

@MainActor
final class GameModel: ObservableObject {
    @Published var phase: GamePhase = .loading

    private var hostStarted = false
    private var monitor: Timer?
    let audio = AudioOut()
    let input = InputManager.shared

    /// Busca disco y ejecutable y arranca el motor. Se puede reintentar mientras no haya arrancado.
    func start() {
        guard !hostStarted else { return }
        phase = .loading
        Storage.prepare()
        #if MV_HOST_EXTRAS
        MVExtras.start()
        #endif

        let discs = Storage.findDiscs()
        if discs.isEmpty {
            Storage.log("sin disco en \(Storage.documents.path)")
            phase = .noDisc
            return
        }
        Storage.log("discos: \(discs.joined(separator: " | "))")

        // El ejecutable sale del propio disco (el paquete no lleva datos del juego).
        guard let exe = Storage.extractExecutable(from: discs[0]) else {
            Storage.log("no se pudo extraer el ejecutable del disco")
            phase = .failed("No se pudo leer el ejecutable del disco. Comprueba que la imagen o la carpeta está completa.")
            return
        }
        guard let sigs = Storage.sigsPath else {
            Storage.log("falta sigs_hle.json en el paquete")
            phase = .failed("Esta compilación no incluye el fichero de firmas. Vuelve a generar la app.")
            return
        }
        mv_host_set_sigs(sigs)
        mv_vrhost_install()   // capa VR (cámara, escenas): antes de arrancar el motor

        let r = mv_host_start(exe.path, discs.joined(separator: ";"), Storage.cardDir.path)
        if r != 0 {
            let msg = String(cString: mv_host_error())
            Storage.log("mv_host_start = \(r): \(msg)")
            phase = r == -3 ? .noDisc : .failed(msg.isEmpty ? "No se pudo iniciar el motor." : msg)
            if r != -3 { hostStarted = true }   // el runtime no admite un segundo arranque tras un fallo tardío
            return
        }
        hostStarted = true
        audio.start()
        input.start()
        phase = .running
        monitor = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.poll() }
        }
    }

    private func poll() {
        if mv_host_state() == MV_HOST_ERROR {
            monitor?.invalidate()
            let msg = String(cString: mv_host_error())
            Storage.log("error del motor: \(msg)")
            audio.stop()
            phase = .failed(msg.isEmpty ? "El motor se detuvo." : msg)
        }
    }

    func scenePhaseChanged(_ p: ScenePhase) {
        guard hostStarted, phase == .running else { return }
        if p == .active {
            mv_host_resume()
            audio.resume()
        } else {
            mv_host_pause()
            audio.pause()
            input.releaseAll()
        }
    }
}
