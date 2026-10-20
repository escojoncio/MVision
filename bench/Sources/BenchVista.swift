import Foundation
import simd

// Pruebas puras (sin Metal) del seguimiento de la cabeza por las pantallas 2D (XRFollow) y de la detección y el registro del recentrado del sistema
// (XRRecenterDetector, XRRecenter). Usan `check` de main.swift.

private func pose(_ p: SIMD3<Float>, yaw: Float, pitch: Float = 0, roll: Float = 0) -> simd_float4x4 {
    XRMath.translation(p) * XRMath.rotationY(yaw) * XRMath.rotationX(pitch) * rotationZ(roll)
}

private func rotationZ(_ a: Float) -> simd_float4x4 {
    let c = cosf(a), s = sinf(a)
    return simd_float4x4(columns: (SIMD4<Float>(c, s, 0, 0), SIMD4<Float>(-s, c, 0, 0), SIMD4<Float>(0, 0, 1, 0), SIMD4<Float>(0, 0, 0, 1)))
}

/// Ruido determinista en [-1, 1].
private struct Noise {
    var s: UInt32 = 12345
    mutating func next() -> Float {
        s = s &* 1664525 &+ 1013904223
        return Float(s >> 8) / Float(1 << 23) - 1
    }
}

func runVistaTests() {
    let tag = "vista-2d"
    let deg: Float = .pi / 180

    // --- filtro: independiente de la tasa de fotogramas
    do {
        func settle(hz: Float, seconds: Float) -> Float {
            var st = XRFollowState()
            XRFollow.step(&st, head: pose(SIMD3<Float>(0, 1.6, 0), yaw: 0), dt: 0.01, snap: true)
            let head = pose(SIMD3<Float>(0, 1.6, 0), yaw: 1.0)
            for _ in 0..<Int(seconds * hz) { XRFollow.step(&st, head: head, dt: 1 / hz) }
            return st.yaw
        }
        let a = settle(hz: 100, seconds: 0.1), b = settle(hz: 60, seconds: 0.1), c = settle(hz: 30, seconds: 0.1)
        let want = 1 - expf(-0.1 / XRFollow.tau)
        check(tag, "respuesta a un escalon igual a 100, 60 y 30 Hz (1 - exp(-t/tau))", abs(a - want) < 0.01 && abs(b - want) < 0.01 && abs(c - want) < 0.01,
              String(format: "100 Hz %.3f, 60 Hz %.3f, 30 Hz %.3f, esperado %.3f", a, b, c, want))
        check(tag, "tau en 0,08..0,15 s", XRFollow.tau >= 0.08 && XRFollow.tau <= 0.15, "\(XRFollow.tau)")
        check(tag, "alfa: 0 con dt <= 0, 1 con tau <= 0, creciente con dt", XRFollow.alpha(dt: 0, tau: 0.1) == 0 && XRFollow.alpha(dt: 0.01, tau: 0) == 1
              && XRFollow.alpha(dt: 0.02, tau: 0.1) > XRFollow.alpha(dt: 0.01, tau: 0.1), "")
        let settled = settle(hz: 100, seconds: 0.6)
        check(tag, "converge a la cabeza en 0,6 s", abs(settled - 1.0) < 0.01, String(format: "%.4f rad", settled))
    }

    // --- tiempo real: seguimiento continuo con la cabeza girando
    do {
        var st = XRFollowState()
        let w: Float = 90 * deg                 // 90 grados/s
        var yaw: Float = 0
        XRFollow.step(&st, head: pose(SIMD3<Float>(0, 1.6, 0), yaw: 0), dt: 0.01, snap: true)
        for _ in 0..<300 { yaw += w * 0.01; XRFollow.step(&st, head: pose(SIMD3<Float>(0, 1.6, 0), yaw: yaw), dt: 0.01) }
        let lag = XRFollow.wrap(yaw - st.yaw)
        check(tag, "girando a 90 grados/s el retraso estacionario es ~ w*tau", abs(lag - w * XRFollow.tau) < 0.02, String(format: "%.3f rad (esperado %.3f)", lag, w * XRFollow.tau))
        // camino corto por el salto de 180 grados
        var s2 = XRFollowState()
        XRFollow.step(&s2, head: pose(SIMD3<Float>(0, 1.6, 0), yaw: 3.0), dt: 0.01, snap: true)
        XRFollow.step(&s2, head: pose(SIMD3<Float>(0, 1.6, 0), yaw: -3.0), dt: 0.01)
        check(tag, "el rumbo toma el camino corto (3,0 -> -3,0 pasa por pi)", s2.yaw > 3.0 || s2.yaw < -3.0, String(format: "%.3f", s2.yaw))
    }

    // --- nivelada, a distancia y altura relativas
    do {
        for (i, c) in [(Float(0.4), Float(0.0), Float(0.3)), (-2.0, 0.5, -0.4), (3.0, -0.8, 0.9), (1.2, 1.2, 0.0)].enumerated() {
            let p = SIMD3<Float>(0.7, 1.55, -0.3)
            let head = pose(p, yaw: c.0, pitch: c.1, roll: c.2)
            var st = XRFollowState()
            for _ in 0..<200 { XRFollow.step(&st, head: head, dt: 0.01) }
            let D: Float = 2.5, drop: Float = 0.05
            let m = XRFollow.model(st, distance: D, drop: drop)
            let case_ = "\(tag)/\(i)"
            check(case_, "sin alabeo (eje X de la pantalla horizontal)", abs(m.columns.0.y) < 1e-4, String(format: "%.5f", m.columns.0.y))
            check(case_, "cabeceo recortado a +-15 grados", abs(st.pitch) <= XRFollow.maxPitch + 1e-5, String(format: "%.3f rad", st.pitch))
            let c3 = XRMath.position(m)
            let d3 = simd_length(c3 - p)
            check(case_, "a D m de la cabeza (distancia relativa constante)", abs(d3 - sqrtf(D * D + drop * drop)) < 1e-3, String(format: "%.3f m", d3))
            let n = simd_normalize(SIMD3<Float>(m.columns.2.x, m.columns.2.y, m.columns.2.z))
            let toHead = simd_normalize(p - c3)
            check(case_, "la cara de la pantalla mira a la cabeza (>0,99)", simd_dot(n, toHead) > 0.99, String(format: "%.4f", simd_dot(n, toHead)))
            // altura: con la cabeza mirando al frente, la pantalla queda drop por debajo de los ojos
            if c.1 == 0 { check(case_, "altura relativa: drop por debajo de los ojos", abs((p.y - c3.y) - drop) < 1e-3, String(format: "%.3f m", p.y - c3.y)) }
        }
        // posición: sigue la cabeza al andar
        var st = XRFollowState()
        XRFollow.step(&st, head: pose(SIMD3<Float>(0, 1.6, 0), yaw: 0), dt: 0.01, snap: true)
        for _ in 0..<100 { XRFollow.step(&st, head: pose(SIMD3<Float>(1.0, 1.6, -1.0), yaw: 0), dt: 0.01) }
        check(tag, "la posicion sigue a la cabeza (1 s)", simd_length(st.pos - SIMD3<Float>(1.0, 1.6, -1.0)) < 0.01, String(format: "%.4f m", simd_length(st.pos - SIMD3<Float>(1.0, 1.6, -1.0))))
        // salto grande de la cabeza (> 3 m): no se persigue
        var s3 = XRFollowState()
        XRFollow.step(&s3, head: pose(SIMD3<Float>(0, 1.6, 0), yaw: 0), dt: 0.01, snap: true)
        XRFollow.step(&s3, head: pose(SIMD3<Float>(10, 1.6, 0), yaw: 0.5), dt: 0.01)
        check(tag, "un salto > 3 m salta el suavizado", simd_length(s3.pos - SIMD3<Float>(10, 1.6, 0)) < 1e-4 && abs(s3.yaw - 0.5) < 1e-4, "")
        // datos no finitos no estropean el estado
        var s4 = XRFollowState()
        XRFollow.step(&s4, head: pose(SIMD3<Float>(0, 1.6, 0), yaw: 0.2), dt: 0.01, snap: true)
        var bad = matrix_identity_float4x4; bad.columns.3.x = .nan
        XRFollow.step(&s4, head: bad, dt: 0.01)
        check(tag, "una cabeza no finita se ignora", s4.pos.x.isFinite && abs(s4.yaw - 0.2) < 1e-5, "")
    }

    // --- menu VR: la versión con posición/rumbo coincide con la de la cabeza
    do {
        let head = pose(SIMD3<Float>(0.3, 1.6, -0.2), yaw: 0.7)
        let a = XRMenuLayout.model(head: head)
        let b = XRMenuLayout.model(position: XRMath.position(head), yaw: 0.7)
        var same = true
        for (x, y) in [(a.columns.0, b.columns.0), (a.columns.1, b.columns.1), (a.columns.2, b.columns.2), (a.columns.3, b.columns.3)] where simd_length(x - y) > 1e-4 { same = false }
        check(tag, "menu VR: model(head:) == model(position:yaw:)", same, "")
        check(tag, "menu VR: la opcion «Pantallas 2D» existe y por defecto siguen la cabeza", XRMenuItem.allCases.contains(.follow2D) && XRViewConfig().follow2D, "")
    }

    // --- detector del recentrado del sistema
    do {
        func run(_ name: String, _ body: (inout XRRecenterDetector) -> Void) { var d = XRRecenterDetector(); body(&d) }
        // cabeza real: giro a 300 grados/s + andar a 1,5 m/s + ruido (1 mm, 0,05 grados) a 100 Hz durante 5 s: sin falsos positivos
        run("movimiento") { d in
            var n = Noise()
            var jumps = 0
            var yaw: Float = 0, x: Float = 0
            for i in 0..<500 {
                yaw += 300 * deg * 0.01 * (i % 200 < 100 ? 1 : -1); x += 1.5 * 0.01
                let m = pose(SIMD3<Float>(x + 0.001 * n.next(), 1.6 + 0.001 * n.next(), 0.001 * n.next()), yaw: yaw + 0.05 * deg * n.next())
                if d.observe(m, time: Double(i) * 0.01) != nil { jumps += 1 }
            }
            check(tag, "cabeza real (300 grados/s, 1,5 m/s, ruido): sin recentrados falsos", jumps == 0, "saltos=\(jumps)")
        }
        // arranque y sacudida rapida (acelera de 0 a 600 grados/s en 3 fotogramas)
        run("arranque") { d in
            var jumps = 0
            var yaw: Float = 0
            let rates: [Float] = [0, 0, 0, 200, 400, 600, 600, 600, 300, 0, 0]
            for (i, r) in rates.enumerated() { yaw += r * deg * 0.01; if d.observe(pose(SIMD3<Float>(0, 1.6, 0), yaw: yaw), time: Double(i) * 0.01) != nil { jumps += 1 } }
            check(tag, "sacudida rapida de la cabeza no cuenta como recentrado", jumps == 0, "saltos=\(jumps)")
        }
        // recentrado del sistema: salto de rumbo de 35 grados y 0,5 m en un fotograma
        run("corona") { d in
            var found: XRRecenterJump?
            var repeats = 0
            for i in 0..<60 {
                let jumpNow = i >= 30
                let m = jumpNow ? pose(SIMD3<Float>(0.4, 1.6, -0.3), yaw: 35 * deg) : pose(SIMD3<Float>(0, 1.6, 0), yaw: 0)
                if let j = d.observe(m, time: Double(i) * 0.01) { if i == 30 { found = j } else { repeats += 1 } }
            }
            check(tag, "salto del sistema (35 grados, 0,5 m) detectado en su fotograma", found != nil, "")
            if let j = found {
                check(tag, "el delta es el salto medido", abs(j.dYaw - 35 * deg) < 0.01 && abs(simd_length(j.dPos) - 0.5) < 0.01, String(format: "%.1f grados, %.2f m", j.dYaw / deg, simd_length(j.dPos)))
            }
            check(tag, "se detecta una sola vez (la pose nueva es la referencia)", repeats == 0, "repeticiones=\(repeats)")
        }
        // salto pequeño de rumbo con la cabeza quieta: 12 grados si, 4 grados no
        for (g, expect) in [(Float(12), true), (4, false)] {
            run("pequeno") { d in
                var got = false
                for i in 0..<20 { if d.observe(pose(SIMD3<Float>(0, 1.6, 0), yaw: i >= 10 ? g * deg : 0), time: Double(i) * 0.01) != nil { got = true } }
                check(tag, "cabeza quieta + salto de \(Int(g)) grados: \(expect ? "detectado" : "ignorado")", got == expect, "detectado=\(got)")
            }
        }
        // solo posicion
        run("posicion") { d in
            var got = false
            for i in 0..<20 { if d.observe(pose(SIMD3<Float>(i >= 10 ? 0.8 : 0, 1.6, 0), yaw: 0), time: Double(i) * 0.01) != nil { got = true } }
            check(tag, "salto de 0,8 m detectado", got, "")
        }
        // hueco (pausa / seguimiento perdido): 1 s
        for (g, expect) in [(Float(20), false), (50, true)] {
            run("hueco") { d in
                _ = d.observe(pose(SIMD3<Float>(0, 1.6, 0), yaw: 0), time: 0)
                _ = d.observe(pose(SIMD3<Float>(0, 1.6, 0), yaw: 0), time: 0.01)
                let j = d.observe(pose(SIMD3<Float>(0, 1.6, 0), yaw: g * deg), time: 1.01)
                check(tag, "tras un hueco de 1 s, giro de \(Int(g)) grados: \(expect ? "detectado" : "ignorado")", (j != nil) == expect, "")
            }
        }
        // mirando casi vertical el rumbo no es fiable: un cambio de rumbo se ignora, el de posicion no
        run("vertical") { d in
            var yawJump = false, posJump = false
            for i in 0..<10 { if d.observe(pose(SIMD3<Float>(0, 1.6, 0), yaw: 0, pitch: 85 * deg), time: Double(i) * 0.01) != nil { yawJump = true } }
            if d.observe(pose(SIMD3<Float>(0, 1.6, 0), yaw: 1.5, pitch: 85 * deg), time: 0.10) != nil { yawJump = true }
            if d.observe(pose(SIMD3<Float>(1.0, 1.6, 0), yaw: 1.5, pitch: 85 * deg), time: 0.11) != nil { posJump = true }
            check(tag, "mirando arriba: el rumbo no dispara, la posicion si", !yawJump && posJump, "rumbo=\(yawJump) posicion=\(posJump)")
        }
        // tiempo repetido o atras y datos no finitos: se ignoran sin alterar el estado
        run("tiempo") { d in
            _ = d.observe(pose(SIMD3<Float>(0, 1.6, 0), yaw: 0), time: 1.0)
            let a = d.observe(pose(SIMD3<Float>(5, 1.6, 0), yaw: 2), time: 1.0)
            let b = d.observe(pose(SIMD3<Float>(5, 1.6, 0), yaw: 2), time: 0.5)
            var bad = matrix_identity_float4x4; bad.columns.3.y = .infinity
            let c = d.observe(bad, time: 1.01)
            let e = d.observe(pose(SIMD3<Float>(0, 1.6, 0), yaw: 0), time: 1.02)
            check(tag, "tiempo repetido/atras y pose no finita se ignoran", a == nil && b == nil && c == nil && e == nil, "")
        }
        // reset
        run("reset") { d in
            _ = d.observe(pose(SIMD3<Float>(0, 1.6, 0), yaw: 0), time: 0)
            d.reset()
            check(tag, "tras reset la primera pose no es un salto", d.observe(pose(SIMD3<Float>(3, 1.6, 0), yaw: 2), time: 0.01) == nil, "")
        }
    }

    // --- registro y delta del recentrado
    do {
        check(tag, "linea de registro", XRRecenter.logLine(source: "corona", dPos: 0.4242, dYaw: -35 * deg) == "[recentrado] fuente=corona delta=0.42m,35.0grados",
              XRRecenter.logLine(source: "corona", dPos: 0.4242, dYaw: -35 * deg))
        let line = XRRecenter.logLine(source: "menu", dPos: 0, dYaw: 0)
        check(tag, "la linea lleva fuente= y delta=", line.hasPrefix("[recentrado] fuente=menu delta="), line)
        let o1 = XRMath.translation(SIMD3<Float>(1, 0, 2)) * XRMath.rotationY(0.3)
        let o2 = XRMath.translation(SIMD3<Float>(4, 0, 6)) * XRMath.rotationY(0.3 + 0.5)
        let dl = XRRecenter.originDelta(old: o1, new: o2)
        check(tag, "delta entre origenes: 5 m y 0,5 rad", abs(dl.dPos - 5) < 1e-4 && abs(dl.dYaw - 0.5) < 1e-4, "\(dl)")
    }

    // --- tras el recentrado la pantalla queda delante del usuario en el nuevo espacio
    do {
        // el usuario estaba mirando a 35 grados del -Z; el sistema recentra: el origen nuevo se calcula como XRRenderLoop.applyRecenter con la pose actual
        let device = pose(SIMD3<Float>(0.4, 1.6, -0.3), yaw: 35 * deg)
        let origin = XRMath.translation(SIMD3<Float>(0.4, 0, -0.3)) * XRMath.rotationY(XRMath.yaw(of: device))
        let head = origin.inverse * device               // cabeza en el espacio recentrado
        var st = XRFollowState()
        XRFollow.step(&st, head: head, dt: 0.01, snap: true)
        let m = XRFollow.model(st, distance: 2.5, drop: 0.05)
        let c = XRMath.position(m)
        let hp = XRMath.position(head)
        check(tag, "recentrado: la pantalla queda en -Z (delante) del nuevo origen", abs(c.x - hp.x) < 1e-3 && c.z < hp.z - 2.4, String(format: "x=%.3f z=%.3f", c.x, c.z))
        check(tag, "recentrado: la cabeza queda en el origen con rumbo 0", abs(XRMath.yaw(of: head)) < 1e-4 && abs(hp.x) < 1e-4 && abs(hp.z) < 1e-4, String(format: "rumbo %.4f", XRMath.yaw(of: head)))
    }
}
