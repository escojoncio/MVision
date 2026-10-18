import Foundation
import UIKit

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

    /// Redirige stderr a un tubo que vacía LogTee: copia local (Documents/mvision.log) y, en cuanto hay acceso a la carpeta
    /// elegida, copia en <carpeta>/Logs (un fichero por sesión con fecha, se conservan los 5 últimos).
    private static var prepared = false
    private static let t0 = ProcessInfo.processInfo.systemUptime

    static func prepare() {
        if prepared { return }   // los reintentos no deben rotar el registro ni reabrir stderr
        prepared = true
        let fm = FileManager.default
        let prev = documents.appendingPathComponent("mvision.prev.log")
        try? fm.removeItem(at: prev)
        try? fm.moveItem(at: logURL, to: prev)
        LogTee.shared.start(primary: logURL)
        log("MVision \(Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "?") build \(buildSHA)")
        log("sistema: \(machineModel()) \(ProcessInfo.processInfo.operatingSystemVersionString), \(ProcessInfo.processInfo.physicalMemory >> 20) MB, \(ProcessInfo.processInfo.activeProcessorCount) núcleos")
        log("registro local: Documents/mvision.log (+ carpeta Logs de la carpeta elegida en cuanto haya acceso)")
        let nc = NotificationCenter.default
        // Vaciar a disco al perder el foco / pasar a segundo plano / terminar (el sistema puede matar la app sin más aviso).
        for n in [UIApplication.willResignActiveNotification, UIApplication.didEnterBackgroundNotification,
                  UIApplication.willTerminateNotification] {
            nc.addObserver(forName: n, object: nil, queue: nil) { note in
                log("ciclo de vida: \(note.name.rawValue)")
                LogTee.shared.flush(wait: true)
            }
        }
    }

    /// Versión de compilación: SHA del commit inyectado por CI en Info.plist (MVBuildSHA); "dev" si no se inyectó.
    static var buildSHA: String {
        let v = (Bundle.main.object(forInfoDictionaryKey: "MVBuildSHA") as? String ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
        return v.isEmpty || v.hasPrefix("$(") ? "dev" : String(v.prefix(12))
    }

    /// Identificador de modelo del hardware (p. ej. "RealityDevice14,1"); no es un dato personal.
    static func machineModel() -> String {
        var u = utsname()
        uname(&u)
        return withUnsafeBytes(of: &u.machine) { String(cString: $0.bindMemory(to: CChar.self).baseAddress!) }
    }

    /// Cuando hay acceso a la carpeta elegida se empieza a copiar el registro allí (sin bloquear: lo hace un hilo aparte).
    static func attachExternalLog() {
        guard let root = FolderAccess.shared.root else { return }
        LogTee.shared.attachExternal(dir: root.appendingPathComponent("Logs", isDirectory: true))
    }

    static func log(_ s: String) {
        fputs(String(format: "[app +%.1fs] ", ProcessInfo.processInfo.systemUptime - t0) + s + "\n", stderr)
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

/// Registro con dos destinos que nunca puede bloquear al motor: stderr (donde escriben el runtime en C y la app) es un tubo;
/// un hilo lo vacía a Documents/mvision.log (disco local) y encola lo mismo para un segundo hilo que lo copia a la carpeta
/// externa elegida por el usuario (puede ser lenta o quedarse esperando al proveedor de ficheros: solo se retrasa esa copia;
/// la cola se acota a 1 MB descartando lo más antiguo). Todo se vacía a disco (fsync) cada segundo con actividad y en
/// los avisos de ciclo de vida.
final class LogTee {
    static let shared = LogTee()
    private let lock = NSLock()
    private var queue = Data()                 // pendiente para la copia externa
    private var extDir: URL?                   // pedido de adjuntar (lo consume el hilo externo)
    private var extFD: Int32 = -1              // solo lo toca el hilo externo
    private var extAttached = false            // bajo lock: ya hay (o se pidió) copia externa
    private var extPath: String?               // bajo lock: carpeta Logs ya adjuntada (los reintentos no abren otro fichero)
    private var flushReq = false
    private let wake = DispatchSemaphore(value: 0)
    private let ack = DispatchSemaphore(value: 0)
    private var primaryFD: Int32 = -1
    private var started = false

    func start(primary: URL) {
        lock.lock(); defer { lock.unlock() }
        guard !started else { return }
        var p: [Int32] = [0, 0]
        guard pipe(&p) == 0 else { return }
        let out = open(primary.path, O_WRONLY | O_CREAT | O_TRUNC, 0o644)
        guard out >= 0 else { close(p[0]); close(p[1]); return }
        started = true
        primaryFD = out
        // El extremo de escritura NO bloquea: si el lector se atasca o muere y el tubo (64 KB) se llena, el emulador descarta
        // líneas en vez de quedarse parado en un fprintf. stdout (print de Swift) va al mismo tubo para no perderlo.
        _ = fcntl(p[1], F_SETFL, fcntl(p[1], F_GETFL) | O_NONBLOCK)
        dup2(p[1], STDERR_FILENO)
        dup2(p[1], STDOUT_FILENO)
        close(p[1])
        setvbuf(stderr, nil, _IOLBF, 0)
        setvbuf(stdout, nil, _IOLBF, 0)
        let rfd = p[0]
        let t = Thread { [unowned self] in self.readLoop(rfd) }
        t.name = "mv.log.pipe"; t.qualityOfService = .default; t.start()
        let e = Thread { [unowned self] in self.externalLoop() }
        e.name = "mv.log.ext"; e.qualityOfService = .utility; e.start()
    }

    private static func writeAll(_ fd: Int32, _ base: UnsafeRawPointer, _ n: Int) {
        var off = 0
        while off < n {
            let w = write(fd, base + off, n - off)
            if w < 0 { if errno == EINTR { continue }; return }
            off += w
        }
    }

    private func readLoop(_ rfd: Int32) {
        var buf = [UInt8](repeating: 0, count: 8192)
        var lastSync = ProcessInfo.processInfo.systemUptime
        while true {
            let n = read(rfd, &buf, buf.count)
            if n < 0 { if errno == EINTR { continue }; break }
            if n == 0 { break }
            buf.withUnsafeBytes { LogTee.writeAll(primaryFD, $0.baseAddress!, n) }
            lock.lock()
            queue.append(contentsOf: buf[0..<n])
            if queue.count > 1_048_576 { queue.removeFirst(queue.count - 524_288) }
            lock.unlock()
            let now = ProcessInfo.processInfo.systemUptime
            if now - lastSync >= 1.0 { fsync(primaryFD); lastSync = now; wake.signal() }
        }
    }

    /// Copia el registro a <dir> (se crea): fichero nuevo mvision_AAAAMMDD_HHMMSS.log, conservando los 5 últimos.
    func attachExternal(dir: URL) {
        lock.lock()
        if extPath == dir.path { lock.unlock(); return }   // reintento con la misma carpeta: se sigue con el mismo fichero
        extPath = dir.path; extDir = dir; extAttached = true
        lock.unlock()
        wake.signal()
    }

    /// Vacía a disco. Con wait espera (hasta 0,6 s) a que la copia externa también se haya escrito.
    func flush(wait: Bool) {
        if primaryFD >= 0 { fsync(primaryFD) }
        lock.lock(); flushReq = true; let hasExt = extAttached; lock.unlock()
        if wait && hasExt { while ack.wait(timeout: .now()) == .success {} }   // descarta acuses atrasados de esperas vencidas
        wake.signal()
        if wait && hasExt { _ = ack.wait(timeout: .now() + 0.6) }
    }

    private func externalLoop() {
        let fm = FileManager.default
        while true {
            _ = wake.wait(timeout: .now() + 1.0)
            lock.lock()
            let dir = extDir; extDir = nil
            let doFlush = flushReq; flushReq = false
            lock.unlock()
            if let dir = dir {
                if extFD >= 0 { close(extFD); extFD = -1 }
                extFD = LogTee.openSessionLog(in: dir, fm: fm)
                if extFD < 0 { Storage.log("no se pudo crear el registro en la carpeta elegida (Logs)") }
            }
            if extFD >= 0 {
                lock.lock(); let data = queue; queue.removeAll(keepingCapacity: true); lock.unlock()
                if !data.isEmpty { data.withUnsafeBytes { LogTee.writeAll(extFD, $0.baseAddress!, data.count) } }
                if !data.isEmpty || doFlush { fsync(extFD) }
            }
            if doFlush { ack.signal() }
        }
    }

    private static func openSessionLog(in dir: URL, fm: FileManager) -> Int32 {
        do { try fm.createDirectory(at: dir, withIntermediateDirectories: true) } catch { return -1 }
        // Rotación: se borran los más antiguos hasta dejar 4 (con el de esta sesión, 5).
        if let names = try? fm.contentsOfDirectory(atPath: dir.path) {
            let logs = names.filter { $0.hasPrefix("mvision_") && $0.hasSuffix(".log") }.sorted()
            if logs.count > 4 { for n in logs.prefix(logs.count - 4) { try? fm.removeItem(at: dir.appendingPathComponent(n)) } }
        }
        let f = DateFormatter()
        f.locale = Locale(identifier: "en_US_POSIX")
        f.dateFormat = "yyyyMMdd_HHmmss"
        let url = dir.appendingPathComponent("mvision_\(f.string(from: Date())).log")
        return open(url.path, O_WRONLY | O_CREAT | O_APPEND, 0o644)
    }
}
