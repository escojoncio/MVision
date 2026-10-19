import Foundation

/// Captura de volcados para desarrollo (vr/native/ns_dump.h): el usuario mantiene los DOS clics de stick durante ~1 s mientras juega y la app guarda en
/// `<carpeta del usuario>/Dumps/d<NNNN>_<fecha>/` la RAM, la VRAM, los registros de la GTE, la última lista de la GPU, el fotograma real (PNG) y la pose
/// de cámara/cabeza. Aviso háptico: una vibración corta al aceptar el gesto y otra larga cuando termina de escribirse (o dos cortas si falló).
/// Se alimenta una vez por fotograma de render desde `VRInput.frame`. Sin carpeta de usuario escribe en Documents/Dumps.
final class DumpCapture: @unchecked Sendable {
    static let shared = DumpCapture()

    private let holdSeconds = 1.0
    private var heldSince: Double?
    private var armed = true          // se rearma al soltar
    private var waiting = false       // hay un volcado pedido sin resultado
    private var buzz: (until: Double, amp: Float)?
    private var counter = 0

    private init() {
        counter = Self.nextIndex()
    }

    private static var root: URL {
        let base = FolderAccess.shared.root ?? Storage.documents
        return base.appendingPathComponent("Dumps", isDirectory: true)
    }

    private static func nextIndex() -> Int {
        let names = (try? FileManager.default.contentsOfDirectory(atPath: root.path)) ?? []
        let used = names.compactMap { n -> Int? in
            guard n.hasPrefix("d") else { return nil }
            return Int(n.dropFirst().prefix(4))
        }
        return (used.max() ?? 0) + 1
    }

    /// `both` = los dos clics de stick pulsados (o L3+R3 en mando normal). Devuelve la amplitud háptica a aplicar ahora (0 = ninguna).
    func frame(both: Bool) -> Float {
        let now = CFAbsoluteTimeGetCurrent()
        if both {
            if heldSince == nil { heldSince = now }
            if armed && !waiting, let t = heldSince, now - t >= holdSeconds { request(now) }
        } else {
            heldSince = nil
            armed = true
        }
        if waiting {
            let r = mv_vrhost_dump_result()
            if r != 0 {
                waiting = false
                if r > 0 { buzz = (now + 0.7, 1.0) }
                else {
                    Storage.log("volcado: ERROR al escribir")
                    buzz = (now + 0.25, 0.8)
                }
            }
        }
        if let b = buzz {
            if now < b.until { return b.amp }
            buzz = nil
        }
        return 0
    }

    private func request(_ now: Double) {
        armed = false
        let fm = FileManager.default
        let root = Self.root
        do { try fm.createDirectory(at: root, withIntermediateDirectories: true) }
        catch { Storage.log("volcado: no se puede crear Dumps (\(error.localizedDescription))"); buzz = (now + 0.25, 0.8); return }
        let f = DateFormatter()
        f.dateFormat = "yyyyMMdd-HHmmss"
        let dir = root.appendingPathComponent(String(format: "d%04d_%@", counter, f.string(from: Date())), isDirectory: true)
        counter += 1
        if mv_vrhost_request_dump(dir.path) == 0 {
            waiting = true
            buzz = (now + 0.12, 0.6)
            Storage.log("volcado pedido: \(dir.lastPathComponent)")
        }
    }
}
