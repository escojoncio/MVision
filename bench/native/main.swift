import Foundation
import Metal
import simd

// Banco del renderer nativo (macOS, Apple Silicon, CI): carga una NativeScene sintética desde un fichero de prueba generado por código
// (bench/c/native_synth.c; ningún dato del juego), la publica por el puente (ns_bridge_*), la dibuja con NativeRenderer en un cubemap (6 caras) con Metal y la
// compara con la salida del rasterizador de referencia por CPU (ns_cpuref) para la MISMA escena. Criterios: diferencia media < 2/255 y huecos 0.
// Además prueba: estéreo con amplificación de vértices frente a un pase por ojo, MSAA 4x, la caché y el ack tras un cambio de mesh_epoch y el sesgo de orden (decals).
// Sube las imágenes (referencia, Metal, diferencia, panoramas) como artefacto.

setvbuf(stdout, nil, _IONBF, 0)
guard CommandLine.arguments.count >= 3 else { print("uso: nbench <native.metallib> <directorio de salida>"); exit(2) }
let libPath = CommandLine.arguments[1]
let outDir = CommandLine.arguments[2]
try? FileManager.default.createDirectory(atPath: outDir, withIntermediateDirectories: true)

var failures = 0
var failList: [String] = []
func annot(_ level: String, _ title: String, _ msg: String) {
    let m = msg.replacingOccurrences(of: "%", with: "%25").replacingOccurrences(of: "\r", with: "%0D").replacingOccurrences(of: "\n", with: "%0A")
    print("::\(level) title=\(title)::\(m)")
}
func check(_ name: String, _ ok: Bool, _ detail: String) {
    print("[\(ok ? "OK  " : "FALLO")] \(name): \(detail)")
    if !ok { failures += 1; failList.append("\(name): \(detail)") }
}
func soft(_ name: String, _ ok: Bool, _ detail: String) { print("[\(ok ? "OK  " : "AVISO")] \(name): \(detail)") }

guard let device = MTLCreateSystemDefaultDevice(), let queue = device.makeCommandQueue() else {
    annot("error", "banco nativo", "No hay dispositivo Metal en este runner")
    print("FALLO: sin dispositivo Metal"); exit(2)
}
print("dispositivo: \(device.name) | amplificacion x2: \(device.supportsVertexAmplificationCount(2)) | msaa4: \(device.supportsTextureSampleCount(4))")
let library: MTLLibrary
do { library = try device.makeLibrary(URL: URL(fileURLWithPath: libPath)) } catch {
    annot("error", "banco nativo", "No se pudo cargar \(libPath): \(error)")
    print("FALLO: \(error)"); exit(2)
}

// MARK: escena sintética

let scenePath = outDir + "/scene.nsy"
check("escena", ns_synth_write(scenePath) == 0, "generada en \(scenePath)")
let lr = ns_synth_load(scenePath)
check("escena", lr == 0, "cargada y publicada por el puente (código \(lr))")
guard lr == 0, let vramPtr = ns_synth_vram() else { print("FALLO: sin escena"); exit(1) }

let vtd = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .r16Uint, width: 1024, height: 512, mipmapped: false)
vtd.usage = [.shaderRead]
vtd.storageMode = .shared
guard let vram = device.makeTexture(descriptor: vtd) else { print("FALLO: sin textura de VRAM"); exit(2) }
vram.replace(region: MTLRegionMake2D(0, 0, 1024, 512), mipmapLevel: 0, withBytes: vramPtr, bytesPerRow: 2048)

guard let nr = NativeRenderer(device: device, library: library, ringSize: 3) else { print("FALLO: NativeRenderer no se pudo crear"); exit(2) }
nr.bridge = ns_synth_bridge()

var headArr = [Float](repeating: 0, count: 3)
ns_synth_head(&headArr)
let head = SIMD3<Float>(headArr[0], headArr[1], headArr[2])

// MARK: cámaras

/// Base (derecha, abajo, delante) de la cara `face` del cubemap en el mundo VR, igual que ns_render_cube; vista de Metal (x derecha, y arriba, -z delante).
func faceView(_ face: Int, _ pos: SIMD3<Float>) -> simd_float4x4 {
    let P = UnsafeBufferPointer(start: ns_synth_pose_basis(), count: 9)
    let F = UnsafeBufferPointer(start: ns_synth_face_basis(Int32(face)), count: 9)
    var A = [Float](repeating: 0, count: 9)
    for r in 0..<3 { for k in 0..<3 { A[r * 3 + k] = F[r * 3] * P[k] + F[r * 3 + 1] * P[3 + k] + F[r * 3 + 2] * P[6 + k] } }
    let right = SIMD3<Float>(A[0], A[1], A[2]), down = SIMD3<Float>(A[3], A[4], A[5]), fwd = SIMD3<Float>(A[6], A[7], A[8])
    let up = -down, back = -fwd
    return simd_float4x4(columns: (SIMD4<Float>(right.x, up.x, back.x, 0), SIMD4<Float>(right.y, up.y, back.y, 0), SIMD4<Float>(right.z, up.z, back.z, 0),
                                   SIMD4<Float>(-simd_dot(right, pos), -simd_dot(up, pos), -simd_dot(back, pos), 1)))
}

/// Proyección de Z invertida con plano lejano INFINITO (la que da el compositor: P22 = 0, P32 = cercano), tangentes simétricas t.
func infProjection(tan t: Float, near: Float) -> simd_float4x4 {
    simd_float4x4(columns: (SIMD4<Float>(1 / t, 0, 0, 0), SIMD4<Float>(0, 1 / t, 0, 0), SIMD4<Float>(0, 0, 0, -1), SIMD4<Float>(0, 0, near, 0)))
}

// MARK: render

func half(_ h: UInt16) -> Float {
    let s = (h >> 15) & 1, e = Int((h >> 10) & 31), m = Int(h & 1023)
    var v: Float
    if e == 0 { v = Float(m) * 5.9604645e-8 }
    else if e == 31 { v = m == 0 ? Float.infinity : Float.nan }
    else { v = (1 + Float(m) / 1024) * exp2f(Float(e - 15)) }
    return s == 1 ? -v : v
}

func makeTexture(_ fmt: MTLPixelFormat, _ n: Int, layers: Int, samples: Int, label: String) -> MTLTexture? {
    let d = MTLTextureDescriptor()
    d.pixelFormat = fmt; d.width = n; d.height = n
    if layers > 1 {
        d.textureType = samples > 1 ? .type2DMultisampleArray : .type2DArray
        d.arrayLength = layers
    } else {
        d.textureType = samples > 1 ? .type2DMultisample : .type2D
    }
    d.sampleCount = samples
    d.usage = [.renderTarget, .shaderRead]
    d.storageMode = .private
    let t = device.makeTexture(descriptor: d)
    t?.label = label
    return t
}

struct RenderResult { var rgba: [[Float]]; var cpuMs: Double }

/// Dibuja `views` (cámaras de Metal) con NativeRenderer. layered: un solo pase con amplificación a las 2 primeras vistas; si no, un pase por vista.
/// Devuelve RGBA (float, 0..1) por vista, la alfa = cobertura.
func renderViews(n: Int, views: [simd_float4x4], projs: [simd_float4x4], eyes: [SIMD3<Float>], layered: Bool, samples: Int, format: MTLPixelFormat = .rgba16Float) -> RenderResult? {
    let nl = layered ? 2 : 1
    guard let colorMS = makeTexture(format, n, layers: nl, samples: samples, label: "color"),
          let depth = makeTexture(.depth32Float, n, layers: nl, samples: samples, label: "depth") else { return nil }
    var resolve: MTLTexture? = nil
    if samples > 1 {
        resolve = makeTexture(format, n, layers: nl, samples: 1, label: "resolve")
        if resolve == nil { return nil }
    }
    let readTex = resolve ?? colorMS
    var out: [[Float]] = []
    var cpu = 0.0
    let passes = layered ? 1 : views.count
    for p in 0..<passes {
        guard let cb = queue.makeCommandBuffer() else { return nil }
        let t0 = CFAbsoluteTimeGetCurrent()
        nr.prepareFrame(sortView: views[p])
        let pd = MTLRenderPassDescriptor()
        pd.colorAttachments[0].texture = colorMS
        pd.colorAttachments[0].loadAction = .clear
        pd.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 0)
        if samples > 1, let r = resolve {
            pd.colorAttachments[0].resolveTexture = r
            pd.colorAttachments[0].storeAction = .multisampleResolve
        } else {
            pd.colorAttachments[0].storeAction = .store
        }
        pd.depthAttachment.texture = depth
        pd.depthAttachment.loadAction = .clear
        pd.depthAttachment.clearDepth = 0
        pd.depthAttachment.storeAction = .dontCare
        if layered { pd.renderTargetArrayLength = 2 }
        guard let enc = cb.makeRenderCommandEncoder(descriptor: pd) else { return nil }
        let vp = MTLViewport(originX: 0, originY: 0, width: Double(n), height: Double(n), znear: 0, zfar: 1)
        if layered {
            enc.setViewports([vp, vp])
            var maps = (0..<2).map { MTLVertexAmplificationViewMapping(viewportArrayIndexOffset: UInt32($0), renderTargetArrayIndexOffset: UInt32($0)) }
            enc.setVertexAmplificationCount(2, viewMappings: &maps)
        } else {
            enc.setViewport(vp)
        }
        let vps = layered ? [projs[0] * views[0], projs[1] * views[1]] : [projs[p] * views[p]]
        let eyeList = layered ? [eyes[0], eyes[1]] : [eyes[p]]
        nr.encode(encoder: enc, vram: vram, viewProj: vps, eyes: eyeList, layered: layered, colorFormat: format, depthFormat: .depth32Float, samples: samples, linear: false)
        enc.endEncoding()
        cpu += CFAbsoluteTimeGetCurrent() - t0
        var bufs: [MTLBuffer] = []
        if let bl = cb.makeBlitCommandEncoder() {
            for s in 0..<nl {
                guard let b = device.makeBuffer(length: n * n * 8, options: .storageModeShared) else { return nil }
                bl.copy(from: readTex, sourceSlice: s, sourceLevel: 0, sourceOrigin: MTLOrigin(x: 0, y: 0, z: 0), sourceSize: MTLSize(width: n, height: n, depth: 1),
                        to: b, destinationOffset: 0, destinationBytesPerRow: n * 8, destinationBytesPerImage: n * n * 8)
                bufs.append(b)
            }
            bl.endEncoding()
        }
        cb.commit()
        cb.waitUntilCompleted()
        if cb.status != .completed { print("FALLO: command buffer con estado \(cb.status.rawValue): \(String(describing: cb.error))"); return nil }
        for b in bufs {
            let h = UnsafeBufferPointer(start: b.contents().assumingMemoryBound(to: UInt16.self), count: n * n * 4)
            out.append(h.map { half($0) })
        }
    }
    return RenderResult(rgba: out, cpuMs: cpu * 1000)
}

func toRGB8(_ rgba: [Float], _ n: Int) -> [UInt8] {
    var o = [UInt8](repeating: 0, count: n * n * 3)
    for i in 0..<(n * n) {
        for k in 0..<3 {
            let c = min(max(rgba[i * 4 + k], 0), 1)
            o[i * 3 + k] = UInt8(min(255, Int(c * 255 + 0.5)))
        }
    }
    return o
}
func holes(_ rgba: [Float], _ n: Int) -> Int {
    var h = 0
    for i in 0..<(n * n) where rgba[i * 4 + 3] < 0.5 { h += 1 }
    return h
}

func crossImage(_ faces: [[UInt8]], _ n: Int) -> [UInt8] {
    let W = 4 * n, H = 3 * n
    var img = [UInt8](repeating: 0, count: W * H * 3)
    let pos = [(1, 1), (2, 1), (3, 1), (0, 1), (1, 0), (1, 2)]
    for f in 0..<6 {
        for y in 0..<n {
            let dst = ((pos[f].1 * n + y) * W + pos[f].0 * n) * 3
            img.replaceSubrange(dst..<(dst + n * 3), with: faces[f][(y * n * 3)..<((y + 1) * n * 3)])
        }
    }
    return img
}
func writePNG(_ name: String, _ w: Int, _ h: Int, _ rgb: [UInt8]) { _ = ns_write_png(outDir + "/" + name, Int32(w), Int32(h), rgb) }

// MARK: 1. cubemap frente a la referencia de CPU

let N = 512
let NEAR: Float = 0.05
let proj = infProjection(tan: 1, near: NEAR)
var faceViews: [simd_float4x4] = []
for f in 0..<6 { faceViews.append(faceView(f, head)) }

var metal: [[Float]] = []
var cubeMs = 0.0
for f in 0..<6 {
    guard let r = renderViews(n: N, views: [faceViews[f]], projs: [proj], eyes: [head], layered: false, samples: 1) else { print("FALLO: render de la cara \(f)"); exit(1) }
    metal.append(r.rgba[0]); cubeMs += r.cpuMs
}
print(nr.diagnostics())

var refFaces = [NsRBuf](repeating: NsRBuf(), count: 6)
var refStats = NsRStats()
let refTris = ns_synth_ref_cube(Int32(N), NEAR, &refFaces, &refStats)
check("referencia", refTris > 300, "\(refTris) triángulos, dibujados \(refStats.tris_drawn), píxeles opacos \(refStats.px_opaque) semi \(refStats.px_semi), huecos \(String(format: "%.5f", refStats.hole_frac)), \(String(format: "%.0f", refStats.ms)) ms")

var refRGB: [[UInt8]] = [], metalRGB: [[UInt8]] = [], diffRGB: [[UInt8]] = []
var maeSum = 0.0, overSum = 0.0, maxFaceMae = 0.0
var metalHoles = 0, refHoles = 0
for f in 0..<6 {
    var rr = [UInt8](repeating: 0, count: N * N * 3)
    ns_buf_to_rgb8(&refFaces[f], &rr, 0, 0)
    let mr = toRGB8(metal[f], N)
    var df = [UInt8](repeating: 0, count: N * N * 3)
    var d = NsRDiff()
    ns_diff_rgb8(rr, mr, Int32(N), Int32(N), 8, &df, &d)
    maeSum += d.mae; overSum += d.frac_over; maxFaceMae = max(maxFaceMae, d.mae)
    let mh = holes(metal[f], N)
    var rh = 0
    for i in 0..<(N * N) where refFaces[f].iz[i] == 0 { rh += 1 }
    metalHoles += mh; refHoles += rh
    print("  cara \(f): dif. media \(String(format: "%.3f", d.mae))/255, píxeles con |dif|>8: \(String(format: "%.3f", d.frac_over * 100)) %, huecos Metal \(mh), referencia \(rh)")
    refRGB.append(rr); metalRGB.append(mr); diffRGB.append(df)
}
let mae = maeSum / 6, over = overSum / 6
check("cubemap", mae < 2.0, String(format: "diferencia media %.3f/255 (< 2), peor cara %.3f, píxeles con dif>8: %.3f %%", mae, maxFaceMae, over * 100))
check("huecos", metalHoles == 0 && refHoles == 0, "Metal \(metalHoles) píxeles sin cobertura, referencia \(refHoles)")
writePNG("ref_cross.png", 4 * N, 3 * N, crossImage(refRGB, N))
writePNG("metal_cross.png", 4 * N, 3 * N, crossImage(metalRGB, N))
writePNG("diff_x4_cross.png", 4 * N, 3 * N, crossImage(diffRGB, N))

// panoramas equirectangulares (mismo remuestreo de cubemap para ambas imágenes)
var mFaces = [NsRBuf](repeating: NsRBuf(), count: 6)
for f in 0..<6 {
    _ = ns_buf_alloc(&mFaces[f], Int32(N), Int32(N))
    for i in 0..<(N * N) {
        for k in 0..<3 { mFaces[f].c[i * 3 + k] = min(max(metal[f][i * 4 + k], 0), 1) * 31 }
        mFaces[f].iz[i] = metal[f][i * 4 + 3] > 0.5 ? 1 : 0
    }
}
var pano = NsRBuf(), panoRef = NsRBuf()
_ = ns_buf_alloc(&pano, 2048, 1024); _ = ns_buf_alloc(&panoRef, 2048, 1024)
var poseCam = NsRCam()
ns_cube_to_equirect(&mFaces, &poseCam, &pano)
ns_cube_to_equirect(&refFaces, &poseCam, &panoRef)
var prgb = [UInt8](repeating: 0, count: 2048 * 1024 * 3), prrgb = [UInt8](repeating: 0, count: 2048 * 1024 * 3), pdiff = [UInt8](repeating: 0, count: 2048 * 1024 * 3)
ns_buf_to_rgb8(&pano, &prgb, 0, 0); ns_buf_to_rgb8(&panoRef, &prrgb, 0, 0)
var pd = NsRDiff()
ns_diff_rgb8(prrgb, prgb, 2048, 1024, 8, &pdiff, &pd)
print("  panorama 2048x1024: dif. media \(String(format: "%.3f", pd.mae))/255")
writePNG("metal_equirect.png", 2048, 1024, prgb)
writePNG("ref_equirect.png", 2048, 1024, prrgb)
writePNG("diff_x4_equirect.png", 2048, 1024, pdiff)

// MARK: 2. estéreo con amplificación de vértices frente a un pase por ojo

let ipd: Float = 0.063
let P0 = UnsafeBufferPointer(start: ns_synth_pose_basis(), count: 9)
let rightV = SIMD3<Float>(P0[0], P0[1], P0[2])
let eyePos = [head - rightV * (ipd / 2), head + rightV * (ipd / 2)]
let eyeViews = eyePos.map { faceView(0, $0) }
let eyeProj = [proj, proj]
let M = 768
if let ref = renderViews(n: M, views: eyeViews, projs: eyeProj, eyes: eyePos, layered: false, samples: 1) {
    writePNG("eye0.png", M, M, toRGB8(ref.rgba[0], M)); writePNG("eye1.png", M, M, toRGB8(ref.rgba[1], M))
    check("estéreo", holes(ref.rgba[0], M) == 0 && holes(ref.rgba[1], M) == 0, "un pase por ojo sin huecos (\(holes(ref.rgba[0], M)), \(holes(ref.rgba[1], M)))")
    if device.supportsVertexAmplificationCount(2) {
        if let amp = renderViews(n: M, views: eyeViews, projs: eyeProj, eyes: eyePos, layered: true, samples: 1) {
            for e in 0..<2 {
                var d = NsRDiff()
                let a = toRGB8(ref.rgba[e], M), b = toRGB8(amp.rgba[e], M)
                ns_diff_rgb8(a, b, Int32(M), Int32(M), 2, nil, &d)
                check("amplificación ojo \(e)", d.mae < 0.05, "dif. media \(String(format: "%.4f", d.mae))/255 frente al pase por ojo (píxeles con dif>2: \(String(format: "%.4f", d.frac_over * 100)) %)")
            }
            writePNG("eye0_layered.png", M, M, toRGB8(amp.rgba[0], M))
        } else { check("amplificación", false, "falló el render con amplificación") }
    } else { soft("amplificación", true, "no soportada en este dispositivo: omitida") }
    if device.supportsTextureSampleCount(4) {
        if let ms = renderViews(n: M, views: [eyeViews[0]], projs: [proj], eyes: [eyePos[0]], layered: false, samples: 4) {
            var d = NsRDiff()
            ns_diff_rgb8(toRGB8(ref.rgba[0], M), toRGB8(ms.rgba[0], M), Int32(M), Int32(M), 8, nil, &d)
            check("MSAA 4x", holes(ms.rgba[0], M) == 0 && d.mae < 3.0, "huecos \(holes(ms.rgba[0], M)), dif. media \(String(format: "%.3f", d.mae))/255 frente a 1x")
            writePNG("eye0_msaa4.png", M, M, toRGB8(ms.rgba[0], M))
        } else { check("MSAA 4x", false, "falló el render") }
    }
    // coste de CPU (preparar + codificar) con la escena completa
    var t = 0.0
    let reps = 50
    for _ in 0..<reps {
        guard let r = renderViews(n: 64, views: [eyeViews[0]], projs: [proj], eyes: [eyePos[0]], layered: false, samples: 1) else { break }
        t += r.cpuMs
    }
    soft("coste CPU", true, "prepareFrame+codificación \(String(format: "%.3f", t / Double(reps))) ms por pase (\(nr.statDraws) llamadas de dibujo, \(nr.statSemiTris) triángulos semi)")
} else { check("estéreo", false, "falló el render por ojo") }

// MARK: 3. cambio de mesh_epoch (el renderer reconoce la época con ns_store_ack tras subir las mallas) y sesgo de orden (decals)

check("decal", ns_synth_publish_decal(2) == 0, "escena de decals publicada con otro mesh_epoch")
let down = faceView(5, SIMD3<Float>(0, 1.0, 0))
if let r = renderViews(n: 64, views: [down], projs: [proj], eyes: [SIMD3<Float>(0, 1.0, 0)], layered: false, samples: 1) {
    let rgb = toRGB8(r.rgba[0], 64)
    func px(_ x: Int, _ y: Int) -> (Int, Int, Int) { (Int(rgb[(y * 64 + x) * 3]), Int(rgb[(y * 64 + x) * 3 + 1]), Int(rgb[(y * 64 + x) * 3 + 2])) }
    let c = px(32, 32), e = px(4, 4)
    func near(_ a: (Int, Int, Int), _ b: (Int, Int, Int)) -> Bool { abs(a.0 - b.0) <= 2 && abs(a.1 - b.1) <= 2 && abs(a.2 - b.2) <= 2 }
    check("decal (depth_bias > 0 gana al coplanar)", near(c, (40, 200, 40)), "centro \(c), esperado (40,200,40)")
    check("decal (base visible fuera)", near(e, (200, 40, 40)), "borde \(e), esperado (200,40,40)")
    writePNG("decal.png", 64, 64, rgb)
}
check("ack de época", ns_synth_acked() == 2, "época reconocida \(ns_synth_acked()), esperada 2")
print(nr.diagnostics())

// MARK: resumen
annot("notice", "banco nativo", "cubemap \(N)x\(N): dif. media \(String(format: "%.3f", mae))/255, huecos Metal \(metalHoles), CPU \(String(format: "%.2f", cubeMs / 6)) ms/cara; fallos: \(failures)")
if failures > 0 {
    annot("error", "banco nativo", failList.joined(separator: "\n"))
    print("FALLO: \(failures) comprobaciones")
    exit(1)
}
print("OK: banco nativo sin fallos")
