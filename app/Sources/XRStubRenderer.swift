import Metal
import simd

// Stub mínimo del renderer de escena: solo existe mientras no haya RenderRG.swift real.
// Al añadir RenderRG.swift definir MV_HAVE_RGRENDERER (SWIFT_ACTIVE_COMPILATION_CONDITIONS en project.yml)
// para desactivar este bloque; la firma de abajo es el contrato que usa XRRenderLoop.
#if !MV_HAVE_RGRENDERER
final class RGRenderer {
    /// Textura del panel 2D (HUD/menús). Opcional; el bucle la dibuja a ~1,5 m si no es nil.
    var hudTexture: MTLTexture?
    var hudTextureSRGB: MTLTexture?
    var linearOutput = false

    init(device: MTLDevice, colorFormat: MTLPixelFormat, depthFormat: MTLPixelFormat, sampleCount: Int, viewCount: Int) {
        self.sampleCount = sampleCount
    }
    private let sampleCount: Int

    /// Limpia a un color y resuelve (color) a los drawables. Profundidad: Z invertida, 0 = lejos, se guarda.
    func encode(commandBuffer: MTLCommandBuffer, colorTargets: [MTLTexture], depthTargets: [MTLTexture],
                resolveTargets: [MTLTexture], views: [simd_float4x4], projections: [simd_float4x4],
                head: simd_float4x4, layered: Bool) {
        let passes = layered ? 1 : colorTargets.count
        for i in 0..<passes {
            guard i < colorTargets.count, i < depthTargets.count else { return }
            let pd = MTLRenderPassDescriptor()
            let c = pd.colorAttachments[0]!
            c.texture = colorTargets[i]
            c.loadAction = .clear
            c.clearColor = MTLClearColor(red: 0.02, green: 0.03, blue: 0.06, alpha: 1)
            if sampleCount > 1, i < resolveTargets.count {
                c.resolveTexture = resolveTargets[i]
                c.storeAction = .multisampleResolve
            } else {
                c.storeAction = .store
            }
            pd.depthAttachment.texture = depthTargets[i]
            pd.depthAttachment.loadAction = .clear
            pd.depthAttachment.clearDepth = 0
            pd.depthAttachment.storeAction = .store
            if layered { pd.renderTargetArrayLength = colorTargets[i].arrayLength }
            commandBuffer.makeRenderCommandEncoder(descriptor: pd)?.endEncoding()
        }
    }
}
#endif
