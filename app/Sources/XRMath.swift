import simd

/// Utilidades de matrices del módulo inmersivo. Convenciones: mano derecha, Y arriba, -Z delante,
/// matrices simd en columnas (como Metal); las proyecciones son de Z invertida (cerca = 1, lejos = 0).
enum XRMath {
    static let identity = matrix_identity_float4x4

    static func translation(_ t: SIMD3<Float>) -> simd_float4x4 {
        var m = matrix_identity_float4x4
        m.columns.3 = SIMD4<Float>(t.x, t.y, t.z, 1)
        return m
    }

    static func rotationY(_ a: Float) -> simd_float4x4 {
        let c = cosf(a), s = sinf(a)
        return simd_float4x4(columns: (
            SIMD4<Float>(c, 0, -s, 0),
            SIMD4<Float>(0, 1, 0, 0),
            SIMD4<Float>(s, 0, c, 0),
            SIMD4<Float>(0, 0, 0, 1)))
    }

    static func scale(_ s: SIMD3<Float>) -> simd_float4x4 {
        simd_float4x4(diagonal: SIMD4<Float>(s.x, s.y, s.z, 1))
    }

    static func position(_ m: simd_float4x4) -> SIMD3<Float> {
        SIMD3<Float>(m.columns.3.x, m.columns.3.y, m.columns.3.z)
    }

    /// Rumbo (giro alrededor de Y) de la dirección de mirada (-Z local) proyectada al plano horizontal.
    /// Si se mira casi vertical (arriba/abajo) usa el eje X local para no depender de una dirección degenerada.
    static func yaw(of m: simd_float4x4) -> Float {
        let fwd = -SIMD3<Float>(m.columns.2.x, m.columns.2.y, m.columns.2.z)
        if hypotf(fwd.x, fwd.z) > 0.25 {
            return atan2f(-fwd.x, -fwd.z)       // rotationY(yaw) * (0,0,-1) = (-sin yaw, 0, -cos yaw)
        }
        let right = SIMD3<Float>(m.columns.0.x, m.columns.0.y, m.columns.0.z)
        return atan2f(-right.z, right.x)        // rotationY(yaw) * (1,0,0) = (cos yaw, 0, -sin yaw)
    }

    /// Proyección de Z invertida a partir de las tangentes del campo de visión (izq, der, arriba, abajo; positivas).
    /// Es el respaldo de `Drawable.computeProjection` (visionOS 2) y equivale a ella con convención rightUpBackward.
    static func projection(tangents t: SIMD4<Float>, near n: Float, far f: Float) -> simd_float4x4 {
        let l = t.x, r = t.y, u = t.z, d = t.w
        let sx = 2 / (l + r), sy = 2 / (u + d)
        let cx = (r - l) / (l + r), cy = (u - d) / (u + d)
        let a = n / (f - n), b = n * f / (f - n)
        return simd_float4x4(columns: (
            SIMD4<Float>(sx, 0, 0, 0),
            SIMD4<Float>(0, sy, 0, 0),
            SIMD4<Float>(cx, cy, a, -1),
            SIMD4<Float>(0, 0, b, 0)))
    }

    /// 16 floats en orden de columnas (formato de mv_vr_set_head).
    static func floats(_ m: simd_float4x4) -> [Float] {
        let c = m.columns
        return [c.0.x, c.0.y, c.0.z, c.0.w, c.1.x, c.1.y, c.1.z, c.1.w,
                c.2.x, c.2.y, c.2.z, c.2.w, c.3.x, c.3.y, c.3.z, c.3.w]
    }

    static func isFinite(_ m: simd_float4x4) -> Bool {
        for c in [m.columns.0, m.columns.1, m.columns.2, m.columns.3] {
            if !(c.x.isFinite && c.y.isFinite && c.z.isFinite && c.w.isFinite) { return false }
        }
        return true
    }
}
