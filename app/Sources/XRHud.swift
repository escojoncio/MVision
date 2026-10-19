import Metal
import simd

/// Un elemento que dibuja el pase final dentro del espacio de la escena (todo en metros, espacio recentrado).
struct XRDrawItem {
    enum Kind { case screen, panel, menu, veil }
    var kind: Kind
    var texture: MTLTexture?
    var model = matrix_identity_float4x4        // rígido: el origen es el centro del quad, +Z mira al usuario
    var uvrect = SIMD4<Float>(0, 0, 1, 1)       // u0, v0 (arriba), u1, v1 (abajo)
    var size = SIMD2<Float>(1, 1)               // ancho, alto en metros
    var distance: Float = 2.5                   // radio de la curvatura (distancia al usuario)
    var curve: Float = 0                        // 0 plano .. 1 cilindro
    var cols: UInt32 = 1                        // segmentos horizontales (más de 1 solo si hay curvatura)
    var imgSize = SIMD2<Float>(1, 1)            // pantalla: tamaño de la imagen en texels
    var sharp: Float = 1                        // pantalla: 0 suave, 1 nítido, 2 píxel
    var opacity: Float = 1                      // veil: opacidad del negro
}

/// Pase final de cada frame, tras PS1Renderer.encode:
///  1) resolución de la profundidad MSAA a la textura de profundidad del drawable (si hay MSAA);
///  2) lista de `XRDrawItem` en orden: pantalla de cine, paneles del HUD, velo de transición, menú VR.
/// Layered: una pasada con `renderTargetArrayLength = vistas` y dibujo instanciado (instance = vista);
/// dedicated: una pasada por vista.
final class XRFinalPass {
    private struct QuadUniforms {          // debe coincidir con XRQuadU de XRShaders.metal (192 B)
        var mvp0 = matrix_identity_float4x4
        var mvp1 = matrix_identity_float4x4
        var uvrect = SIMD4<Float>(0, 0, 1, 1)
        var geom = SIMD4<Float>(1, 1, 2.5, 0)
        var tex = SIMD4<Float>(1, 1, 1, 1)
        var cfg = SIMD4<UInt32>(0, 1, 0, 0)
    }

    private let layered: Bool
    private let resolvePSO: MTLRenderPipelineState
    private let screenPSO: MTLRenderPipelineState
    private let panelPSO: MTLRenderPipelineState
    private let veilPSO: MTLRenderPipelineState
    private let dsAlways: MTLDepthStencilState
    private let dsHud: MTLDepthStencilState
    private let dsOverlay: MTLDepthStencilState
    private let sampler: MTLSamplerState

    init?(device: MTLDevice, colorFormat: MTLPixelFormat, depthFormat: MTLPixelFormat, layered: Bool) {
        self.layered = layered
        guard let lib = device.makeDefaultLibrary() else { return nil }
        let sfx = layered ? "_layered" : ""
        guard let fsV = lib.makeFunction(name: "xr_fs_vertex" + sfx),
              let drF = lib.makeFunction(name: "xr_depth_resolve" + sfx),
              let qV = lib.makeFunction(name: "xr_q_vertex" + sfx),
              let scF = lib.makeFunction(name: "xr_screen_fragment" + sfx),
              let pnF = lib.makeFunction(name: "xr_panel_fragment" + sfx),
              let vlF = lib.makeFunction(name: "xr_veil_fragment" + sfx) else { return nil }

        let d1 = MTLRenderPipelineDescriptor()
        d1.label = "mv.xr.depthResolve"
        d1.vertexFunction = fsV; d1.fragmentFunction = drF
        d1.colorAttachments[0].pixelFormat = colorFormat
        d1.colorAttachments[0].writeMask = []
        d1.depthAttachmentPixelFormat = depthFormat
        if layered { d1.inputPrimitiveTopology = .triangle }   // requerido al escribir render_target_array_index

        func blended(_ label: String, _ v: MTLFunction, _ f: MTLFunction, topology: Bool) -> MTLRenderPipelineDescriptor {
            let d = MTLRenderPipelineDescriptor()
            d.label = label
            d.vertexFunction = v; d.fragmentFunction = f
            let c = d.colorAttachments[0]!
            c.pixelFormat = colorFormat
            c.isBlendingEnabled = true
            c.rgbBlendOperation = .add; c.alphaBlendOperation = .add
            c.sourceRGBBlendFactor = .one; c.sourceAlphaBlendFactor = .one
            c.destinationRGBBlendFactor = .oneMinusSourceAlpha; c.destinationAlphaBlendFactor = .oneMinusSourceAlpha
            d.depthAttachmentPixelFormat = depthFormat
            if layered && topology { d.inputPrimitiveTopology = .triangle }
            return d
        }
        // Las tiras de los quads usan topología de triángulos al escribir el índice de capa (layered), igual que el resto.
        let d2 = blended("mv.xr.screen", qV, scF, topology: true)
        let d3 = blended("mv.xr.panel", qV, pnF, topology: true)
        let d4 = blended("mv.xr.veil", fsV, vlF, topology: true)

        guard let p1 = try? device.makeRenderPipelineState(descriptor: d1),
              let p2 = try? device.makeRenderPipelineState(descriptor: d2),
              let p3 = try? device.makeRenderPipelineState(descriptor: d3),
              let p4 = try? device.makeRenderPipelineState(descriptor: d4) else { return nil }
        resolvePSO = p1; screenPSO = p2; panelPSO = p3; veilPSO = p4

        let a = MTLDepthStencilDescriptor()
        a.depthCompareFunction = .always; a.isDepthWriteEnabled = true
        let b = MTLDepthStencilDescriptor()
        b.depthCompareFunction = .greaterEqual; b.isDepthWriteEnabled = true      // Z invertida
        let ov = MTLDepthStencilDescriptor()
        ov.depthCompareFunction = .always; ov.isDepthWriteEnabled = false         // velo y menú: siempre encima, sin tocar la profundidad
        guard let s1 = device.makeDepthStencilState(descriptor: a),
              let s2 = device.makeDepthStencilState(descriptor: b),
              let s3 = device.makeDepthStencilState(descriptor: ov) else { return nil }
        dsAlways = s1; dsHud = s2; dsOverlay = s3

        let sd = MTLSamplerDescriptor()
        sd.minFilter = .linear; sd.magFilter = .linear
        sd.sAddressMode = .clampToEdge; sd.tAddressMode = .clampToEdge
        guard let sm = device.makeSamplerState(descriptor: sd) else { return nil }
        sampler = sm
    }

    func encode(commandBuffer cb: MTLCommandBuffer, colorTextures: [MTLTexture], depthTextures: [MTLTexture],
                msaaDepth: [MTLTexture]?, viewports: [MTLViewport], views: [simd_float4x4],
                projections: [simd_float4x4], items: [XRDrawItem], viewCount: Int) {
        let resolve = msaaDepth != nil
        if !resolve && items.isEmpty { return }       // nada que hacer
        guard views.count >= 2, projections.count >= 2, viewports.count >= 1 else { return }

        if layered {
            guard let ct = colorTextures.first, let dt = depthTextures.first else { return }
            let pd = MTLRenderPassDescriptor()
            pd.colorAttachments[0].texture = ct
            pd.colorAttachments[0].loadAction = .load
            pd.colorAttachments[0].storeAction = .store
            pd.depthAttachment.texture = dt
            pd.depthAttachment.loadAction = resolve ? .dontCare : .load
            pd.depthAttachment.storeAction = .store
            pd.renderTargetArrayLength = viewCount
            guard let enc = cb.makeRenderCommandEncoder(descriptor: pd) else { return }
            enc.label = "mv.xr.final"
            enc.setViewport(viewports[0])
            draw(enc, msaaDepth: msaaDepth?.first, items: items, views: views, projections: projections, base: 0, instances: viewCount)
            enc.endEncoding()
        } else {
            for i in 0..<viewCount {
                guard i < colorTextures.count, i < depthTextures.count, i < viewports.count else { return }
                let pd = MTLRenderPassDescriptor()
                pd.colorAttachments[0].texture = colorTextures[i]
                pd.colorAttachments[0].loadAction = .load
                pd.colorAttachments[0].storeAction = .store
                pd.depthAttachment.texture = depthTextures[i]
                pd.depthAttachment.loadAction = resolve ? .dontCare : .load
                pd.depthAttachment.storeAction = .store
                guard let enc = cb.makeRenderCommandEncoder(descriptor: pd) else { return }
                enc.label = "mv.xr.final.\(i)"
                enc.setViewport(viewports[i])
                draw(enc, msaaDepth: msaaDepth.map { $0[min(i, $0.count - 1)] }, items: items, views: views,
                     projections: projections, base: UInt32(i), instances: 1)
                enc.endEncoding()
            }
        }
    }

    private func draw(_ enc: MTLRenderCommandEncoder, msaaDepth: MTLTexture?, items: [XRDrawItem], views: [simd_float4x4],
                      projections: [simd_float4x4], base: UInt32, instances: Int) {
        if let md = msaaDepth {
            enc.setRenderPipelineState(resolvePSO)
            enc.setDepthStencilState(dsAlways)
            enc.setFragmentTexture(md, index: 0)
            enc.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3, instanceCount: instances)
        }
        for it in items {
            if it.kind == .veil {
                if it.opacity <= 0.001 { continue }
                var c = SIMD4<Float>(min(max(it.opacity, 0), 1), 0, 0, 0)
                enc.setRenderPipelineState(veilPSO)
                enc.setDepthStencilState(dsOverlay)
                enc.setFragmentBytes(&c, length: MemoryLayout<SIMD4<Float>>.stride, index: 0)
                enc.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3, instanceCount: instances)
                continue
            }
            guard let tex = it.texture else { continue }
            var u = QuadUniforms()
            u.mvp0 = projections[0] * views[0] * it.model
            u.mvp1 = projections[1] * views[1] * it.model
            u.uvrect = it.uvrect
            u.geom = SIMD4<Float>(it.size.x, it.size.y, it.distance, it.curve)
            u.tex = SIMD4<Float>(it.imgSize.x, it.imgSize.y, it.sharp, it.kind == .screen ? 1 : it.opacity)
            u.cfg = SIMD4<UInt32>(base, max(it.cols, 1), 0, 0)
            enc.setRenderPipelineState(it.kind == .screen ? screenPSO : panelPSO)
            enc.setDepthStencilState(it.kind == .menu ? dsOverlay : dsHud)
            enc.setVertexBytes(&u, length: MemoryLayout<QuadUniforms>.stride, index: 0)
            enc.setFragmentBytes(&u, length: MemoryLayout<QuadUniforms>.stride, index: 0)
            enc.setFragmentTexture(tex, index: 0)
            enc.setFragmentSamplerState(sampler, index: 0)
            enc.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: Int(2 * (max(it.cols, 1) + 1)), instanceCount: instances)
        }
    }
}
