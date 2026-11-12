import Foundation
import Metal

/// Filtro de la capa 2D (códec, créditos, menús, HUD, cine). `mmpx` (defecto) = MMPX x2 en la GPU (XRShaders.metal, xr_mmpx2; mismo algoritmo que vr/scene/mmpx.c) sobre la imagen nativa, y luego el muestreo
/// «sharp-bilinear» al tamaño del panel; `xbr` = xBR-lv2 x3 en la GPU (xr_xbr3) y el mismo muestreo; `sharp` = sin ampliación previa: la imagen nativa se muestrea en el shader con «sharp-bilinear» (cada texel plano
/// y solo se mezcla con el vecino en una banda de un píxel de pantalla alrededor de su borde); `nearest` = vecino más próximo puro. La CPU rasteriza siempre a escala 1 (vl2a_options_native).
enum XR2DFilter {
    case mmpx, xbr, sharp, nearest

    /// Ajuste guardado («Filtro 2D» del menú, XRViewConfig.filter2D): 0 MMPX (de serie), 1 xBR, 2 nítido, 3 sin filtro.
    static let key = "xr.view.filter2d"
    static var storedIndex: Int { UserDefaults.standard.object(forKey: key) == nil ? 0 : min(max(UserDefaults.standard.integer(forKey: key), 0), 3) }
    var index: Int { self == .mmpx ? 0 : self == .xbr ? 1 : self == .sharp ? 2 : 3 }
    /// Filtro del ajuste guardado; MV_2D_FILTER (entorno) manda si está.
    private static func configured() -> XR2DFilter {
        if let e = getenv("MV_2D_FILTER") {
            switch String(cString: e).lowercased() {
            case "nearest": return .nearest
            case "xbr": return .xbr
            case "sharp": return .sharp
            default: return .mmpx
            }
        }
        return [XR2DFilter.mmpx, .xbr, .sharp, .nearest][storedIndex]
    }
    /// Filtro vigente. Solo cambia en `reload`, al abrir la vista y ANTES de crear RGRenderer / XRPresenter: los destinos de la ampliación se crean con su factor.
    private(set) static var current: XR2DFilter = configured()
    static func reload() { current = configured() }

    /// El filtro amplía la imagen nativa en la GPU antes de muestrearla (MMPX x2, xBR x3).
    static var upscales: Bool { current == .mmpx || current == .xbr }
    /// Factor de ampliación del filtro vigente (xr_mmpx2: 2; xr_xbr3: 3).
    static var factor: Int { current == .mmpx ? 2 : 3 }
    /// Muestreo del panel en el shader (XRQuadU.cfg.w): 1 sharp-bilinear (sobre la imagen nativa o la ampliada), 2 vecino más próximo.
    var panelMode: UInt32 { self == .nearest ? 2 : 1 }
}

/// Composición de la capa 2D fiel sobre el mundo (XRFinalPass). `framebufferFetch` (defecto true): el shader lee el color del destino y compone como la GPU original, salida = c + clamp(t * mundo - d, 0, g) en
/// gamma (xr_panel_l2d_fragment); false: mezcla fija «over» premultiplicada (xr_panel_l2d_over_fragment; la resta y el tope se pierden).
enum XR2DCompose {
    static let key = "xr.view.exact2d"       // «Composición 2D exacta» del menú (XRViewConfig.exact2D)
    static var framebufferFetch = UserDefaults.standard.object(forKey: key) == nil ? true : UserDefaults.standard.bool(forKey: key)
}

/// Pasada de cómputo del filtro vigente (MMPX x2 o xBR-lv2 x3): imagen RGBA8 nativa -> textura RGBA8 ampliada (la misma codificación de canales y de color que la entrada). Con la capa 2D fiel amplía además
/// sus planos de resta y tope (textura aparte, dos planos apilados en vertical).
final class XRPixelScaler {
    private let pso: MTLComputePipelineState
    private let mmpx: Bool

    init?(device: MTLDevice) {
        mmpx = XR2DFilter.current == .mmpx
        guard let lib = MVLib.library(device), let fn = lib.makeFunction(name: mmpx ? "xr_mmpx2" : "xr_xbr3"),
              let p = try? device.makeComputePipelineState(function: fn) else { return nil }
        pso = p
    }

    /// Destino ampliado de una imagen de `width` x `height` y su vista sRGB (para destinos lineales). nil si no cabe en una textura.
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

    /// Ampliar la zona (0, 0, width, height) de `src` a `dst`. `planes` / `planesDst`: los planos de resta y tope de la capa 2D fiel (`width` x 2 * `height`) y su destino ampliado; nil = imagen de un solo plano.
    func encode(_ cb: MTLCommandBuffer, src: MTLTexture, dst: MTLTexture, width: Int, height: Int, planes: MTLTexture? = nil, planesDst: MTLTexture? = nil) {
        let f = XR2DFilter.factor
        guard width > 0, height > 0, width <= src.width, height <= src.height, dst.width >= width * f, dst.height >= height * f,
              let enc = cb.makeComputeCommandEncoder() else { return }
        var extra: (src: MTLTexture, dst: MTLTexture)?
        if let p = planes, let pd = planesDst, p.width >= width, p.height >= 2 * height, pd.width >= width * f, pd.height >= 2 * height * f { extra = (p, pd) }
        let group = MTLSize(width: 16, height: 8, depth: 1)
        enc.setComputePipelineState(pso)
        if mmpx {
            // una pasada: decide con todos los planos y escribe todos
            enc.label = "mv.2d.mmpx2"
            enc.setTexture(src, index: 0)
            enc.setTexture(dst, index: 1)
            enc.setTexture(extra?.src ?? src, index: 2)
            enc.setTexture(extra?.dst ?? dst, index: 3)
            var dims = SIMD4<UInt32>(UInt32(width), UInt32(height), extra == nil ? 0 : 2, 0)
            enc.setBytes(&dims, length: MemoryLayout<SIMD4<UInt32>>.stride, index: 0)
            enc.dispatchThreads(MTLSize(width: width, height: height, depth: 1), threadsPerThreadgroup: group)   // un hilo por píxel de origen
        } else {
            enc.label = "mv.2d.xbr3"
            enc.setTexture(src, index: 0)
            enc.setTexture(dst, index: 1)
            var dims = SIMD4<UInt32>(UInt32(width), UInt32(height), 1, 0)
            enc.setBytes(&dims, length: MemoryLayout<SIMD4<UInt32>>.stride, index: 0)
            enc.dispatchThreads(MTLSize(width: width * f, height: height * f, depth: 1), threadsPerThreadgroup: group)
            if let e = extra {
                enc.setTexture(e.src, index: 0)
                enc.setTexture(e.dst, index: 1)
                var dims2 = SIMD4<UInt32>(UInt32(width), UInt32(height), 2, 0)
                enc.setBytes(&dims2, length: MemoryLayout<SIMD4<UInt32>>.stride, index: 0)
                enc.dispatchThreads(MTLSize(width: width * f, height: 2 * height * f, depth: 1), threadsPerThreadgroup: group)
            }
        }
        enc.endEncoding()
    }
}
