import Foundation

/// Interruptores de diagnóstico del render inmersivo (submenú «Diagnóstico» del menú VR). Se aplican en vivo (se leen en cada fotograma), a las dos rutas de
/// dibujo (lista de la GPU y escena nativa) cuando tiene sentido, y se guardan en UserDefaults. El estado «normal» de todos es el de la app sin diagnóstico.
/// Seguro desde cualquier hilo.
final class XRDiag {
    static let shared = XRDiag()

    struct Settings: Equatable {
        /// Color de borrado magenta en vez de negro: si el hueco pasa a magenta, ahí no se dibuja nada; si sigue negro, algo pinta negro encima.
        var magentaClear = false
        /// Umbral (m, mundo VR) por encima del cual se subdividen en CPU los triángulos con una arista más larga. 0 = no partir.
        var splitMeters: Float = 0
        /// Ignorar el mapa de tasa de rasterización del drawable (la app no lo usa nunca: el interruptor solo deja constancia en el registro).
        var ignoreRateMap = false
        /// MSAA 1x en vez del valor de los ajustes (4x), sin resolve.
        var msaa1 = false
        /// Sin velo, viñeta, oscurecimiento por colisión de la cabeza, pantalla de cine, ejes, filtro de pantalla, radar ni arma: solo mundo, HUD 2D y textos.
        var noLayers = false
        /// Plano cercano fijado (m) con la proyección propia. 0 = el del sistema.
        var nearPlane: Float = 0
        /// 0 = automática (por defecto: la del sistema, con su rango de profundidad, si es válida; si no, la propia), 1 = la propia, 2 = la del sistema.
        var projMode = 0
        /// Sin prueba de profundidad en el mundo (solo diagnóstico).
        var noZTest = false
        /// Profundidad de relleno (por defecto ACTIVADA): tras el mundo, todo píxel que conserve la profundidad de borrado (0 en Z invertida = infinito) recibe una profundidad
        /// finita (50 m). El compositor reproyecta con NUESTRA profundidad: sin ella, los píxeles sin geometría opaca (agua y efectos semitransparentes no escriben profundidad) se
        /// tratan como vacíos y se pintan en negro por bloques. Apagarlo vuelve al comportamiento anterior.
        var fillDepth = true
        /// No entregar profundidad útil al sistema: el compositor recibe una profundidad CONSTANTE (50 m) en todos los píxeles (la API no permite quitar la textura de profundidad).
        var flatDepth = false

        var isNormal: Bool { self == Settings() }
    }

    static let splitChoices: [Float] = [0, 4, 2, 1]
    static let nearChoices: [Float] = [0, 0.05, 0.1, 0.3]

    private let lock = NSLock()
    private var cur: Settings

    private init() { cur = Self.load() }

    var settings: Settings { lock.lock(); defer { lock.unlock() }; return cur }

    func update(_ f: (inout Settings) -> Void) {
        lock.lock()
        var s = cur
        f(&s)
        cur = s
        lock.unlock()
        Self.save(s)
    }

    /// Todo vuelve a su estado normal.
    func reset() { update { $0 = Settings() } }

    // MARK: persistencia
    private static func load() -> Settings {
        let d = UserDefaults.standard
        func b(_ k: String) -> Bool { d.bool(forKey: "xr.diag." + k) }
        func f(_ k: String, _ list: [Float]) -> Float {
            guard d.object(forKey: "xr.diag." + k) != nil else { return 0 }
            let v = Float(d.double(forKey: "xr.diag." + k))
            return list.contains(v) ? v : 0
        }
        var s = Settings()
        s.magentaClear = b("magenta")
        s.splitMeters = f("split", splitChoices)
        s.ignoreRateMap = b("rateMap")
        s.msaa1 = b("msaa1")
        s.noLayers = b("noLayers")
        s.nearPlane = f("near", nearChoices)
        let pm = d.integer(forKey: "xr.diag.proj")
        s.projMode = (pm >= 0 && pm <= 2) ? pm : 0
        s.noZTest = b("noZ")
        s.fillDepth = d.object(forKey: "xr.diag.fillDepth") == nil ? true : b("fillDepth")
        s.flatDepth = b("flatDepth")
        return s
    }

    private static func save(_ s: Settings) {
        let d = UserDefaults.standard
        d.set(s.magentaClear, forKey: "xr.diag.magenta")
        d.set(Double(s.splitMeters), forKey: "xr.diag.split")
        d.set(s.ignoreRateMap, forKey: "xr.diag.rateMap")
        d.set(s.msaa1, forKey: "xr.diag.msaa1")
        d.set(s.noLayers, forKey: "xr.diag.noLayers")
        d.set(Double(s.nearPlane), forKey: "xr.diag.near")
        d.set(s.projMode, forKey: "xr.diag.proj")
        d.set(s.noZTest, forKey: "xr.diag.noZ")
        d.set(s.fillDepth, forKey: "xr.diag.fillDepth")
        d.set(s.flatDepth, forKey: "xr.diag.flatDepth")
    }

    // MARK: texto (menú y registro)
    static func metersText(_ v: Float) -> String {
        let t = v == v.rounded() ? String(format: "%.0f", v) : String(format: "%.2f", v)
        return t.replacingOccurrences(of: ".", with: ",") + " m"
    }

    /// Estado de los interruptores en una línea (registro 1 vez por segundo).
    static func summary(_ s: Settings) -> String {
        func yn(_ b: Bool) -> String { b ? "si" : "no" }
        return "magenta=\(yn(s.magentaClear)) partir=\(s.splitMeters > 0 ? metersText(s.splitMeters) : "no") sin-mapa-tasa=\(yn(s.ignoreRateMap)) msaa1x=\(yn(s.msaa1)) " +
            "sin-capas=\(yn(s.noLayers)) cercano=\(s.nearPlane > 0 ? metersText(s.nearPlane) : "sistema") proyeccion=\(["auto", "propia", "sistema"][min(max(s.projMode, 0), 2)]) sin-ztest=\(yn(s.noZTest)) prof-relleno=\(yn(s.fillDepth)) prof-constante=\(yn(s.flatDepth))" +
            (s.isNormal ? " (todo normal)" : "")
    }
}
