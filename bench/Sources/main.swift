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

/// Comprobación informativa: se imprime como aviso, no cuenta como fallo.
func soft(_ caso: String, _ name: String, _ ok: Bool, _ detail: String) {
    print("[\(ok ? "OK  " : "AVISO")] \(caso): \(name) \(detail)")
    if !ok { warnList.append("\(caso) \(name): \(detail)") }
}
var warnList: [String] = []

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
var sysInf = false                        // simula `computeProjection` del compositor: Z invertida con plano lejano INFINITO (P22 = 0, P32 = cercano)
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
            head: (Int) -> simd_float4x4, before: ((Int) -> Void)? = nil, axes: [XRAxisFrame] = []) -> [Img]? {
    var result: [Img]?
    for i in 0..<frames {
        before?(i)
        guard let cb = queue.makeCommandBuffer() else { return nil }
        let ok = enc.encode(commandBuffer: cb, gs: gs, originFromDevice: head(i), recenterInverse: matrix_identity_float4x4, dt: 1.0 / 90.0,
                            eyes: eyes, depthRange: depthRange, colorTextures: color, depthTextures: depth,
                            systemProjections: sysInf ? eyes.map { e -> simd_float4x4 in
                                var p = XRMath.projection(tangents: e.tangents, near: 0.098, far: 1000); p.columns.2.z = 0; p.columns.3.z = 0.098; return p } : nil,
                            debugAxes: axes)
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
    d.set(sharp, forKey: "xr.view.sharp"); d.set(false, forKey: "xr.view.integer"); d.set("cinema", forKey: "xr.view.cutscene2")
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

struct Cfg { var name: String; var layered: Bool; var msaa: Int; var range = SIMD2<Float>(1000, 0.1); var sysInf = false }

func runCase(_ cfg: Cfg, first: Bool) {
    firstConfig = first
    depthRange = cfg.range
    sysInf = cfg.sysInf
    print("\n######## \(cfg.name) ########")
    if cfg.layered && !device.supportsVertexAmplificationCount(2) { annot("warning", cfg.name, "sin amplificacion de vertices x2: caso omitido"); return }
    if cfg.msaa > 1 && !device.supportsTextureSampleCount(cfg.msaa) { annot("warning", cfg.name, "sin msaa \(cfg.msaa): caso omitido"); return }

    // ---------- A: pantalla de cine (curva 0 para comprobar la geometría exacta, luego la curva por defecto) ----------
    for (variant, curve, sharp) in [("cine-plano", 0.0, 0), ("cine-curvo", 0.35, 0), ("cine-defecto", 0.35, 1), ("cine-pixel", 0.35, 2)] {
        let caso = "\(cfg.name)/\(variant)"
        setDefaults(curve: curve, sharp: sharp)
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

    // ---------- C: pantalla 3D estereoscópica (cinemática del motor): dos cámaras del juego separadas un IPD en una pantalla fija en el mundo ----------
    stereoBlock: do {
        let caso = "\(cfg.name)/pantalla3d"
        setDefaults(curve: 0.0, sharp: 1)
        UserDefaults.standard.set("stereo", forKey: "xr.view.cutscene2")
        guard let tg = makeTargets(layered: cfg.layered) else { check(caso, "texturas", false, "no se pudieron crear"); break stereoBlock }
        guard let enc = XRFrameEncoder(device: device, colorFormat: colorFormat, depthFormat: depthFormat, layered: cfg.layered, sampleCount: cfg.msaa) else {
            check(caso, "codificador", false, "XRFrameEncoder no se pudo crear"); break stereoBlock
        }
        enc.log = { print("[diag] " + $0) }
        bench_make_pattern()
        bench_make_scene()
        bench_set_cine_camera(1)
        bench_scene_post()
        bench_enable_radar(0)
        bench_enable_weapon(0)
        let headA = headPose(SIMD3<Float>(0.10, 1.60, 0.20), yaw: 0.45)
        let headB = headPose(SIMD3<Float>(0.35, 1.45, -0.10), yaw: 0.20)      // la cabeza se mueve tras anclar: el encuadre de la escena NO debe cambiar
        var gs = MvVrGameState(); gs.view_mode = 0; gs.view_kind = 2
        guard let imgs = render(enc, layered: cfg.layered, color: tg.color, depth: tg.depth, gs: gs, frames: 80, head: { $0 == 79 ? headB : headA }) else {
            check(caso, "render", false, "fallo al renderizar"); bench_set_cine_camera(0); break stereoBlock
        }
        bench_set_cine_camera(0)
        report(caso, imgs, notice: true)
        print("[diag] última: \(enc.lastDiag)")
        check(caso, "presentacion = pantalla 3D", "\(enc.presenter.shown)" == "stereo", "mostrando=\(enc.presenter.shown)")
        check(caso, "imagen estereo dibujada", enc.renderer.stereoRendered, "stereoRendered=\(enc.renderer.stereoRendered)")
        check(caso, "velo disipado", enc.presenter.veil < 0.01, "velo=\(enc.presenter.veil)")
        let W: Float = 3.0, D: Float = 2.5
        let tanX = W / (2 * D), tanY = tanX * 0.75
        let hp = XRMath.position(headA)
        let anchor = XRMath.translation(SIMD3<Float>(hp.x, 0, hp.z)) * XRMath.rotationY(XRMath.yaw(of: headA))
        let anchorY = max(0.8, hp.y - 0.05)
        var gv = matrix_identity_float4x4
        gv.columns.0.x = 0.001; gv.columns.1.y = -0.001; gv.columns.2.z = -0.001; gv.columns.3 = SIMD4<Float>(0, 1.6, 0, 1)
        var disparity: [String: Int] = [:]
        for e in 0..<2 {
            let im = imgs[e]
            check(caso, "ojo \(e) no negro", im.nonBlack > 0.10, String(format: "%.1f %% de pixeles no negros (minimo 10 %%)", im.nonBlack * 100))
            guard let cam = XRStereo.eye(gameView: gv, eye: e, ipd: IPD, conv: XRStereo.convergence, tanX: tanX, tanY: tanY) else { check(caso, "camara estereo", false, "XRStereo.eye devolvio nil"); continue }
            let pts: [(String, SIMD3<Float>, (Int, Int, Int))] = [
                ("cubo rojo", SIMD3<Float>(-1.2, 0.5, -3), (255, 0, 0)),
                ("cubo azul", SIMD3<Float>(1.5, 0.5, -3), (0, 0, 255)),
                ("cubo amarillo cercano (oclusion)", SIMD3<Float>(0, 1.6, -2), (255, 255, 0)),
                ("suelo oscuro", SIMD3<Float>(-0.5, 0, -4.5), (70, 70, 70)),
                ("suelo claro", SIMD3<Float>(0.5, 0, -4.5), (150, 150, 150)),
            ]
            for p in pts {
                // punto del mundo -> posición en la imagen de la cámara del ojo e -> punto de la pantalla en el mundo -> pixel del ojo del usuario (con la cabeza movida)
                let c = cam.view * SIMD4<Float>(p.1, 1)
                guard c.z < -0.01 else { check(caso, "ojo \(e) \(p.0)", false, "detras de la camara estereo"); continue }
                let u = (c.x / -c.z + cam.tangents.x) / (cam.tangents.x + cam.tangents.y)
                let v = (cam.tangents.z - c.y / -c.z) / (cam.tangents.z + cam.tangents.w)
                let local = SIMD4<Float>((u - 0.5) * W, (0.5 - v) * W * 0.75, 0, 1)
                let w = anchor * XRMath.translation(SIMD3<Float>(0, anchorY, -D)) * local
                guard let px = project(SIMD3<Float>(w.x, w.y, w.z), eye: e, head: headB) else { check(caso, "ojo \(e) \(p.0)", false, "detras de la camara"); continue }
                let col = im.avg(px.0, px.1, r: 2)
                check(caso, "ojo \(e) \(p.0)", near(col, p.2, 50), "pixel (\(px.0),\(px.1)) color \(col) esperado \(p.2)")
                if p.0 == "cubo rojo" { disparity["rojo\(e)"] = px.0 }
            }
        }
        // paralaje: el cubo rojo (a 3 m = plano de convergencia) cae en el mismo punto de la pantalla en los dos ojos de la cámara, y la pantalla está a 2,5 m,
        // así que sus posiciones en los ojos del usuario difieren como las de un objeto de la pantalla (no como las de uno lejano)
        if let a = disparity["rojo0"], let b = disparity["rojo1"] { print("[diag] cubo rojo: x ojo izq \(a) der \(b)") }
        // la cabeza no cambia el encuadre: segunda imagen con otra pose y misma escena -> la imagen del juego en la pantalla es la misma textura
        check(caso, "texturas estereo con tamano", enc.renderer.stereoImage?.arrayLength == 2, "arrayLength=\(String(describing: enc.renderer.stereoImage?.arrayLength))")
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
        bench_enable_radar(1)
        bench_enable_weapon(1)
        let headA = headPose(SIMD3<Float>(0, 1.6, 0), yaw: 0)
        let headB = headPose(SIMD3<Float>(0.2, 1.6, -0.3), yaw: 0.1745)
        var gs = MvVrGameState(); gs.view_mode = 1; gs.game = 1; gs.in_control = 1; gs.cam_active = 1; gs.first_person = 1
        // radar de muñeca: antebrazo izquierdo sintético (codo, muñeca, dorso hacia arriba)
        let forearm: (elbow: SIMD3<Float>, wrist: SIMD3<Float>, dorsal: SIMD3<Float>) = (SIMD3<Float>(-0.30, 1.20, -0.40), SIMD3<Float>(-0.25, 1.25, -0.80), SIMD3<Float>(0, 1, 0))
        let hpB = XRMath.position(headB)
        // ejes de depuración (opción del menú VR): un marco sin rotación a la derecha y 1,3 m delante, visto de lado; X rojo, Y verde, Z azul (hacia la cabeza)
        let axOrigin = SIMD3<Float>(0.7, 1.3, -1.6)
        var axModel = matrix_identity_float4x4; axModel.columns.3 = SIMD4<Float>(axOrigin.x, axOrigin.y, axOrigin.z, 1)
        let axes = [XRAxisFrame(model: axModel, length: 0.3, thickness: 0.02)]
        guard let imgs = render(enc, layered: cfg.layered, color: tg.color, depth: tg.depth, gs: gs, frames: 70, head: { $0 == 69 ? headB : headA },
                                before: { _ in enc.renderer.radar?.updatePose(forearm: forearm, leftGrip: nil, headPosition: hpB, headForward: SIMD3<Float>(0, 0, -1)) },
                                axes: axes) else {
            check(caso, "render", false, "fallo al renderizar"); return
        }
        report(caso, imgs, notice: true)
        print("[diag] última: \(enc.lastDiag)")
        check(caso, "presentacion = mundo", "\(enc.presenter.shown)" == "world", "mostrando=\(enc.presenter.shown)")
        if cfg.sysInf { check(caso, "proyeccion del sistema (lejano infinito) usada", enc.projSource == "sistema", "proyeccion usada=\(enc.projSource)") }
        check(caso, "velo disipado", enc.presenter.veil < 0.01, "velo=\(enc.presenter.veil)")
        for e in 0..<2 {
            let im = imgs[e]
            check(caso, "ojo \(e) no negro", im.nonBlack > 0.25, String(format: "%.1f %% de pixeles no negros (minimo 25 %%)", im.nonBlack * 100))
            let pts: [(String, SIMD3<Float>, (Int, Int, Int))] = [
                ("cubo rojo", SIMD3<Float>(-1.2, 0.5, -3), (255, 0, 0)),
                ("cubo azul", SIMD3<Float>(1.5, 0.5, -3), (0, 0, 255)),
                ("cubo amarillo cercano (oclusion)", SIMD3<Float>(0, 1.6, -8), (255, 255, 0)),
                ("cubo verde lejano visible", SIMD3<Float>(1.3, 1.6, -7.5), (0, 255, 0)),
                ("suelo oscuro", SIMD3<Float>(-0.5, 0, -2.2), (70, 70, 70)),
                ("suelo claro", SIMD3<Float>(0.5, 0, -2.5), (150, 150, 150)),
            ]
            for p in pts {
                guard let px = project(p.1, eye: e, head: headB) else { check(caso, "ojo \(e) \(p.0)", false, "detras de la camara"); continue }
                let c = im.avg(px.0, px.1, r: 2)
                check(caso, "ojo \(e) \(p.0)", near(c, p.2, 45), "pixel (\(px.0),\(px.1)) color \(c) esperado \(p.2)")
            }
            // cuadro texturizado desde la VRAM (eventos de relleno y subida aplicados antes del lote)
            let texPts: [(String, Float, Float, (Int, Int, Int))] = [
                ("VRAM: relleno azul", -3.75, 2.75, (0, 0, 255)), ("VRAM: subida verde", -3.25, 2.75, (0, 255, 0)), ("VRAM: degradado", -2.25, 1.25, (231, 231, 132)),
            ]
            for t in texPts {
                guard let px = project(SIMD3<Float>(t.1, t.2, -5), eye: e, head: headB) else { check(caso, "ojo \(e) \(t.0)", false, "detras de la camara"); continue }
                let c = im.avg(px.0, px.1, r: 2)
                check(caso, "ojo \(e) \(t.0)", near(c, t.3, 45), "pixel (\(px.0),\(px.1)) color \(c) esperado \(t.3)")
            }
            // ejes de depuración: un píxel de cada color puro cerca del punto medio de cada eje
            for (nm, d, col) in [("X rojo", SIMD3<Float>(1, 0, 0), (255, 0, 0)), ("Y verde", SIMD3<Float>(0, 1, 0), (0, 255, 0)), ("Z azul", SIMD3<Float>(0, 0, 1), (0, 0, 255))] {
                guard let px = project(axOrigin + d * 0.15, eye: e, head: headB) else { check(caso, "ojo \(e) eje \(nm)", false, "detras de la camara"); continue }
                var found = false
                for dy in -5...5 { for dx in -5...5 where !found { found = near(im.rgb(px.0 + dx, px.1 + dy), col, 60) } }
                check(caso, "ojo \(e) eje \(nm)", found, "ningun pixel \(col) cerca de (\(px.0),\(px.1))")
            }
            // radar de muñeca y arma en mano (informativos: no hacen fallar el banco, ver "soft")
            if let wf = enc.renderer.radar?.wristFrame, let px = project(wf.center, eye: e, head: headB) {
                let c = im.avg(px.0, px.1, r: 3)
                soft(caso, "ojo \(e) radar de muneca", c.1 > 80 || c.0 > 120, "centro del radar \(wf.center) pixel (\(px.0),\(px.1)) color \(c) esperado verde o punto rojo")
            } else { soft(caso, "ojo \(e) radar de muneca", false, "sin marco de muneca (radar sin pose o sin escena)") }
            if let px = project(SIMD3<Float>(0.25, 1.2, -0.6), eye: e, head: headB) {
                let c = im.avg(px.0, px.1, r: 3)
                soft(caso, "ojo \(e) arma en mano", c.0 > 100 && c.0 > c.2 + 40, "pixel (\(px.0),\(px.1)) color \(c) esperado naranja")
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
                check(caso, "ojo \(e) HUD magenta (encima del cubo ciano que lo tapa)", c.0 > 150 && c.2 > 150 && c.1 < 150, "pixel (\(px.0),\(px.1)) color \(c) esperado magenta (R,B > 150, G < 150); ciano (0,255,255) = el mundo tapa el panel")
                // el cubo ciano existe: un píxel de su cara a un lado del rectángulo del panel (misma línea de visión del cubo, fuera del rectángulo magenta) es ciano
                if let q = project(SIMD3<Float>(-0.133, 1.776, -0.94), eye: e, head: headB) {
                    let cc = im.avg(q.0, q.1, r: 1)
                    soft(caso, "ojo \(e) cubo ciano delante del panel visible", near(cc, (0, 255, 255), 80), "pixel (\(q.0),\(q.1)) color \(cc) esperado ciano")
                }
            } else { check(caso, "ojo \(e) HUD magenta", false, "detras de la camara") }
        }
    }
}

// ---------- Arranque limpio: sin cine ni escena no hay entorno mínimo; el ojo sale en negro limpio (sin rejilla ni panel) y sin fallos ----------
do {
    for (nm, layered) in [("layered", true), ("dedicated", false)] {
        let caso = "negro-\(nm)-sin-nada"
        if layered && !device.supportsVertexAmplificationCount(2) { continue }
        setDefaults(curve: 0.35, sharp: 1)
        guard let tg = makeTargets(layered: layered) else { check(caso, "texturas", false, "no se pudieron crear"); continue }
        guard let enc = XRFrameEncoder(device: device, colorFormat: colorFormat, depthFormat: depthFormat, layered: layered, sampleCount: 4) else {
            check(caso, "codificador", false, "XRFrameEncoder no se pudo crear"); continue
        }
        enc.log = { print("[diag] " + $0) }
        var gs = MvVrGameState(); gs.view_mode = 1
        let head = headPose(SIMD3<Float>(0, 1.6, 0), yaw: 0)
        guard let imgs = render(enc, layered: layered, color: tg.color, depth: tg.depth, gs: gs, frames: 6, head: { _ in head }) else {
            check(caso, "render", false, "fallo al renderizar"); continue
        }
        report(caso, imgs, notice: false)
        print("[diag] última: \(enc.lastDiag)")
        for e in 0..<2 { check(caso, "ojo \(e) negro limpio", imgs[e].nonBlack < 0.005, String(format: "%.2f %% de pixeles no negros (maximo 0,5 %%: ni rejilla ni panel)", imgs[e].nonBlack * 100)) }
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
// Proyección del sistema con plano lejano infinito (P22 = 0) y rango degenerado (inf, 0.1) como en el visor: debe usarse y dar la misma imagen
runCase(Cfg(name: "layered-msaa4-sistema-inf", layered: true, msaa: 4, range: SIMD2<Float>(Float.infinity, 0.1), sysInf: true), first: false)
sysInf = false

// ---------- Fluidez de la locomoción (vr_smooth): escenas sintéticas a 25 Hz, movimiento constante, render a 100 Hz ----------
// El "juego" publica una escena nueva cada 40 ms (bench_publish_motion: el mundo ya re-expresado respecto al personaje y al giro que traía su cámara) y el visor dibuja a 100 Hz.
// Se sigue el centroide del cubo rojo en el ojo izquierdo, fotograma a fotograma, con la corrección de fluidez apagada (comportamiento anterior) y encendida, y se compara con la posición
// ideal del movimiento continuo. Caminar: el personaje avanza 1 m/s de lado (el mundo va a la izquierda). Girar: el stick gira 30 grados/s y la cámara de cada escena trae el giro de 100 ms antes.
struct FluidezRes { var cv = 0.0, mean = 0.0, maxStep = 0.0, sdev = 0.0, maxErr = 0.0, rmsErr = 0.0, frames = 0 }

func fluidezScenario(_ caso: String, layered: Bool, color: [MTLTexture], depth: [MTLTexture], enc: XRFrameEncoder, walkV: Double, turnDps: Double, smooth: Bool,
                     frames: Int, sceneHz: Double = 25, pipelineLag: Double = 0.1) -> FluidezRes? {
    let K = 0.001
    let lead = 0.012
    bench_smooth_config(1, Float(lead))
    enc.smoothMotion = smooth
    bench_make_pattern(); bench_make_scene(); bench_enable_radar(0); bench_enable_weapon(0)
    let head = headPose(SIMD3<Float>(0, 1.6, 0), yaw: 0)
    var gs = MvVrGameState(); gs.view_mode = 1; gs.game = 1; gs.in_control = 1; gs.cam_active = 1; gs.first_person = 1
    func turnReq(_ t: Double) -> Double { turnDps * max(t, 0) }
    func rotY(_ a: Double, _ v: SIMD3<Double>) -> SIMD3<Double> {
        let r = a * Double.pi / 180, c = cos(r), s = sin(r)
        return SIMD3<Double>(c * v.x - s * v.z, v.y, s * v.x + c * v.z)
    }
    let marker = SIMD3<Double>(-1.2, 0.5, -3.0)
    func projF(_ w: SIMD3<Double>) -> (Double, Double)? {
        let v = (head * eyes[0].transform).inverse
        let p = XRFrameEncoder.projection(eyes[0], depthRange: SIMD2<Float>(1000, 0.1))
        let c = p * v * SIMD4<Float>(Float(w.x), Float(w.y), Float(w.z), 1)
        guard c.w > 0.01 else { return nil }
        return (Double((c.x / c.w * 0.5 + 0.5) * Float(EW)), Double((1 - (c.y / c.w * 0.5 + 0.5)) * Float(EH)))
    }
    var k = 0
    var meas: [Double] = [], ideal: [Double] = []
    var lastOx = 0.0, lastTurn = 0.0
    for i in 0..<frames {
        let t = Double(i) / 100.0
        while t >= Double(k) / sceneHz + 0.002 {
            let tg = Double(k) / sceneHz
            lastOx = walkV * tg / K; lastTurn = turnReq(tg - pipelineLag)
            bench_publish_motion(tg, tg, lastOx, 0, lastTurn)
            k += 1
        }
        bench_smooth_set_time(t, turnReq(t))
        guard let cb = queue.makeCommandBuffer() else { return nil }
        let ok = enc.encode(commandBuffer: cb, gs: gs, originFromDevice: head, recenterInverse: matrix_identity_float4x4, dt: 0.01,
                            eyes: eyes, depthRange: depthRange, colorTextures: color, depthTextures: depth, systemProjections: nil, debugAxes: [])
        if !ok { check(caso, "encode", false, "devolvió false"); return nil }
        let buf = blitEye(cb, color, layered: layered, eye: 0)
        cb.commit(); cb.waitUntilCompleted()
        guard cb.status == .completed, let b = buf else { check(caso, "render", false, "estado \(cb.status.rawValue)"); return nil }
        guard i >= 40 else { continue }
        let px = b.contents().assumingMemoryBound(to: UInt8.self)
        var sw = 0.0, sx = 0.0
        for y in 0..<EH { for x in 0..<EW {
            let o = (y * EW + x) * 4
            let w = max(0.0, Double(px[o + 2]) - (Double(px[o + 1]) + Double(px[o])) * 0.5)
            if w > 8 { sw += w; sx += w * Double(x) }
        } }
        guard sw > 2000 else { check(caso, "marcador visible", false, "fotograma \(i): el cubo rojo no se ve (peso \(sw))"); return nil }
        meas.append(sx / sw)
        // Referencia ideal: la MISMA escena dibujada por el mismo camino pero con el mundo exacto del instante t + lead (sin suavizador); así el
        // centroide del cubo (que no es la proyección de su centro) se compara con su propio valor ideal.
        let te = t + lead
        bench_publish_geom(walkV * te / K, 0, turnReq(t))
        enc.smoothMotion = false
        guard let cb2 = queue.makeCommandBuffer() else { return nil }
        let ok2 = enc.encode(commandBuffer: cb2, gs: gs, originFromDevice: head, recenterInverse: matrix_identity_float4x4, dt: 0.01,
                             eyes: eyes, depthRange: depthRange, colorTextures: color, depthTextures: depth, systemProjections: nil, debugAxes: [])
        enc.smoothMotion = smooth
        let buf2 = ok2 ? blitEye(cb2, color, layered: layered, eye: 0) : nil
        cb2.commit(); cb2.waitUntilCompleted()
        guard cb2.status == .completed, let b2 = buf2 else { check(caso, "render ideal", false, "estado \(cb2.status.rawValue)"); return nil }
        let px2 = b2.contents().assumingMemoryBound(to: UInt8.self)
        var sw2 = 0.0, sx2 = 0.0
        for y in 0..<EH { for x in 0..<EW {
            let o = (y * EW + x) * 4
            let w = max(0.0, Double(px2[o + 2]) - (Double(px2[o + 1]) + Double(px2[o])) * 0.5)
            if w > 8 { sw2 += w; sx2 += w * Double(x) }
        } }
        guard sw2 > 2000 else { check(caso, "marcador ideal visible", false, "fotograma \(i)"); return nil }
        ideal.append(sx2 / sw2)
        bench_publish_geom(lastOx, 0, lastTurn)     // deja la escena vigente tal como la publicó el juego
    }
    var steps: [Double] = []
    for i in 1..<meas.count { steps.append(abs(meas[i] - meas[i - 1])) }
    var r = FluidezRes()
    r.frames = meas.count
    r.mean = steps.reduce(0, +) / Double(steps.count)
    r.sdev = sqrt(steps.map { ($0 - r.mean) * ($0 - r.mean) }.reduce(0, +) / Double(steps.count))
    r.cv = r.mean > 1e-9 ? r.sdev / r.mean : 0
    r.maxStep = steps.max() ?? 0
    let bias = zip(meas, ideal).map { $0 - $1 }.reduce(0, +) / Double(meas.count)
    let errs = zip(meas, ideal).map { abs($0 - $1 - bias) }
    r.maxErr = errs.max() ?? 0
    r.rmsErr = sqrt(errs.map { $0 * $0 }.reduce(0, +) / Double(errs.count))
    return r
}

func runFluidez(layered: Bool) {
    let name = layered ? "layered" : "dedicated"
    if layered && !device.supportsVertexAmplificationCount(2) { return }
    depthRange = SIMD2<Float>(1000, 0.1); sysInf = false
    setDefaults(curve: 0.35, sharp: 1)
    guard let tg = makeTargets(layered: layered) else { check("fluidez-\(name)", "texturas", false, "no se pudieron crear"); return }
    guard let enc = XRFrameEncoder(device: device, colorFormat: colorFormat, depthFormat: depthFormat, layered: layered, sampleCount: 4) else {
        check("fluidez-\(name)", "codificador", false, "XRFrameEncoder no se pudo crear"); return
    }
    enc.log = { print("[diag] " + $0) }
    print("\n######## fluidez \(name) ########")
    for (label, walkV, turnDps, nf) in [("caminar 1 m/s", 1.0, 0.0, 160), ("girar 30 gr/s", 0.0, 30.0, 110), ("caminar 0,5 m/s y girar 30 gr/s", 0.5, 30.0, 110)] {
        let caso = "fluidez-\(name)/\(label)"
        guard let off = fluidezScenario(caso, layered: layered, color: tg.color, depth: tg.depth, enc: enc, walkV: walkV, turnDps: turnDps, smooth: false, frames: nf),
              let on = fluidezScenario(caso, layered: layered, color: tg.color, depth: tg.depth, enc: enc, walkV: walkV, turnDps: turnDps, smooth: true, frames: nf) else { continue }
        print(String(format: "[fluidez] %@ | sin: paso medio %.2f px, desviacion %.2f, CV %.3f, paso max %.2f px, error vs ideal rms %.2f max %.2f px | con: paso medio %.2f, desviacion %.2f, CV %.3f, paso max %.2f, error rms %.2f max %.2f px",
                     caso, off.mean, off.sdev, off.cv, off.maxStep, off.rmsErr, off.maxErr, on.mean, on.sdev, on.cv, on.maxStep, on.rmsErr, on.maxErr))
        print("[diag] fluidez última corrección: activa=\(enc.lastSmooth.active) giro=\(enc.lastSmooth.delta_deg) gr")
        check(caso, "sensibilidad: sin corregir hay judder o retraso", off.cv > 0.3 || off.maxErr > 6, String(format: "CV %.3f error max %.2f px", off.cv, off.maxErr))
        check(caso, "CV del paso por fotograma", on.cv < 0.25 && on.cv < off.cv * 0.5 + 1e-9, String(format: "con %.3f, sin %.3f (limite 0,25 y menos de la mitad)", on.cv, off.cv))
        check(caso, "error frente al movimiento continuo", on.maxErr < 3.0, String(format: "max %.2f px rms %.2f px (sin corregir: max %.2f)", on.maxErr, on.rmsErr, off.maxErr))
        check(caso, "paso maximo sin saltos", on.maxStep < max(on.mean * 2.0, 1.5), String(format: "paso max %.2f px, medio %.2f px (sin corregir: max %.2f)", on.maxStep, on.mean, off.maxStep))
    }
    enc.smoothMotion = true
}

runFluidez(layered: true)
runFluidez(layered: false)
print("\nresumen: \(failures) comprobaciones falladas")
if !warnList.isEmpty { annot("warning", "banco de render (informativo)", warnList.prefix(30).joined(separator: "\n")) }
if failures > 0 {
    annot("error", "banco de render", "\(failures) comprobaciones falladas\n" + failList.prefix(60).joined(separator: "\n"))
    exit(1)
}
print("TODO OK")
