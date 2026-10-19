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

    static func rotationX(_ a: Float) -> simd_float4x4 {
        let c = cosf(a), s = sinf(a)
        return simd_float4x4(columns: (
            SIMD4<Float>(1, 0, 0, 0),
            SIMD4<Float>(0, c, s, 0),
            SIMD4<Float>(0, -s, c, 0),
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

    /// Profundidad del buffer (0..1, Z invertida: cerca = 1) de un punto a `meters` m delante del ojo con la proyección `p`: z_clip = P22*z + P32 con z = -meters, w = meters.
    /// Con plano lejano infinito (P22 = 0) es near/meters. Nil si la proyección no es de Z invertida o el resultado sale de (0, 1).
    static func depthValue(projection p: simd_float4x4, meters: Float) -> Float? {
        guard meters > 0 else { return nil }
        let a = p.columns.2.z, b = p.columns.3.z
        let v = (b - a * meters) / meters
        return (v.isFinite && v > 0 && v < 1) ? v : nil
    }

    static func isFinite(_ m: simd_float4x4) -> Bool {
        for c in [m.columns.0, m.columns.1, m.columns.2, m.columns.3] {
            if !(c.x.isFinite && c.y.isFinite && c.z.isFinite && c.w.isFinite) { return false }
        }
        return true
    }
}

/// Cámaras de la pantalla 3D (cinemáticas del motor). Réplica de `vrcine_eye` de vr/host/vr_cine.c (probada allí): mantener sincronizado.
/// Pareja de cámaras PARALELAS con ventana desplazada: el plano a `conv` m de la cámara del juego queda sin paralaje (en el plano de la pantalla), lo más cercano
/// sale de ella y lo lejano queda detrás. Con el campo de visión igual al ángulo que la pantalla ocupa desde el usuario y la separación igual a su IPD la escena se ve
/// a escala real. No depende de la pose de la cabeza.
enum XRStereo {
    static let convergence: Float = 3.0

    /// `gameView`: VrScene.game_view (vista del motor en unidades -> mundo VR en metros). `eye`: 0 izquierdo, 1 derecho. Devuelve la vista (mundo VR -> espacio del ojo GL:
    /// x derecha, y arriba, -z delante) y las tangentes del frusto (izquierda, derecha, arriba, abajo; positivas) o nil si los datos no son válidos.
    static func eye(gameView gv: simd_float4x4, eye: Int, ipd: Float, conv: Float, tanX: Float, tanY: Float) -> (view: simd_float4x4, tangents: SIMD4<Float>)? {
        guard XRMath.isFinite(gv), ipd > 0.01, conv > 0.1, tanX > 0.05, tanY > 0.05 else { return nil }
        let det = simd_determinant(gv)
        guard det.isFinite, abs(det) > 1e-13 else { return nil }
        let s: Float = 0.001                                         // metros por unidad del motor
        let f = simd_float4x4(diagonal: SIMD4<Float>(s, -s, -s, 1))  // vista del motor (x der, y abajo, z delante) -> GL (x der, y arriba, -z delante)
        let o: Float = eye == 0 ? -0.5 * ipd : 0.5 * ipd             // posición del ojo en x de la cámara
        let view = XRMath.translation(SIMD3<Float>(-o, 0, 0)) * f * gv.inverse
        guard XRMath.isFinite(view) else { return nil }
        let sh = 0.5 * ipd / conv                                    // desplazamiento de la ventana (tangente)
        let t = eye == 0 ? SIMD4<Float>(tanX - sh, tanX + sh, tanY, tanY) : SIMD4<Float>(tanX + sh, tanX - sh, tanY, tanY)
        return (view, t)
    }
}

/// Colocación del menú VR y ajustes de legibilidad de la pantalla de cine (funciones puras: las prueba el banco de render).
/// El menú ya no sale a 1,1 m a la altura de los ojos, justo delante de la pantalla de cine / códec (a 2-4 m): se coloca más cerca, más abajo, inclinado hacia la cara
/// y más pequeño, de modo que nunca coincide con la pantalla (ni en ángulo ni en profundidad) y se lee entero sin girar la cabeza.
enum XRMenuLayout {
    static let distance: Float = 0.70       // m delante de la cabeza (en horizontal)
    static let drop: Float = 0.45           // m por debajo de los ojos
    static let width: Float = 0.66          // ancho del panel (m); alto = ancho * h / w de la textura

    /// Modelo del panel (centro del quad, +Z hacia el usuario) para una cabeza `head` (espacio recentrado).
    static func model(head: simd_float4x4) -> simd_float4x4 {
        let p = XRMath.position(head)
        let pitch = atan2f(drop, distance)      // inclina el panel hacia la cara: -pitch gira +Z hacia arriba
        return XRMath.translation(p) * XRMath.rotationY(XRMath.yaw(of: head)) * XRMath.translation(SIMD3<Float>(0, -drop, -distance)) * XRMath.rotationX(-pitch)
    }

    /// Altura del panel (m) para una textura de `w` x `h` px.
    static func height(w: Int, h: Int) -> Float { width * Float(h) / Float(max(w, 1)) }
}

/// Ventana 2D del juego (selector de armas/objetos, pausa) sobre el mundo en primera persona (VRV_R_SELECTOR = 10, VRV_R_PAUSE_WORLD = 11): la vista sigue en MUNDO y la capa 2D
/// completa del juego se dibuja como un panel flotante delante de la cabeza, por delante de la geometría (sin prueba de profundidad), en lugar de los paneles sueltos del HUD.
enum XRMenuPanelLayout {
    static let distance: Float = 0.95       // m delante de la cabeza (en horizontal)
    static let width: Float = 1.40          // m; alto = 3/4 del ancho (la capa 2D es 4:3)
    static let drop: Float = 0.02           // m por debajo de los ojos

    static func isOverlay(reason: Int32) -> Bool { reason == 10 || reason == 11 }

    /// Modelo del panel (centro del quad, +Z hacia el usuario) para una posición de cabeza `p` y un rumbo `yaw` (el del HUD, con zona muerta).
    static func model(position p: SIMD3<Float>, yaw: Float) -> simd_float4x4 {
        XRMath.translation(p) * XRMath.rotationY(yaw) * XRMath.translation(SIMD3<Float>(0, -drop, -distance))
    }
}

enum XRCinemaLayout {
    /// Motivos de vista (VRV_R_*) cuya pantalla lleva texto pequeño de 1 px (títulos, créditos, nombres de la intro, pantallas de resultado, códec): sin partida, radio y juego detenido.
    static func legible(reason: Int32) -> Bool { reason == 1 || reason == 3 || reason == 4 }
    /// Ancho de la pantalla (m): un 25 % mayor para esas escenas (hasta 4,5 m).
    static func width(base: Float, legible: Bool) -> Float { legible ? min(base * 1.25, 4.5) : base }
    /// Curvatura: el cilindro deforma la rejilla de píxeles (trazos de 1 px de grosor desigual); en escenas de texto se limita a 0,12.
    static func curve(base: Float, legible: Bool) -> Float { legible ? min(base, 0.12) : base }
    /// Nitidez: nunca «Suave» (el bilineal borra los trazos finos).
    static func sharp(base: Int, legible: Bool) -> Int { legible ? max(base, 1) : base }
    /// Escalado entero (cada texel = nº entero de píxeles del visor): siempre en esas escenas.
    static func integerScale(base: Bool, legible: Bool) -> Bool { base || legible }
}
