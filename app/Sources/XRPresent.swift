import Foundation
import Metal
import simd
import CoreGraphics
#if !MV_BENCH
import UIKit
#endif

// Presentación en VR de todo lo que NO es el juego en primera persona (ver bitácora, "Presentación en VR").
//  - Pantalla de cine: el fotograma completo que dibuja el juego (título, menús, radio, cargas, vídeos, cinemáticas del motor, ventanas de
//    armas/objetos, pausa, fin de partida) en una pantalla virtual grande, anclada delante del usuario, en un entorno negro.
//  - Cinemáticas del motor (con 3D en la escena y la cámara del guion; vídeos pregrabados, menús y radio siguen en la pantalla de cine):
//      * Pantalla 3D (defecto): la escena se dibuja en estéreo con la cámara del juego (cámaras paralelas separadas un IPD, frustos desplazados, campo de visión
//        igual al que la pantalla ocupa desde el usuario: escala real) en una pantalla grande y curva fija en el mundo. Mover la cabeza NO cambia el encuadre ni la
//        perspectiva, solo hacia dónde de la pantalla se mira. Los cortes de plano los hace el juego (sin fundido). Subtítulos/créditos/bandas van en el panel 2D encima.
//      * Dentro de la escena (opción): la cabeza en la cámara del plano, anclada al empezar el plano (fundido de 0,15 s en cada corte).
//      * Diorama (opción): la escena 3D en miniatura delante del usuario.
//  - 2D del juego en primera persona (HUD, radar del juego, menús L2/R2, pausa, textos): la capa 2D COMPLETA en UN solo panel plano delante de la cabeza (paso 6b). El troceo antiguo (paneles de regiones del HUD, ventana
//    del menú en la mano, radar aparte) queda desactivado tras XRLayer2D.split (apagado por defecto).
//  - Menú de opciones VR: se abre manteniendo el botón de menú izquierdo (o Select del mando) y se maneja con el mando.
// Todo en el hilo de render. Sin llamadas al compositor: solo compone `XRDrawItem` para XRFinalPass.

/// Ajustes de presentación (UserDefaults; se leen al crear el presentador y tras cada cambio del menú).
struct XRViewConfig {
    var width: Float = 3.0          // ancho de la pantalla de cine (m); alto = 3/4
    var distance: Float = 2.5       // distancia de la pantalla (m)
    var curve: Float = 0.35         // 0 plana .. 1 cilindro con el usuario en el eje
    var sharp = 1                   // 0 suave, 1 nítida (bilineal de píxel fijo), 2 píxel (vecino más próximo)
    var smooth2D = 0                // suavizado de la capa 2D (HUD, menús, créditos, pantalla de cine): 0 original, 1 nítido (píxel art), 2 suave
    var integerScale = false        // ajusta el ancho para que cada texel ocupe un número entero de píxeles del visor
    var filter2D = 0                // filtro de píxel art del 2D (XR2DFilter): 0 MMPX, 1 xBR, 2 nítido, 3 sin filtro; se aplica al reabrir la vista (XR2DFilter.reload)
    var exact2D = true              // composición de la capa 2D sobre el mundo como la GPU original (XR2DCompose.framebufferFetch)
    var cutscene: Cutscene = .stereo // cinemáticas del motor: pantalla de cine 3D estéreo con profundidad real (defecto), la misma en mono (sin profundidad), pantalla de cine plana, diorama o dentro de la escena
    var dioramaScale: Float = 0.15  // metros por metro del motor (0,15: una sala de 8 m cabe en ~1,2 m)
    var hudPanels = true
    var hudScale: Float = 0.8
    var wristHud = true             // vida, objeto y arma/munición del HUD sobre el antebrazo izquierdo, junto al radar
    var wristScale: Float = 0.2     // ancho (m) que ocuparía el panel 2D completo en la muñeca (el recorte de la vida mide ~35 % de él)
    var wristHandSide = false       // false: los paneles se apilan hacia el codo (encima del radar); true: hacia la mano
    var hideRealHands = true        // pide al sistema ocultar tus manos y brazos reales mientras dura el espacio inmersivo (se ven los del personaje)
    var debugAxes = false           // dibuja los ejes de los mandos (dispositivo, agarre, apuntado) y de las manos del personaje
    var aiRange = 0                 // IA de enemigos: alcance de visión, índice de aiRanges (0 = x1, 1 = x1,5, 2 = x2)
    var aiHead = true               // IA de enemigos: ven también la cabeza VR (no solo el ojo del personaje)
    var newHands = true             // manos del personaje en primera persona: nuevas (articuladas, con puño propio) u originales del motor
    // Diagnóstico de la pantalla 3D con la cámara del motor: las tres reglas de descarte por triángulo del render nativo (ns_gpu_engine_tri), cada una apagable por separado. Encendidas de serie.
    var engBehind = true            // «Descarte por detrás (3D)»: triángulo con los tres vértices detrás de la cámara del motor
    var engSize = true              // «Tope de tamaño (3D)»: triángulo lejano de más de 1023 x 511 px de la pantalla del motor
    var engBack = true              // «Reversos (3D)»: reverso de las caras de una sola cara
    var engSplit = true             // «Caras cercanas (3D)»: true = «Original» (las caras grandes y cercanas de los objetos que el motor parte se ven por trozos, como en el motor original); false = «Recortadas» (como el juego con el parche sin-subdivisión: desaparecen enteras)
    var follow2D = true             // pantallas 2D (cine, pantalla 3D, ventanas del juego, menú VR): siguen la cabeza en cada fotograma (true) o quedan fijas en el mundo (false)
    var smoothMotion = true         // fluidez de la locomoción: el mundo se corrige por fotograma de render (giro del stick y avance extrapolados entre escenas del juego)

    enum Cutscene: String, CaseIterable { case screen, stereo, cinema, diorama, scene }
    static let widths: [Float] = [2.4, 3.0, 3.6, 4.5]
    static let distances: [Float] = [2.0, 2.5, 3.0, 4.0]
    static let curves: [Float] = [0, 0.35, 0.7, 1.0]
    static let hudScales: [Float] = [0.6, 0.8, 1.0, 1.3]
    static let wristScales: [Float] = [0.16, 0.2, 0.26, 0.34]
    static let dioramaScales: [Float] = [0.10, 0.15, 0.22]
    static let aiRanges: [Float] = [1.0, 1.5, 2.0]
    static let aiRangeNames = ["x1", "x1,5", "x2"]

    /// Aplica los ajustes de la IA al motor (vr/ai): alcance de visión y visión con la cabeza VR. Idempotente; se llama al arrancar y tras cada cambio del menú.
    static func applyAI(range: Int, head: Bool) {
#if !MV_BENCH
        mv_vrhost_set_ai_range(aiRanges[min(max(range, 0), aiRanges.count - 1)])
        mv_vrhost_set_ai_head(head ? 1 : 0)
#endif
    }
    /// Manos nuevas/originales del personaje en el render nativo (vr/host): idempotente; se llama al arrancar y tras cada cambio del menú.
    static func applyHands(_ on: Bool) {
#if !MV_BENCH
        mv_vrhost_set_hands_new(on ? 1 : 0)
#endif
    }
    /// Reglas de descarte del modo ENGINE del render nativo (vr/native/ns_gpu: NSG_ENG_OFF_*): cada ajuste apagado deja de descartar por esa regla en la pantalla 3D. Idempotente; se llama al arrancar y tras cada cambio del menú.
    static func applyDiscards(behind: Bool, size: Bool, back: Bool, split: Bool) {
#if !MV_BENCH
        var m: UInt32 = 0
        if !behind { m |= UInt32(NSG_ENG_OFF_BEHIND) }
        if !size { m |= UInt32(NSG_ENG_OFF_SIZE) }
        if !back { m |= UInt32(NSG_ENG_OFF_BACK) }
        if !split { m |= UInt32(NSG_ENG_OFF_SPLIT) }
        ns_gpu_engine_set_off(m)
#endif
    }
    static func applyStoredAI() { let c = load(); applyAI(range: c.aiRange, head: c.aiHead); applyHands(c.newHands); applyDiscards(behind: c.engBehind, size: c.engSize, back: c.engBack, split: c.engSplit) }

    private static func f(_ k: String, _ d: Float, _ lo: Float, _ hi: Float) -> Float {
        guard UserDefaults.standard.object(forKey: k) != nil else { return d }
        let v = Float(UserDefaults.standard.double(forKey: k))
        return v.isFinite ? min(max(v, lo), hi) : d
    }
    private static func b(_ k: String, _ d: Bool) -> Bool {
        UserDefaults.standard.object(forKey: k) == nil ? d : UserDefaults.standard.bool(forKey: k)
    }

    static func load() -> XRViewConfig {
        var c = XRViewConfig()
        c.width = f("xr.view.width", 3.0, 1.0, 8.0)
        c.distance = f("xr.view.distance", 2.5, 1.0, 8.0)
        c.curve = f("xr.view.curve", 0.35, 0, 1)
        let s = UserDefaults.standard.integer(forKey: "xr.view.sharp")
        c.sharp = UserDefaults.standard.object(forKey: "xr.view.sharp") == nil ? 1 : min(max(s, 0), 2)
        c.integerScale = b("xr.view.integer", false)
        c.filter2D = XR2DFilter.storedIndex
        c.exact2D = b(XR2DCompose.key, true)
        let s2 = UserDefaults.standard.integer(forKey: "xr.view.smooth2d2")   // clave nueva: el defecto pasó de «nítido» a «original» (el texto no se deforma)
        c.smooth2D = UserDefaults.standard.object(forKey: "xr.view.smooth2d2") == nil ? 0 : min(max(s2, 0), 2)
        // El defecto es la pantalla de cine 3D estéreo (IPD real, profundidad). Un «screen» (mono, sin profundidad) guardado por el defecto anterior migra a «stereo» UNA sola vez (clave de marca);
        // elegido después en el menú, «Pantalla de cine 3D (mono)» sobrevive al reinicio.
        if !UserDefaults.standard.bool(forKey: "xr.view.cutscene3.toStereo") {
            if UserDefaults.standard.string(forKey: "xr.view.cutscene3") == "screen" { UserDefaults.standard.set(Cutscene.stereo.rawValue, forKey: "xr.view.cutscene3") }
            UserDefaults.standard.set(true, forKey: "xr.view.cutscene3.toStereo")
        }
        if let v = UserDefaults.standard.string(forKey: "xr.view.cutscene3"), let m = Cutscene(rawValue: v) { c.cutscene = m }
        else if let v = UserDefaults.standard.string(forKey: "xr.view.cutscene2"), v != "stereo", let m = Cutscene(rawValue: v) { c.cutscene = m }
        else { c.cutscene = .stereo }
        c.dioramaScale = f("xr.view.dioramaScale", 0.15, 0.03, 0.5)
        c.hudPanels = b("xr.view.hud", true)
        c.hudScale = f("xr.view.hudScale", 0.8, 0.3, 2.0)
        c.wristHud = b("xr.view.wristHud", true)
        c.wristScale = f("xr.view.wristScale", 0.2, 0.1, 0.5)
        c.wristHandSide = b("xr.view.wristHand", false)
        c.hideRealHands = b("xr.view.hideHands", true)
        c.debugAxes = b("xr.view.debugAxes", false)
        c.smoothMotion = b("xr.view.smoothMotion", true)
        c.follow2D = b("xr.view.follow2d", true)
        c.newHands = b("xr.hands.new", true)
        c.engBehind = b("xr.eng.behind", true)
        c.engSize = b("xr.eng.size", true)
        c.engBack = b("xr.eng.back", true)
        c.engSplit = b("xr.eng.split", true)
        // El defecto pasó de x1,5 a x1 (alcance original del juego): lo guardado por el defecto anterior migra a x1 UNA sola vez (clave de marca); elegido después en el menú, sobrevive al reinicio.
        if !UserDefaults.standard.bool(forKey: "xr.ai.range.toX1") {
            UserDefaults.standard.set(0, forKey: "xr.ai.range")
            UserDefaults.standard.set(true, forKey: "xr.ai.range.toX1")
        }
        let ar = UserDefaults.standard.integer(forKey: "xr.ai.range")
        c.aiRange = UserDefaults.standard.object(forKey: "xr.ai.range") == nil ? 0 : min(max(ar, 0), aiRanges.count - 1)
        c.aiHead = b("xr.ai.head", true)
        return c
    }

    func save() {
        let d = UserDefaults.standard
        d.set(Double(width), forKey: "xr.view.width")
        d.set(Double(distance), forKey: "xr.view.distance")
        d.set(Double(curve), forKey: "xr.view.curve")
        d.set(sharp, forKey: "xr.view.sharp")
        d.set(integerScale, forKey: "xr.view.integer")
        d.set(filter2D, forKey: XR2DFilter.key)
        d.set(exact2D, forKey: XR2DCompose.key)
        d.set(smooth2D, forKey: "xr.view.smooth2d2")
        d.set(cutscene.rawValue, forKey: "xr.view.cutscene3")
        d.set(Double(dioramaScale), forKey: "xr.view.dioramaScale")
        d.set(hudPanels, forKey: "xr.view.hud")
        d.set(Double(hudScale), forKey: "xr.view.hudScale")
        d.set(wristHud, forKey: "xr.view.wristHud")
        d.set(Double(wristScale), forKey: "xr.view.wristScale")
        d.set(wristHandSide, forKey: "xr.view.wristHand")
        d.set(hideRealHands, forKey: "xr.view.hideHands")
        d.set(debugAxes, forKey: "xr.view.debugAxes")
        d.set(smoothMotion, forKey: "xr.view.smoothMotion")
        d.set(follow2D, forKey: "xr.view.follow2d")
        d.set(newHands, forKey: "xr.hands.new")
        d.set(engBehind, forKey: "xr.eng.behind")
        d.set(engSize, forKey: "xr.eng.size")
        d.set(engBack, forKey: "xr.eng.back")
        d.set(engSplit, forKey: "xr.eng.split")
        d.set(aiRange, forKey: "xr.ai.range")
        d.set(aiHead, forKey: "xr.ai.head")
    }
}

/// Último fotograma del juego (el que mostraría la pantalla) en texturas RGBA8 con anillo de 4 (la GPU puede seguir leyendo las anteriores).
final class XRCinema {
    private var ring: [MTLTexture] = []
    private var ringSRGB: [MTLTexture] = []
    private var idx = 0
    private var buf = [UInt8](repeating: 0, count: Int(MV_HOST_FB_BYTES))
    private var seq: UInt32 = 0
    private(set) var w = 320
    private(set) var h = 240
    private(set) var haveFrame = false

    init?(device: MTLDevice) {
        self.device = device
        let td = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm, width: Int(MV_HOST_MAX_W), height: Int(MV_HOST_MAX_H), mipmapped: false)
        td.usage = [.shaderRead, .pixelFormatView]
        td.storageMode = .shared
        for i in 0..<4 {
            guard let t = device.makeTexture(descriptor: td), let v = t.makeTextureView(pixelFormat: .rgba8Unorm_srgb) else { return nil }
            t.label = "mv.cine.\(i)"
            ring.append(t); ringSRGB.append(v)
        }
        if XR2DFilter.upscales { scaler = XRPixelScaler(device: device) }
    }

    /// Sube el último fotograma si hay uno nuevo (no espera ni retiene nada del hilo de emulación).
    func update() {
        var nw: Int32 = 0, nh: Int32 = 0
        let s = buf.withUnsafeMutableBufferPointer { p -> UInt32 in
            guard let base = p.baseAddress else { return 0 }
            return mv_host_framebuffer(base, p.count, &nw, &nh, seq)
        }
        guard s != 0, nw > 0, nh > 0, Int(nw) <= Int(MV_HOST_MAX_W), Int(nh) <= Int(MV_HOST_MAX_H) else { return }
        seq = s
        w = Int(nw); h = Int(nh)
        idx = (idx + 1) % ring.count
        let tex = ring[idx]
        buf.withUnsafeBytes { p in
            if let base = p.baseAddress {
                tex.replace(region: MTLRegionMake2D(0, 0, w, h), mipmapLevel: 0, withBytes: base, bytesPerRow: w * 4)
            }
        }
        haveFrame = true
        if !hqActive { stamp &+= 1 }
    }

    private var hqSeq: UInt32 = 0
    private var hqRing: [MTLTexture] = [], hqRingSRGB: [MTLTexture] = []
    private var hqIdx = 0, hqW = 0, hqH = 0
    private(set) var hqActive = false
    private var device: MTLDevice?

    /// Fotograma completo de la capa 2D fiel (vr_layer2d, a alta resolución): sustituye al fotograma de la VRAM a 1x mientras llegue. nil = vuelve al fotograma normal.
    func setHighRes(_ f: (bytes: [UInt8], w: Int, h: Int, seq: UInt32)?) {
        guard let f = f, f.w > 0, f.h > 0, f.bytes.count >= f.w * f.h * 4, let dev = device else { hqActive = false; return }
        if f.seq == hqSeq && hqActive && hqW == f.w && hqH == f.h { return }
        if hqW != f.w || hqH != f.h || hqRing.isEmpty {
            let td = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm, width: f.w, height: f.h, mipmapped: false)
            td.usage = [.shaderRead, .pixelFormatView]
            td.storageMode = .shared
            var a: [MTLTexture] = [], b: [MTLTexture] = []
            for i in 0..<3 {
                guard let t = dev.makeTexture(descriptor: td), let v = t.makeTextureView(pixelFormat: .rgba8Unorm_srgb) else { hqActive = false; return }
                t.label = "mv.cine.hq.\(i)"; a.append(t); b.append(v)
            }
            hqRing = a; hqRingSRGB = b; hqW = f.w; hqH = f.h
        }
        hqIdx = (hqIdx + 1) % hqRing.count
        f.bytes.withUnsafeBytes { p in
            if let base = p.baseAddress { hqRing[hqIdx].replace(region: MTLRegionMake2D(0, 0, f.w, f.h), mipmapLevel: 0, withBytes: base, bytesPerRow: f.w * 4) }
        }
        hqSeq = f.seq; hqActive = true
        stamp &+= 1
    }

    // xBR-lv2 x3 (XRPixelScaler) de la imagen vigente (fotograma de alta resolución o el de la VRAM): prepareUpscale (en compose) elige el destino y deja el trabajo; encodeUpscale (con el búfer de comandos, antes del pase final) lo codifica.
    private var scaler: XRPixelScaler?
    private var xbr: [MTLTexture] = [], xbrSRGB: [MTLTexture] = []
    private var xbrIdx = 0, xbrW = 0, xbrH = 0, xbrKey = -1
    private var stamp = 0                                    // cambia con cada contenido nuevo de la imagen vigente
    private var xbrJob: (src: MTLTexture, dst: MTLTexture, w: Int, h: Int)?
    private(set) var xbrActive = false

    /// `video`: vídeo FMV/MDEC (view_kind 3 de vr_view.h, el mismo que XRCinemaLayout.legible): sin xBR, solo sharp-bilinear sobre el marco nativo.
    func prepareUpscale(video: Bool) {
        xbrActive = false
        guard !video, XR2DFilter.upscales, scaler != nil, let dev = device else { return }
        let sw = hqActive ? hqW : w, sh = hqActive ? hqH : h
        guard sw > 0, sh > 0, let src = sourceTexture(srgb: false) else { return }
        if xbr.isEmpty || xbrW != sw || xbrH != sh {
            var a: [MTLTexture] = [], b: [MTLTexture] = []
            for i in 0..<4 {
                guard let t = XRPixelScaler.makeTarget(dev, width: sw, height: sh, label: "mv.cine.xbr.\(i)") else { xbr = []; xbrSRGB = []; return }
                a.append(t.tex); b.append(t.srgb)
            }
            xbr = a; xbrSRGB = b; xbrW = sw; xbrH = sh; xbrKey = -1
        }
        let key = stamp &* 2 &+ (hqActive ? 1 : 0)
        if key != xbrKey {
            xbrKey = key
            xbrIdx = (xbrIdx + 1) % xbr.count
            xbrJob = (src, xbr[xbrIdx], sw, sh)
        }
        xbrActive = true
    }

    func encodeUpscale(_ cb: MTLCommandBuffer) {
        guard let j = xbrJob else { return }
        xbrJob = nil
        scaler?.encode(cb, src: j.src, dst: j.dst, width: j.w, height: j.h)
    }

    var width: Int { xbrActive ? xbrW * XR2DFilter.factor : (hqActive ? hqW : w) }
    var height: Int { xbrActive ? xbrH * XR2DFilter.factor : (hqActive ? hqH : h) }

    func texture(srgb: Bool) -> MTLTexture? {
        if xbrActive, !xbr.isEmpty { return srgb ? xbrSRGB[xbrIdx] : xbr[xbrIdx] }
        return sourceTexture(srgb: srgb)
    }

    private func sourceTexture(srgb: Bool) -> MTLTexture? {
        if hqActive, !hqRing.isEmpty { return srgb ? hqRingSRGB[hqIdx] : hqRing[hqIdx] }
        return haveFrame ? (srgb ? ringSRGB[idx] : ring[idx]) : nil
    }
}

/// Entrada del menú (flancos ya detectados).
struct XRNav: Equatable {
    var up = false, down = false, left = false, right = false, select = false, back = false
}

enum XRMenuItem: Int, CaseIterable {
    case resume, recenter, calibrateHands, width, distance, follow2D, curve, sharp, smooth2D, filter2D, cutscene, hud, hudScale, wristHud, wristScale, wristSide, smoothMotion, turnMode, vignette, hideHands, newHands, aiMenu, diagnostics, exit
}

/// Submenú «IA de enemigos».
enum XRAIItem: Int, CaseIterable {
    case back, range, head
}

/// Submenú «Diagnóstico» (interruptores en vivo de XRDiag).
enum XRDiagItem: Int, CaseIterable {
    case back, reset, magenta, split, rateMap, msaa1, noLayers, nearPlane, projection, noZTest, fillDepth, flatDepth, engBehind, engSize, engBack, engSplit, exact2D, integerScale, debugAxes, giveWeapons
}

final class XRPresenter {
    enum Display { case world, cinema, diorama, stereo, scene }

    struct Result {
        var items: [XRDrawItem] = []
        var drawWorld = true
        var drawBody = true
        /// false = la geometría 3D no va a los ojos (pantalla 3D: va a las texturas estéreo de `stereo`).
        var drawEyes = true
        /// Pantalla 3D: parámetros de las cámaras estéreo y tamaño de las texturas (RGRenderer calcula las vistas con la cámara de la escena que dibuja).
        var stereo: RGRenderer.StereoRequest?
        /// Diorama: se multiplica a la vista de cada ojo (mundo VR del juego en metros -> espacio recentrado del usuario).
        var dioramaModel: simd_float4x4?
    }

    private(set) var cfg = XRViewConfig.load()
    private let cinema: XRCinema?
    /// Fotograma completo de la capa 2D fiel a alta resolución (lo fija el bucle de render desde RGRenderer.hqFull antes de componer); nil = fotograma normal.
    var hqFull: (bytes: [UInt8], w: Int, h: Int, seq: UInt32)?
    /// Colocación de la capa 2D fiel del HUD en la ventana visible fija del televisor (RGRenderer.hudWin; lo fija el bucle de render antes de componer); nil = HUD de reserva.
    var hudWin: XR2DWindow?
    private(set) var shown: Display = .cinema
    private(set) var veil: Float = 1
    /// Fundido a negro por la cabeza empujando contra el escenario (0..1; lo fija el bucle de render con mv_vrhost_head_limit; ya viene suavizado).
    var collisionFade: Float = 0

    // Anclas (espacio recentrado)
    private var needAnchor = true
    private var anchor = matrix_identity_float4x4          // suelo bajo la cabeza, rumbo de la cabeza al pasar a cine
    private var anchorY: Float = 1.5
    private var hudYaw: Float = 0
    private var hudInit = false
    // Seguimiento continuo de la cabeza por las pantallas 2D (opción «Pantallas 2D: siguen la cabeza»).
    private var follow = XRFollowState()
    private var menuFollow = XRFollowState()   // seguimiento propio del menú VR: se congela mientras se interactúa con él
    private var menuHold: Float = 0            // s que quedan de congelación tras la última entrada sobre el menú
    private var menuSnap = false
    private var followSnap = true
    private var prevOverlay2D = false
    private var overlayAim: (yaw: Float, pitch: Float)?    // mirada (rumbo y cabeceo de la cabeza) al abrirse la ventana 2D del juego: el panel 6b queda centrado ahí mientras dure
    private var hudSeen = false                            // el último compose recibió textura de la capa 2D (diagnóstico)
    private var overlayHand = -1               // mano física (0 izq, 1 der) que abrió la ventana 2D del juego mientras dura; -1 = sin mano (pausa, sin pose)
    // Dentro de la escena: cámara del plano (vista del motor -> mundo VR al empezar el plano) y plano visto
    private var shotView = matrix_identity_float4x4
    private var shotSeen: UInt32 = 0
    private var cutVeil: Float = 0
    private(set) var pendVeil: Float = 0       // velo mientras la vista espera a la cámara VR (VRV_R_PENDING) o el anfitrión pide negro (carga: MvVrGameState.view_black)

    // Menú
    private(set) var menuOpen = false
    private var sel = 0
    private var page = 0                  // 0 = menú principal, 1 = submenú «Diagnóstico», 2 = submenú «IA de enemigos»
    private var menuDirty = true
    private var menuModel = matrix_identity_float4x4
    private var menuTex: [MTLTexture] = []
    private var menuTexSRGB: [MTLTexture] = []
    private var menuIdx = 0
    private var holdT: Float = 0
    private var diagT: Float = 0           // s con R3 mantenido (abre el submenú «Diagnóstico» a los 3 s)
    private var prevHeld = false
    private var prevNav = XRNav()
    private var blockUntilNeutral = false
    private var blockT: Float = 0
    private var needRelease = false        // tras cerrar con el botón de menú: hasta soltarlo no cuenta para reabrir
    var onRecenter: (() -> Void)?
    var onExit: (() -> Void)?

    private static let menuW = 1024, menuH = 1140     // 24 filas de 40 px (la página más larga: el menú principal con troceo 2D) desde y = 106 (la última acaba en 1066) + pie de ayuda en y = menuH - 56 = 1084

    init(device: MTLDevice) {
        cinema = XRCinema(device: device)
#if !MV_BENCH
        mv_vrhost_set_hud2d_smooth(Int32(XRViewConfig.load().smooth2D))
        XRViewConfig.applyStoredAI()
#endif
        if cinema == nil { Storage.log("xr: no se pudo crear la textura de la pantalla de cine") }
        let td = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm, width: Self.menuW, height: Self.menuH, mipmapped: false)
        td.usage = [.shaderRead, .pixelFormatView]
        td.storageMode = .shared
        for i in 0..<4 {
            if let t = device.makeTexture(descriptor: td), let v = t.makeTextureView(pixelFormat: .rgba8Unorm_srgb) {
                t.label = "mv.menu.\(i)"
                menuTex.append(t); menuTexSRGB.append(v)
            }
        }
    }

    /// Recentrado manual: la pantalla se recoloca delante de la cabeza la próxima vez que se muestre (o ahora, si ya está).
    /// También se salta el suavizado (la cabeza cambia de espacio al recentrar) y se reinicia el rumbo del HUD.
    func reanchor() { needAnchor = true; followSnap = true; hudInit = false }

    // MARK: entrada / menú

#if !MV_BENCH
    /// Entrada del menú VR. Devuelve true si el juego NO debe recibir la entrada de este fotograma (menú abierto, o recién cerrado
    /// con algo aún pulsado: evita un disparo o un movimiento accidental).
    func updateInput(_ s: ControllerHub.Sample, dt: Float, head: simd_float4x4) -> Bool {
        // Menú VR: botón de menú izquierdo (Create/Options) mantenido 0,8 s. Select del mando NO abre nada (llega al juego).
        // Submenú «Diagnóstico» directo: R3 (clic del stick derecho; bit 2 del mando) mantenido 3 s. No cuenta con L3 pulsado (L3 + R3 = gesto de volcado de DumpCapture) ni mientras ese gesto está enganchado.
        let menuHeld = s.left.menu
        let r3 = s.right.stickClick || (s.pad.buttons & 4) != 0
        let l3 = s.left.stickClick || (s.pad.buttons & 2) != 0
        let diagHeld = r3 && !l3 && !DumpCapture.shared.suppressesStickClicks
        let held = menuHeld || diagHeld
        if menuOpen {
            if menuHeld && !prevHeld { closeMenu() }      // cerrar solo con el botón de menú (B también); un clic de stick sin querer al navegar no cierra
        } else if held && !needRelease {
            if menuHeld { holdT += dt; diagT = 0 } else { diagT += dt; holdT = 0 }
            if holdT >= 0.8 { openMenu(head: head, diag: false); holdT = 0 }
            else if diagT >= 3 { openMenu(head: head, diag: true); diagT = 0 }
        } else {
            holdT = 0; diagT = 0
        }
        if !held { needRelease = false }
        prevHeld = menuHeld

        if menuOpen {
            let nav = Self.nav(from: s)
            let p = prevNav
            prevNav = nav
            let rc = rowCount
            if nav.up && !p.up { sel = (sel + rc - 1) % rc; menuDirty = true }
            if nav.down && !p.down { sel = (sel + 1) % rc; menuDirty = true }
            if nav.left && !p.left { activate(dir: -1) }
            if nav.right && !p.right { activate(dir: 1) }
            if nav.select && !p.select { activate(dir: 1) }
            if nav.back && !p.back { if page != 0 { leaveSub() } else { closeMenu() } }
            // Entrada activa sobre el menú (direcciones, selección, mando o Sense con algo pulsado/inclinado): el menú se congela 0,6 s tras la última.
            if nav.up || nav.down || nav.left || nav.right || nav.select || nav.back || s.pad.active || s.left.active || s.right.active { menuHold = 0.6 }
            else { menuHold = max(0, menuHold - dt) }
        } else {
            prevNav = XRNav()
            menuHold = 0
        }
        if !menuOpen && blockUntilNeutral {
            // Tras cerrar: entrada bloqueada hasta que todo esté en neutro (máx. 2 s: un stick con deriva o un grip sostenido no la dejan bloqueada).
            blockT += dt
            if blockT > 2 || !(s.pad.active || s.left.active || s.right.active) { blockUntilNeutral = false }
        }
        // Mientras se mantiene el botón de menú (>0,3 s) el juego no lo recibe (evita que la pulsación larga se vea como Select mantenido).
        // R3 mantenido NO bloquea nada: el juego lo sigue recibiendo (R3 corto, y el movimiento / disparo; mantener L3/R3 no abre ninguna ventana de armas/objetos en vr_gesture). A los 3 s se abre el menú
        // «Diagnóstico»; al abrirse, `menuOpen` bloquea la entrada y VRInput.releaseAll suelta R2/R3.
        return menuOpen || blockUntilNeutral || (!menuOpen && menuHeld && holdT > 0.3)
    }

    private static func nav(from s: ControllerHub.Sample) -> XRNav {
        var n = XRNav()
        let sx = abs(s.left.stickX) > abs(s.right.stickX) ? s.left.stickX : s.right.stickX
        let sy = abs(s.left.stickY) > abs(s.right.stickY) ? s.left.stickY : s.right.stickY
        n.up = sy > 0.6; n.down = sy < -0.6; n.left = sx < -0.6; n.right = sx > 0.6
        n.select = s.left.trigger > 0.6 || s.right.trigger > 0.6 || s.left.low || s.right.low
        n.back = s.left.up || s.right.up
        let b = s.pad.buttons
        if b & (1 << 4) != 0 { n.up = true }
        if b & (1 << 6) != 0 { n.down = true }
        if b & (1 << 7) != 0 { n.left = true }
        if b & (1 << 5) != 0 { n.right = true }
        if s.pad.ly > 0.6 { n.up = true }
        if s.pad.ly < -0.6 { n.down = true }
        if s.pad.lx < -0.6 { n.left = true }
        if s.pad.lx > 0.6 { n.right = true }
        if b & ((1 << 13) | (1 << 14)) != 0 { n.select = true }     // círculo / cruz
        if b & (1 << 12) != 0 { n.back = true }                     // triángulo
        return n
    }

#endif

    private func openMenu(head: simd_float4x4, diag: Bool) {
        menuOpen = true
        sel = 0
        page = diag ? 1 : 0      // 1 = submenú «Diagnóstico»
        menuDirty = true
        prevNav = XRNav()
        menuModel = XRMenuLayout.model(head: head)      // cerca, abajo e inclinado hacia la cara: nunca sobre la pantalla de cine / códec
        followSnap = true                               // el menú sale ya delante de la cabeza; después la sigue
        menuSnap = true; menuHold = 0
    }

    private func closeMenu() {
        menuOpen = false
        blockUntilNeutral = true
        blockT = 0
        needRelease = true
        cfg.save()
    }

    private func cycle(_ v: Float, _ list: [Float], _ dir: Int) -> Float {
        var best = 0
        for (i, x) in list.enumerated() where abs(x - v) < abs(list[best] - v) { best = i }
        return list[(best + dir + list.count) % list.count]
    }

    /// Filas del menú principal. Sin troceo del 2D (XRLayer2D.split, el defecto) los paneles del HUD, su tamaño y los de la muñeca no hacen nada (el 2D es un solo panel): se ocultan. `sel` indexa esta lista.
    private var mainItems: [XRMenuItem] {
        let splitOnly: [XRMenuItem] = [.hud, .hudScale, .wristHud, .wristScale, .wristSide]
        return XRLayer2D.split ? XRMenuItem.allCases : XRMenuItem.allCases.filter { !splitOnly.contains($0) }
    }
    private var rowCount: Int { page == 0 ? mainItems.count : (page == 1 ? XRDiagItem.allCases.count : XRAIItem.allCases.count) }

    private func leaveSub() {
        sel = mainItems.firstIndex(of: page == 2 ? .aiMenu : .diagnostics) ?? 0
        page = 0
        menuDirty = true
    }

    private func activateAI(dir: Int) {
        guard let it = XRAIItem(rawValue: sel) else { return }
        switch it {
        case .back: leaveSub()
        case .range:
            let n = XRViewConfig.aiRanges.count
            cfg.aiRange = (cfg.aiRange + (dir < 0 ? n - 1 : 1)) % n
        case .head: cfg.aiHead.toggle()
        }
        XRViewConfig.applyAI(range: cfg.aiRange, head: cfg.aiHead)
        cfg.save()
        menuDirty = true
    }

    /// Un paso de la lista `list` desde el valor actual `v` (el más próximo), en el sentido `dir`.
    private func stepList(_ v: Float, _ list: [Float], _ dir: Int) -> Float {
        var best = 0
        for (i, x) in list.enumerated() where abs(x - v) < abs(list[best] - v) { best = i }
        return list[(best + (dir < 0 ? list.count - 1 : 1)) % list.count]
    }

    private func activateDiag(dir: Int) {
        guard let it = XRDiagItem(rawValue: sel) else { return }
        let d = XRDiag.shared
        switch it {
        case .back: leaveSub()
        case .reset: if dir >= 0 { d.reset() }
        case .magenta: d.update { $0.magentaClear.toggle() }
        case .split: d.update { $0.splitMeters = stepList($0.splitMeters, XRDiag.splitChoices, dir) }
        case .rateMap: d.update { $0.ignoreRateMap.toggle() }
        case .msaa1: d.update { $0.msaa1.toggle() }
        case .noLayers: d.update { $0.noLayers.toggle() }
        case .nearPlane: d.update { $0.nearPlane = stepList($0.nearPlane, XRDiag.nearChoices, dir) }
        case .projection: d.update { $0.projMode = ($0.projMode + (dir < 0 ? 2 : 1)) % 3 }
        case .noZTest: d.update { $0.noZTest.toggle() }
        case .fillDepth: d.update { $0.fillDepth.toggle() }
        case .flatDepth: d.update { $0.flatDepth.toggle() }
        case .engBehind: cfg.engBehind.toggle(); XRViewConfig.applyDiscards(behind: cfg.engBehind, size: cfg.engSize, back: cfg.engBack, split: cfg.engSplit); cfg.save()
        case .engSize: cfg.engSize.toggle(); XRViewConfig.applyDiscards(behind: cfg.engBehind, size: cfg.engSize, back: cfg.engBack, split: cfg.engSplit); cfg.save()
        case .engBack: cfg.engBack.toggle(); XRViewConfig.applyDiscards(behind: cfg.engBehind, size: cfg.engSize, back: cfg.engBack, split: cfg.engSplit); cfg.save()
        case .engSplit: cfg.engSplit.toggle(); XRViewConfig.applyDiscards(behind: cfg.engBehind, size: cfg.engSize, back: cfg.engBack, split: cfg.engSplit); cfg.save()
        case .exact2D: cfg.exact2D.toggle(); XR2DCompose.framebufferFetch = cfg.exact2D
        case .integerScale: cfg.integerScale.toggle()
        case .debugAxes: cfg.debugAxes.toggle()
        case .giveWeapons:      // truco de depuración: acción única (no se guarda); el hilo de emulación la aplica en el siguiente VBlank si hay partida
            if dir >= 0 {
#if !MV_BENCH
                mv_vrgame_cheat_all_weapons()
#endif
            }
        }
        menuDirty = true
    }

    private func activate(dir: Int) {
        if page == 1 { activateDiag(dir: dir); return }
        if page == 2 { activateAI(dir: dir); return }
        let main = mainItems
        guard sel >= 0, sel < main.count else { return }
        let it = main[sel]
        switch it {
        case .resume: closeMenu()
        case .recenter: onRecenter?(); closeMenu()
        case .calibrateHands:
            // Izquierda borra la calibración (orientación de las manos y largo de brazos); seleccionar/derecha empieza una cuenta atrás de 3 s (un pulso háptico por segundo) para poner los brazos rectos al frente: mide la orientación de los mandos y el largo de los brazos.
            if dir < 0 { mv_vrgame_clear_hand_calibration() } else { mv_vrgame_calibrate_hands(3.0); closeMenu() }
        case .width: cfg.width = cycle(cfg.width, XRViewConfig.widths, dir)
        case .distance: cfg.distance = cycle(cfg.distance, XRViewConfig.distances, dir)
        case .follow2D: cfg.follow2D.toggle(); needAnchor = true; followSnap = true; cfg.save()      // al fijarlas, se anclan donde está la cabeza ahora
        case .curve: cfg.curve = cycle(cfg.curve, XRViewConfig.curves, dir)
        case .sharp: cfg.sharp = (cfg.sharp + dir + 3) % 3
        case .smooth2D:
            cfg.smooth2D = (cfg.smooth2D + dir + 3) % 3
#if !MV_BENCH
            mv_vrhost_set_hud2d_smooth(Int32(cfg.smooth2D))
#endif
        case .filter2D: cfg.filter2D = (cfg.filter2D + dir + 4) % 4      // se aplica al reabrir la vista: los destinos de la ampliación se crean con el factor del filtro (XR2DFilter.reload)
        case .cutscene:
            let all = XRViewConfig.Cutscene.allCases
            let i = all.firstIndex(of: cfg.cutscene) ?? 0
            cfg.cutscene = all[(i + dir + all.count) % all.count]
        case .hud: cfg.hudPanels.toggle()
        case .hudScale: cfg.hudScale = cycle(cfg.hudScale, XRViewConfig.hudScales, dir)
        case .wristHud: cfg.wristHud.toggle()
        case .wristScale: cfg.wristScale = cycle(cfg.wristScale, XRViewConfig.wristScales, dir)
        case .wristSide: cfg.wristHandSide.toggle()
        case .smoothMotion: cfg.smoothMotion.toggle(); cfg.save()
        case .turnMode: Self.cycleTurnMode(dir)
        case .vignette: Self.cycleVignette(dir)
        case .hideHands: cfg.hideRealHands.toggle(); cfg.save()      // la escena lo lee de UserDefaults (@AppStorage): se guarda al momento
        case .newHands: cfg.newHands.toggle(); XRViewConfig.applyHands(cfg.newHands); cfg.save()
        case .aiMenu: if dir >= 0 { page = 2; sel = 0 }
        case .diagnostics: if dir >= 0 { page = 1; sel = 0 }
        case .exit: cfg.save(); onExit?()
        }
        menuDirty = true
    }

    // MARK: menú: dibujo

    private func diagRows() -> [(String, String)] {
        func yn(_ b: Bool) -> String { b ? "Sí" : "No" }
        let s = XRDiag.shared.settings
        return XRDiagItem.allCases.map { it -> (String, String) in
            switch it {
            case .back: return ("Volver", "")
            case .reset: return ("Restablecer todo", s.isNormal ? "ya normal" : "")
            case .magenta: return ("Fondo de borrado magenta", yn(s.magentaClear))
            case .split: return ("Partir triángulos grandes", s.splitMeters > 0 ? "arista > " + XRDiag.metersText(s.splitMeters) : "No")
            case .rateMap: return ("Sin mapa de tasa de rasterización", yn(s.ignoreRateMap))
            case .msaa1: return ("MSAA 1x (sin resolve)", yn(s.msaa1))
            case .noLayers: return ("Sin capas finales", yn(s.noLayers))
            case .nearPlane: return ("Plano cercano", s.nearPlane > 0 ? XRDiag.metersText(s.nearPlane) : "Normal (sistema)")
            case .projection: return ("Proyección", ["Automática", "Propia", "Del sistema"][min(max(s.projMode, 0), 2)])
            case .noZTest: return ("Sin prueba de profundidad", yn(s.noZTest))
            case .fillDepth: return ("Profundidad de relleno", yn(s.fillDepth))
            case .flatDepth: return ("No entregar profundidad al sistema", yn(s.flatDepth))
            case .engBehind: return ("Descarte por detrás (3D)", yn(cfg.engBehind))
            case .engSize: return ("Tope de tamaño (3D)", yn(cfg.engSize))
            case .engBack: return ("Reversos (3D)", yn(cfg.engBack))
            case .engSplit: return ("Caras cercanas (3D)", cfg.engSplit ? "Original" : "Recortadas")
            case .exact2D: return ("Composición 2D exacta", yn(cfg.exact2D))
            case .integerScale: return ("Escalado entero", yn(cfg.integerScale))
            case .debugAxes: return ("Ejes de mandos y manos", yn(cfg.debugAxes))
            case .giveWeapons: return ("Truco: dar todas las armas", "")
            }
        }
    }

    private func rows() -> [(String, String)] {
        if page == 1 { return diagRows() }
        if page == 2 {
            return XRAIItem.allCases.map { it -> (String, String) in
                switch it {
                case .back: return ("Volver", "")
                case .range: return ("IA: alcance de visión", XRViewConfig.aiRangeNames[min(max(cfg.aiRange, 0), XRViewConfig.aiRangeNames.count - 1)])
                case .head: return ("IA: ver la cabeza VR", cfg.aiHead ? "Sí" : "No")
                }
            }
        }
        func m(_ v: Float) -> String { String(format: "%.1f m", v).replacingOccurrences(of: ".", with: ",") }
        func yn(_ b: Bool) -> String { b ? "Sí" : "No" }
        let curveName = ["Plana", "Suave", "Media", "Fuerte"]
        var ci = 0
        for (i, x) in XRViewConfig.curves.enumerated() where abs(x - cfg.curve) < abs(XRViewConfig.curves[ci] - cfg.curve) { ci = i }
        return mainItems.map { it -> (String, String) in
            switch it {
            case .resume: return ("Volver al juego", "")
            case .recenter: return ("Recentrar la vista", "")
            case .calibrateHands:
                let cd = mv_vrgame_calibration_countdown()
                var q: [Float] = [0, 0, 0, 0]
                var armLen: Float = 0
                let have = mv_vrgame_hand_calibration(0, &q) != 0 || mv_vrgame_hand_calibration(1, &q) != 0
                    || mv_vrgame_arm_length(0, &armLen) != 0 || mv_vrgame_arm_length(1, &armLen) != 0
                return ("Calibrar manos y brazos", cd > 0 ? "brazos rectos: \(Int(cd.rounded(.up))) s" : (have ? "Hecha (izq. borra)" : "Sin calibrar"))
            case .width: return ("Tamaño de la pantalla", m(cfg.width))
            case .distance: return ("Distancia de la pantalla", m(cfg.distance))
            case .follow2D: return ("Pantallas 2D", cfg.follow2D ? "Siguen la cabeza" : "Fijas en el mundo")
            case .curve: return ("Curvatura de la pantalla", curveName[ci])
            case .sharp: return ("Nitidez de la imagen", ["Suave", "Nítida", "Píxel"][min(max(cfg.sharp, 0), 2)])
            case .smooth2D: return ("Suavizado 2D", ["Original", "Nítido", "Suave"][min(max(cfg.smooth2D, 0), 2)])
            case .filter2D:
                let f = min(max(cfg.filter2D, 0), 3)
                return ("Filtro 2D", ["MMPX", "xBR", "Nítido", "Sin filtro"][f] + (f != XR2DFilter.current.index ? " (al reabrir la vista)" : ""))
            case .cutscene:
                let n: [XRViewConfig.Cutscene: String] = [.screen: "Pantalla de cine 3D (mono)", .stereo: "Pantalla de cine 3D (estéreo)", .cinema: "Pantalla de cine", .diorama: "Diorama", .scene: "Dentro de la escena"]
                return ("Cinemáticas del motor", n[cfg.cutscene] ?? "")
            case .hud: return ("Paneles del HUD", yn(cfg.hudPanels))
            case .hudScale: return ("Tamaño de los paneles", "\(Int((cfg.hudScale * 100).rounded())) %")
            case .wristHud: return ("Vida y arma en la muñeca", yn(cfg.wristHud))
            case .wristScale: return ("Tamaño en la muñeca", "\(Int((cfg.wristScale * 100).rounded())) cm")
            case .wristSide: return ("Lado en la muñeca", cfg.wristHandSide ? "Hacia la mano" : "Hacia el codo")
            case .smoothMotion: return ("Suavizado del movimiento", yn(cfg.smoothMotion))
            case .turnMode: return ("Giro con la palanca derecha", Self.turnModeName())
            case .vignette: return ("Viñeta de confort", Self.vignetteName())
            case .hideHands: return ("Ocultar mis manos reales", yn(cfg.hideRealHands))
            case .newHands: return ("Manos del personaje", cfg.newHands ? "Nuevas" : "Originales")
            case .aiMenu: return ("IA de enemigos", "▸")
            case .diagnostics: return ("Diagnóstico", XRDiag.shared.settings.isNormal ? "▸" : "activo ▸")
            case .exit: return ("Salir de VR", "")
            }
        }
    }

    // Giro del stick y viñeta de confort: viven en los ajustes de vr_input (VRInputSettings, "vri.*"). El banco de render no enlaza VRInput.
    private static let turnModes: [(String, Bool, Float)] = [("Por pasos de 30°", false, 30), ("Por pasos de 45°", false, 45), ("Suave", true, 30)]
    private static let vignetteLevels: [(String, Float)] = [("No", 0), ("Suave", 0.35), ("Media", 0.6), ("Fuerte", 0.9)]
    private static func turnModeName() -> String {
#if MV_BENCH
        return turnModes[0].0
#else
        let s = VRInput.shared.settings
        if s.smoothTurn { return turnModes[2].0 }
        return s.snapDegrees >= 37.5 ? turnModes[1].0 : turnModes[0].0
#endif
    }
    private static func cycleTurnMode(_ dir: Int) {
#if !MV_BENCH
        let s = VRInput.shared.settings
        var i = s.smoothTurn ? 2 : (s.snapDegrees >= 37.5 ? 1 : 0)
        i = (i + dir + turnModes.count) % turnModes.count
        VRInput.shared.update { $0.smoothTurn = turnModes[i].1; if !turnModes[i].1 { $0.snapDegrees = turnModes[i].2 } }
#endif
    }
    private static func vignetteIndex() -> Int {
#if MV_BENCH
        return 0
#else
        let v = VRInput.shared.settings.vignette
        var best = 0
        for (i, l) in vignetteLevels.enumerated() where abs(l.1 - v) < abs(vignetteLevels[best].1 - v) { best = i }
        return best
#endif
    }
    private static func vignetteName() -> String { vignetteLevels[vignetteIndex()].0 }
    private static func cycleVignette(_ dir: Int) {
#if !MV_BENCH
        let i = (vignetteIndex() + dir + vignetteLevels.count) % vignetteLevels.count
        VRInput.shared.update { $0.vignette = vignetteLevels[i].1 }
#endif
    }

    private func drawMenu() {
#if MV_BENCH
        return
#else
        guard !menuTex.isEmpty else { return }
        let w = Self.menuW, h = Self.menuH
        guard let ctx = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w * 4,
                                  space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else { return }
        ctx.translateBy(x: 0, y: CGFloat(h))
        ctx.scaleBy(x: 1, y: -1)
        UIGraphicsPushContext(ctx)
        UIColor(white: 0.07, alpha: 1).setFill()
        UIBezierPath(roundedRect: CGRect(x: 0, y: 0, width: w, height: h), cornerRadius: 56).fill()
        let title: [NSAttributedString.Key: Any] = [.font: UIFont.systemFont(ofSize: 44, weight: .semibold), .foregroundColor: UIColor.white]
        ((page == 1 ? "Diagnóstico" : (page == 2 ? "IA de enemigos" : "Opciones VR")) as NSString).draw(at: CGPoint(x: 60, y: 36), withAttributes: title)
        let right = NSMutableParagraphStyle()
        right.alignment = .right
        let rowH: CGFloat = 40
        for (i, r) in rows().enumerated() {
            let y = 106 + CGFloat(i) * rowH
            let on = i == sel
            if on {
                UIColor(red: 0.12, green: 0.45, blue: 0.40, alpha: 1).setFill()
                UIBezierPath(roundedRect: CGRect(x: 36, y: y - 4, width: CGFloat(w) - 72, height: rowH - 4), cornerRadius: 14).fill()
            }
            let a: [NSAttributedString.Key: Any] = [.font: UIFont.systemFont(ofSize: 28, weight: on ? .semibold : .regular),
                                                    .foregroundColor: UIColor.white]
            (r.0 as NSString).draw(at: CGPoint(x: 60, y: y), withAttributes: a)
            if !r.1.isEmpty {
                let v: [NSAttributedString.Key: Any] = [.font: UIFont.systemFont(ofSize: 28, weight: .regular),
                                                        .foregroundColor: UIColor(white: on ? 1 : 0.75, alpha: 1), .paragraphStyle: right]
                let txt = on ? "◀  \(r.1)  ▶" : r.1
                (txt as NSString).draw(in: CGRect(x: 380, y: y, width: CGFloat(w) - 380 - 60, height: rowH), withAttributes: v)
            }
        }
        let help: [NSAttributedString.Key: Any] = [.font: UIFont.systemFont(ofSize: 24), .foregroundColor: UIColor(white: 0.6, alpha: 1)]
        ((page != 0 ? "Palanca: mover y cambiar · Gatillo: elegir · B: volver · Menú: cerrar" : "Palanca: mover y cambiar · Gatillo: elegir · B: cerrar · Menú: cerrar") as NSString).draw(at: CGPoint(x: 60, y: CGFloat(h) - 56), withAttributes: help)
        UIGraphicsPopContext()
        guard let data = ctx.data else { return }
        menuIdx = (menuIdx + 1) % menuTex.count
        menuTex[menuIdx].replace(region: MTLRegionMake2D(0, 0, w, h), mipmapLevel: 0, withBytes: data, bytesPerRow: w * 4)
#endif
    }

    // MARK: composición

    private static let hudRegions: [(u0: Float, v0: Float, u1: Float, v1: Float, x: Float, y: Float)] = [
        (0.00, 0.00, 0.35, 0.17, -0.52, 0.354),   // arriba izquierda: vida y oxígeno (acaba en v 0,17: los créditos de partida empiezan en y 45 de 256 = v 0,176). y 0,354: el borde superior queda donde estaba (centro original 0,30 con v 0..0,35)
        (0.00, 0.17, 0.35, 0.35, -0.52, 0.249),   // izquierda bajo la vida: créditos y textos (framed); y 0,249 = la posición que tenía esa franja de la región original (0,30 en v 0,175; 0,6 de y por unidad de v)
        (0.35, 0.00, 0.65, 0.35, 0.00, 0.38),     // arriba centro: alertas
        (0.65, 0.00, 1.00, 0.35, 0.52, 0.30),     // arriba derecha (el radar va en el antebrazo; su marco mide u 0,73-0,95, v 0,06-0,27)
        (0.00, 0.35, 1.00, 0.62, 0.00, 0.00),     // centro: cualquier otra cosa (casi siempre vacío)
        (0.00, 0.62, 0.25, 1.00, -0.50, -0.34),   // abajo izquierda: objeto equipado
        (0.25, 0.62, 0.75, 1.00, 0.00, -0.44),    // abajo centro: subtítulos
        (0.75, 0.62, 1.00, 1.00, 0.50, -0.34),    // abajo derecha: arma equipada y munición
    ]

    /// Marco del radar de muñeca (espacio recentrado): centro, ejes unitarios (derecha = hacia la mano) y medios lados. Los paneles del HUD
    /// de la muñeca se colocan en el mismo plano, a continuación del radar.
    struct Wrist {
        var center: SIMD3<Float>, right: SIMD3<Float>, up: SIMD3<Float>
        var halfW: Float, halfH: Float
    }

    /// Qué es HUD y qué no (vista mundo): las regiones de vida/oxígeno, alertas, radar, objeto, subtítulos y arma/munición son HUD: van a paneles pequeños (o al antebrazo). Lo demás 2D de partida
    /// (franja de créditos y texto bajo la vida, «cualquier otra cosa») NO es HUD: va en un plano fijo a la cabeza con el mismo tamaño angular que la ventana de la lista en el cine (cfg.width a cfg.distance),
    /// alineado con el encuadre del juego; nunca en la mano ni en la muñeca.
    private static let hudIsFramed = [false, true, false, false, true, false, false, false]

    /// Recorte del panel 2D -> ranura de la muñeca (-1: flota delante de la vista). La vida/oxígeno, el objeto y el arma/munición van al brazo.
    private static let wristSlot = [0, -1, -1, -1, -1, 1, -1, 2]

    private static func wristModel(_ w: Wrist) -> (n: SIMD3<Float>, ok: Bool) {
        let n = simd_cross(w.right, w.up)
        let l = simd_length(n)
        let vals = [w.center.x, w.center.y, w.center.z, w.right.x, w.right.y, w.right.z, w.up.x, w.up.y, w.up.z, w.halfW, w.halfH, l]
        guard vals.allSatisfy({ $0.isFinite }), l > 0.5, w.halfW > 0.001, w.halfH > 0.001 else { return (n, false) }
        return (n / l, true)
    }

    /// `hud`: panel 2D (textura del renderer, ya la vista sRGB si el destino es lineal). `eyePx`/`p00`: ancho en píxeles de un ojo y P[0][0].
    func compose(gs: MvVrGameState, head: simd_float4x4, dt: Float, hud: MTLTexture?, eyePx: Float, p00: Float,
                 gameView: simd_float4x4, linear: Bool, wrist: Wrist? = nil, ipd: Float = 0.063, shot: UInt32 = 0, stereo2DInside: Bool = false, hudMenuUV: SIMD4<Float>? = nil, hudLogical: SIMD2<Float>? = nil, hudWindow: MTLTexture? = nil, hudFull: MTLTexture? = nil) -> Result {
        diagK = p00 > 0 && eyePx > 0 ? p00 * eyePx / 2 : 0      // diagnóstico del filtro 2D: píxeles del visor de un objeto de `s` m a `d` m = s * diagK / d
        // 1) qué toca mostrar. view_kind 2 (vr_view.h, VRV_K_ENGINE) = cinemática del motor con 3D y cámara del guion; los vídeos pregrabados (3), los menús, el códec/radio,
        //    las cargas, la pausa y las ventanas de armas/objetos (0) siguen en la pantalla de cine.
        var target: Display = gs.view_mode == 1 ? .world : .cinema
        if target == .cinema && gs.view_kind == 2 && Self.gameViewOK(gameView) {
            switch cfg.cutscene {
            case .screen, .stereo: target = .stereo
            case .diorama: target = .diorama
            case .scene: target = .scene
            case .cinema: break
            }
        }
        // Transición con control pero sin cámara VR todavía (fin de radio / de escena, VRV_R_PENDING = 12): la vista queda congelada y se oscurece; nunca se enseña el fotograma
        // plano del motor (cenital, baja resolución) como paso intermedio. El destino es el mundo.
        let pending = gs.view_reason == 12
        // Fundido a negro de carga (vr_view.h, VrView.black): el motor desmonta / carga la zona con una vista nativa en pantalla, o aún no hay una escena nueva completa. La escena nativa
        // retenida no se puede enseñar (el juego reescribe la VRAM de la que salen sus texturas): negro en 0,15 s y hasta que el anfitrión lo levante.
        let blackout = gs.view_black != 0
        // El mundo ya se pide (motivo 0) pero la histéresis del anfitrión aún no ha cambiado de modo (3 VBlanks): el velo no baja en ese hueco sobre la pantalla que se va.
        let worldDue = gs.view_mode != 1 && gs.view_reason == 0
        if (target != .world || shown != .world) && !pending { cinema?.update(); cinema?.setHighRes(hqFull); cinema?.prepareUpscale(video: gs.view_kind == 3) }
        // Dentro de la escena: la cámara del plano se fija al empezar el plano; en cada corte nuevo anclaje y fundido rápido (0,15 s).
        if shown != .scene { shotView = gameView; shotSeen = shot }
        if shot != shotSeen {
            shotSeen = shot
            if shown == .scene { shotView = gameView; needAnchor = true; cutVeil = 1 }
        }

        // 2) transición: negro -> cambio -> negro (0,22 s + 0,28 s)
        let d = min(max(dt, 0.001), 0.1)
        if target != shown {
            veil += d / 0.22
            if veil >= 1 || pendVeil >= 1 { veil = 1; shown = target; needAnchor = true }      // ya en negro por la espera o la carga: el cambio de pantalla no espera a otro fundido
        } else {
            veil = max(0, veil - d / 0.28)
        }

        // 3) anclas
        let hp = XRMath.position(head)
        let hy = XRMath.yaw(of: head)
        // Ventana 2D del juego sobre el mundo (selector de armas/objetos, pausa): ver 5b.
        let overlay2D = shown == .world && gs.view_mode == 1 && XRMenuPanelLayout.isOverlay(reason: gs.view_reason)
        // Seguimiento de la cabeza por fotograma de render (todo el tiempo, para que al activar la opción o abrir una pantalla ya esté en su sitio). Se salta el suavizado
        // al recentrar, al abrir el menú, al aparecer una ventana 2D y al colocar una pantalla nueva tras la transición (el velo está en negro).
        let snapNow = followSnap || (overlay2D && !prevOverlay2D) || (needAnchor && shown != .world)
        XRFollow.step(&follow, head: head, dt: d, snap: snapNow)
        // El menú VR se queda quieto mientras haya un puntero, rayo o entrada activa sobre él (menuHold > 0): no se mueve bajo el dedo; al soltar vuelve a seguir la cabeza.
        if !(menuOpen && menuHold > 0) { XRFollow.step(&menuFollow, head: head, dt: d, snap: snapNow || menuSnap); menuSnap = false }
        followSnap = false
        prevOverlay2D = overlay2D
        hudSeen = hud != nil
        if !overlay2D { overlayAim = nil } else if overlayAim == nil { overlayAim = (hy, XRFollow.pitch(of: head)) }
        if needAnchor && shown != .world {
            anchorY = max(0.8, hp.y - (shown == .scene ? 0 : 0.05))
            anchor = XRMath.translation(SIMD3<Float>(hp.x, 0, hp.z)) * XRMath.rotationY(hy)
            needAnchor = false
        } else if shown == .stereo || (shown == .cinema && !cfg.follow2D) {      // la pantalla 3D siempre va fija en el mundo (ver screenModel)
            // Seguimiento perezoso de la pantalla (cine y pantalla 3D): al mirar a otro lado más de ~35 grados o alejarse más de 0,5 m, la pantalla se
            // recoloca suavemente delante de la vista (antes se quedaba donde se creó: muerte, cámaras de evento y escenas cortas se perdían a la espalda).
            // La cabeza dentro de la zona muerta no la mueve, así que no hay arrastre al mirar alrededor de la imagen.
            var ay = XRMath.yaw(of: anchor)
            let da = atan2f(sinf(hy - ay), cosf(hy - ay))
            if abs(da) > Self.followDead { ay += (da - (da > 0 ? Self.followDead : -Self.followDead)) * (1 - expf(-d / 0.6)) }
            var ax = anchor.columns.3.x, az = anchor.columns.3.z
            let ddx = hp.x - ax, ddz = hp.z - az
            if (ddx * ddx + ddz * ddz).squareRoot() > 0.5 { let k = 1 - expf(-d / 0.8); ax += ddx * k; az += ddz * k }
            anchor = XRMath.translation(SIMD3<Float>(ax, 0, az)) * XRMath.rotationY(ay)
        }
        if !hudInit { hudYaw = hy; hudInit = true }
        var dy = hy - hudYaw
        dy = atan2f(sinf(dy), cosf(dy))
        let dead: Float = 0.2
        if abs(dy) > dead { hudYaw += (dy - (dy > 0 ? dead : -dead)) * (1 - expf(-d / 0.25)) }
        hudYaw = atan2f(sinf(hudYaw), cosf(hudYaw))

        var r = Result()
        r.drawWorld = shown != .cinema
        r.drawBody = shown == .world
        r.drawEyes = shown != .stereo
        cutVeil = max(0, cutVeil - d / 0.15)

        // 4) pantalla de cine (también como respaldo del diorama sin geometría)
        // Cinemática del motor: la pantalla de cine plana (imagen de la VRAM, baja resolución) no se dibuja nunca (ni en el fundido de entrada, con la pantalla aún en .cinema, ni sin cámara del juego): solo el render nativo; si no está listo, negro.
        let nativeOnly = gs.view_kind == 2 && cfg.cutscene != .cinema
        if shown == .cinema, !nativeOnly, let c = cinema, let tex = c.texture(srgb: linear) {
            let leg = XRCinemaLayout.legible(reason: Int32(gs.view_reason), kind: gs.view_kind)
            var W = XRCinemaLayout.width(base: cfg.width, legible: leg)
            let D = cfg.distance
            if XRCinemaLayout.integerScale(base: cfg.integerScale, legible: leg), p00 > 0, eyePx > 0 {
                let ppm = p00 * eyePx / (2 * D)                       // píxeles del visor por metro, en el centro de la pantalla
                // Sobre el ancho LÓGICO del fotograma (c.w: 320/384 px del juego), no sobre el de la textura (c.width = lógico x escala de raster, que baja y sube bajo carga: la pantalla cambiaba de tamaño).
                W = XRCinemaLayout.snappedWidth(W, logicalWidth: c.w, pixelsPerMeter: ppm)
            }
            var it = XRDrawItem(kind: .screen, texture: tex)
            it.model = screenModel(D)
            it.size = SIMD2<Float>(W, W * XRCinemaLayout.aspect(width: c.width, height: c.height))       // la imagen es la ventana visible fija del televisor: siempre 4:3, en cualquier modo de vídeo
            it.distance = D
            let cv = XRCinemaLayout.curve(base: cfg.curve, legible: leg)
            it.curve = cv
            it.cols = cv > 0.01 ? 48 : 1
            it.imgSize = SIMD2<Float>(Float(c.width), Float(c.height))
            it.sharp = XR2DFilter.current == .nearest ? 2 : Float(max(XRCinemaLayout.sharp(base: cfg.sharp, legible: leg), 1))   // con filtro (MMPX / xBR / nítido): sharp-bilinear (nunca bilineal blando); nearest: vecino más próximo
            r.items.append(it)
        }

        // 4b) pantalla 3D: dos cámaras del juego (separadas un IPD, frustos desplazados) dibujadas por RGRenderer en texturas; cada ojo ve la suya en la pantalla anclada al mundo.
        //     La cabeza no interviene en las cámaras: solo cambia hacia dónde de la pantalla se mira.
        if shown == .stereo, Self.gameViewOK(gameView) {
            // Pantalla de cine 3D estéreo (defecto): dos cámaras del juego separadas un IPD real (profundidad).
            // Pantalla de cine 3D mono (opción .screen): una sola imagen (IPD 0: ambos ojos ven la misma textura) en la geometría y el tamaño de la pantalla de cine, sin profundidad.
            let mono = cfg.cutscene == .screen
            let leg = XRCinemaLayout.legible(reason: Int32(gs.view_reason), kind: gs.view_kind)
            let W = mono ? XRCinemaLayout.width(base: cfg.width, legible: leg) : cfg.width, D = cfg.distance
            let cv = mono ? XRCinemaLayout.curve(base: cfg.curve, legible: leg) : cfg.curve
            let tanX = max(0.2, W / (2 * D)), tanY = tanX * 0.75      // campo de visión = ángulo que la pantalla ocupa desde el usuario (escala real)
            do {
                r.stereo = RGRenderer.StereoRequest(ipd: mono ? 0 : ipd, tanX: tanX, tanY: tanY, width: Self.stereoW, height: Self.stereoH, distance: D)
                let model = screenModel(D, fixed: true)
                var it = XRDrawItem(kind: .screen3d, texture: nil)
                it.model = model
                it.size = SIMD2<Float>(W, W * 0.75)
                it.distance = D
                it.curve = cv
                it.cols = cv > 0.01 ? 48 : 1
                it.opacity = 1
                r.items.append(it)
                // subtítulos, créditos y bandas del juego (panel 2D completo) sobre la misma pantalla, 4 mm por delante. Con stereo2DInside (modo gpu) el 2D ya va dentro de las texturas de la pantalla 3D
                // (registrado con el 3D de su misma lista): no se pone el panel aparte.
                if let h = hud, !stereo2DInside {
                    var hp2 = XRDrawItem(kind: .panel, texture: h)
                    hp2.model = model * XRMath.translation(SIMD3<Float>(0, 0, 0.004))
                    // Registro con el 3D (solo en esta ruta): la pantalla 3D mide W x 0,75 W para 320 x 240 píxeles del juego centrados en el marco, así que el panel lleva el mismo paso W / 320 por píxel en
                    // los dos ejes y el desplazamiento del centro de la ventana visible respecto al centro del marco (XR2DWindow.panelOnScreen3D). Sin datos de ventana (HUD de reserva, 320x240): W x 0,75 W.
                    let lg = hudLogical ?? SIMD2<Float>(Float(h.width), Float(h.height))
                    hp2.uvrect = SIMD4<Float>(0, 0, 1, 1)
                    hp2.size = SIMD2<Float>(W, W * XRCinemaLayout.aspect(width: Int(lg.x.rounded()), height: Int(lg.y.rounded())))
                    if let g = hudWin?.panelOnScreen3D(screenW: W) {
                        hp2.size = g.size
                        hp2.model = model * XRMath.translation(SIMD3<Float>(g.offset.x, g.offset.y, 0.004))
                    }
                    hp2.distance = D
                    hp2.curve = cv
                    hp2.cols = it.cols
                    hp2.opacity = 1
                    hp2.onTop = true
                    hp2.pixelMode = XR2DFilter.current.panelMode
                    r.items.append(hp2)
                }
            }
        }

        // 4c) dentro de la escena: la cabeza en la cámara del plano (vista del motor en unidades -> metros a escala 1)
        if shown == .scene, Self.gameViewOK(shotView) {
            let s: Float = 0.001
            let f = simd_float4x4(diagonal: SIMD4<Float>(s, -s, -s, 1))
            r.dioramaModel = anchor * XRMath.translation(SIMD3<Float>(0, anchorY, 0)) * f * shotView.inverse
        }

        // 5) diorama
        if shown == .diorama, Self.dioramaOK(gameView) {
            let s = cfg.dioramaScale * 0.001
            let f = simd_float4x4(diagonal: SIMD4<Float>(s, -s, -s, 1))      // vista del juego (x der, y abajo, z delante; unidades) -> usuario (m)
            r.dioramaModel = anchor * XRMath.translation(SIMD3<Float>(0, anchorY - 0.1, -1.3)) * f * gameView.inverse
        }

        // Troceo del 2D (XRLayer2D.split, APAGADO por defecto): apagado, 5b y 6 no dibujan nada y todo el 2D (HUD, radar del juego, menús, pausa, textos, créditos) va en el panel único de 6b.
        let split = XRLayer2D.split
        // 5b) ventana 2D del juego sobre el mundo (selector de armas/objetos, pausa): el mundo sigue en primera persona y la capa 2D completa va en un panel flotante delante,
        //     por delante de la geometría. Sustituye a los paneles sueltos del HUD mientras dura (la capa 2D ya los trae).
        //     Si la abrió una mano (gesto del pecho: L2 izquierda, R2 derecha) el panel va sobre esa mano, de cara a la cabeza, y la sigue mientras dura; sin mano o sin pose válida, delante de la cabeza.
        if !overlay2D { overlayHand = -1 } else if overlayHand < 0 { overlayHand = Int(mv_vrgame_menu_hand()) }
        if split && overlay2D, let h = hud {
            var onHand: simd_float4x4?
            if overlayHand >= 0 {
                var g = [Float](repeating: 0, count: 16)
                if mv_vrgame_hand_matrix(Int32(overlayHand), &g) != 0 {
                    let grip = simd_float4x4(columns: (SIMD4<Float>(g[0], g[1], g[2], g[3]), SIMD4<Float>(g[4], g[5], g[6], g[7]),
                                                       SIMD4<Float>(g[8], g[9], g[10], g[11]), SIMD4<Float>(g[12], g[13], g[14], g[15])))
                    onHand = XRMenuPanelLayout.handModel(grip: grip, head: hp)
                }
            }
            var it = XRDrawItem(kind: .panel, texture: h)
            // Con rect de la ventana (hudMenuUV) solo ella va en el panel de la mano (la capa del selector ya no lleva créditos ni HUD, vr_layer2d.c) y los paneles del HUD (6) no se dibujan mientras dura.
            let mr = hudMenuUV ?? SIMD4<Float>(0, 0, 1, 1)
            // Aspecto de la capa: la ventana visible fija del televisor, siempre 4:3 (su tamaño en píxeles no es cuadrado).
            let la = XRFramedLayout.aspect
            let psize = XRMenuPanelLayout.handSize(uv: hudMenuUV, aspect: la)
            if let hm = onHand {
                it.model = hm
                it.size = psize
                it.distance = simd_length(XRMath.position(hm) - hp)
            } else {
                it.model = cfg.follow2D ? XRFollow.model(follow, distance: XRMenuPanelLayout.distance, drop: XRMenuPanelLayout.drop)
                                        : XRMenuPanelLayout.model(position: hp, yaw: hudYaw)
                it.size = hudMenuUV != nil ? psize : XRMenuPanelLayout.frontSize(uv: nil, aspect: la)       // con rect: el mismo tamaño fijo de la ventana; sin rect: la capa entera
                it.distance = XRMenuPanelLayout.distance
            }
            it.uvrect = mr
            it.opacity = 1
            it.onTop = true
            it.pixelMode = XR2DFilter.current.panelMode
            r.items.append(it)
        }

        // 6) paneles del HUD (mundo y diorama; en el cine el HUD ya va dentro de la imagen). Con una ventana 2D abierta (selector, pausa) no se dibuja ninguno (vida, créditos, textos de partida): solo el panel de 5b.
        if split && shown != .cinema && shown != .stereo && cfg.hudPanels && !overlay2D, let h = hud {
            let frame = XRMath.translation(SIMD3<Float>(hp.x, hp.y, hp.z)) * XRMath.rotationY(hudYaw)
            let sc = cfg.hudScale
            var wm: (n: SIMD3<Float>, ok: Bool)?
            if shown == .world && cfg.wristHud, let w = wrist { wm = Self.wristModel(w) }
            var slotOff: [Float] = [0, 0, 0]      // desplazamiento (m) del centro de cada ranura a lo largo del brazo, desde el centro del radar
            if let w = wrist, let m = wm, m.ok {
                var edge = w.halfW + 0.004
                for (i, rg) in Self.hudRegions.enumerated() where Self.wristSlot[i] >= 0 {
                    let wd = (rg.u1 - rg.u0) * cfg.wristScale
                    slotOff[Self.wristSlot[i]] = edge + wd / 2
                    edge += wd + 0.004
                }
            }
            for (i, rg) in Self.hudRegions.enumerated() {
                var it = XRDrawItem(kind: .panel, texture: h)
                it.pixelMode = XR2DFilter.current.panelMode
                if shown == .world && Self.hudIsFramed[i] {
                    // Plano del encuadre del juego (créditos y textos de partida): la capa entera es la ventana visible fija en 4:3 (XRFramedLayout), a un tamaño y una distancia cómodos que no dependen del
                    // tamaño de la pantalla de cine; solo se ve el recorte de la región, en su sitio del encuadre. Las regiones van en fracciones del MARCO del juego: se pasan a fracciones de la ventana.
                    let fuv = SIMD4<Float>(rg.u0, rg.v0, rg.u1, rg.v1)
                    let uv = hudWin?.frameUV(fuv) ?? fuv
                    let lp = hudLogical ?? SIMD2<Float>(Float(h.width), Float(h.height))
                    let c = XRFramedLayout.center(uv: uv, layerPx: lp)
                    it.model = frame * XRMath.translation(SIMD3<Float>(c.x, c.y, -XRFramedLayout.distance))
                    it.uvrect = uv
                    it.size = XRFramedLayout.size(uv: uv, layerPx: lp)
                    it.distance = XRFramedLayout.distance
                    it.opacity = 1
                    it.onTop = true
                    r.items.append(it)
                    continue
                }
                if Self.wristSlot[i] >= 0, let w = wrist, let m = wm, m.ok {
                    // Plano del radar (+0,002 m hacia el espectador para no solaparse con el cuerpo), hacia el codo (-derecha) o la mano.
                    let dir: Float = cfg.wristHandSide ? 1 : -1
                    let pos = w.center + m.n * 0.002 + w.right * (dir * slotOff[Self.wristSlot[i]])
                    it.model = simd_float4x4(columns: (SIMD4<Float>(w.right, 0), SIMD4<Float>(w.up, 0), SIMD4<Float>(m.n, 0), SIMD4<Float>(pos, 1)))
                    it.uvrect = SIMD4<Float>(rg.u0, rg.v0, rg.u1, rg.v1)
                    if let hw = hudWin { it.uvrect = hw.frameUV(it.uvrect) }
                    it.size = SIMD2<Float>((rg.u1 - rg.u0) * cfg.wristScale, (rg.v1 - rg.v0) * cfg.wristScale * 0.75 * (hudWin.map { $0.dh / $0.h } ?? 1))   // v en fracciones del marco (dh filas), no de la ventana (h)
                    it.opacity = 1      // pegado al antebrazo: con prueba de profundidad normal (no onTop)
                    r.items.append(it)
                    continue
                }
                it.model = frame * XRMath.translation(SIMD3<Float>(rg.x * sc / 0.8, rg.y * sc / 0.8, -1.4))
                it.uvrect = SIMD4<Float>(rg.u0, rg.v0, rg.u1, rg.v1)
                if let hw = hudWin { it.uvrect = hw.frameUV(it.uvrect) }
                it.size = SIMD2<Float>((rg.u1 - rg.u0) * sc, (rg.v1 - rg.v0) * sc * 0.75 * (hudWin.map { $0.dh / $0.h } ?? 1))   // v en fracciones del marco (dh filas), no de la ventana (h)
                it.opacity = 0.92
                it.onTop = true
                r.items.append(it)
            }
        }

        // 6b) UN solo panel con la capa 2D COMPLETA del juego (mundo y diorama; en el cine el 2D ya va dentro de la imagen): HUD, radar del juego, menús de objetos/armas (L2/R2), pausa, textos y créditos tal como los dibuja
        //     el original, con transparencia donde el juego no dibuja 2D. Geometría de pantalla completa XRFramedLayout (la ventana visible fija del televisor, siempre 4:3) delante de la cabeza con
        //     el mismo seguimiento perezoso del rumbo (hudYaw); sin prueba de profundidad (onTop). El muestreo es el de XR2DFilter.current (MMPX x2 + sharp-bilinear por defecto). «Paneles del HUD» (cfg.hudPanels) solo
        //     afecta al troceo (paso 6): sin trocear, el panel único se dibuja siempre (vida, radar, créditos). Es la capa HUD (clases HUD_WORLD, SCOPE_TEXT, UNKNOWN de vr_prov.h) y queda fija delante de la cabeza con el rumbo perezoso;
        //     con la ventana 2D abierta (selector, pausa) se acerca al mismo plano que la ventana (6c), centrada en la mirada fijada al abrirse (overlayAim).
        if !split && shown != .cinema && shown != .stereo, let h = hud {
            let uv = XRFramedLayout.fullUV
            let lp = hudLogical ?? SIMD2<Float>(Float(h.width), Float(h.height))
            let c = XRFramedLayout.center(uv: uv, layerPx: lp)
            var it = XRDrawItem(kind: .panel, texture: h)
            it.model = XRMath.translation(SIMD3<Float>(hp.x, hp.y, hp.z)) * XRMath.rotationY(hudYaw) * XRMath.translation(SIMD3<Float>(c.x, c.y, -XRFramedLayout.distance))
            it.size = XRFramedLayout.size(uv: uv, layerPx: lp)
            it.distance = XRFramedLayout.distance
            // Con la ventana 2D abierta (selector de armas R2 / objetos L2, pausa) la capa HUD va en el MISMO plano que la ventana (6c): partes de un mismo menú caen en una u otra capa según el escritor que
            // las dibuje (vr_prov: ids de ventana aprendidos, UNKNOWN/HUD_WORLD sin aprender o degradados), y a 1,6 m frente a 0,9 m el menú se veía partido en dos profundidades.
            if overlay2D, let aim = overlayAim {
                it.model = XRFramedLayout.overlayModel(position: hp, yaw: aim.yaw, pitch: aim.pitch)
                it.size = it.size * XRFramedLayout.overlayScale
                it.distance = XRFramedLayout.overlayDistance
            }
            it.uvrect = uv
            it.opacity = 1
            it.onTop = true
            it.pixelMode = XR2DFilter.current.panelMode
            r.items.append(it)
        }
        // 6c) capa WINDOW (ventana del selector / pausa, clase de procedencia aprendida): lo único que se acerca (XRFramedLayout.overlayDistance) y se centra en la mirada fijada al abrirse (overlayAim); va tras el HUD fijo.
        if !split && shown != .cinema && shown != .stereo && overlay2D, let aim = overlayAim, let wh = hudWindow {
            let uv = XRFramedLayout.fullUV
            let wp = SIMD2<Float>(Float(wh.width), Float(wh.height))
            var it = XRDrawItem(kind: .panel, texture: wh)
            it.model = XRFramedLayout.overlayModel(position: hp, yaw: aim.yaw, pitch: aim.pitch)
            it.size = XRFramedLayout.size(uv: uv, layerPx: wp) * XRFramedLayout.overlayScale
            it.distance = XRFramedLayout.overlayDistance
            it.uvrect = uv
            it.opacity = 1
            it.onTop = true
            it.pixelMode = XR2DFilter.current.panelMode
            r.items.append(it)
        }
        // 6d) capa FULL (pantalla completa del juego: créditos, telop): panel plano con la misma geometría fija que el HUD, tras él.
        if !split && shown != .cinema && shown != .stereo, let fh = hudFull {
            let uv = XRFramedLayout.fullUV
            let fp = SIMD2<Float>(Float(fh.width), Float(fh.height))
            let c = XRFramedLayout.center(uv: uv, layerPx: fp)
            var it = XRDrawItem(kind: .panel, texture: fh)
            it.model = XRMath.translation(SIMD3<Float>(hp.x, hp.y, hp.z)) * XRMath.rotationY(hudYaw) * XRMath.translation(SIMD3<Float>(c.x, c.y, -XRFramedLayout.distance))
            it.size = XRFramedLayout.size(uv: uv, layerPx: fp)
            it.distance = XRFramedLayout.distance
            it.uvrect = uv
            it.opacity = 1
            it.onTop = true
            it.pixelMode = XR2DFilter.current.panelMode
            r.items.append(it)
        }

        // 7) velo de transición y menú
        // El velo de espera / de carga no baja mientras la pantalla mostrada no sea la pedida: bajarlo a la vez que sube el de la transición dejaba ver la vista vieja a media luz.
        if pending || blackout || worldDue { pendVeil = min(1, pendVeil + d / 0.15) } else if target == shown { pendVeil = max(0, pendVeil - d / 0.28) }
        let veilNow = max(veil, cutVeil, collisionFade, pendVeil)
        if veilNow > 0.001 {
            var v = XRDrawItem(kind: .veil, texture: nil)
            v.opacity = veilNow
            r.items.append(v)
        }
        if menuOpen {
            if cfg.follow2D && menuHold <= 0 { menuModel = XRMenuLayout.model(position: menuFollow.pos, yaw: menuFollow.yaw) }
            if menuDirty { drawMenu(); menuDirty = false }
            if !menuTex.isEmpty {
                var it = XRDrawItem(kind: .menu, texture: linear ? menuTexSRGB[menuIdx] : menuTex[menuIdx])
                it.model = menuModel
                it.size = SIMD2<Float>(XRMenuLayout.width, XRMenuLayout.height(w: Self.menuW, h: Self.menuH))
                it.opacity = 1
                r.items.append(it)
            }
        }
        return r
    }

    /// Pantalla de cine / pantalla 3D: `D` m delante de la cabeza suavizada (siguen la cabeza) o en el ancla fija del mundo.
    /// `fixed`: la pantalla 3D (escenas del motor en estéreo, vista de pared) va SIEMPRE fija en el mundo aunque la opción «Pantallas 2D» sea seguir la cabeza: una imagen 3D en movimiento
    /// de ~70 grados que persigue cada giro de la cabeza (zona muerta de 2,5 grados) no deja ninguna referencia quieta y marea. Solo se recoloca, despacio, si se mira más de ~35 grados
    /// a un lado o uno se aleja más de 0,5 m (rama de `compose`).
    private func screenModel(_ D: Float, fixed: Bool = false) -> simd_float4x4 {
        (cfg.follow2D && !fixed) ? XRFollow.model(follow, distance: D, drop: 0.05) : anchor * XRMath.translation(SIMD3<Float>(0, anchorY, -D))
    }

    /// Codifica los trabajos de la GPU de la presentación (xBR de la imagen de cine); antes del pase final.
    func encodeUpscale(_ cb: MTLCommandBuffer) { cinema?.encodeUpscale(cb) }

    /// Estado de la pantalla de cine para el diagnóstico [radio-src]: capa 2D fiel en uso y xBR aplicado.
    var cineSrc: (hq: Bool, xbr: Bool) { (cinema?.hqActive ?? false, cinema?.xbrActive ?? false) }

    private var diagK: Float = 0
    /// Resumen para el registro (1 vez por segundo): qué presentación está activa, dónde está anclada la pantalla y qué se dibuja.
    func diagnostics(items: [XRDrawItem]) -> String {
        var n = [XRDrawItem.Kind: Int]()
        for it in items { n[it.kind, default: 0] += 1 }
        let a = anchor.columns.3
        var d = "mostrando=\(shown) " + String(format: "velo=%.2f ancla=(%.2f %.2f %.2f) rumbo=%.0fgrados alturaAncla=%.2f", veil, a.x, a.y, a.z,
                                               XRMath.yaw(of: anchor) * 180 / .pi, anchorY)
        d += String(format: " veloNegro=%.2f", pendVeil)
        d += " pantallas2D=" + (cfg.follow2D ? "siguen" : "fijas")
        d += " descartes3D=detras:" + (cfg.engBehind ? "si" : "no") + ",tamano:" + (cfg.engSize ? "si" : "no") + ",reversos:" + (cfg.engBack ? "si" : "no") + ",caras:" + (cfg.engSplit ? "original" : "recortadas")
        d += " items: pantalla=\(n[.screen] ?? 0) pantalla3d=\(n[.screen3d] ?? 0) panel=\(n[.panel] ?? 0) menu=\(n[.menu] ?? 0) velo=\(n[.veil] ?? 0) hudPanels=\(cfg.hudPanels) hud!=nil=\(hudSeen)"
        if let c = cinema {
            d += " cine: fotograma=\(c.haveFrame) \(c.w)x\(c.h) tex=\(c.texture(srgb: true).map { "\($0.width)x\($0.height)/fmt\($0.pixelFormat.rawValue)" } ?? "nil")"
        } else {
            d += " cine: sin textura"
        }
        for it in items where it.kind == .screen {
            let p = it.model.columns.3
            d += String(format: " pantalla: pos=(%.2f %.2f %.2f) tam=%.2fx%.2f dist=%.2f curva=%.2f", p.x, p.y, p.z, it.size.x, it.size.y, it.distance, it.curve)
            d += String(format: " px_visor=%.0f", it.size.x * diagK / max(it.distance, 0.01))
        }
        // Filtro 2D efectivo (MMPX mejora solo si la textura muestreada es mayor que la nativa) y, por panel 2D, textura muestreada y píxeles que ocupa en el visor (ancho).
        d += " filtro2D=\(XR2DFilter.current)(guardado=\(XR2DFilter.storedIndex),env=\(getenv("MV_2D_FILTER") != nil ? "si" : "no")) factor=\(XR2DFilter.upscales ? XR2DFilter.factor : 1) cine_ampliado=\(cinema?.xbrActive ?? false)"
        for it in items where it.kind == .panel {
            d += " panel2D: tex=\(it.texture?.width ?? 0)x\(it.texture?.height ?? 0)" + String(format: " tam=%.2fm px_visor=%.0f", it.size.x, it.size.x * diagK / max(it.distance, 0.01))
        }
        return d
    }

    /// Resolución de las texturas de la pantalla 3D (por ojo).
    static let stereoW = 1280, stereoH = 960
    /// Zona muerta (rad, ~35 grados) del seguimiento de la pantalla de cine / pantalla 3D.
    static let followDead: Float = 0.61

    /// `game_view` es una transformación vista del motor (unidades) -> metros: su determinante es ~1e-9 (0,001 al cubo), así que el umbral va muy por debajo.
    private static func gameViewOK(_ m: simd_float4x4) -> Bool {
        guard XRMath.isFinite(m) else { return false }
        let det = simd_determinant(m)
        return det.isFinite && abs(det) > 1e-13
    }
    private static func dioramaOK(_ m: simd_float4x4) -> Bool { gameViewOK(m) }
}
