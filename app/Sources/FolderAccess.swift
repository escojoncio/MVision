import Foundation
import CryptoKit

/// Acceso a la carpeta del juego, elegida por el usuario FUERA del contenedor de la app
/// (Archivos > En mi Apple Vision Pro > <carpeta>).
///
/// - La carpeta se elige con el selector del sistema (`fileImporter`, `.folder`). La URL devuelta lleva ámbito de
///   seguridad: hay que llamar a `startAccessingSecurityScopedResource()` y mantenerlo abierto mientras se lee o escribe.
/// - Se guarda un bookmark persistente en UserDefaults. En iOS/visionOS NO existe `.withSecurityScope` (es de macOS):
///   los bookmarks de URLs elegidas por el usuario ya son de ámbito de seguridad de forma implícita, así que se crean
///   con `options: []` y al resolverlos se vuelve a llamar a `startAccessingSecurityScopedResource()`.
/// - El nombre NO está en claro en el binario: `MVFolderKey` (Info.plist, de `$(MV_FOLDER_KEY)`) guarda solo el SHA-256.
///   Formato: hex en minúsculas (64 caracteres) del SHA-256 de los bytes UTF-8 del nombre, normalizado así:
///   sin espacios/saltos al principio y al final, en minúsculas y en forma Unicode NFC.
///   Si la clave está vacía o no es un hash válido, no se exige nombre (cualquier carpeta vale; se registra en el log).
///
/// Solo se usa desde el hilo principal. El acceso de seguridad es del proceso (no de un hilo), así que el hilo de
/// emulación puede leer los ficheros con `fopen` mientras `root` siga abierto.
final class FolderAccess {
    static let shared = FolderAccess()

    enum Result {
        case ok
        case missing     // no hay carpeta guardada
        case lost        // el bookmark ya no se puede resolver o la carpeta no es accesible
        case wrongName   // el nombre no coincide con el exigido
    }

    private let bookmarkKey = "mv.folderBookmark"
    private(set) var root: URL?
    private var accessing = false

    // MARK: Nombre

    /// Hash exigido (64 hex en minúsculas) o nil si no se exige nombre.
    static var requiredHash: String? {
        let raw = (Bundle.main.object(forInfoDictionaryKey: "MVFolderKey") as? String) ?? ""
        let k = raw.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
        let hex = CharacterSet(charactersIn: "0123456789abcdef")
        guard k.count == 64, k.unicodeScalars.allSatisfy({ hex.contains($0) }) else { return nil }
        return k
    }

    /// SHA-256 (hex minúsculas) del nombre normalizado. Mismo cálculo que debe hacerse para generar el secreto.
    static func hash(of name: String) -> String {
        let norm = name.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
            .precomposedStringWithCanonicalMapping
        return SHA256.hash(data: Data(norm.utf8)).map { String(format: "%02x", $0) }.joined()
    }

    static func nameIsValid(_ url: URL) -> Bool {
        guard let want = requiredHash else { return true }
        return hash(of: url.lastPathComponent) == want
    }

    // MARK: Ciclo de vida

    /// Recupera la carpeta guardada (arranque) y abre el acceso.
    func restore() -> Result {
        if root != nil { return .ok }
        guard let data = UserDefaults.standard.data(forKey: bookmarkKey) else { return .missing }
        var stale = false
        let url: URL
        do {
            url = try URL(resolvingBookmarkData: data, options: [], relativeTo: nil, bookmarkDataIsStale: &stale)
        } catch {
            Storage.log("bookmark de carpeta no resoluble: \(error.localizedDescription)")
            forget()
            return .lost
        }
        guard Self.nameIsValid(url) else {
            Storage.log("la carpeta guardada no tiene el nombre exigido")
            forget()
            return .wrongName
        }
        guard open(url) else { forget(); return .lost }
        if stale {
            // Con el acceso ya abierto se regenera el bookmark.
            Storage.log("bookmark obsoleto: se regenera")
            save(url)
        }
        return .ok
    }

    /// Adopta una carpeta recién elegida en el selector (la URL ya viene con ámbito de seguridad).
    func adopt(_ url: URL) -> Result {
        guard Self.nameIsValid(url) else {
            Storage.log("carpeta rechazada: el nombre no coincide")
            return .wrongName
        }
        release()
        guard open(url) else { return .lost }
        save(url)
        return .ok
    }

    /// Abre el acceso de seguridad y comprueba que el directorio se puede listar.
    private func open(_ url: URL) -> Bool {
        let started = url.startAccessingSecurityScopedResource()
        // Si devuelve false (URL que no necesita ámbito) se intenta igualmente: lo que manda es poder listar.
        guard (try? FileManager.default.contentsOfDirectory(atPath: url.path)) != nil else {
            if started { url.stopAccessingSecurityScopedResource() }
            Storage.log("carpeta no accesible (acceso de seguridad: \(started))")
            return false
        }
        root = url
        accessing = started
        if Self.requiredHash == nil { Storage.log("sin MVFolderKey: se acepta cualquier nombre de carpeta") }
        return true
    }

    private func save(_ url: URL) {
        do {
            // options: [] (en iOS/visionOS no existe .withSecurityScope; .minimalBookmark también valdría).
            let data = try url.bookmarkData(options: [], includingResourceValuesForKeys: nil, relativeTo: nil)
            UserDefaults.standard.set(data, forKey: bookmarkKey)
        } catch {
            Storage.log("no se pudo crear el bookmark: \(error.localizedDescription)")
        }
    }

    /// Cierra el acceso (al salir de la app o al cambiar de carpeta). No toca el bookmark.
    func release() {
        if accessing, let r = root { r.stopAccessingSecurityScopedResource() }
        accessing = false
        root = nil
    }

    /// Olvida la carpeta guardada (la próxima vez se pedirá de nuevo). No cierra el acceso abierto.
    func forget() {
        UserDefaults.standard.removeObject(forKey: bookmarkKey)
    }
}
