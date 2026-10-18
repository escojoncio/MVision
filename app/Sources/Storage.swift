import Foundation

/// Carpetas de la app, detección de discos y registro a fichero.
enum Storage {
    /// Documents de la app (visible en Archivos gracias a UIFileSharingEnabled).
    static var documents: URL {
        FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
    }

    /// Ajuste oculto de desarrollo (desactivado por defecto): además de la carpeta elegida, busca discos en Documents.
    /// Se alterna con una pulsación larga sobre el título de la pantalla de elegir carpeta.
    static var devDocuments: Bool {
        get { UserDefaults.standard.bool(forKey: "mv.devDocuments") }
        set { UserDefaults.standard.set(newValue, forKey: "mv.devDocuments") }
    }

    /// Tarjeta de memoria (saves): "Saves" dentro de la carpeta elegida; si no se puede escribir, Documents/Saves.
    /// Se fija en `resolveCardDir()` antes de arrancar el motor.
    private(set) static var cardDir: URL = documents.appendingPathComponent("Saves", isDirectory: true)
    static var logURL: URL { documents.appendingPathComponent("mvision.log") }

    /// Redirige stderr al registro (en Documents, que sí es visible en Archivos).
    private static var prepared = false

    static func prepare() {
        if prepared { return }   // los reintentos no deben rotar el registro ni reabrir stderr
        prepared = true
        let fm = FileManager.default
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

    /// Elige dónde van las partidas: Saves dentro de la carpeta externa (junto al juego) o, si no se puede escribir,
    /// Documents/Saves. Requiere el acceso de seguridad abierto.
    static func resolveCardDir() {
        let fm = FileManager.default
        let fallback = documents.appendingPathComponent("Saves", isDirectory: true)
        if let root = FolderAccess.shared.root {
            let dir = root.appendingPathComponent("Saves", isDirectory: true)
            let probe = dir.appendingPathComponent(".w")
            do {
                try fm.createDirectory(at: dir, withIntermediateDirectories: true)
                try Data("1".utf8).write(to: probe)
                try? fm.removeItem(at: probe)
                cardDir = dir
                log("partidas en la carpeta elegida (Saves)")
                return
            } catch {
                log("no se puede escribir en la carpeta elegida (\(error.localizedDescription)): partidas en Documents/Saves")
            }
        }
        try? fm.createDirectory(at: fallback, withIntermediateDirectories: true)
        cardDir = fallback
    }

    /// Discos en la carpeta elegida (con el acceso de seguridad abierto) y en cualquiera de sus subcarpetas (hasta
    /// 2 niveles): imágenes .cue/.bin y carpetas extraídas con SYSTEM.CNF. Con el ajuste oculto `devDocuments`, si ahí
    /// no hay nada se busca también en Documents.
    /// La detección es por contenido (runtime/disc_img): si un disco está como imagen y como carpeta gana la imagen,
    /// y el orden es el de disco (código de serie ascendente). Devuelve las rutas en orden de disco.
    static func findDiscs() -> [String] {
        var dirs: [String] = []
        if let r = FolderAccess.shared.root { dirs.append(r.path) }
        if devDocuments { dirs.append(documents.path) }
        for d in dirs {
            var buf = [CChar](repeating: 0, count: 8192)
            let n = mv_host_scan_discs(d, &buf, buf.count)
            if n > 0 { return String(cString: buf).split(separator: ";").map(String.init) }
        }
        return []
    }

    /// Fichero de firmas del SDK (sin código del juego): recurso del paquete.
    static var sigsPath: String? { Bundle.main.path(forResource: "sigs_hle", ofType: "json") }

    /// Ejecutable del juego: se extrae del propio disco (el paquete no lo lleva) a Caches; con el ajuste de desarrollo,
    /// si ya hay un main.exe en Documents se usa ese (útil para pruebas). nil si no se pudo.
    static func extractExecutable(from disc: String) -> URL? {
        let manual = documents.appendingPathComponent("main.exe")
        if devDocuments, FileManager.default.fileExists(atPath: manual.path) { return manual }
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
