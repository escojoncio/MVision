import Metal
import simd

/// Pase final de cada frame, tras PS1Renderer.encode:
///  1) resolución de la profundidad MSAA a la textura de profundidad del drawable (si hay MSAA);
///  2) panel 2D (HUD/menús) como quad texturizado anclado al mundo, con prueba de profundidad contra la escena.
/// Layered: una pasada con `renderTargetArrayLength = vistas` y dibujo instanciado (instance = vista);
/// dedicated: una pasada por vista.
final class XRFinalPass {
    private struct HudUniforms {          // debe coincidir con XRHudUniforms de XRShaders.metal (144 B)
        var mvp0 = matrix_identity_float4x4
        var mvp1 = matrix_identity_float4x4
        var baseView: UInt32 = 0
        var opacity: Float = 1
        var pad = SIMD2<Float>(0, 0)
    }

    private let layered: Bool
    private let resolvePSO: MTLRenderPipelineState
    private let hudPSO: MTLRenderPipelineState
    private let dsAlways: MTLDepthStencilState
    private let dsHud: MTLDepthStencilState
    private let sampler: MTLSamplerState

    /// Tamaño del panel en metros (4:3) y opacidad.
    var opacity: Float = 1

    init?(device: MTLDevice, colorFormat: MTLPixelFormat, depthFormat: MTLPixelFormat, layered: Bool) {
        self.layered = layered
        guard let lib = device.makeDefaultLibrary() else { return nil }
        let sfx = layered ? "_layered" : ""
        guard let fsV = lib.makeFunction(name: "xr_fs_vertex" + sfx),
              let drF = lib.makeFunction(name: "xr_depth_resolve" + sfx),
              let hV = lib.makeFunction(name: "xr_hud_vertex" + sfx),
              let hF = lib.makeFunction(name: "xr_hud_fragment" + sfx) else { return nil }

        let d1 = MTLRenderPipelineDescriptor()
        d1.label = "mv.xr.depthResolve"
        d1.vertexFunction = fsV; d1.fragmentFunction = drF
        d1.colorAttachments[0].pixelFormat = colorFormat
        d1.colorAttachments[0].writeMask = []
        d1.depthAttachmentPixelFormat = depthFormat
        if layered { d1.inputPrimitiveTopology = .triangle }   // requerido al escribir render_target_array_index

        let d2 = MTLRenderPipelineDescriptor()
        d2.label = "mv.xr.hud"
        d2.vertexFunction = hV; d2.fragmentFunction = hF
        let c = d2.colorAttachments[0]!
        c.pixelFormat = colorFormat
        c.isBlendingEnabled = true
        c.rgbBlendOperation = .add; c.alphaBlendOperation = .add
        c.sourceRGBBlendFactor = .one; c.sourceAlphaBlendFactor = .one
        c.destinationRGBBlendFactor = .oneMinusSourceAlpha; c.destinationAlphaBlendFactor = .oneMinusSourceAlpha
        d2.depthAttachmentPixelFormat = depthFormat
        if layered { d2.inputPrimitiveTopology = .triangle }

        guard let p1 = try? device.makeRenderPipelineState(descriptor: d1),
              let p2 = try? device.makeRenderPipelineState(descriptor: d2) else { return nil }
        resolvePSO = p1; hudPSO = p2

        let a = MTLDepthStencilDescriptor()
        a.depthCompareFunction = .always; a.isDepthWriteEnabled = true
        let b = MTLDepthStencilDescriptor()
        b.depthCompareFunction = .greaterEqual; b.isDepthWriteEnabled = true      // Z invertida
        guard let s1 = device.makeDepthStencilState(descriptor: a),
              let s2 = device.makeDepthStencilState(descriptor: b) else { return nil }
        dsAlways = s1; dsHud = s2

        let sd = MTLSamplerDescriptor()
        sd.minFilter = .linear; sd.magFilter = .linear
        sd.sAddressMode = .clampToEdge; sd.tAddressMode = .clampToEdge
        guard let sm = device.makeSamplerState(descriptor: sd) else { return nil }
        sampler = sm
    }

    func encode(commandBuffer cb: MTLCommandBuffer, colorTextures: [MTLTexture], depthTextures: [MTLTexture],
                msaaDepth: [MTLTexture]?, viewports: [MTLViewport], views: [simd_float4x4],
                projections: [simd_float4x4], hudModel: simd_float4x4, hud: MTLTexture?, viewCount: Int) {
        let resolve = msaaDepth != nil
        if !resolve && hud == nil { return }       // nada que hacer

        var u = HudUniforms()
        u.mvp0 = projections[0] * views[0] * hudModel
        u.mvp1 = projections[1] * views[1] * hudModel
        u.opacity = opacity

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
            draw(enc, msaaDepth: msaaDepth?.first, hud: hud, uniforms: &u, base: 0, instances: viewCount)
            enc.endEncoding()
        } else {
            for i in 0..<viewCount {
                guard i < colorTextures.count, i < depthTextures.count else { return }
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
                draw(enc, msaaDepth: msaaDepth.map { $0[min(i, $0.count - 1)] }, hud: hud,
                     uniforms: &u, base: UInt32(i), instances: 1)
                enc.endEncoding()
            }
        }
    }

    private func draw(_ enc: MTLRenderCommandEncoder, msaaDepth: MTLTexture?, hud: MTLTexture?,
                      uniforms u: inout HudUniforms, base: UInt32, instances: Int) {
        if let md = msaaDepth {
            enc.setRenderPipelineState(resolvePSO)
            enc.setDepthStencilState(dsAlways)
            enc.setFragmentTexture(md, index: 0)
            enc.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3, instanceCount: instances)
        }
        if let h = hud {
            u.baseView = base
            enc.setRenderPipelineState(hudPSO)
            enc.setDepthStencilState(dsHud)
            enc.setVertexBytes(&u, length: MemoryLayout<HudUniforms>.stride, index: 0)
            enc.setFragmentBytes(&u, length: MemoryLayout<HudUniforms>.stride, index: 0)
            enc.setFragmentTexture(h, index: 0)
            enc.setFragmentSamplerState(sampler, index: 0)
            enc.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4, instanceCount: instances)
        }
    }
}
