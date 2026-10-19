import Foundation

/// Captura de volcados para desarrollo (vr/native/ns_dump.h): el usuario mantiene los DOS clics de stick durante ~1 s mientras juega y la app guarda en
/// `<carpeta del usuario>/Dumps/d<NNNN>_<fecha>_stage/` y `..._frame/` (dos volcados del mismo fotograma: punto de la captura nativa y final de fotograma) la RAM, la VRAM, los registros del coprocesador de geometría, la última lista de la GPU, el fotograma real (PNG)
/// y la pose de cámara/cabeza. Aviso háptico: una vibración corta al aceptar el gesto y otra larga cuando termina de escribirse (dos cortas si falló o
/// si pasan 10 s sin resultado).
/// `frame(both:)` se llama una vez por fotograma de render desde `VRInput.frame` y NO toca disco ni `FolderAccess`: la carpeta raíz se refresca desde el
/// hilo principal y la creación de directorios y el pedido al anfitrión van a una cola de fondo. Mientras los dos clics están pulsados `VRInput`
/// los oculta al juego (el gesto no debe cambiar el seguro/selector del arma).
final class DumpCapture: @unchecked Sendable {
    static let shared = DumpCapture()

    private let holdSeconds = 1.0
    private let timeoutSeconds = 10.0     // dos volcados seguidos (pareja)
    private let gapReset = 0.2             // sin llamadas durante más de esto: el contador de pulsación vuelve a cero
    private let io = DispatchQueue(label: "dump.io", qos: .utility)
    private let lock = NSLock()            // protege todo lo de abajo salvo `heldSince`, `armed`, `lastCall` (sólo hilo de render)

    private var heldSince: Double?
    private var lastCall: Double?
    private var armed = true               // se rearma al soltar
    private var latched = false            // los dos clics se ocultan hasta soltar ambos
    private var buzz: (until: Double, amp: Float)?

    // Estado compartido con la cola de fondo.
    private var rootURL: URL?
    private var rootRefreshAt = 0.0
    private var busy = false               // hay un pedido en marcha (preparando o esperando resultado)
    private var requestedAt = 0.0
    private var pendingBuzz: (amp: Float, dur: Double)?
    private var counter = 0
    private var counterReady = false

    private init() {}

    /// Los dos clics están (o estaban hasta soltar ambos) pulsados: `VRInput` debe ocultarlos al juego.
    var suppressesStickClicks: Bool { latched }

    /// `both` = los dos clics de stick pulsados (o L3+R3 en mando normal). Devuelve la amplitud háptica a aplicar ahora (0 = ninguna).
    func frame(both: Bool, any: Bool = false) -> Float {
        let now = CFAbsoluteTimeGetCurrent()
        if let l = lastCall, now - l > gapReset { heldSince = nil }   // M9: huecos largos entre llamadas no cuentan como pulsación continua
        lastCall = now
        refreshRoot(now)
        if both {
            latched = true
            if heldSince == nil { heldSince = now }
            if armed, let t = heldSince, now - t >= holdSeconds { startRequest(now) }
        } else {
            heldSince = nil
            if !any { latched = false; armed = true }   // `any` = alguno de los dos sigue pulsado
        }
        lock.lock()
        if busy && requestedAt > 0 && now - requestedAt > timeoutSeconds {
            busy = false
            requestedAt = 0
            lock.unlock()
            mv_vrhost_cancel_dump()
            Storage.log("volcado: sin resultado en 10 s")
            buzz = (now + 0.25, 0.8)
            lock.lock()
        }
        if busy && requestedAt > 0 {
            let r = mv_vrhost_dump_result()
            if r != 0 {
                busy = false; requestedAt = 0
                if r > 0 { buzz = (now + 0.7, 1.0) }
                else { buzz = (now + 0.25, 0.8) }
                if r < 0 { Storage.log("volcado: ERROR al escribir") }
            }
        }
        if let p = pendingBuzz { buzz = (now + p.dur, p.amp); pendingBuzz = nil }
        lock.unlock()
        if let b = buzz {
            if now < b.until { return b.amp }
            buzz = nil
        }
        return 0
    }

    /// Refresca la carpeta raíz desde el hilo principal (como mucho cada 2 s); el hilo de render nunca lee `FolderAccess`.
    private func refreshRoot(_ now: Double) {
        if now - rootRefreshAt < 2.0 && rootURL != nil { return }
        rootRefreshAt = now
        DispatchQueue.main.async { [self] in
            let base = FolderAccess.shared.root ?? Storage.documents
            let dumps = base.appendingPathComponent("Dumps", isDirectory: true)
            lock.lock(); rootURL = dumps; lock.unlock()
        }
    }

    private func startRequest(_ now: Double) {
        lock.lock()
        let root = rootURL
        if busy || root == nil { lock.unlock(); if root == nil { armed = false; buzz = (now + 0.25, 0.8) }; return }
        busy = true
        requestedAt = 0                       // aún preparando: el plazo de 5 s cuenta desde que el anfitrión acepta
        lock.unlock()
        armed = false
        buzz = (now + 0.12, 0.6)
        io.async { [self] in
            let fm = FileManager.default
            do { try fm.createDirectory(at: root!, withIntermediateDirectories: true) }
            catch {
                Storage.log("volcado: no se puede crear Dumps (\(error.localizedDescription))")
                fail(); return
            }
            if !counterReady {
                let names = (try? fm.contentsOfDirectory(atPath: root!.path)) ?? []
                let used = names.compactMap { n -> Int? in n.hasPrefix("d") ? Int(n.dropFirst().prefix(4)) : nil }
                lock.lock(); counter = (used.max() ?? 0) + 1; counterReady = true; lock.unlock()
            }
            let f = DateFormatter()
            f.dateFormat = "yyyyMMdd-HHmmss"
            lock.lock(); let idx = counter; counter += 1; lock.unlock()
            // Un gesto = DOS volcados del mismo fotograma de juego: el del punto de enganche (instante de la captura nativa, point=stage) y el del final del fotograma
            // (point=frame). Sin el enganche nativo el anfitrión hace uno solo (el segundo).
            let base = String(format: "d%04d_%@", idx, f.string(from: Date()))
            let dirStage = root!.appendingPathComponent(base + "_stage", isDirectory: true)
            let dirFrame = root!.appendingPathComponent(base + "_frame", isDirectory: true)
            if mv_vrhost_request_dump_pair(dirStage.path, dirFrame.path) == 0 {
                lock.lock(); requestedAt = CFAbsoluteTimeGetCurrent(); lock.unlock()
                Storage.log("volcado pedido (pareja stage+frame): \(base)")
            } else { fail() }
        }
    }

    private func fail() {
        lock.lock(); busy = false; requestedAt = 0; pendingBuzz = (0.8, 0.25); lock.unlock()
    }
}
