import Metal
import simd
import Foundation

// Renderer del arma en mano: dibuja la geometría 3D propia (piezas del modelo original cortado + objetos sueltos) que entrega
// vr/hands (mv_host_hands -> VrHandsFrame), DENTRO del mismo pase de escena de PS1Renderer. SIN COMPILAR (escrito sin
// toolchain Apple): revisar en la primera compilación. Shaders: render/metal/vr_weapon.metal.
//
// Estado que replica de PS1Renderer.encodeScene: Z invertida (profundidad limpiada a 0, compare greaterEqual, escritura
// activa), cullMode .none (doble cara; el shader invierte la normal de las caras vistas desde dentro), MSAA con el mismo
// sampleCount, amplificación de vértices si `layered` (el encoder ya trae setVertexAmplificationCount y viewports; el
// pipeline declara maxVertexAmplificationCount = viewCount), formatos de color/profundidad (con stencil si hace falta).
// Matrices: views = mundo->ojo, projections = ojo->clip (clip = P * V * mundo), mundo VR en metros, Y arriba, -Z delante.
//
// USO (ver el informe de integración): crear una vez junto a PS1Renderer con los mismos formatos/sampleCount/viewCount y,
// dentro del pase de escena del ÚLTIMO segmento (el que se resuelve), llamar a encode(...) tras los opacos del juego.
// encode PISA los enlaces del encoder (búfer de vértices 0-2, textura 0, estado de profundidad, sesgo de profundidad):
// si después se siguen dibujando lotes de la escena hay que volver a enlazarlos (ver PS1Renderer.bindSceneState).

// Mismo layout que VrWpnFrame / VrWpnDraw de vr_weapon.metal (192 y 64 bytes).
struct WpnFrameU {
    var viewProj0 = matrix_identity_float4x4
    var viewProj1 = matrix_identity_float4x4
    var eye0 = simd_float4(0, 0, 0, 1)
    var eye1 = simd_float4(0, 0, 0, 1)
    var light = simd_float4(0, 1, 0, 0.78)     // xyz = hacia la luz, w = ambiente
    var params = simd_float4(0.62, 0.5, 0, 0)  // x = difuso, y = alfa de corte
}
struct WpnDrawU {
    var model = matrix_identity_float4x4
}

final class WeaponRenderer {
    // MARK: API pública
    let device: MTLDevice
    /// Dibujar o no (permite desactivarlo sin quitar la llamada).
    var enabled = true
    /// Suavizado bilineal del atlas. Por defecto nearest (el atlas ya viene escalado x2/x4 por wload).
    var textureFilter = false
    /// true si el destino es sRGB/lineal (se deduce del formato; ajustable antes del primer encode).
    var linearOutput: Bool
    /// Texels con alfa menor que esto se descartan.
    var alphaCutoff: Float = 0.5

    // MARK: estado
    private let colorFormat: MTLPixelFormat
    private let depthFormat: MTLPixelFormat
    private let sampleCount: Int
    private let viewCount: Int
    private let library: MTLLibrary?
    private var depthState: MTLDepthStencilState?
    private var whiteTexture: MTLTexture?

    private struct PKey: Hashable {
        var layered: Bool, filter: Bool, linear: Bool
        var color: MTLPixelFormat, depth: MTLPixelFormat, samples: Int
    }
    private var pipes: [PKey: MTLRenderPipelineState] = [:]
    private var failed = Set<String>()

    private final class MeshGPU {
        var vbuf: MTLBuffer; var ibuf: MTLBuffer; var nverts: Int; var nidx: Int; var seen = 0
        init(vbuf: MTLBuffer, ibuf: MTLBuffer, nverts: Int, nidx: Int) { self.vbuf = vbuf; self.ibuf = ibuf; self.nverts = nverts; self.nidx = nidx }
    }
    private final class TexGPU {
        var tex: MTLTexture; var w: Int; var h: Int; var seen = 0
        init(tex: MTLTexture, w: Int, h: Int) { self.tex = tex; self.w = w; self.h = h }
    }
    private var meshCache: [UInt32: MeshGPU] = [:]
    private var texCache: [UInt32: TexGPU] = [:]
    private var tick = 0
    /// Último frame recibido (para el segundo ojo del modo no layered: misma lista, no se vuelve a pedir).
    private var lastFrame: VrHandsFrame?
    private static let staleTicks = 600   // llamadas a encode sin verlo antes de liberar una malla/textura

    // MARK: init
    init(device: MTLDevice, colorFormat: MTLPixelFormat, depthFormat: MTLPixelFormat, sampleCount: Int, viewCount: Int) {
        self.device = device
        self.colorFormat = colorFormat
        self.depthFormat = depthFormat
        self.sampleCount = max(1, sampleCount)
        self.viewCount = max(1, min(2, viewCount))
        switch colorFormat {
        case .bgra8Unorm_srgb, .rgba8Unorm_srgb, .rgba16Float: linearOutput = true
        default: linearOutput = false
        }
        let lib = device.makeDefaultLibrary()
        library = lib
        if lib == nil { print("[WeaponRenderer] sin biblioteca Metal por defecto: falta vr_weapon.metal en el target") }
        if MemoryLayout<WpnFrameU>.stride != 192 || MemoryLayout<WpnDrawU>.stride != 64 {
            print("[WeaponRenderer] layout de uniformes incompatible con vr_weapon.metal (\(MemoryLayout<WpnFrameU>.stride)/\(MemoryLayout<WpnDrawU>.stride))")
        }
        if MemoryLayout<VrHandsVertex>.stride != 36 { print("[WeaponRenderer] VrHandsVertex no mide 36 bytes") }
        let dd = MTLDepthStencilDescriptor()
        dd.depthCompareFunction = .greaterEqual    // Z invertida, igual que dsOpaque de PS1Renderer
        dd.isDepthWriteEnabled = true
        depthState = device.makeDepthStencilState(descriptor: dd)
        let td = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm, width: 1, height: 1, mipmapped: false)
        td.usage = .shaderRead
        whiteTexture = device.makeTexture(descriptor: td)
        var px: [UInt8] = [255, 255, 255, 255]
        whiteTexture?.replace(region: MTLRegionMake2D(0, 0, 1, 1), mipmapLevel: 0, withBytes: &px, bytesPerRow: 4)
        whiteTexture?.label = "WPN blanco"
    }

    // MARK: pipeline
    private func pipeline(_ k: PKey) -> MTLRenderPipelineState? {
        if let p = pipes[k] { return p }
        guard let lib = library else { return nil }
        do {
            let fc = MTLFunctionConstantValues()
            var f = k.filter, l = k.linear
            fc.setConstantValue(&f, type: .bool, index: 0)   // VR_WPN_FC_FILTER
            fc.setConstantValue(&l, type: .bool, index: 1)   // VR_WPN_FC_LINEAR_OUT
            let fs = try lib.makeFunction(name: "vr_fs_weapon", constantValues: fc)
            guard let vs = lib.makeFunction(name: k.layered ? "vr_vs_weapon_amp" : "vr_vs_weapon") else {
                if !failed.contains("vs") { failed.insert("vs"); print("[WeaponRenderer] falta la función de vértices") }
                return nil
            }
            let d = MTLRenderPipelineDescriptor()
            d.label = "WPN \(k.layered ? "amp" : "ojo")"
            d.vertexFunction = vs
            d.fragmentFunction = fs
            d.rasterSampleCount = k.samples
            d.inputPrimitiveTopology = .triangle
            if k.layered { d.maxVertexAmplificationCount = viewCount }
            d.colorAttachments[0].pixelFormat = k.color
            d.depthAttachmentPixelFormat = k.depth
            if k.depth == .depth32Float_stencil8 { d.stencilAttachmentPixelFormat = k.depth }   // igual que PS1Renderer (depth24Unorm_stencil8 no existe en visionOS)
            let p = try device.makeRenderPipelineState(descriptor: d)
            pipes[k] = p
            return p
        } catch {
            let key = "\(k)"
            if !failed.contains(key) { failed.insert(key); print("[WeaponRenderer] pipeline: \(error)") }
            return nil
        }
    }

    // MARK: cachés (subida única por id)
    private func texture(for t: VrHandsTex) -> MTLTexture? {
        if let c = texCache[t.id], c.w == Int(t.w), c.h == Int(t.h) { c.seen = tick; return c.tex }
        guard t.w > 0, t.h > 0, t.w <= 16384, t.h <= 16384, let px = t.rgba else { return nil }
        let td = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm, width: Int(t.w), height: Int(t.h), mipmapped: false)
        td.usage = .shaderRead
        guard let tex = device.makeTexture(descriptor: td) else { return nil }
        tex.label = "WPN atlas \(t.id)"
        tex.replace(region: MTLRegionMake2D(0, 0, Int(t.w), Int(t.h)), mipmapLevel: 0, withBytes: px, bytesPerRow: Int(t.w) * 4)
        let c = TexGPU(tex: tex, w: Int(t.w), h: Int(t.h)); c.seen = tick
        texCache[t.id] = c
        return tex
    }

    private func mesh(for m: VrHandsMesh) -> MeshGPU? {
        let nv = Int(m.nverts), ni = Int(m.nidx)
        if let c = meshCache[m.mesh_id], c.nverts == nv, c.nidx == ni { c.seen = tick; return c }
        guard nv > 0, ni >= 3, ni % 3 == 0, let v = m.verts, let ix = m.idx else { return nil }
        // los índices vienen validados por vrhands_bind (< nverts); se vuelve a comprobar antes de entregarlos a la GPU
        for i in 0..<ni where Int(ix[i]) >= nv { return nil }
        guard let vb = device.makeBuffer(bytes: UnsafeRawPointer(v), length: nv * MemoryLayout<VrHandsVertex>.stride, options: .storageModeShared),
              let ib = device.makeBuffer(bytes: UnsafeRawPointer(ix), length: ni * 2, options: .storageModeShared) else { return nil }
        vb.label = "WPN vértices \(m.mesh_id)"; ib.label = "WPN índices \(m.mesh_id)"
        let c = MeshGPU(vbuf: vb, ibuf: ib, nverts: nv, nidx: ni); c.seen = tick
        meshCache[m.mesh_id] = c
        return c
    }

    private func evictStale() {
        if tick % 120 != 0 { return }
        meshCache = meshCache.filter { tick - $0.value.seen <= WeaponRenderer.staleTicks }
        texCache = texCache.filter { tick - $0.value.seen <= WeaponRenderer.staleTicks }
    }

    private static func matrix(_ m: VrHandsMesh) -> simd_float4x4 {
        var out = simd_float4x4()
        var src = m.model
        withUnsafeMutableBytes(of: &out) { d in withUnsafeBytes(of: &src) { s in d.copyMemory(from: s) } }
        return out
    }

    // MARK: encode
    /// Dibuja el arma en el pase de escena en curso. `viewIndex` solo se usa si !layered (índice del ojo del pase).
    /// Llamar con el encoder del ÚLTIMO segmento de escena (el que resuelve el MSAA), tras los lotes opacos del juego.
    func encode(encoder: MTLRenderCommandEncoder, views: [simd_float4x4], projections: [simd_float4x4], layered: Bool, viewIndex: Int = 0) {
        tick += 1
        defer { evictStale() }
        // El frame de mv_host_hands (y los vértices/atlas a los que apunta) debe vivir hasta que se termine de usar: se avisa al
        // salir del último uso (layered: este mismo encode; no layered: el segundo ojo reutiliza la lista del primero, así que
        // libera el último ojo). Va antes de cualquier salida temprana para que el productor nunca espere en vano.
        defer {
            if layered || viewIndex >= min(views.count, projections.count) - 1 {
                if !layered { lastFrame = nil }
                mv_host_hands_done()
            }
        }
        guard enabled else { return }
        let n = min(views.count, projections.count)
        guard n >= 1 else { return }
        // 1) lista de mallas: en layered o en el primer ojo se pide una nueva; el segundo ojo reutiliza la del primero
        var fr = VrHandsFrame()
        if layered || viewIndex == 0 {
            if mv_host_hands(&fr) == 0 { lastFrame = nil; return }
            lastFrame = fr
        } else if let lf = lastFrame {
            fr = lf
        } else { return }
        let count = Int(fr.nmeshes)
        guard count > 0, let meshes = fr.meshes else { return }

        // 2) texturas del frame (subida única)
        var texById: [UInt32: MTLTexture] = [:]
        if fr.ntex > 0, let tl = fr.tex {
            for i in 0..<Int(fr.ntex) { if let t = texture(for: tl[i]) { texById[tl[i].id] = t } }
        }

        // 3) pipeline y uniformes del frame
        let colorFmt = colorFormat, depthFmt = depthFormat
        let key = PKey(layered: layered, filter: textureFilter, linear: linearOutput, color: colorFmt, depth: depthFmt, samples: sampleCount)
        guard let pso = pipeline(key), let ds = depthState else { return }
        var u = WpnFrameU()
        if layered {
            let cnt = min(n, viewCount)
            let i1 = min(1, cnt - 1)
            u.viewProj0 = projections[0] * views[0]
            u.viewProj1 = projections[i1] * views[i1]
            u.eye0 = simd_inverse(views[0]).columns.3
            u.eye1 = simd_inverse(views[i1]).columns.3
        } else {
            let i = min(max(viewIndex, 0), n - 1)
            let m = projections[i] * views[i]
            u.viewProj0 = m; u.viewProj1 = m
            let e = simd_inverse(views[i]).columns.3
            u.eye0 = e; u.eye1 = e
        }
        let ld = simd_normalize(simd_float3(fr.light_dir.0, fr.light_dir.1, fr.light_dir.2))
        u.light = simd_float4(ld.x, ld.y, ld.z, fr.ambient)
        u.params = simd_float4(fr.diffuse, alphaCutoff, 0, 0)

        // 4) estado y dibujo
        encoder.pushDebugGroup("WPN arma")
        encoder.setRenderPipelineState(pso)
        encoder.setDepthStencilState(ds)
        encoder.setDepthBias(0, slopeScale: 0, clamp: 0)
        encoder.setCullMode(.none)
        encoder.setVertexBytes(&u, length: MemoryLayout<WpnFrameU>.stride, index: 1)      // VR_WPN_BUF_FRAME
        encoder.setFragmentBytes(&u, length: MemoryLayout<WpnFrameU>.stride, index: 1)
        let white = whiteTexture
        for i in 0..<count {
            let m = meshes[i]
            guard let g = mesh(for: m) else { continue }
            var d = WpnDrawU(model: WeaponRenderer.matrix(m))
            encoder.setVertexBuffer(g.vbuf, offset: 0, index: 0)                           // VR_WPN_BUF_VERTS
            encoder.setVertexBytes(&d, length: MemoryLayout<WpnDrawU>.stride, index: 2)    // VR_WPN_BUF_DRAW
            encoder.setFragmentTexture(m.tex_id != 0 ? (texById[m.tex_id] ?? white) : white, index: 0)
            encoder.drawIndexedPrimitives(type: .triangle, indexCount: g.nidx, indexType: .uint16, indexBuffer: g.ibuf, indexBufferOffset: 0)
        }
        encoder.popDebugGroup()
    }
}
