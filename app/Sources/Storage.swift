import Foundation

/// Carpetas de la app, detección de discos y registro a fichero.
enum Storage {
    /// Documents de la app (visible en Archivos gracias a UIFileSharingEnabled).
    static var documents: URL {
        FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
    }

    /// Tarjeta de memoria (saves): Documents/Saves.
    static var cardDir: URL { documents.appendingPathComponent("Saves", isDirectory: true) }
    static var logURL: URL { documents.appendingPathComponent("mvision.log") }

    /// Crea las carpetas (así aparecen en Archivos desde el primer arranque) y redirige stderr al registro.
    private static var prepared = false

    static func prepare() {
        if prepared { return }   // los reintentos no deben rotar el registro ni reabrir stderr
        prepared = true
        let fm = FileManager.default
        try? fm.createDirectory(at: cardDir, withIntermediateDirectories: true)
        let prev = documents.appendingPathComponent("mvision.prev.log")
        try? fm.removeItem(at: prev)
        try? fm.moveItem(at: logURL, to: prev)
        if freopen(logURL.path, "w", stderr) != nil {
            setvbuf(stderr, nil, _IOLBF, 0)
        }
        log("MVision \(Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "?")")
    }

    static func log(_ s: String) {
        fputs("[app] \(s)\n", stderr)
    }

    /// Discos en Documents y en cualquiera de sus subcarpetas (hasta 2 niveles): imágenes .cue/.bin y carpetas extraídas
    /// con SYSTEM.CNF.
    /// La detección es por contenido (runtime/disc_img): si un disco está como imagen y como carpeta gana la imagen,
    /// y el orden es el de disco (código de serie ascendente). Devuelve las rutas en orden de disco.
    static func findDiscs() -> [String] {
        var buf = [CChar](repeating: 0, count: 8192)
        let n = mv_host_scan_discs(documents.path, &buf, buf.count)
        if n <= 0 { return [] }
        return String(cString: buf).split(separator: ";").map(String.init)
    }

    /// Fichero de firmas del SDK (sin código del juego): recurso del paquete.
    static var sigsPath: String? { Bundle.main.path(forResource: "sigs_hle", ofType: "json") }

    /// Ejecutable del juego: se extrae del propio disco (el paquete no lo lleva) a Caches; si ya hay un
    /// main.exe en Documents se usa ese (útil para pruebas). nil si no se pudo.
    static func extractExecutable(from disc: String) -> URL? {
        let manual = documents.appendingPathComponent("main.exe")
        if FileManager.default.fileExists(atPath: manual.path) { return manual }
        let caches = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0]
        try? FileManager.default.createDirectory(at: caches, withIntermediateDirectories: true)
        let out = caches.appendingPathComponent("boot.exe")
        let tmp = caches.appendingPathComponent("boot.exe.tmp")
        try? FileManager.default.removeItem(at: tmp)
        // Se extrae a un temporal y solo se sustituye el anterior si difiere: no se pierde un boot.exe bueno si falla la
        // extracción y no se reescribe el definitivo en cada arranque.
        guard mv_host_extract_exe(disc, tmp.path) == 0 else {
            try? FileManager.default.removeItem(at: tmp)
            return nil
        }
        if FileManager.default.contentsEqual(atPath: tmp.path, andPath: out.path) {
            try? FileManager.default.removeItem(at: tmp)
            return out
        }
        try? FileManager.default.removeItem(at: out)
        do { try FileManager.default.moveItem(at: tmp, to: out) } catch { return nil }
        return out
    }
}
