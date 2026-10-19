import Foundation
import Metal
import simd
import CoreGraphics
import ImageIO

// Banco de render de CI (macOS, Apple Silicon): ejecuta el MISMO código de render del visor (XRFrameEncoder + RGRenderer + XRFinalPass + XRPresenter)
// con un shim en lugar de CompositorServices y con datos SINTÉTICOS (ver bench/c/bench_host.c). Escribe un PNG por ojo y caso, imprime
// métricas y un mapa ASCII de cada imagen, y sale con código != 0 si algo sale negro o fuera de sitio.

setvbuf(stdout, nil, _IONBF, 0)
guard CommandLine.arguments.count >= 3 else { print("uso: bench <default.metallib> <directorio de salida>"); exit(2) }
MVLib.path = CommandLine.arguments[1]
let outDir = CommandLine.arguments[2]
try? FileManager.default.createDirectory(atPath: outDir, withIntermediateDirectories: true)

var failures = 0
var firstConfig = false
var failList: [String] = []
var noticeCount = 0
func annot(_ level: String, _ title: String, _ msg: String) {
    let m = msg.replacingOccurrences(of: "%", with: "%25").replacingOccurrences(of: "\r", with: "%0D").replacingOccurrences(of: "\n", with: "%0A")
    print("::\(level) title=\(title)::\(m)")
}
func check(_ caso: String, _ name: String, _ ok: Bool, _ detail: String) {
    print("[\(ok ? "OK  " : "FALLO")] \(caso): \(name) \(detail)")
    if !ok { failures += 1; failList.append("\(caso) \(name): \(detail)") }
}

// MARK: imágenes

struct Img {
    let w: Int, h: Int
    var bgra: [UInt8]
    func rgb(_ x: Int, _ y: Int) -> (Int, Int, Int) {
        let cx = min(max(x, 0), w - 1), cy = min(max(y, 0), h - 1)
        let i = (cy * w + cx) * 4
        return (Int(bgra[i + 2]), Int(bgra[i + 1]), Int(bgra[i]))
    }
    /// media de un cuadrado (2r+1)^2 centrado
    func avg(_ x: Int, _ y: Int, r: Int = 2) -> (Int, Int, Int) {
        var s = (0, 0, 0), n = 0
        for dy in -r...r { for dx in -r...r { let c = rgb(x + dx, y + dy); s.0 += c.0; s.1 += c.1; s.2 += c.2; n += 1 } }
        return (s.0 / n, s.1 / n, s.2 / n)
    }
    var nonBlack: Double {
        var n = 0
        for i in stride(from: 0, to: w * h * 4, by: 4) where max(bgra[i], bgra[i + 1], bgra[i + 2]) > 12 { n += 1 }
        return Double(n) / Double(w * h)
    }
    func ascii(cols: Int = 64, rows: Int = 32) -> String {
        var out = ""
        for r in 0..<rows {
            for c in 0..<cols {
                let x0 = c * w / cols, x1 = (c + 1) * w / cols, y0 = r * h / rows, y1 = (r + 1) * h / rows
                var s = (0, 0, 0), n = 0
                var y = y0
                while y < y1 { var x = x0; while x < x1 { let p = rgb(x, y); s.0 += p.0; s.1 += p.1; s.2 += p.2; n += 1; x += 2 }; y += 2 }
                let a = (s.0 / max(n, 1), s.1 / max(n, 1), s.2 / max(n, 1))
                let mx = max(a.0, a.1, a.2), mn = min(a.0, a.1, a.2)
                var ch: Character
                if mx < 10 { ch = " " }
                else if mx > 60 && Double(mx - mn) / Double(mx) > 0.5 {
                    let r = a.0 > mx * 6 / 10, g = a.1 > mx * 6 / 10, b = a.2 > mx * 6 / 10
                    switch (r, g, b) {
                    case (true, false, false): ch = "R"
                    case (false, true, false): ch = "G"
                    case (false, false, true): ch = "B"
                    case (true, true, false): ch = "Y"
                    case (true, false, true): ch = "M"
                    case (false, true, true): ch = "C"
                    default: ch = "W"
                    }
                } else if mx < 60 { ch = "-" } else if mx < 140 { ch = "+" } else if mx < 220 { ch = "#" } else { ch = "W" }
                out.append(ch)
            }
            out.append("\n")
        }
        return out
    }
    func writePNG(_ path: String) {
        var rgba = [UInt8](repeating: 255, count: w * h * 4)
        for i in 0..<(w * h) { rgba[i * 4] = bgra[i * 4 + 2]; rgba[i * 4 + 1] = bgra[i * 4 + 1]; rgba[i * 4 + 2] = bgra[i * 4] }
        rgba.withUnsafeMutableBytes { p in
            guard let ctx = CGContext(data: p.baseAddress, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w * 4,
                                      space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue),
                  let cg = ctx.makeImage(),
                  let dest = CGImageDestinationCreateWithURL(URL(fileURLWithPath: path) as CFURL, "public.png" as CFString, 1, nil) else { return }
            CGImageDestinationAddImage(dest, cg, nil)
            CGImageDestinationFinalize(dest)
        }
    }
}

func near(_ c: (Int, Int, Int), _ e: (Int, Int, Int), _ tol: Int = 60) -> Bool {
    abs(c.0 - e.0) <= tol && abs(c.1 - e.1) <= tol && abs(c.2 - e.2) <= tol
}

// MARK: dispositivo

guard let device = MTLCreateSystemDefaultDevice() else {
    annot("error", "banco de render", "No hay dispositivo Metal en este runner")
    print("FALLO: sin dispositivo Metal"); exit(2)
}
print("dispositivo: \(device.name) | amplificacion de vertices x2: \(device.supportsVertexAmplificationCount(2)) | msaa4: \(device.supportsTextureSampleCount(4)) | apple7: \(device.supportsFamily(.apple7)) | apple9: \(device.supportsFamily(.apple9))")
guard let queue = device.makeCommandQueue() else { print("FALLO: sin cola Metal"); exit(2) }

let EW = 1024, EH = 1024                 // tamaño por ojo
let colorFormat = MTLPixelFormat.bgra8Unorm_srgb
let depthFormat = MTLPixelFormat.depth32Float
var depthRange = SIMD2<Float>(1000, 0.1)  // (lejos, cerca) en metros: el renderer toma min/max, como con el sistema
let IPD: Float = 0.063
let eyes: [XREyeView] = [
    XREyeView(transform: XRMath.translation(SIMD3<Float>(-IPD / 2, 0, 0)), tangents: SIMD4<Float>(1.30, 1.05, 1.10, 1.10),
              viewport: MTLViewport(originX: 0, originY: 0, width: Double(EW), height: Double(EH), znear: 0, zfar: 1)),
    XREyeView(transform: XRMath.translation(SIMD3<Float>(IPD / 2, 0, 0)), tangents: SIMD4<Float>(1.05, 1.30, 1.10, 1.10),
              viewport: MTLViewport(originX: 0, originY: 0, width: Double(EW), height: Double(EH), znear: 0, zfar: 1)),
]

func headPose(_ p: SIMD3<Float>, yaw: Float) -> simd_float4x4 { XRMath.translation(p) * XRMath.rotationY(yaw) }

/// Pixel (x derecha, y abajo; nil si detrás de la cámara) donde cae un punto del mundo en el ojo `e` con la cabeza `head`.
func project(_ world: SIMD3<Float>, eye e: Int, head: simd_float4x4) -> (Int, Int)? {
    let v = (head * eyes[e].transform).inverse
    let p = XRFrameEncoder.projection(eyes[e], depthRange: SIMD2<Float>(1000, 0.1))
    let c = p * v * SIMD4<Float>(world, 1)
    guard c.w > 0.01 else { return nil }
    let nx = c.x / c.w, ny = c.y / c.w
    return (Int(((nx * 0.5 + 0.5) * Float(EW)).rounded()), Int(((1 - (ny * 0.5 + 0.5)) * Float(EH)).rounded()))
}

func makeTargets(layered: Bool) -> (color: [MTLTexture], depth: [MTLTexture])? {
    func tex(_ fmt: MTLPixelFormat, array: Bool, label: String) -> MTLTexture? {
        let d = MTLTextureDescriptor()
        d.pixelFormat = fmt; d.width = EW; d.height = EH
        d.textureType = array ? .type2DArray : .type2D
        d.arrayLength = array ? 2 : 1
        d.usage = [.renderTarget, .shaderRead]
        d.storageMode = .private
        let t = device.makeTexture(descriptor: d)
        t?.label = label
        return t
    }
    if layered {
        guard let c = tex(colorFormat, array: true, label: "color"), let z = tex(depthFormat, array: true, label: "depth") else { return nil }
        return ([c], [z])
    }
    guard let c0 = tex(colorFormat, array: false, label: "color0"), let c1 = tex(colorFormat, array: false, label: "color1"),
          let z0 = tex(depthFormat, array: false, label: "depth0"), let z1 = tex(depthFormat, array: false, label: "depth1") else { return nil }
    return ([c0, c1], [z0, z1])
}

func blitEye(_ cb: MTLCommandBuffer, _ color: [MTLTexture], layered: Bool, eye: Int) -> MTLBuffer? {
    guard let buf = device.makeBuffer(length: EW * EH * 4, options: .storageModeShared), let bl = cb.makeBlitCommandEncoder() else { return nil }
    let t = layered ? color[0] : color[eye]
    bl.copy(from: t, sourceSlice: layered ? eye : 0, sourceLevel: 0, sourceOrigin: MTLOrigin(x: 0, y: 0, z: 0), sourceSize: MTLSize(width: EW, height: EH, depth: 1),
            to: buf, destinationOffset: 0, destinationBytesPerRow: EW * 4, destinationBytesPerImage: EW * EH * 4)
    bl.endEncoding()
    return buf
}

/// Codifica `frames` fotogramas; en el último lee los dos ojos. `head(i)` = pose de la cabeza (espacio recentrado) del fotograma i.
func render(_ enc: XRFrameEncoder, layered: Bool, color: [MTLTexture], depth: [MTLTexture], gs: MvVrGameState, frames: Int,
            head: (Int) -> simd_float4x4) -> [Img]? {
    var result: [Img]?
    for i in 0..<frames {
        guard let cb = queue.makeCommandBuffer() else { return nil }
        let ok = enc.encode(commandBuffer: cb, gs: gs, originFromDevice: head(i), recenterInverse: matrix_identity_float4x4, dt: 1.0 / 90.0,
                            eyes: eyes, depthRange: depthRange, colorTextures: color, depthTextures: depth)
        if !ok { print("FALLO: encode devolvió false"); return nil }
        var bufs: [MTLBuffer] = []
        if i == frames - 1 { for e in 0..<2 { if let b = blitEye(cb, color, layered: layered, eye: e) { bufs.append(b) } } }
        cb.commit()
        cb.waitUntilCompleted()
        if cb.status != .completed { print("FALLO: el command buffer terminó con estado \(cb.status.rawValue): \(String(describing: cb.error))"); return nil }
        if bufs.count == 2 {
            result = bufs.map { b in Img(w: EW, h: EH, bgra: Array(UnsafeBufferPointer(start: b.contents().assumingMemoryBound(to: UInt8.self), count: EW * EH * 4))) }
        }
    }
    return result
}

func setDefaults(width: Double = 3.0, distance: Double = 2.5, curve: Double, sharp: Int) {
    let d = UserDefaults.standard
    d.set(width, forKey: "xr.view.width"); d.set(distance, forKey: "xr.view.distance"); d.set(curve, forKey: "xr.view.curve")
    d.set(sharp, forKey: "xr.view.sharp"); d.set(false, forKey: "xr.view.integer"); d.set("cine", forKey: "xr.view.cutscene")
    d.set(true, forKey: "xr.view.hud"); d.set(0.8, forKey: "xr.view.hudScale"); d.set(false, forKey: "xr.view.wristHud")
}

func report(_ caso: String, _ imgs: [Img], notice: Bool = false) {
    for (i, im) in imgs.enumerated() {
        let nm = "\(caso)-\(i == 0 ? "izq" : "der")"
        im.writePNG("\(outDir)/\(nm).png")
        print("== \(nm): no negro = \(String(format: "%.1f", im.nonBlack * 100)) %")
        print(im.ascii())
        // GitHub admite 10 anotaciones de cada tipo por paso: solo se anotan las imágenes principales
        if notice && (i == 0 || firstConfig) && noticeCount < 9 {
            noticeCount += 1
            annot("notice", "imagen \(nm)", String(format: "no negro %.1f %%\n", im.nonBlack * 100) + im.ascii())
        }
    }
}

// MARK: casos

struct Cfg { var name: String; var layered: Bool; var msaa: Int; var range = SIMD2<Float>(1000, 0.1) }

func runCase(_ cfg: Cfg, first: Bool) {
    firstConfig = first
    depthRange = cfg.range
    print("\n######## \(cfg.name) ########")
    if cfg.layered && !device.supportsVertexAmplificationCount(2) { annot("warning", cfg.name, "sin amplificacion de vertices x2: caso omitido"); return }
    if cfg.msaa > 1 && !device.supportsTextureSampleCount(cfg.msaa) { annot("warning", cfg.name, "sin msaa \(cfg.msaa): caso omitido"); return }

    // ---------- A: pantalla de cine (curva 0 para comprobar la geometría exacta, luego la curva por defecto) ----------
    for (variant, curve) in [("cine-plano", 0.0), ("cine-curvo", 0.35)] {
        let caso = "\(cfg.name)/\(variant)"
        setDefaults(curve: curve, sharp: 0)
        guard let tg = makeTargets(layered: cfg.layered) else { check(caso, "texturas", false, "no se pudieron crear"); continue }
        guard let enc = XRFrameEncoder(device: device, colorFormat: colorFormat, depthFormat: depthFormat, layered: cfg.layered, sampleCount: cfg.msaa) else {
            check(caso, "codificador", false, "XRFrameEncoder no se pudo crear (pipelines?)"); continue
        }
        enc.log = { print("[diag] " + $0) }
        bench_make_pattern()
        let headA = headPose(SIMD3<Float>(0.10, 1.60, 0.20), yaw: 0.45)
        let headB = headPose(SIMD3<Float>(0.15, 1.55, 0.10), yaw: 0.50)      // la cabeza se mueve tras anclar: la pantalla debe quedarse
        var gs = MvVrGameState(); gs.view_mode = 0
        guard let imgs = render(enc, layered: cfg.layered, color: tg.color, depth: tg.depth, gs: gs, frames: 40, head: { $0 == 39 ? headB : headA }) else {
            check(caso, "render", false, "fallo al renderizar"); continue
        }
        report(caso, imgs, notice: variant == "cine-plano")
        print("[diag] última: \(enc.lastDiag)")
        check(caso, "velo disipado", enc.presenter.veil < 0.01, "velo=\(enc.presenter.veil)")
        let wDist: Float = 3.0, D: Float = 2.5, cv = Float(curve)
        let hp = XRMath.position(headA)
        let anchor = XRMath.translation(SIMD3<Float>(hp.x, 0, hp.z)) * XRMath.rotationY(XRMath.yaw(of: headA))
        let anchorY = max(0.8, hp.y - 0.05)
        func scr(_ pxl: Float, _ pyl: Float) -> SIMD3<Float> {
            let u = pxl / 320, v = pyl / 240
            let s = (u - 0.5) * wDist, a = s / D
            let x = s + (D * sinf(a) - s) * cv, z = (D * (1 - cosf(a))) * cv
            let y = (0.5 - v) * wDist * 0.75
            let m = anchor * XRMath.translation(SIMD3<Float>(0, anchorY, -D)) * SIMD4<Float>(x, y, z, 1)
            return SIMD3<Float>(m.x, m.y, m.z)
        }
        for e in 0..<2 {
            let im = imgs[e]
            check(caso, "ojo \(e) no negro", im.nonBlack > 0.12, String(format: "%.1f %% de pixeles no negros (minimo 12 %%)", im.nonBlack * 100))
            let marks: [(String, Float, Float, (Int, Int, Int))] = [
                ("rojo (sup. izq.)", 20, 20, (255, 0, 0)), ("verde (sup. der.)", 300, 20, (0, 255, 0)),
                ("azul (inf. izq.)", 20, 220, (0, 0, 255)), ("amarillo (inf. der.)", 300, 220, (255, 255, 0)),
                ("cruz blanca", 160, 60, (255, 255, 255)), ("F cian", 64, 120, (0, 255, 255)),
            ]
            for m in marks {
                guard let p = project(scr(m.1, m.2), eye: e, head: headB) else { check(caso, "ojo \(e) \(m.0)", false, "detras de la camara"); continue }
                let c = im.avg(p.0, p.1, r: 2)
                check(caso, "ojo \(e) \(m.0)", near(c, m.3), "pixel esperado (\(p.0),\(p.1)) color \(c) esperado \(m.3)")
            }
        }
    }

    // ---------- B: mundo ----------
    do {
        let caso = "\(cfg.name)/mundo"
        setDefaults(curve: 0.35, sharp: 1)
        guard let tg = makeTargets(layered: cfg.layered) else { check(caso, "texturas", false, "no se pudieron crear"); return }
        guard let enc = XRFrameEncoder(device: device, colorFormat: colorFormat, depthFormat: depthFormat, layered: cfg.layered, sampleCount: cfg.msaa) else {
            check(caso, "codificador", false, "XRFrameEncoder no se pudo crear"); return
        }
        enc.log = { print("[diag] " + $0) }
        bench_make_pattern()
        bench_make_scene()
        bench_scene_post()
        let headA = headPose(SIMD3<Float>(0, 1.6, 0), yaw: 0)
        let headB = headPose(SIMD3<Float>(0.2, 1.6, -0.3), yaw: 0.1745)
        var gs = MvVrGameState(); gs.view_mode = 1; gs.game = 1; gs.in_control = 1; gs.cam_active = 1; gs.first_person = 1
        guard let imgs = render(enc, layered: cfg.layered, color: tg.color, depth: tg.depth, gs: gs, frames: 70, head: { $0 == 69 ? headB : headA }) else {
            check(caso, "render", false, "fallo al renderizar"); return
        }
        report(caso, imgs, notice: true)
        print("[diag] última: \(enc.lastDiag)")
        check(caso, "presentacion = mundo", "\(enc.presenter.shown)" == "world", "mostrando=\(enc.presenter.shown)")
        check(caso, "velo disipado", enc.presenter.veil < 0.01, "velo=\(enc.presenter.veil)")
        for e in 0..<2 {
            let im = imgs[e]
            check(caso, "ojo \(e) no negro", im.nonBlack > 0.25, String(format: "%.1f %% de pixeles no negros (minimo 25 %%)", im.nonBlack * 100))
            let pts: [(String, SIMD3<Float>, (Int, Int, Int))] = [
                ("cubo rojo", SIMD3<Float>(-1.2, 0.5, -3), (255, 0, 0)),
                ("cubo azul", SIMD3<Float>(1.5, 0.5, -3), (0, 0, 255)),
                ("cubo amarillo cercano (oclusion)", SIMD3<Float>(0, 1.6, -8), (255, 255, 0)),
                ("cubo verde lejano visible", SIMD3<Float>(1.3, 1.6, -7.5), (0, 255, 0)),
                ("suelo oscuro", SIMD3<Float>(1.5, 0, -2.5), (70, 70, 70)),
                ("suelo claro", SIMD3<Float>(0.5, 0, -2.5), (150, 150, 150)),
            ]
            for p in pts {
                guard let px = project(p.1, eye: e, head: headB) else { check(caso, "ojo \(e) \(p.0)", false, "detras de la camara"); continue }
                let c = im.avg(px.0, px.1, r: 2)
                check(caso, "ojo \(e) \(p.0)", near(c, p.2, 45), "pixel (\(px.0),\(px.1)) color \(c) esperado \(p.2)")
            }
            // panel del HUD: rectángulo magenta de la esquina sup. izq. del panel 2D, en el recorte 0 (arriba izquierda)
            let hp = XRMath.position(headB)
            let frame = XRMath.translation(SIMD3<Float>(hp.x, hp.y, hp.z)) * XRMath.rotationY(0)
            let sc: Float = 0.8
            let u = (35.0 / 320.0) / 0.35, v = (25.0 / 240.0) / 0.35       // dentro del recorte 0: (0..0.35) x (0..0.35)
            let local = SIMD4<Float>(-0.52 * sc / 0.8 + Float(u - 0.5) * 0.35 * sc, 0.30 * sc / 0.8 + Float(0.5 - v) * 0.35 * sc * 0.75, -1.4, 1)
            let w = frame * local
            if let px = project(SIMD3<Float>(w.x, w.y, w.z), eye: e, head: headB) {
                let c = im.avg(px.0, px.1, r: 2)
                check(caso, "ojo \(e) HUD magenta", near(c, (235, 0, 235), 70), "pixel (\(px.0),\(px.1)) color \(c) esperado ~(235,0,235)")
            } else { check(caso, "ojo \(e) HUD magenta", false, "detras de la camara") }
        }
    }
}

for (n, cfg) in [Cfg(name: "layered-msaa4", layered: true, msaa: 4), Cfg(name: "dedicated-msaa4", layered: false, msaa: 4), Cfg(name: "layered-msaa1", layered: true, msaa: 1)].enumerated() {
    runCase(cfg, first: n == 0)
}
// Rangos de profundidad que el sistema podría devolver (orden/valores distintos): la imagen debe salir igual
for (nm, r) in [("rango-1-0", SIMD2<Float>(1, 0)), ("rango-0.1-1000", SIMD2<Float>(0.1, 1000)), ("rango-0.3-50", SIMD2<Float>(50, 0.3)),
                ("rango-0-0", SIMD2<Float>(0, 0)), ("rango-1e-3-1e5", SIMD2<Float>(1e5, 1e-3))] {
    runCase(Cfg(name: "layered-msaa4-" + nm, layered: true, msaa: 4, range: r), first: false)
}
print("\nresumen: \(failures) comprobaciones falladas")
if failures > 0 {
    annot("error", "banco de render", "\(failures) comprobaciones falladas\n" + failList.prefix(60).joined(separator: "\n"))
    exit(1)
}
print("TODO OK")
