import SwiftUI

enum GamePhase: Equatable {
    case loading
    case running
    case needFolder(String?)   // hay que elegir la carpeta del juego (con aviso opcional)
    case noDisc
    case failed(String)
}

@MainActor
final class GameModel: ObservableObject {
    @Published var phase: GamePhase = .loading
    @Published var showPicker = false
    @Published var aviso: String?

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

        // Carpeta del juego (fuera del contenedor): el acceso queda abierto durante toda la sesión.
        if FolderAccess.shared.root == nil {
            switch FolderAccess.shared.restore() {
            case .ok: break
            case .missing:
                if !Storage.devDocuments { phase = .needFolder(nil); return }
            case .lost:
                phase = .needFolder("Se perdió el permiso de acceso a la carpeta. Vuelve a elegirla.")
                return
            case .wrongName:
                phase = .needFolder(Self.nombreIncorrecto)
                return
            }
        }
        Storage.attachExternalLog()   // copia del registro en <carpeta elegida>/Logs (hilo aparte, no bloquea)
        Storage.resolveCardDir()

        let discs = Storage.findDiscs()
        if discs.isEmpty {
            Storage.log("sin disco en la carpeta elegida")
            phase = .noDisc
            return
        }
        Storage.log("discos: \(discs.count) (\(discs.map { ($0 as NSString).pathExtension.isEmpty ? "carpeta" : ($0 as NSString).pathExtension.lowercased() }.joined(separator: ", ")))")

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

        let r = mv_host_start(exe.path, discs.joined(separator: ";"), Storage.cardDir.path)   // el hilo de emulación lee del disco con el acceso de seguridad aún abierto
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

    static let nombreIncorrecto = "La carpeta debe tener el nombre indicado en las instrucciones."

    /// Resultado del selector de carpetas del sistema.
    func folderPicked(_ r: Result<[URL], Error>) {
        switch r {
        case .failure(let e):
            Storage.log("selector de carpeta: \(e.localizedDescription)")
            phase = .needFolder("No se pudo abrir la carpeta elegida. Inténtalo de nuevo.")
        case .success(let urls):
            guard let url = urls.first else { return }
            switch FolderAccess.shared.adopt(url) {
            case .ok: start()
            case .wrongName: phase = .needFolder(Self.nombreIncorrecto)
            default: phase = .needFolder("No se pudo acceder a la carpeta elegida. Inténtalo de nuevo.")
            }
        }
    }

    /// Botón "Cambiar carpeta". El motor no admite reiniciarse: con la partida en marcha solo se olvida la carpeta
    /// (se pedirá al volver a abrir la app) y el acceso actual sigue abierto hasta salir.
    func changeFolder() {
        FolderAccess.shared.forget()
        if hostStarted {
            aviso = "La carpeta se pedirá de nuevo la próxima vez que abras la app. Cierra la app para cambiarla ahora."
        } else {
            FolderAccess.shared.release()
            phase = .needFolder(nil)
        }
    }

    /// Cierra el acceso a la carpeta (al terminar la app).
    func shutdown() { FolderAccess.shared.release() }

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
            audio.pause()
            mv_host_pause()
            input.releaseAll()
        }
    }
}
