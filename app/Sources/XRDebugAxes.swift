import Metal
import simd

// Ejes de depuración de los mandos y de las manos del personaje (opción "Ejes de depuración" del menú VR).
// Cada marco se dibuja como tres ejes (X rojo, Y verde, Z azul) hechos de DOS láminas cruzadas por eje (vistas de canto desaparecerían) y una
// marca cuadrada en la punta positiva (distingue + de -). Todo son `XRDrawItem` del pase final (sin shaders ni pipelines nuevos): el
// panel descarta lo transparente y es de doble cara. Sin dependencias de ARKit ni de C: el banco de render lo compila igual.

/// Un marco a dibujar: `model` rígido en el espacio recentrado (columnas = ejes x, y, z y origen), `length` de los ejes en metros.
struct XRAxisFrame {
    var model: simd_float4x4
    var length: Float
    /// Grosor de las láminas (m).
    var thickness: Float = 0.004
}

final class XRDebugAxes {
    private var tex: [MTLTexture] = []        // rojo, verde, azul
    private var texSRGB: [MTLTexture] = []

    init?(device: MTLDevice) {
        let colors: [(UInt8, UInt8, UInt8)] = [(255, 40, 40), (50, 255, 60), (70, 110, 255)]
        for (i, c) in colors.enumerated() {
            let td = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm, width: 2, height: 2, mipmapped: false)
            td.usage = [.shaderRead, .pixelFormatView]
            td.storageMode = .shared
            guard let t = device.makeTexture(descriptor: td), let v = t.makeTextureView(pixelFormat: .rgba8Unorm_srgb) else { return nil }
            t.label = "mv.axes.\(i)"
            var px: [UInt8] = []
            for _ in 0..<4 { px += [c.0, c.1, c.2, 255] }
            px.withUnsafeBytes { t.replace(region: MTLRegionMake2D(0, 0, 2, 2), mipmapLevel: 0, withBytes: $0.baseAddress!, bytesPerRow: 8) }
            tex.append(t); texSRGB.append(v)
        }
    }

    /// Láminas de los ejes de `frames` (máximo 16 marcos). Marcos con valores no finitos se omiten.
    func items(frames: [XRAxisFrame], linear: Bool) -> [XRDrawItem] {
        var out: [XRDrawItem] = []
        for f in frames.prefix(16) {
            let m = f.model
            guard XRMath.isFinite(m), f.length > 0.001 else { continue }
            let origin = SIMD3<Float>(m.columns.3.x, m.columns.3.y, m.columns.3.z)
            let cols = [m.columns.0, m.columns.1, m.columns.2]
            for a in 0..<3 {
                let d0 = SIMD3<Float>(cols[a].x, cols[a].y, cols[a].z)
                let l0 = simd_length(d0)
                guard l0 > 1e-4 else { continue }
                let d = d0 / l0
                // Dos perpendiculares cualesquiera a `d`.
                let helper: SIMD3<Float> = abs(d.y) < 0.9 ? SIMD3(0, 1, 0) : SIMD3(1, 0, 0)
                let p1 = simd_normalize(simd_cross(helper, d))
                let p2 = simd_cross(d, p1)
                let texture = linear ? texSRGB[a] : tex[a]
                // Eje: lámina larga (mitad de longitud de centro). Punta: lámina cuadrada pequeña en el extremo.
                for k in 0..<2 {
                    let (bx, bz) = k == 0 ? (p1, p2) : (p2, -p1)     // la segunda lámina gira 90 grados alrededor del eje
                    var shaft = XRDrawItem(kind: .panel, texture: texture)
                    shaft.model = XRDebugAxes.model(x: bx, y: d, z: bz, at: origin + d * (f.length * 0.5))
                    shaft.size = SIMD2<Float>(f.thickness, f.length)
                    out.append(shaft)
                    var tip = XRDrawItem(kind: .panel, texture: texture)
                    let tipSize = f.thickness * 4
                    tip.model = XRDebugAxes.model(x: bx, y: d, z: bz, at: origin + d * (f.length + tipSize * 0.5))
                    tip.size = SIMD2<Float>(tipSize, tipSize)
                    out.append(tip)
                }
            }
        }
        return out
    }

    private static func model(x: SIMD3<Float>, y: SIMD3<Float>, z: SIMD3<Float>, at p: SIMD3<Float>) -> simd_float4x4 {
        simd_float4x4(columns: (SIMD4<Float>(x, 0), SIMD4<Float>(y, 0), SIMD4<Float>(z, 0), SIMD4<Float>(p, 1)))
    }
}
