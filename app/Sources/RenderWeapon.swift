import Metal
import simd
import Foundation
import QuartzCore

// Renderer del arma en mano: dibuja la geometría 3D propia (piezas del modelo original cortado + objetos sueltos) que entrega
// vr/hands (mv_host_hands -> VrHandsFrame), DENTRO del mismo pase de escena de RGRenderer. SIN COMPILAR (escrito sin
// toolchain Apple): revisar en la primera compilación. Shaders: render/metal/vr_weapon.metal.
//
// Estado que replica de RGRenderer.encodeScene: Z invertida (profundidad limpiada a 0, compare greaterEqual, escritura
// activa), cullMode .none (doble cara; el shader invierte la normal de las caras vistas desde dentro), MSAA con el mismo
// sampleCount, amplificación de vértices si `layered` (el encoder ya trae setVertexAmplificationCount y viewports; el
// pipeline declara maxVertexAmplificationCount = viewCount), formatos de color/profundidad (con stencil si hace falta).
// Matrices: views = mundo->ojo, projections = ojo->clip (clip = P * V * mundo), mundo VR en metros, Y arriba, -Z delante.
//
// USO (ver el informe de integración): crear una vez junto a RGRenderer con los mismos formatos/sampleCount/viewCount y,
// dentro del pase de escena del ÚLTIMO segmento (el que se resuelve), llamar a encode(...) tras los opacos del juego.
// encode PISA los enlaces del encoder (búfer de vértices 0-2, textura 0, estado de profundidad, sesgo de profundidad):
// si después se siguen dibujando lotes de la escena hay que volver a enlazarlos (ver RGRenderer.bindSceneState).

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
// Mismo layout que VrSpcDraw de vr_weapon.metal (192 bytes): vistas especiales del arma.
struct SpcDrawU {
    var model = matrix_identity_float4x4
    var uv = simd_float4(0, 0, 1, 1)
    var p0 = simd_float4(0, 1, 0, 0)      // x = modo (0 mira, 1 HUD, 2 misil), y = opacidad, z = ruido, w = tiempo
    var p1 = simd_float4(0, 0, 0, 1)      // x = disco, y = grosor del marco oscuro de las pantallas LED (fracción de la altura; modos 2 y 3), z = terminado, w = proporción ancho/alto
    var p2 = simd_float4(0, 0, 0, 0)      // x = la imagen ya es lineal, y = (modo misil) pantalla LED en espera, z = (modo misil) HUD del juego encima (0 no, 1 alfa directo, 2 premultiplicado), w = nº de vértices de la máscara poligonal de la mira (0 = sin máscara)
    var poly = (simd_float4(0, 0, 0, 0), simd_float4(0, 0, 0, 0), simd_float4(0, 0, 0, 0), simd_float4(0, 0, 0, 0))   // vértices (u, v) de la máscara, 2 por simd_float4
}
// Mismo layout que VrSpcHud de vr_weapon.metal (48 bytes, fragmento, buffer 3): dónde caen el marco del juego y la vista 3D 4:3 dentro de la textura del HUD.
struct SpcHudU {
    var frame = simd_float4(0, 0, 1, 1)   // x, y, ancho, alto (uv de la textura) del MARCO del juego
    var view = simd_float4(0, 0, 1, 1)    // x, y, ancho, alto (uv de la textura) de la vista 3D 4:3 (GD_VIEW), centrada en el marco
    var nik = simd_float4(-1, 0, 0, 0)    // HUD propio del Nikita: x = anchura de la barra de combustible en píxeles del marco (<0 = sin HUD propio), yzw = posición del misil
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
    /// Muestras del pase donde se dibuja (la fija RGRenderer en cada fotograma: pueden cambiar en vivo con el interruptor de diagnóstico «MSAA 1x»).
    var sampleCount: Int
    private let viewCount: Int
    private let library: MTLLibrary?
    private var depthState: MTLDepthStencilState?
    private var whiteTexture: MTLTexture?

    private struct PKey: Hashable {
        var layered: Bool, filter: Bool, linear: Bool
        var color: MTLPixelFormat, depth: MTLPixelFormat, samples: Int
    }
    private var pipes: [PKey: MTLRenderPipelineState] = [:]
    private struct SKey: Hashable {
        var layered: Bool, linear: Bool
        var color: MTLPixelFormat, depth: MTLPixelFormat, samples: Int
    }
    private var spcPipes: [SKey: MTLRenderPipelineState] = [:]
    private var spcDepth: MTLDepthStencilState?
    private var lensDepth: MTLDepthStencilState?     // la lente de la mira (SCOPE con occlude): con prueba de profundidad, sin escribir
    private var overlayTex: MTLTexture?
    private let t0 = CACurrentMediaTime()
    private var failed = Set<String>()

    private final class MeshGPU {
        var vbuf: MTLBuffer; var ibuf: MTLBuffer; var nverts: Int; var nidx: Int; var seen = 0
        init(vbuf: MTLBuffer, ibuf: MTLBuffer, nverts: Int, nidx: Int) { self.vbuf = vbuf; self.ibuf = ibuf; self.nverts = nverts; self.nidx = nidx }
    }
    private final class TexGPU {
        var tex: MTLTexture; var w: Int; var h: Int; var seen = 0
        init(tex: MTLTexture, w: Int, h: Int) { self.tex = tex; self.w = w; self.h = h }
    }
    /// Mallas DINÁMICAS (VRH_MF_DYNAMIC: guante, vértices nuevos en cada frame): solo se cachean los índices (por mesh_id; cambia si cambia la topología).
    private final class IdxGPU {
        var ibuf: MTLBuffer; var nidx: Int; var seen = 0
        init(ibuf: MTLBuffer, nidx: Int) { self.ibuf = ibuf; self.nidx = nidx }
    }
    private var idxCache: [UInt32: IdxGPU] = [:]
    private var meshCache: [UInt32: MeshGPU] = [:]
    private var texCache: [UInt32: TexGPU] = [:]
    private var tick = 0
    /// Anillo de búferes para los vértices de las mallas dinámicas (el shader los lee como `device`, no `constant`: setVertexBytes no sirve). Cada encode usa el hueco tick % dynSlots
    /// (6 = 3 fotogramas en vuelo x 2 ojos del modo no layered) y reparte dynMeshes bloques de dynBlock bytes (un bloque por malla).
    private var dynRing: [MTLBuffer] = []
    private var dynUsed = 0
    private static let dynSlots = 6, dynMeshes = 8, dynBlock = 4096
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
        let lib = MVLib.library(device)
        library = lib
        if lib == nil { print("[WeaponRenderer] sin biblioteca Metal por defecto: falta vr_weapon.metal en el target") }
        if MemoryLayout<WpnFrameU>.stride != 192 || MemoryLayout<WpnDrawU>.stride != 64 || MemoryLayout<SpcDrawU>.stride != 192 || MemoryLayout<SpcHudU>.stride != 48 {
            print("[WeaponRenderer] layout de uniformes incompatible con vr_weapon.metal (\(MemoryLayout<WpnFrameU>.stride)/\(MemoryLayout<WpnDrawU>.stride))")
        }
        if MemoryLayout<VrHandsVertex>.stride != 36 { print("[WeaponRenderer] VrHandsVertex no mide 36 bytes") }
        let dd = MTLDepthStencilDescriptor()
        dd.depthCompareFunction = .greaterEqual    // Z invertida, igual que dsOpaque de RGRenderer
        dd.isDepthWriteEnabled = true
        depthState = device.makeDepthStencilState(descriptor: dd)
        let sd = MTLDepthStencilDescriptor()
        sd.depthCompareFunction = .always          // la lente / el panel van por encima del arma y del mundo
        sd.isDepthWriteEnabled = false
        spcDepth = device.makeDepthStencilState(descriptor: sd)
        let ld = MTLDepthStencilDescriptor()
        ld.depthCompareFunction = .greaterEqual    // Z invertida, igual que depthState: el escenario y el arma que tenga delante tapan la lente
        ld.isDepthWriteEnabled = false
        lensDepth = device.makeDepthStencilState(descriptor: ld)
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
            if k.depth == .depth32Float_stencil8 { d.stencilAttachmentPixelFormat = k.depth }   // igual que RGRenderer (depth24Unorm_stencil8 no existe en visionOS)
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
        idxCache = idxCache.filter { tick - $0.value.seen <= WeaponRenderer.staleTicks }
        texCache = texCache.filter { tick - $0.value.seen <= WeaponRenderer.staleTicks }
    }

    /// Dibuja una malla dinámica (guante): los vértices (<= 4096 bytes) se copian a un bloque del anillo dynRing, los índices del caché. La matriz `model` es la identidad (vértices ya en el espacio VR).
    private func drawDynamic(_ encoder: MTLRenderCommandEncoder, _ m: VrHandsMesh, texture: MTLTexture?) {
        let nv = Int(m.nverts), ni = Int(m.nidx)
        let vlen = nv * MemoryLayout<VrHandsVertex>.stride
        guard nv > 0, ni >= 3, ni % 3 == 0, vlen <= WeaponRenderer.dynBlock, dynUsed < WeaponRenderer.dynMeshes, let v = m.verts, let ix = m.idx else { return }
        for i in 0..<ni where Int(ix[i]) >= nv { return }
        if dynRing.isEmpty {
            var ring: [MTLBuffer] = []
            for k in 0..<WeaponRenderer.dynSlots {
                guard let b = device.makeBuffer(length: WeaponRenderer.dynMeshes * WeaponRenderer.dynBlock, options: .storageModeShared) else { return }
                b.label = "WPN vértices dinámicos \(k)"
                ring.append(b)
            }
            dynRing = ring
        }
        let vb = dynRing[tick % WeaponRenderer.dynSlots]
        let voff = dynUsed * WeaponRenderer.dynBlock
        dynUsed += 1
        (vb.contents() + voff).copyMemory(from: UnsafeRawPointer(v), byteCount: vlen)
        let g: IdxGPU
        if let c = idxCache[m.mesh_id], c.nidx == ni {
            c.seen = tick; g = c
        } else {
            guard let ib = device.makeBuffer(bytes: UnsafeRawPointer(ix), length: ni * 2, options: .storageModeShared) else { return }
            ib.label = "WPN índices dinámicos \(m.mesh_id)"
            g = IdxGPU(ibuf: ib, nidx: ni); g.seen = tick
            idxCache[m.mesh_id] = g
        }
        var d = WpnDrawU(model: WeaponRenderer.matrix(m))
        encoder.setVertexBuffer(vb, offset: voff, index: 0)                                // VR_WPN_BUF_VERTS
        encoder.setVertexBytes(&d, length: MemoryLayout<WpnDrawU>.stride, index: 2)        // VR_WPN_BUF_DRAW
        encoder.setFragmentTexture(texture, index: 0)
        encoder.drawIndexedPrimitives(type: .triangle, indexCount: g.nidx, indexType: .uint16, indexBuffer: g.ibuf, indexBufferOffset: 0)
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
        dynUsed = 0
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
            if m.flags & UInt32(VRH_MF_DYNAMIC) != 0 {
                drawDynamic(encoder, m, texture: m.tex_id != 0 ? (texById[m.tex_id] ?? white) : white)
                continue
            }
            guard let g = mesh(for: m) else { continue }
            var d = WpnDrawU(model: WeaponRenderer.matrix(m))
            encoder.setVertexBuffer(g.vbuf, offset: 0, index: 0)                           // VR_WPN_BUF_VERTS
            encoder.setVertexBytes(&d, length: MemoryLayout<WpnDrawU>.stride, index: 2)    // VR_WPN_BUF_DRAW
            encoder.setFragmentTexture(m.tex_id != 0 ? (texById[m.tex_id] ?? white) : white, index: 0)
            encoder.drawIndexedPrimitives(type: .triangle, indexCount: g.nidx, indexType: .uint16, indexBuffer: g.ibuf, indexBufferOffset: 0)
        }
        encoder.popDebugGroup()
    }

    // MARK: vistas especiales (mira óptica, visor antiaéreo, pantalla del misil)
    private func specialPipeline(_ k: SKey) -> MTLRenderPipelineState? {
        if let p = spcPipes[k] { return p }
        if failed.contains("S\(k)") || failed.contains("svs") { return nil }      // ya falló: no recompilar un pipeline por fotograma
        guard let lib = library else { return nil }
        do {
            let fc = MTLFunctionConstantValues()
            var f = false, l = k.linear
            fc.setConstantValue(&f, type: .bool, index: 0)   // VR_WPN_FC_FILTER (no se usa aquí, pero la función lo declara)
            fc.setConstantValue(&l, type: .bool, index: 1)   // VR_WPN_FC_LINEAR_OUT
            let fs = try lib.makeFunction(name: "vr_fs_spc", constantValues: fc)
            guard let vs = lib.makeFunction(name: k.layered ? "vr_vs_spc_amp" : "vr_vs_spc") else {
                if !failed.contains("svs") { failed.insert("svs"); print("[WeaponRenderer] faltan las funciones de las vistas especiales") }
                return nil
            }
            let d = MTLRenderPipelineDescriptor()
            d.label = "WPN vista especial \(k.layered ? "amp" : "ojo")"
            d.vertexFunction = vs
            d.fragmentFunction = fs
            d.rasterSampleCount = k.samples
            d.inputPrimitiveTopology = .triangle
            if k.layered { d.maxVertexAmplificationCount = viewCount }
            let ca = d.colorAttachments[0]!
            ca.pixelFormat = k.color
            ca.isBlendingEnabled = true
            ca.rgbBlendOperation = .add
            ca.alphaBlendOperation = .add
            ca.sourceRGBBlendFactor = .sourceAlpha
            ca.destinationRGBBlendFactor = .oneMinusSourceAlpha
            ca.sourceAlphaBlendFactor = .zero
            ca.destinationAlphaBlendFactor = .one
            d.depthAttachmentPixelFormat = k.depth
            if k.depth == .depth32Float_stencil8 { d.stencilAttachmentPixelFormat = k.depth }
            let p = try device.makeRenderPipelineState(descriptor: d)
            spcPipes[k] = p
            return p
        } catch {
            let key = "S\(k)"
            if !failed.contains(key) { failed.insert(key); print("[WeaponRenderer] pipeline de vistas especiales: \(error)") }
            return nil
        }
    }

    /// Textura de la abertura y la retícula de la mira (vr_scope), generada una vez: 256 x 256 RGBA, alfa recto.
    private func scopeOverlay() -> MTLTexture? {
        if let t = overlayTex { return t }
        let n = 256
        var px = [UInt32](repeating: 0, count: n * n)
        vrscope_draw_overlay(&px, Int32(n))
        let td = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm, width: n, height: n, mipmapped: false)
        td.usage = .shaderRead
        guard let t = device.makeTexture(descriptor: td) else { return nil }
        t.label = "WPN mira (abertura y retícula)"
        px.withUnsafeBytes { raw in
            if let base = raw.baseAddress { t.replace(region: MTLRegionMake2D(0, 0, n, n), mipmapLevel: 0, withBytes: base, bytesPerRow: n * 4) }
        }
        overlayTex = t
        return t
    }

    /// Dibuja la vista especial vigente en el pase de escena en curso (después del arma). `aux` = segunda vista renderizada (mira, misil; valores
    /// en gamma, sin decodificar sRGB), `hud` = panel del HUD del juego en bruto (visor antiaéreo). `auxLinear`: los valores de `aux` ya son lineales.
    /// `hudPremultiplied`: `hud` es la capa 2D fiel (color premultiplicado, vr_layer2d) y no el HUD de reserva (alfa directo).
    /// `hudWin`: colocación del marco del juego dentro de `hud` (RGRenderer.hudWin; nil = la textura es el marco / la vista entera, como el HUD de reserva).
    func encodeSpecial(encoder: MTLRenderCommandEncoder, view svIn: MvVrSpecialView, aux: MTLTexture?, auxLinear: Bool, hud: MTLTexture?, hudPremultiplied: Bool = false, hudWin: XR2DWindow? = nil,
                       views: [simd_float4x4], projections: [simd_float4x4], layered: Bool, viewIndex: Int = 0) {
        guard enabled, svIn.kind != 0 else { return }
        var sv = svIn      // copia propia: la sincronización con la mano del modelo solo cambia la superficie
        // La superficie pegada al arma se recoloca con la misma mano con la que se acaba de dibujar el modelo (vrgame.c: mv_vrgame_special_sync); leída al inicio del fotograma iba varios ms por detrás y se hundía tras las caras del modelo.
        var synced = sv
        if mv_vrgame_special_sync(&synced) != 0 { sv.surf_pos = synced.surf_pos; sv.surf_rot = synced.surf_rot }
        let n = min(views.count, projections.count)
        guard n >= 1 else { return }
        let finite = [sv.surf_pos.0, sv.surf_pos.1, sv.surf_pos.2, sv.surf_rot.0, sv.surf_rot.1, sv.surf_rot.2, sv.surf_rot.3, sv.surf_w, sv.surf_h, sv.blend]
        guard finite.allSatisfy({ $0.isFinite }), sv.surf_w > 0.001, sv.surf_h > 0.001 else { return }
        var d = SpcDrawU()
        var image: MTLTexture?
        var overlay: MTLTexture?
        switch sv.kind {
        case Int32(MV_VIEW_SCOPE):
            // Mira óptica (abertura + retícula de vr_scope.h). Los prismáticos del juego no pasan por aquí: van en estéreo con el aumento en el mundo y su marco como MV_VIEW_AA_SIGHT (XRFrameEncoder).
            guard sv.blend > 0.02, sv.cam_valid != 0, let a = aux, let o = scopeOverlay() else { return }
            image = a; overlay = o
            d.p0 = simd_float4(0, min(max(sv.blend, 0), 1), 0, 0)
            let np = Int(sv.npoly)
            if np >= 3 && np <= 8 {      // máscara poligonal convexa de la abertura de la mira (MvVrSpecialView.poly, u/v sobre la superficie)
                let pv: [Float] = withUnsafeBytes(of: sv.poly) { Array($0.bindMemory(to: Float.self)) }
                if pv.count >= 16 && pv.allSatisfy({ $0.isFinite }) {
                    d.poly = (simd_float4(pv[0], pv[1], pv[2], pv[3]), simd_float4(pv[4], pv[5], pv[6], pv[7]),
                              simd_float4(pv[8], pv[9], pv[10], pv[11]), simd_float4(pv[12], pv[13], pv[14], pv[15]))
                    d.p2.w = Float(np)
                }
            }
        case Int32(MV_VIEW_AA_SCREEN):
            // Visor del lanzador antiaéreo en su pieza del modelo: segunda vista viva (textura 0) y, encima, la mira y el marco de fijación del HUD del juego (textura 1, subrectángulo `uv`), recortado al contorno de la pieza.
            guard sv.blend > 0.02, sv.cam_valid != 0, let a = aux else { return }
            image = a; overlay = hud      // sin capa del HUD (cambio de vista) la pantalla sigue viva, solo sin la mira del juego
            d.uv = simd_float4(sv.uv.0, sv.uv.1, sv.uv.2, sv.uv.3)
            d.p0 = simd_float4(3, min(max(sv.blend, 0), 1), hudPremultiplied ? 1 : 0, 0)
            d.p2.z = hud != nil ? 1 : 0      // 1 = hay capa del HUD que mezclar encima de la imagen
            d.p1.y = sv.frame.isFinite ? min(max(sv.frame, 0), 0.2) : 0      // marco oscuro del sombreador (fracción de la altura)
            let np = Int(sv.npoly)
            if np >= 3 && np <= 8 {
                let pv: [Float] = withUnsafeBytes(of: sv.poly) { Array($0.bindMemory(to: Float.self)) }
                if pv.count >= 16 && pv.allSatisfy({ $0.isFinite }) {
                    d.poly = (simd_float4(pv[0], pv[1], pv[2], pv[3]), simd_float4(pv[4], pv[5], pv[6], pv[7]),
                              simd_float4(pv[8], pv[9], pv[10], pv[11]), simd_float4(pv[12], pv[13], pv[14], pv[15]))
                    d.p2.w = Float(np)
                }
            }
        case Int32(MV_VIEW_AA_SIGHT):
            guard let h = hud else { return }
            image = h
            d.uv = simd_float4(sv.uv.0, sv.uv.1, sv.uv.2, sv.uv.3)
            d.p0 = simd_float4(1, 1, hudPremultiplied ? 1 : 0, 0)
        case Int32(MV_VIEW_MISSILE):
            // Pantalla LED del lanzador (o la pantalla grande de comodidad): cámara del misil en vuelo; en espera (standby) o en la estática final no hay imagen renderizada y se dibuja con la textura blanca.
            guard sv.standby != 0 || sv.ended != 0 || aux != nil else { return }
            image = aux ?? whiteTexture
            let live = sv.cam_valid != 0 && sv.ended == 0
            let t = Float((CACurrentMediaTime() - t0).truncatingRemainder(dividingBy: 64))
            d.p0 = simd_float4(2, 1, min(max(sv.noise, 0), 1), t)
            d.p1.y = sv.frame.isFinite ? min(max(sv.frame, 0), 0.2) : 0      // marco oscuro del sombreador (0 = sin marco, p. ej. pantalla grande)
            d.p1.z = live ? 0 : 1
            d.p2.y = sv.standby != 0 ? 1 : 0
            if live, let h = hud { overlay = h; d.p2.z = hudPremultiplied ? 2 : 1 }      // textos y marcas del juego (capa 2D) sobre la imagen del misil, sin los iconos de vida / equipo (sombreador)
            if sv.uv.2 > 0.001 && sv.uv.3 > 0.001 && [sv.uv.0, sv.uv.1, sv.uv.2, sv.uv.3].allSatisfy({ $0.isFinite }) {      // recorte 4:3 de la imagen sobre la pantalla (por defecto 0, 0, 1, 1)
                d.uv = simd_float4(sv.uv.0, sv.uv.1, sv.uv.2, sv.uv.3)
            }
            let np = Int(sv.npoly)
            if np >= 3 && np <= 8 {      // contorno convexo de la cara del visor del modelo (como la lente del PSG1)
                let pv: [Float] = withUnsafeBytes(of: sv.poly) { Array($0.bindMemory(to: Float.self)) }
                if pv.count >= 16 && pv.allSatisfy({ $0.isFinite }) {
                    d.poly = (simd_float4(pv[0], pv[1], pv[2], pv[3]), simd_float4(pv[4], pv[5], pv[6], pv[7]),
                              simd_float4(pv[8], pv[9], pv[10], pv[11]), simd_float4(pv[12], pv[13], pv[14], pv[15]))
                    d.p2.w = Float(np)
                }
            }
        default:
            return
        }
        // Prismáticos del juego: AA_SIGHT con round = 2. surf_w/h = rectángulo del HUD (la pantalla 4:3 del original); el quad real es una pantalla de `bigSide` metros que pinta el HUD en vivo dentro del rectángulo y negro fuera.
        let binoMask = sv.kind == Int32(MV_VIEW_AA_SIGHT) && sv.round == 2
        d.p1.x = sv.round != 0 && !binoMask ? 1 : 0
        d.p1.w = binoMask ? 1 : sv.surf_w / sv.surf_h
        d.p2.x = auxLinear && sv.kind != Int32(MV_VIEW_AA_SIGHT) ? 1 : 0
        var quadW = sv.surf_w, quadH = sv.surf_h
        if binoMask {
            let bigSide: Float = 48          // a 1 m de la cabeza cubre +-87,6 grados
            d.p2.y = 0.5 * bigSide / sv.surf_w
            d.p2.z = 0.5 * bigSide / sv.surf_h
            quadW = bigSide; quadH = bigSide
        }
        let q = simd_quatf(ix: sv.surf_rot.0, iy: sv.surf_rot.1, iz: sv.surf_rot.2, r: sv.surf_rot.3).normalized
        var m = simd_float4x4(q)
        m.columns.0 *= quadW
        m.columns.1 *= quadH
        m.columns.3 = simd_float4(sv.surf_pos.0, sv.surf_pos.1, sv.surf_pos.2, 1)
        d.model = m
        // Marco y vista 4:3 dentro de la textura del HUD: con la capa HQ es la ventana visible (PAL 320x268, marco 320x256 con oy = 5), así que las fracciones del marco (iconos) y el recorte 4:3 del misil se
        // pasan a uv de la textura con el mismo frameUV que XRPresent. La vista 3D (320x240, GD_VIEW) está centrada en el marco (XR2DWindow.panelOnScreen3D).
        var hd = SpcHudU()
        if sv.kind == Int32(MV_VIEW_MISSILE) && sv.nik_hud != 0 && sv.ended == 0 && sv.nik_fuel.isFinite && sv.nik_pos.0.isFinite && sv.nik_pos.1.isFinite && sv.nik_pos.2.isFinite {
            hd.nik = simd_float4(min(max(sv.nik_fuel, 0), 60), sv.nik_pos.0, sv.nik_pos.1, sv.nik_pos.2)      // HUD propio del Nikita (el juego no lo emite sin el botón ▲): lo dibuja el sombreador
        }
        if let w = hudWin {
            let f = w.frameUV(simd_float4(0, 0, 1, 1))
            hd.frame = simd_float4(f.x, f.y, f.z - f.x, f.w - f.y)
            if sv.kind == Int32(MV_VIEW_MISSILE) || sv.kind == Int32(MV_VIEW_AA_SCREEN) {      // ambos recortes uv (vrgame.c) son sobre la vista 3D 4:3 del juego, 320x240
                let hx = 320 / (2 * w.dw), hy = 240 / (2 * w.dh)
                let v = w.frameUV(simd_float4(0.5 - hx, 0.5 - hy, 0.5 + hx, 0.5 + hy))
                hd.view = simd_float4(v.x, v.y, v.z - v.x, v.w - v.y)
            } else {
                hd.view = hd.frame
            }
        }

        let key = SKey(layered: layered, linear: linearOutput, color: colorFormat, depth: depthFormat, samples: sampleCount)
        let occlude = (sv.kind == Int32(MV_VIEW_SCOPE) || sv.kind == Int32(MV_VIEW_AA_SCREEN) || sv.kind == Int32(MV_VIEW_MISSILE)) && sv.occlude != 0      // objetos del mundo: lente de la mira, pantallas LED (antiaéreo y misil); el visor HUD antiaéreo de reserva, la pantalla grande del misil y los prismáticos siguen por encima de todo
        guard let pso = specialPipeline(key), let ds = occlude ? lensDepth : spcDepth else { return }
        var u = WpnFrameU()
        if layered {
            let cnt = min(n, viewCount)
            let i1 = min(1, cnt - 1)
            u.viewProj0 = projections[0] * views[0]
            u.viewProj1 = projections[i1] * views[i1]
        } else {
            let i = min(max(viewIndex, 0), n - 1)
            let vp = projections[i] * views[i]
            u.viewProj0 = vp; u.viewProj1 = vp
        }
        encoder.pushDebugGroup("WPN vista especial")
        encoder.setRenderPipelineState(pso)
        encoder.setDepthStencilState(ds)
        if occlude {
            encoder.setDepthBias(1, slopeScale: 1, clamp: 0)      // hacia el ojo (Z invertida): gana a la placa coplanar del modelo
            encoder.setDepthClipMode(.clamp)                       // el plano cercano (0,1 m) no recorta la lente al acercar el arma al ojo
        } else {
            encoder.setDepthBias(0, slopeScale: 0, clamp: 0)
        }
        encoder.setCullMode(.none)
        encoder.setVertexBytes(&u, length: MemoryLayout<WpnFrameU>.stride, index: 1)
        encoder.setVertexBytes(&d, length: MemoryLayout<SpcDrawU>.stride, index: 2)
        encoder.setFragmentBytes(&d, length: MemoryLayout<SpcDrawU>.stride, index: 2)
        encoder.setFragmentBytes(&hd, length: MemoryLayout<SpcHudU>.stride, index: 3)
        encoder.setFragmentTexture(image, index: 0)
        encoder.setFragmentTexture(overlay ?? whiteTexture, index: 1)
        encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 6)
        if occlude {
            encoder.setDepthBias(0, slopeScale: 0, clamp: 0)
            encoder.setDepthClipMode(.clip)
        }
        encoder.popDebugGroup()
    }
}
