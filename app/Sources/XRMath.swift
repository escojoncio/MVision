import Foundation
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
        model(position: XRMath.position(head), yaw: XRMath.yaw(of: head))
    }

    /// Igual, para una posición de cabeza y un rumbo dados (la pose suavizada de `XRFollow` cuando las pantallas 2D siguen la cabeza).
    static func model(position p: SIMD3<Float>, yaw: Float) -> simd_float4x4 {
        let pitch = atan2f(drop, distance)      // inclina el panel hacia la cara: -pitch gira +Z hacia arriba
        return XRMath.translation(p) * XRMath.rotationY(yaw) * XRMath.translation(SIMD3<Float>(0, -drop, -distance)) * XRMath.rotationX(-pitch)
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

    static let handWidth: Float = 0.30      // m (panel sobre la mano que abrió la ventana); alto = 3/4 del ancho
    static let handLift: Float = 0.12       // m por encima de la mano (vertical del mundo; el eje +Y del agarre puede apuntar hacia abajo)
    static let handMinEye: Float = 0.45     // m: distancia mínima ojo -> centro del panel (más cerca no se enfoca); si hace falta se aleja a lo largo del rayo cabeza -> centro

    /// Modelo del panel anclado a la mano que abrió la ventana: centro `handLift` sobre el origen del agarre `grip`, +Z (la cara) hacia la cabeza `head`, con el «arriba» de la
    /// imagen en la vertical del mundo. nil si la pose no es finita o la cabeza está casi sobre la mano (el llamador usa `model(position:yaw:)` como reserva).
    static func handModel(grip: simd_float4x4, head: SIMD3<Float>) -> simd_float4x4? {
        guard XRMath.isFinite(grip), head.x.isFinite, head.y.isFinite, head.z.isFinite else { return nil }
        var c = XRMath.position(grip) + SIMD3<Float>(0, handLift, 0)
        let d = head - c
        let len = simd_length(d)
        guard len > 0.05 else { return nil }
        let n = d / len
        if len < handMinEye { c = head - n * handMinEye }
        let r = simd_cross(SIMD3<Float>(0, 1, 0), n)
        let rl = simd_length(r)
        guard rl > 0.05 else { return nil }
        let right = r / rl
        let up = simd_cross(n, right)
        return simd_float4x4(columns: (SIMD4<Float>(right, 0), SIMD4<Float>(up, 0), SIMD4<Float>(n, 0), SIMD4<Float>(c, 1)))
    }

    /// Modelo del panel (centro del quad, +Z hacia el usuario) para una posición de cabeza `p` y un rumbo `yaw` (el del HUD, con zona muerta).
    static func model(position p: SIMD3<Float>, yaw: Float) -> simd_float4x4 {
        XRMath.translation(p) * XRMath.rotationY(yaw) * XRMath.translation(SIMD3<Float>(0, -drop, -distance))
    }
}

enum XRCinemaLayout {
    /// Motivos de vista (VRV_R_*) cuya pantalla lleva texto pequeño de 1 px (títulos, créditos, nombres de la intro, pantallas de resultado, códec): sin partida, radio y juego detenido.
    static func legible(reason: Int32) -> Bool { reason == 1 || reason == 3 || reason == 4 }
    /// Igual, y además los fotogramas emulados de un FMV (clase 3) o del respaldo de una escena del motor (clase 4, view_kind de vr_view.h): imagen de la VRAM a pantalla completa, grande,
    /// plana y con el escalado de píxel fijo (nítida), sin la deformación del cilindro.
    static func legible(reason: Int32, kind: Int32) -> Bool { legible(reason: reason) || kind == 3 || kind == 4 }
    /// Ancho de la pantalla (m): un 25 % mayor para esas escenas (hasta 4,5 m).
    static func width(base: Float, legible: Bool) -> Float { legible ? min(base * 1.25, 4.5) : base }
    /// Curvatura: el cilindro deforma la rejilla de píxeles (trazos de 1 px de grosor desigual); en escenas de texto se limita a 0,12.
    static func curve(base: Float, legible: Bool) -> Float { legible ? min(base, 0.12) : base }
    /// Nitidez: nunca «Suave» (el bilineal borra los trazos finos).
    static func sharp(base: Int, legible: Bool) -> Int { legible ? max(base, 1) : base }
    /// Escalado entero (cada texel = nº entero de píxeles del visor): siempre en esas escenas.
    static func integerScale(base: Bool, legible: Bool) -> Bool { base || legible }
}

/// Seguimiento suave de la cabeza por las pantallas 2D (cine, pantalla 3D, ventanas del juego, menú VR). Funciones puras (las prueba el banco).
/// Estándar VR de «lazy follow» por fotograma: la pantalla sigue la posición y el rumbo de la cabeza con un filtro exponencial independiente de la tasa de
/// fotogramas (alfa = 1 - exp(-dt/tau), tau corto) y se mantiene NIVELADA (sin alabeo; el cabeceo sigue a la cabeza solo a la mitad y con tope de ~15 grados).
/// Zona muerta pequeña (2,5 grados de rumbo, 3 de cabeceo, 3 cm) para que el temblor y la respiración no muevan la pantalla: mientras el error de la pose
/// respecto a la cabeza quede dentro, la pantalla está quieta; fuera, la sigue filtrando solo el exceso (sin saltos al cruzar el borde).
struct XRFollowState {
    var pos = SIMD3<Float>(0, 1.6, 0)
    var yaw: Float = 0
    var pitch: Float = 0
    var valid = false
}

enum XRFollow {
    static let tau: Float = 0.12            // constante de tiempo (s): 0,08..0,15 s
    static let deadYaw: Float = 2.5 * Float.pi / 180     // rad: zona muerta del rumbo
    static let deadPitch: Float = 3.0 * Float.pi / 180   // rad: zona muerta del cabeceo (de la pose ya con ganancia)
    static let deadPos: Float = 0.03                // m: zona muerta de la posición
    static let pitchGain: Float = 0.5
    static let maxPitch: Float = 0.26       // rad (~15 grados)
    static let snapDistance: Float = 3.0    // m: un salto de cabeza mayor (teletransporte, recentrado del sistema) no se persigue, se salta

    static func wrap(_ a: Float) -> Float { atan2f(sinf(a), cosf(a)) }

    /// Factor del filtro exponencial para un paso `dt` (0 si dt <= 0; 1 si tau <= 0).
    static func alpha(dt: Float, tau: Float) -> Float {
        guard dt > 0, dt.isFinite else { return 0 }
        guard tau > 0 else { return 1 }
        return 1 - expf(-dt / tau)
    }

    /// Cabeceo de la mirada (rad, + = mirar arriba).
    static func pitch(of m: simd_float4x4) -> Float {
        let f = -SIMD3<Float>(m.columns.2.x, m.columns.2.y, m.columns.2.z)
        let l = simd_length(f)
        guard l > 1e-6, l.isFinite else { return 0 }
        return asinf(min(max(f.y / l, -1), 1))
    }

    /// Pose objetivo para una cabeza `head`: posición, rumbo y cabeceo recortado.
    static func target(head: simd_float4x4) -> (pos: SIMD3<Float>, yaw: Float, pitch: Float) {
        (XRMath.position(head), XRMath.yaw(of: head), min(max(pitch(of: head) * pitchGain, -maxPitch), maxPitch))
    }

    /// Un paso por fotograma de render. `snap` (o el primer paso, o un salto de la cabeza > `snapDistance`) coloca la pose directamente sobre la cabeza.
    static func step(_ s: inout XRFollowState, head: simd_float4x4, dt: Float, tau: Float = XRFollow.tau, snap: Bool = false) {
        guard XRMath.isFinite(head) else { return }
        let t = target(head: head)
        guard t.pos.x.isFinite, t.pos.y.isFinite, t.pos.z.isFinite, t.yaw.isFinite else { return }
        if snap || !s.valid || simd_length(t.pos - s.pos) > snapDistance {
            s.pos = t.pos; s.yaw = t.yaw; s.pitch = t.pitch; s.valid = true
            return
        }
        let k = alpha(dt: min(dt, 0.1), tau: tau)
        let dp = t.pos - s.pos, dl = simd_length(dp)
        if dl > deadPos { s.pos += dp * ((dl - deadPos) / dl * k) }
        s.yaw = wrap(s.yaw + excess(wrap(t.yaw - s.yaw), deadYaw) * k)
        s.pitch += excess(t.pitch - s.pitch, deadPitch) * k
    }

    /// Parte del error `e` que sobrepasa la zona muerta `dead` (0 dentro de ella; con el signo de `e`).
    static func excess(_ e: Float, _ dead: Float) -> Float { abs(e) <= dead ? 0 : e - (e > 0 ? dead : -dead) }

    /// Modelo (centro de la pantalla, +Z hacia el usuario): `distance` m delante de la cabeza suavizada y `drop` m por debajo de los ojos. Sin alabeo.
    static func model(_ s: XRFollowState, distance: Float, drop: Float) -> simd_float4x4 {
        XRMath.translation(s.pos) * XRMath.rotationY(s.yaw) * XRMath.rotationX(s.pitch) * XRMath.translation(SIMD3<Float>(0, -drop, -distance))
    }
}

/// Salto de la pose del dispositivo entre dos fotogramas: posición (m) y rumbo (rad).
struct XRRecenterJump {
    var dPos: SIMD3<Float>
    var dYaw: Float
}

/// Detección del recentrado del sistema (Digital Crown). visionOS no avisa de él ni en CompositorServices ni en ARKit: al recentrar mueve el origen del
/// espacio y la pose del dispositivo (`originFromAnchorTransform`) da un salto instantáneo. Se detecta comparando cada pose con la extrapolada de la
/// velocidad del fotograma anterior: una cabeza real no tiene un residuo de 10 cm ni de 8 grados en 10 ms. SOLO se juzga entre fotogramas normales
/// (dt <= `maxDt`): con un hueco mayor (pausa, seguimiento perdido, tirón del sistema) no hay forma de distinguir un recentrado de una cabeza que se movió
/// mientras no se la veía, así que el detector se reinicia y la pose nueva pasa a ser la referencia (sin declarar salto). El salto de rumbo se ignora si se
/// mira casi vertical (el rumbo es entonces poco fiable).
struct XRRecenterDetector {
    static let posThreshold: Float = 0.10       // m de residuo en un paso normal
    static let yawThreshold: Float = 0.14       // rad (~8 grados)
    static let maxDt: Double = 0.05             // s: más que esto entre muestras es un hueco, no un paso
    static let maxSpeed: Float = 8              // m/s, tope de la velocidad extrapolada
    static let maxYawRate: Float = 17           // rad/s (~1000 grados/s)

    private var lastPos = SIMD3<Float>(0, 0, 0)
    private var lastYaw: Float = 0
    private var lastYawOK = false
    private var lastT: Double = 0
    private var vel = SIMD3<Float>(0, 0, 0)
    private var yawRate: Float = 0
    private var have = false

    mutating func reset() { have = false; vel = SIMD3<Float>(0, 0, 0); yawRate = 0 }

    /// Alimenta la pose del dispositivo (origen ARKit -> dispositivo) en el instante `time` (s). Devuelve el salto si lo hay.
    mutating func observe(_ m: simd_float4x4, time: Double) -> XRRecenterJump? {
        guard XRMath.isFinite(m), time.isFinite, !have || time > lastT else { return nil }
        let p = XRMath.position(m), y = XRMath.yaw(of: m)
        let f = -SIMD3<Float>(m.columns.2.x, m.columns.2.y, m.columns.2.z)
        let yawOK = hypotf(f.x, f.z) > 0.5
        let prevT = lastT
        defer { lastPos = p; lastYaw = y; lastYawOK = yawOK; lastT = time }
        guard have else { have = true; vel = SIMD3<Float>(0, 0, 0); yawRate = 0; return nil }
        if time - prevT > Self.maxDt {      // hueco: reinicio sin juicio
            vel = SIMD3<Float>(0, 0, 0); yawRate = 0
            return nil
        }
        let dt = Float(time - prevT)
        let dp = p - lastPos
        let bothOK = yawOK && lastYawOK
        let dy = bothOK ? XRFollow.wrap(y - lastYaw) : 0
        let jump = simd_length(dp - vel * dt) > Self.posThreshold || abs(XRFollow.wrap(dy - yawRate * dt)) > Self.yawThreshold
        if jump {
            vel = SIMD3<Float>(0, 0, 0); yawRate = 0
            return XRRecenterJump(dPos: dp, dYaw: dy)
        }
        if dt > 0.0005 {
            var nv = dp / dt
            let sp = simd_length(nv)
            if sp > Self.maxSpeed { nv *= Self.maxSpeed / sp }
            vel = nv
            yawRate = bothOK ? min(max(dy / dt, -Self.maxYawRate), Self.maxYawRate) : 0
        } else {
            vel = SIMD3<Float>(0, 0, 0); yawRate = 0
        }
        return nil
    }
}

/// Línea de registro del recentrado y delta entre dos orígenes (funciones puras).
enum XRRecenter {
    /// `[recentrado] fuente=<corona|menu|boton|gesto|inicio> delta=<m>m,<grados>grados`
    static func logLine(source: String, dPos: Float, dYaw: Float) -> String {
        "[recentrado] fuente=\(source) delta=" + String(format: "%.2fm,%.1fgrados", dPos, abs(dYaw) * 180 / .pi)
    }

    /// Distancia (m, en horizontal) y giro (rad) entre dos orígenes de recentrado.
    static func originDelta(old: simd_float4x4, new: simd_float4x4) -> (dPos: Float, dYaw: Float) {
        let d = XRMath.position(new) - XRMath.position(old)
        return (hypotf(d.x, d.z), XRFollow.wrap(XRMath.yaw(of: new) - XRMath.yaw(of: old)))
    }
}
