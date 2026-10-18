import SwiftUI
import MetalKit

/// Vista Metal que dibuja el último frame del motor con aspecto 4:3 y filtrado nearest.
struct GameView: UIViewRepresentable {
    func makeCoordinator() -> Renderer? { Renderer(setup: ()) }

    func makeUIView(context: Context) -> MTKView {
        let v = MTKView()
        guard let r = context.coordinator else { return v }
        v.device = r.device
        v.colorPixelFormat = .bgra8Unorm
        v.framebufferOnly = true
        v.clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
        v.isPaused = false
        v.enableSetNeedsDisplay = false
        v.preferredFramesPerSecond = 60
        v.delegate = r
        return v
    }

    func updateUIView(_ v: MTKView, context: Context) {}
}

private let shaderSource = """
#include <metal_stdlib>
using namespace metal;
struct U { float2 scale; float2 maxuv; };
struct VOut { float4 pos [[position]]; float2 uv; };
vertex VOut vs(uint vid [[vertex_id]], constant U &u [[buffer(0)]]) {
    float2 p = float2((vid & 1) ? 1.0 : -1.0, (vid & 2) ? -1.0 : 1.0);
    VOut o;
    o.pos = float4(p, 0.0, 1.0);
    o.uv = min(float2((p.x + 1.0) * 0.5, (1.0 - p.y) * 0.5) * u.scale, u.maxuv);
    return o;
}
fragment float4 fs(VOut in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {
    return float4(t.sample(s, in.uv).rgb, 1.0);
}
"""

final class Renderer: NSObject, MTKViewDelegate {
    let device: MTLDevice
    private let queue: MTLCommandQueue
    private let pipeline: MTLRenderPipelineState
    private let sampler: MTLSamplerState
    private var textures: [MTLTexture] = []   // anillo: la GPU puede seguir leyendo la anterior mientras la CPU sube la nueva
    private var texIndex = 0
    private var buffer = [UInt8](repeating: 0, count: Int(MV_HOST_FB_BYTES))
    private var seq: UInt32 = 0
    private var w: Int32 = 320
    private var h: Int32 = 240

    init?(setup: Void) {
        guard let dev = MTLCreateSystemDefaultDevice(), let q = dev.makeCommandQueue() else { return nil }
        device = dev
        queue = q
        do {
            let lib = try dev.makeLibrary(source: shaderSource, options: nil)
            let d = MTLRenderPipelineDescriptor()
            d.vertexFunction = lib.makeFunction(name: "vs")
            d.fragmentFunction = lib.makeFunction(name: "fs")
            d.colorAttachments[0].pixelFormat = .bgra8Unorm
            pipeline = try dev.makeRenderPipelineState(descriptor: d)
        } catch {
            Storage.log("Metal: \(error)")
            return nil
        }
        let sd = MTLSamplerDescriptor()
        sd.minFilter = .nearest
        sd.magFilter = .nearest
        sd.sAddressMode = .clampToEdge
        sd.tAddressMode = .clampToEdge
        guard let s = dev.makeSamplerState(descriptor: sd) else { return nil }
        sampler = s
        let td = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .rgba8Unorm, width: Int(MV_HOST_MAX_W), height: Int(MV_HOST_MAX_H), mipmapped: false)
        td.usage = .shaderRead
        td.storageMode = .shared
        var ring = [MTLTexture]()
        for _ in 0..<3 {
            guard let t = dev.makeTexture(descriptor: td) else { return nil }
            ring.append(t)
        }
        textures = ring
        super.init()
    }

    func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}

    func draw(in view: MTKView) {
        // Sube el frame solo si hay uno nuevo.
        var nw: Int32 = 0, nh: Int32 = 0
        let s = buffer.withUnsafeMutableBufferPointer { p in
            mv_host_framebuffer(p.baseAddress, p.count, &nw, &nh, seq)
        }
        if s != 0 && nw > 0 && nh > 0 {
            seq = s
            w = nw
            h = nh
            texIndex = (texIndex + 1) % textures.count
            let tex = textures[texIndex]
            buffer.withUnsafeBytes { p in
                tex.replace(region: MTLRegionMake2D(0, 0, Int(nw), Int(nh)), mipmapLevel: 0,
                                withBytes: p.baseAddress!, bytesPerRow: Int(nw) * 4)
            }
        }
        guard seq != 0, let rpd = view.currentRenderPassDescriptor, let drawable = view.currentDrawable,
              let cb = queue.makeCommandBuffer(), let enc = cb.makeRenderCommandEncoder(descriptor: rpd) else { return }

        // Rectángulo 4:3 centrado dentro del drawable.
        let dw = Double(view.drawableSize.width), dh = Double(view.drawableSize.height)
        guard dw > 0, dh > 0 else { return }
        var vw = dw, vh = dw * 3.0 / 4.0
        if vh > dh { vh = dh; vw = dh * 4.0 / 3.0 }
        enc.setViewport(MTLViewport(originX: (dw - vw) / 2, originY: (dh - vh) / 2, width: vw, height: vh,
                                    znear: 0, zfar: 1))
        var u: [Float] = [Float(w) / Float(MV_HOST_MAX_W), Float(h) / Float(MV_HOST_MAX_H),
                          (Float(w) - 0.5) / Float(MV_HOST_MAX_W), (Float(h) - 0.5) / Float(MV_HOST_MAX_H)]
        enc.setRenderPipelineState(pipeline)
        enc.setVertexBytes(&u, length: MemoryLayout<Float>.size * 4, index: 0)
        enc.setFragmentTexture(textures[texIndex], index: 0)
        enc.setFragmentSamplerState(sampler, index: 0)
        enc.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
        enc.endEncoding()
        cb.present(drawable)
        cb.commit()
    }
}
