import Foundation
import Metal
import simd
import CoreGraphics
import UIKit

// Presentación en VR de todo lo que NO es el juego en primera persona (ver bitácora, "Presentación en VR").
//  - Pantalla de cine: el fotograma completo que dibuja el juego (título, menús, códec, cargas, vídeos, cinemáticas del motor, ventanas de
//    armas/objetos, pausa, fin de partida) en una pantalla virtual grande, anclada delante del usuario, en un entorno negro.
//  - Diorama (opcional, solo cinemáticas del motor): la escena 3D vista desde la cámara del juego, en miniatura delante del usuario.
//  - HUD del juego en primera persona: recortes del panel 2D como paneles pequeños que siguen a la cabeza con retardo.
//  - Menú de opciones VR: se abre manteniendo el botón de menú izquierdo (o Select del mando) y se maneja con el mando.
// Todo en el hilo de render. Sin llamadas al compositor: solo compone `XRDrawItem` para XRFinalPass.

/// Ajustes de presentación (UserDefaults; se leen al crear el presentador y tras cada cambio del menú).
struct XRViewConfig {
    var width: Float = 3.0          // ancho de la pantalla de cine (m); alto = 3/4
    var distance: Float = 2.5       // distancia de la pantalla (m)
    var curve: Float = 0.35         // 0 plana .. 1 cilindro con el usuario en el eje
    var sharp = 1                   // 0 suave, 1 nítida (bilineal de píxel fijo), 2 píxel (vecino más próximo)
    var integerScale = false        // ajusta el ancho para que cada texel ocupe un número entero de píxeles del visor
    var diorama = false             // cinemáticas del motor: false = pantalla de cine, true = diorama
    var dioramaScale: Float = 0.15  // metros por metro del motor (0,15: una sala de 8 m cabe en ~1,2 m)
    var hudPanels = true
    var hudScale: Float = 0.8
    var wristHud = true             // vida, objeto y arma/munición del HUD sobre el antebrazo izquierdo, junto al radar
    var wristScale: Float = 0.2     // ancho (m) que ocuparía el panel 2D completo en la muñeca (el recorte de la vida mide ~35 % de él)
    var wristHandSide = false       // false: los paneles se apilan hacia el codo (encima del radar); true: hacia la mano

    static let widths: [Float] = [2.4, 3.0, 3.6, 4.5]
    static let distances: [Float] = [2.0, 2.5, 3.0, 4.0]
    static let curves: [Float] = [0, 0.35, 0.7, 1.0]
    static let hudScales: [Float] = [0.6, 0.8, 1.0, 1.3]
    static let wristScales: [Float] = [0.16, 0.2, 0.26, 0.34]
    static let dioramaScales: [Float] = [0.10, 0.15, 0.22]

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
        c.diorama = (UserDefaults.standard.string(forKey: "xr.view.cutscene") ?? "cine") == "diorama"
        c.dioramaScale = f("xr.view.dioramaScale", 0.15, 0.03, 0.5)
        c.hudPanels = b("xr.view.hud", true)
        c.hudScale = f("xr.view.hudScale", 0.8, 0.3, 2.0)
        c.wristHud = b("xr.view.wristHud", true)
        c.wristScale = f("xr.view.wristScale", 0.2, 0.1, 0.5)
        c.wristHandSide = b("xr.view.wristHand", false)
        return c
    }

    func save() {
        let d = UserDefaults.standard
        d.set(Double(width), forKey: "xr.view.width")
        d.set(Double(distance), forKey: "xr.view.distance")
        d.set(Double(curve), forKey: "xr.view.curve")
        d.set(sharp, forKey: "xr.view.sharp")
        d.set(integerScale, forKey: "xr.view.integer")
        d.set(diorama ? "diorama" : "cine", forKey: "xr.view.cutscene")
        d.set(Double(dioramaScale), forKey: "xr.view.dioramaScale")
        d.set(hudPanels, forKey: "xr.view.hud")
        d.set(Double(hudScale), forKey: "xr.view.hudScale")
        d.set(wristHud, forKey: "xr.view.wristHud")
        d.set(Double(wristScale), forKey: "xr.view.wristScale")
        d.set(wristHandSide, forKey: "xr.view.wristHand")
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
        let td = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm, width: Int(MV_HOST_MAX_W), height: Int(MV_HOST_MAX_H), mipmapped: false)
        td.usage = [.shaderRead, .pixelFormatView]
        td.storageMode = .shared
        for i in 0..<4 {
            guard let t = device.makeTexture(descriptor: td), let v = t.makeTextureView(pixelFormat: .rgba8Unorm_srgb) else { return nil }
            t.label = "mv.cine.\(i)"
            ring.append(t); ringSRGB.append(v)
        }
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
    }

    func texture(srgb: Bool) -> MTLTexture? { haveFrame ? (srgb ? ringSRGB[idx] : ring[idx]) : nil }
}

/// Entrada del menú (flancos ya detectados).
struct XRNav: Equatable {
    var up = false, down = false, left = false, right = false, select = false, back = false
}

enum XRMenuItem: Int, CaseIterable {
    case resume, recenter, width, distance, curve, sharp, integerScale, cutscene, hud, hudScale, wristHud, wristScale, wristSide, exit
}

final class XRPresenter {
    enum Display { case world, cinema, diorama }

    struct Result {
        var items: [XRDrawItem] = []
        var drawWorld = true
        var drawBody = true
        /// Diorama: se multiplica a la vista de cada ojo (mundo VR del juego en metros -> espacio recentrado del usuario).
        var dioramaModel: simd_float4x4?
    }

    private(set) var cfg = XRViewConfig.load()
    private let cinema: XRCinema?
    private(set) var shown: Display = .cinema
    private(set) var veil: Float = 1

    // Anclas (espacio recentrado)
    private var needAnchor = true
    private var anchor = matrix_identity_float4x4          // suelo bajo la cabeza, rumbo de la cabeza al pasar a cine
    private var anchorY: Float = 1.5
    private var hudYaw: Float = 0
    private var hudInit = false

    // Menú
    private(set) var menuOpen = false
    private var sel = 0
    private var menuDirty = true
    private var menuModel = matrix_identity_float4x4
    private var menuTex: [MTLTexture] = []
    private var menuTexSRGB: [MTLTexture] = []
    private var menuIdx = 0
    private var holdT: Float = 0
    private var prevHeld = false
    private var prevNav = XRNav()
    private var blockUntilNeutral = false
    private var blockT: Float = 0
    private var needRelease = false        // tras cerrar con el botón de menú: hasta soltarlo no cuenta para reabrir
    var onRecenter: (() -> Void)?
    var onExit: (() -> Void)?

    private static let menuW = 1024, menuH = 768

    init(device: MTLDevice) {
        cinema = XRCinema(device: device)
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
    func reanchor() { needAnchor = true }

    // MARK: entrada / menú

    /// Entrada del menú VR. Devuelve true si el juego NO debe recibir la entrada de este fotograma (menú abierto, o recién cerrado
    /// con algo aún pulsado: evita un disparo o un movimiento accidental).
    func updateInput(_ s: ControllerHub.Sample, dt: Float, head: simd_float4x4) -> Bool {
        let held = s.left.menu || (s.kind == .gamepad && (s.pad.buttons & 1) != 0)   // Create/Options izquierdo o Select del mando
        if menuOpen {
            if held && !prevHeld { closeMenu() }
        } else if held && !needRelease {
            holdT += dt
            if holdT >= 0.8 { openMenu(head: head); holdT = 0 }
        } else {
            holdT = 0
        }
        if !held { needRelease = false }
        prevHeld = held

        if menuOpen {
            let nav = Self.nav(from: s)
            let p = prevNav
            prevNav = nav
            if nav.up && !p.up { sel = (sel + XRMenuItem.allCases.count - 1) % XRMenuItem.allCases.count; menuDirty = true }
            if nav.down && !p.down { sel = (sel + 1) % XRMenuItem.allCases.count; menuDirty = true }
            if nav.left && !p.left { activate(dir: -1) }
            if nav.right && !p.right { activate(dir: 1) }
            if nav.select && !p.select { activate(dir: 1) }
            if nav.back && !p.back { closeMenu() }
        } else {
            prevNav = XRNav()
        }
        if !menuOpen && blockUntilNeutral {
            // Tras cerrar: entrada bloqueada hasta que todo esté en neutro (máx. 2 s: un stick con deriva o un grip sostenido no la dejan bloqueada).
            blockT += dt
            if blockT > 2 || !(s.pad.active || s.left.active || s.right.active) { blockUntilNeutral = false }
        }
        // Mientras se mantiene el botón de menú (>0,3 s) el juego no lo recibe (evita que la pulsación larga se vea como Select mantenido).
        return menuOpen || blockUntilNeutral || (held && !menuOpen && holdT > 0.3)
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

    private func openMenu(head: simd_float4x4) {
        menuOpen = true
        sel = 0
        menuDirty = true
        prevNav = XRNav()
        let p = XRMath.position(head)
        menuModel = XRMath.translation(SIMD3<Float>(p.x, p.y - 0.05, p.z)) * XRMath.rotationY(XRMath.yaw(of: head))
            * XRMath.translation(SIMD3<Float>(0, 0, -1.1))
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

    private func activate(dir: Int) {
        guard let it = XRMenuItem(rawValue: sel) else { return }
        switch it {
        case .resume: closeMenu()
        case .recenter: onRecenter?(); closeMenu()
        case .width: cfg.width = cycle(cfg.width, XRViewConfig.widths, dir)
        case .distance: cfg.distance = cycle(cfg.distance, XRViewConfig.distances, dir)
        case .curve: cfg.curve = cycle(cfg.curve, XRViewConfig.curves, dir)
        case .sharp: cfg.sharp = (cfg.sharp + dir + 3) % 3
        case .integerScale: cfg.integerScale.toggle()
        case .cutscene: cfg.diorama.toggle()
        case .hud: cfg.hudPanels.toggle()
        case .hudScale: cfg.hudScale = cycle(cfg.hudScale, XRViewConfig.hudScales, dir)
        case .wristHud: cfg.wristHud.toggle()
        case .wristScale: cfg.wristScale = cycle(cfg.wristScale, XRViewConfig.wristScales, dir)
        case .wristSide: cfg.wristHandSide.toggle()
        case .exit: cfg.save(); onExit?()
        }
        menuDirty = true
    }

    // MARK: menú: dibujo

    private func rows() -> [(String, String)] {
        func m(_ v: Float) -> String { String(format: "%.1f m", v).replacingOccurrences(of: ".", with: ",") }
        func yn(_ b: Bool) -> String { b ? "Sí" : "No" }
        let curveName = ["Plana", "Suave", "Media", "Fuerte"]
        var ci = 0
        for (i, x) in XRViewConfig.curves.enumerated() where abs(x - cfg.curve) < abs(XRViewConfig.curves[ci] - cfg.curve) { ci = i }
        return XRMenuItem.allCases.map { it -> (String, String) in
            switch it {
            case .resume: return ("Volver al juego", "")
            case .recenter: return ("Recentrar la vista", "")
            case .width: return ("Tamaño de la pantalla", m(cfg.width))
            case .distance: return ("Distancia de la pantalla", m(cfg.distance))
            case .curve: return ("Curvatura de la pantalla", curveName[ci])
            case .sharp: return ("Nitidez de la imagen", ["Suave", "Nítida", "Píxel"][min(max(cfg.sharp, 0), 2)])
            case .integerScale: return ("Escalado entero", yn(cfg.integerScale))
            case .cutscene: return ("Cinemáticas del motor", cfg.diorama ? "Diorama" : "Pantalla de cine")
            case .hud: return ("Paneles del HUD", yn(cfg.hudPanels))
            case .hudScale: return ("Tamaño de los paneles", "\(Int((cfg.hudScale * 100).rounded())) %")
            case .wristHud: return ("Vida y arma en la muñeca", yn(cfg.wristHud))
            case .wristScale: return ("Tamaño en la muñeca", "\(Int((cfg.wristScale * 100).rounded())) cm")
            case .wristSide: return ("Lado en la muñeca", cfg.wristHandSide ? "Hacia la mano" : "Hacia el codo")
            case .exit: return ("Salir de VR", "")
            }
        }
    }

    private func drawMenu() {
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
        ("Opciones VR" as NSString).draw(at: CGPoint(x: 60, y: 36), withAttributes: title)
        let right = NSMutableParagraphStyle()
        right.alignment = .right
        let rowH: CGFloat = 44
        for (i, r) in rows().enumerated() {
            let y = 112 + CGFloat(i) * rowH
            let on = i == sel
            if on {
                UIColor(red: 0.12, green: 0.45, blue: 0.40, alpha: 1).setFill()
                UIBezierPath(roundedRect: CGRect(x: 36, y: y - 4, width: CGFloat(w) - 72, height: rowH - 4), cornerRadius: 14).fill()
            }
            let a: [NSAttributedString.Key: Any] = [.font: UIFont.systemFont(ofSize: 30, weight: on ? .semibold : .regular),
                                                    .foregroundColor: UIColor.white]
            (r.0 as NSString).draw(at: CGPoint(x: 60, y: y), withAttributes: a)
            if !r.1.isEmpty {
                let v: [NSAttributedString.Key: Any] = [.font: UIFont.systemFont(ofSize: 30, weight: .regular),
                                                        .foregroundColor: UIColor(white: on ? 1 : 0.75, alpha: 1), .paragraphStyle: right]
                let txt = on ? "◀  \(r.1)  ▶" : r.1
                (txt as NSString).draw(in: CGRect(x: 380, y: y, width: CGFloat(w) - 380 - 60, height: rowH), withAttributes: v)
            }
        }
        let help: [NSAttributedString.Key: Any] = [.font: UIFont.systemFont(ofSize: 24), .foregroundColor: UIColor(white: 0.6, alpha: 1)]
        ("Palanca: mover y cambiar · Gatillo: elegir · B: cerrar · Menú: cerrar" as NSString).draw(at: CGPoint(x: 60, y: 730), withAttributes: help)
        UIGraphicsPopContext()
        guard let data = ctx.data else { return }
        menuIdx = (menuIdx + 1) % menuTex.count
        menuTex[menuIdx].replace(region: MTLRegionMake2D(0, 0, w, h), mipmapLevel: 0, withBytes: data, bytesPerRow: w * 4)
    }

    // MARK: composición

    private static let hudRegions: [(u0: Float, v0: Float, u1: Float, v1: Float, x: Float, y: Float)] = [
        (0.00, 0.00, 0.35, 0.35, -0.52, 0.30),    // arriba izquierda: vida y oxígeno
        (0.35, 0.00, 0.65, 0.35, 0.00, 0.38),     // arriba centro: alertas
        (0.65, 0.00, 1.00, 0.35, 0.52, 0.30),     // arriba derecha (el radar va en el antebrazo)
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

    /// Recorte del panel 2D -> ranura de la muñeca (-1: flota delante de la vista). La vida/oxígeno, el objeto y el arma/munición van al brazo.
    private static let wristSlot = [0, -1, -1, -1, 1, -1, 2]

    private static func wristModel(_ w: Wrist) -> (n: SIMD3<Float>, ok: Bool) {
        let n = simd_cross(w.right, w.up)
        let l = simd_length(n)
        let vals = [w.center.x, w.center.y, w.center.z, w.right.x, w.right.y, w.right.z, w.up.x, w.up.y, w.up.z, w.halfW, w.halfH, l]
        guard vals.allSatisfy({ $0.isFinite }), l > 0.5, w.halfW > 0.001, w.halfH > 0.001 else { return (n, false) }
        return (n / l, true)
    }

    /// `hud`: panel 2D (textura del renderer, ya la vista sRGB si el destino es lineal). `eyePx`/`p00`: ancho en píxeles de un ojo y P[0][0].
    func compose(gs: MvVrGameState, head: simd_float4x4, dt: Float, hud: MTLTexture?, eyePx: Float, p00: Float,
                 gameView: simd_float4x4, linear: Bool, wrist: Wrist? = nil) -> Result {
        // 1) qué toca mostrar
        var target: Display = gs.view_mode == 1 ? .world : .cinema
        if target == .cinema && cfg.diorama && gs.cinematic != 0 && Self.dioramaOK(gameView) { target = .diorama }
        if target != .world || shown != .world { cinema?.update() }

        // 2) transición: negro -> cambio -> negro (0,22 s + 0,28 s)
        let d = min(max(dt, 0.001), 0.1)
        if target != shown {
            veil += d / 0.22
            if veil >= 1 { veil = 1; shown = target; needAnchor = true }
        } else {
            veil = max(0, veil - d / 0.28)
        }

        // 3) anclas
        let hp = XRMath.position(head)
        let hy = XRMath.yaw(of: head)
        if needAnchor && shown != .world {
            anchorY = max(0.8, hp.y - 0.05)
            anchor = XRMath.translation(SIMD3<Float>(hp.x, 0, hp.z)) * XRMath.rotationY(hy)
            needAnchor = false
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

        // 4) pantalla de cine (también como respaldo del diorama sin geometría)
        if shown == .cinema, let c = cinema, let tex = c.texture(srgb: linear) {
            var W = cfg.width
            let D = cfg.distance
            if cfg.integerScale, p00 > 0, eyePx > 0, c.w > 0 {
                let ppm = p00 * eyePx / (2 * D)                       // píxeles del visor por metro, en el centro de la pantalla
                let texPx = W / Float(c.w) * ppm
                if texPx.isFinite, texPx > 1 { W = max(1, texPx.rounded()) * Float(c.w) / ppm }
            }
            var it = XRDrawItem(kind: .screen, texture: tex)
            it.model = anchor * XRMath.translation(SIMD3<Float>(0, anchorY, -D))
            it.size = SIMD2<Float>(W, W * 0.75)
            it.distance = D
            it.curve = cfg.curve
            it.cols = cfg.curve > 0.01 ? 48 : 1
            it.imgSize = SIMD2<Float>(Float(c.w), Float(c.h))
            it.sharp = Float(cfg.sharp)
            r.items.append(it)
        }

        // 5) diorama
        if shown == .diorama, Self.dioramaOK(gameView) {
            let s = cfg.dioramaScale * 0.001
            let f = simd_float4x4(diagonal: SIMD4<Float>(s, -s, -s, 1))      // vista del juego (x der, y abajo, z delante; unidades) -> usuario (m)
            r.dioramaModel = anchor * XRMath.translation(SIMD3<Float>(0, anchorY - 0.1, -1.3)) * f * gameView.inverse
        }

        // 6) paneles del HUD (mundo y diorama; en el cine el HUD ya va dentro de la imagen)
        if shown != .cinema && cfg.hudPanels, let h = hud {
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
                if Self.wristSlot[i] >= 0, let w = wrist, let m = wm, m.ok {
                    // Plano del radar (+0,002 m hacia el espectador para no solaparse con el cuerpo), hacia el codo (-derecha) o la mano.
                    let dir: Float = cfg.wristHandSide ? 1 : -1
                    let pos = w.center + m.n * 0.002 + w.right * (dir * slotOff[Self.wristSlot[i]])
                    it.model = simd_float4x4(columns: (SIMD4<Float>(w.right, 0), SIMD4<Float>(w.up, 0), SIMD4<Float>(m.n, 0), SIMD4<Float>(pos, 1)))
                    it.uvrect = SIMD4<Float>(rg.u0, rg.v0, rg.u1, rg.v1)
                    it.size = SIMD2<Float>((rg.u1 - rg.u0) * cfg.wristScale, (rg.v1 - rg.v0) * cfg.wristScale * 0.75)
                    it.opacity = 1
                    r.items.append(it)
                    continue
                }
                it.model = frame * XRMath.translation(SIMD3<Float>(rg.x * sc / 0.8, rg.y * sc / 0.8, -1.4))
                it.uvrect = SIMD4<Float>(rg.u0, rg.v0, rg.u1, rg.v1)
                it.size = SIMD2<Float>((rg.u1 - rg.u0) * sc, (rg.v1 - rg.v0) * sc * 0.75)
                it.opacity = 0.92
                r.items.append(it)
            }
        }

        // 7) velo de transición y menú
        if veil > 0.001 {
            var v = XRDrawItem(kind: .veil, texture: nil)
            v.opacity = veil
            r.items.append(v)
        }
        if menuOpen {
            if menuDirty { drawMenu(); menuDirty = false }
            if !menuTex.isEmpty {
                var it = XRDrawItem(kind: .menu, texture: linear ? menuTexSRGB[menuIdx] : menuTex[menuIdx])
                it.model = menuModel
                it.size = SIMD2<Float>(0.9, 0.675)
                it.opacity = 1
                r.items.append(it)
            }
        }
        return r
    }

    private static func dioramaOK(_ m: simd_float4x4) -> Bool {
        guard XRMath.isFinite(m) else { return false }
        let det = simd_determinant(m)
        return det.isFinite && abs(det) > 1e-9
    }
}
