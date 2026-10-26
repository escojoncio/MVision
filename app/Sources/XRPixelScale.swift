import Foundation
import Metal

/// Filtro de la capa 2D (códec, créditos, menús, HUD, cine). MV_2D_FILTER: `xbr` (defecto) = xBR-lv2 x3 en la GPU (XRShaders.metal, xr_xbr3) sobre la imagen nativa y muestreo «sharp-bilinear» al tamaño del panel;
/// `nearest` = sin xBR, escala entera nítida (vecino más próximo). La CPU rasteriza siempre a escala 1 (vl2a_options_native).
enum XR2DFilter {
    case xbr, nearest

    static let current: XR2DFilter = {
        if let e = getenv("MV_2D_FILTER"), String(cString: e).lowercased() == "nearest" { return .nearest }
        return .xbr
    }()

    /// Factor de ampliación de xr_xbr3.
    static let factor = 3
    /// Muestreo del panel en el shader (XRQuadU.cfg.w): 1 sharp-bilinear (sobre la imagen xBR), 2 vecino más próximo.
    var panelMode: UInt32 { self == .xbr ? 1 : 2 }
}

/// Pasada de cómputo xBR-lv2 x3: imagen RGBA8 nativa -> textura RGBA8 de 3x3 por píxel (la misma codificación de canales y de color que la entrada).
final class XRPixelScaler {
    private let pso: MTLComputePipelineState

    init?(device: MTLDevice) {
        guard let lib = MVLib.library(device), let fn = lib.makeFunction(name: "xr_xbr3"),
              let p = try? device.makeComputePipelineState(function: fn) else { return nil }
        pso = p
    }

    /// Destino x3 de una imagen de `width` x `height` y su vista sRGB (para destinos lineales). nil si no cabe en una textura.
    static func makeTarget(_ device: MTLDevice, width: Int, height: Int, label: String) -> (tex: MTLTexture, srgb: MTLTexture)? {
        let f = XR2DFilter.factor
        guard width > 0, height > 0, width * f <= 8192, height * f <= 8192 else { return nil }
        let td = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm, width: width * f, height: height * f, mipmapped: false)
        td.usage = [.shaderRead, .shaderWrite, .pixelFormatView]
        td.storageMode = .private
        guard let t = device.makeTexture(descriptor: td), let v = t.makeTextureView(pixelFormat: .rgba8Unorm_srgb) else { return nil }
        t.label = label
        return (t, v)
    }

    /// Ampliar la zona (0, 0, width, height) de `src` a `dst` (3*width x 3*height).
    func encode(_ cb: MTLCommandBuffer, src: MTLTexture, dst: MTLTexture, width: Int, height: Int) {
        let f = XR2DFilter.factor
        guard width > 0, height > 0, width <= src.width, height <= src.height, dst.width >= width * f, dst.height >= height * f,
              let enc = cb.makeComputeCommandEncoder() else { return }
        enc.label = "mv.2d.xbr3"
        enc.setComputePipelineState(pso)
        enc.setTexture(src, index: 0)
        enc.setTexture(dst, index: 1)
        var dims = SIMD2<UInt32>(UInt32(width), UInt32(height))
        enc.setBytes(&dims, length: MemoryLayout<SIMD2<UInt32>>.stride, index: 0)
        enc.dispatchThreads(MTLSize(width: width * f, height: height * f, depth: 1), threadsPerThreadgroup: MTLSize(width: 16, height: 8, depth: 1))
        enc.endEncoding()
    }
}
