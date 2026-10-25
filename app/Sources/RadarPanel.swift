import Metal
import simd
import Foundation

// Radar de muñeca: renderiza el radar del HUD a su propia textura y lo dibuja como un quad (con marco) sobre el antebrazo
// izquierdo, dentro del pase de escena. SIN COMPILAR (escrito sin toolchain Apple): revisar en la primera compilación.
//
// Datos: la escena del radar (VrRadarScene, vr/radar/vr_radar.h) la ensambla el host en C junto a cada escena del HUD
// (vrradar_split + vrradar_build) y se pide con `mv_host_radar` justo después de cada `mv_host_scene`; el HUD flotante ya no
// lleva las primitivas del radar (se le pasó la lista sin ellas). Shaders: vr_vs_hud / vr_fs_scene (vr_scene.metal, mismo muestreo
// de VRAM, semitransparencia y paletas que el HUD) para la textura; vr_vs_radar(_amp) / vr_fs_radar (vr_radar.metal) para el quad.
//
// ---------------------------------------------------------------------------------------------------------------------------
// INTEGRACIÓN (diff pedido para RenderRG.swift y el bucle; no aplicado aquí)
//
//   RGRenderer (RenderRG.swift):
//   1) exponer la VRAM:            `var vramTexture: MTLTexture { vram }`
//   2) propiedad:                  `var radar: RadarPanel?`
//   3) en `init`, tras crear los pipelines:
//         radar = RadarPanel(device: device, library: library, colorFormat: colorFormat, depthFormat: depthFormat,
//                            sampleCount: sampleCount, viewCount: self.viewCount, linearOutput: linearOutput)
//   4) en `encode(...)`, dentro del bucle `for _ in 0..<4 { ... if mv_host_scene(&sc) == 0 { break } ...`, justo tras
//      `storeScene(sc, &newSlot)`:                          `radar?.ingest(device: device)`
//   5) en `encode(...)`, tras el bucle de grupos y los "eventos restantes" (VRAM ya en el estado final de la lista) y antes de
//      `finishHud`:                                         `radar?.renderTexture(commandBuffer, vram: vram)`
//   6) en `encodeScene(...)`, al nivel de la función y justo después de `defer { enc.endEncoding() }` (los defer corren al revés:
//      este se ejecuta ANTES de cerrar el codificador, tras dibujar el 3D, y también cuando el `guard haveScene, has3D, ...` retorna):
//         defer { if last { radar?.encodeQuad(enc, uniforms: uniforms, layered: amplify > 0, color: color.pixelFormat,
//                                             depth: depth.pixelFormat, samples: samples) } }
//      (solo en el último segmento, el que resuelve el MSAA: la profundidad ya contiene el 3D y el quad se prueba contra ella.)
//      OJO: como `renderTexture` (5) se codifica DESPUÉS de los pases de escena del mismo `encode`, el quad muestra la textura del
//      `encode` anterior (un fotograma de retraso, imperceptible en un radar); por eso `encodeQuad` no dibuja hasta que la textura
//      se ha renderizado al menos una vez (`textureReady`: una textura privada recién creada tiene contenido indefinido).
//   7) cada fotograma, antes de `encode`, el bucle inmersivo pasa la pose:
//         rg.radar?.updatePose(forearm: body.leftForearm /* (elbow, wrist, dorsal) o nil */, leftGrip: pose.leftGrip,
//                               headPosition: headPos, headForward: headFwd)
//      con `leftForearm` = vrbody_forearm_pose(0, ...) de vr/body (ver vr_radar.h, "4.").
//   Host C (vrhost.c / vrhost.h): ver vr/radar/vr_radar.h, "Integración en el host".
//
// Exactitud: la textura se dibuja con la VRAM en su estado FINAL de la lista (los eventos de VRAM intercalados con las primitivas del
// radar, p. ej. la tira de 7 segmentos que el juego sube antes de dibujar el contador, se ven con su último valor). Basta para el
// radar (solo cambian los 32x2 texels del contador, una vez por fotograma). Si algún día hiciera falta exactitud por evento, hay que
// repartir los lotes del radar por `ev_end` igual que hace `encode` con los del HUD.
// ---------------------------------------------------------------------------------------------------------------------------

/// Layout idéntico a RadarQuadU de vr_radar.metal (5 x float4 = 80 B).
struct RadarQuadU {
    var center = SIMD4<Float>(0, 0, 0, 0)      // xyz centro, w = half_w
    var right = SIMD4<Float>(1, 0, 0, 0)       // xyz derecha, w = half_h
    var up = SIMD4<Float>(0, 1, 0, 0)          // xyz arriba, w = ancho del marco
    var params = SIMD4<Float>(1, 0, 0, 0)      // x brillo, y marco hacia atrás, z 0 pantalla / 1 marco
    var bezelColor = SIMD4<Float>(0.02, 0.025, 0.03, 1)
}

final class RadarPanel {
    // MARK: configuración
    /// Muestreo de la textura en el quad: false = nearest (píxeles del original nítidos), true = bilineal + mips + anisotropía 4.
    var smooth = false { didSet { sampler = nil } }
    /// Si false no se dibuja el quad (y tampoco se renderiza la textura).
    var enabled = true
    /// Fondo de la pantalla del dispositivo (donde el original dibuja sobre la escena 3D). Las mezclas semitransparentes del radar
    /// (suma, resta, media) actúan sobre este color.
    var backdrop = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
    /// Color del marco (espacio de la textura, gamma).
    var bezelColor = SIMD3<Float>(0.02, 0.025, 0.03)
    /// Geometría del dispositivo (tamaño, posición en el antebrazo, brillo al mirarlo...): ver VrRadarWristCfg.
    var wrist: VrRadarWristCfg = vrradar_wrist_cfg_default()
    /// Resolución de la textura respecto al radar del original (71x54 px): 4 = 284x216.
    let scale = 4
    /// Dorso de respaldo = +Y del agarre (convención de vr/body y vrgame.h, "-Z dedos, +Y dorso"). Solo se usa si la cabeza está casi sobre el eje del antebrazo (< 2 cm): normalmente
    /// la normal del panel apunta a la cabeza (vrradar_wrist_quad_eye) y esto no interviene. true invierte ese dorso de respaldo.
    var flipDorsal = false

    // MARK: estado público
    private(set) var texture: MTLTexture?
    /// El último fotograma del juego traía radar (si el juego lo oculta o lo apaga, no hay quad: igual que en el HUD original).
    private(set) var present = false
    private(set) var quadValid = false
    private(set) var glow: Float = 0

    // MARK: privado
    private let device: MTLDevice
    private let library: MTLLibrary?
    private let colorFormat: MTLPixelFormat
    private let depthFormat: MTLPixelFormat
    private let sampleCount: Int
    private let viewCount: Int
    private let linearOutput: Bool
    private let texW = 71, texH = 54
    private var texPixW: Int { texW * scale }
    private var texPixH: Int { texH * scale }

    private struct Batch { var first: Int, count: Int, semi: Bool, mode: Int, hasTex: Bool }
    private final class Slot { var buf: MTLBuffer?; var cap = 0; var stOff = 0 }
    private var slots = [Slot(), Slot(), Slot()]
    private var cur = 0
    private var batches: [Batch] = []
    private var rect = simd_float4(234, 15, 71, 54)
    private var dirty = false
    private var textureReady = false
    /// El anillo de búferes avanza como mucho UNA vez por `encode` (como `storeScene` en RGRenderer): con 3 comandos en vuelo, un `encode` que
    /// ingiriera 2-4 escenas pisaría el búfer que la GPU aún lee de un fotograma anterior. Lo rearma `renderTexture` (una vez por `encode`).
    private var advanced = false

    private struct PKey: Hashable { var pass: UInt32, mode: Int, filter: Bool }     // mode -1 = sin mezcla
    private var pipes: [PKey: MTLRenderPipelineState] = [:]
    private struct QKey: Hashable { var layered: Bool, color: MTLPixelFormat, depth: MTLPixelFormat, samples: Int }
    private var quadPipes: [QKey: MTLRenderPipelineState] = [:]
    private var depthState: MTLDepthStencilState?
    private var sampler: MTLSamplerState?
    private var quadU = RadarQuadU()
    private var failed = Set<String>()
    private var lastRadarLog = 0.0       // systemUptime del último registro [radar]

    init(device: MTLDevice, library: MTLLibrary?, colorFormat: MTLPixelFormat, depthFormat: MTLPixelFormat,
         sampleCount: Int, viewCount: Int, linearOutput: Bool) {
        self.device = device
        self.library = library
        self.colorFormat = colorFormat
        self.depthFormat = depthFormat
        self.sampleCount = sampleCount
        self.viewCount = max(1, min(2, viewCount))
        self.linearOutput = linearOutput
        let td = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm, width: texW * scale, height: texH * scale,
                                                          mipmapped: true)
        td.usage = [.renderTarget, .shaderRead]
        td.storageMode = .private
        texture = device.makeTexture(descriptor: td)
        texture?.label = "Radar de muñeca"
        let d = MTLDepthStencilDescriptor()
        d.depthCompareFunction = .greaterEqual      // Z invertida (cerca = 1), igual que el pase de escena: pegado al brazo, lo tapan el mundo, el cuerpo y el arma
        d.isDepthWriteEnabled = true
        depthState = device.makeDepthStencilState(descriptor: d)
    }

    // MARK: datos del host
    /// Llamar justo después de cada `mv_host_scene` que devolvió una escena: recoge la escena del radar asociada (si la hay) y
    /// copia sus vértices a memoria propia (los punteros del host solo valen hasta la siguiente `mv_host_scene`).
    func ingest(device: MTLDevice) {
        var rs = VrRadarScene()
        guard mv_host_radar(&rs) != 0, rs.present != 0, let v = rs.v, let b = rs.b, rs.nv > 0, rs.nb > 0 else {
            present = false
            dirty = false
            return
        }
        present = true
        if !advanced { cur = (cur + 1) % slots.count; advanced = true }
        let slot = slots[cur]
        let nv = Int(rs.nv), ns = Int(rs.nstates)
        let stOff = (nv * MemoryLayout<VrGpuVtx>.stride + 15) & ~15
        let bytes = max(stOff + max(ns, 1) * MemoryLayout<VrGpuState>.stride, 64)
        if bytes > slot.cap {
            let cap = max(bytes, slot.cap * 2, 1 << 16)
            slot.buf = device.makeBuffer(length: cap, options: [.storageModeShared, .cpuCacheModeWriteCombined])
            slot.buf?.label = "Radar vértices"
            slot.cap = slot.buf == nil ? 0 : cap
        }
        guard let buf = slot.buf else { present = false; return }
        memcpy(buf.contents(), v, nv * MemoryLayout<VrGpuVtx>.stride)
        slot.stOff = stOff
        if ns > 0, let st = rs.states { memcpy(buf.contents() + stOff, st, ns * MemoryLayout<VrGpuState>.stride) }
        else { memset(buf.contents() + stOff, 0, MemoryLayout<VrGpuState>.stride) }
        rect = simd_float4(Float(rs.rect.0), Float(rs.rect.1), Float(rs.rect.2), Float(rs.rect.3))
        batches.removeAll(keepingCapacity: true)
        let texBit = UInt16(VR_VF_TEXTURED)
        for i in 0..<Int(rs.nb) {
            let bt = b[i]
            if bt.kind != 2 { continue }
            let first = Int(bt.first)
            if first >= nv { continue }
            var cnt = min(Int(bt.count), nv - first)
            cnt -= cnt % 3
            if cnt <= 0 { continue }
            var hasTex = false
            for j in first..<(first + cnt) where (v[j].flags & texBit) != 0 { hasTex = true; break }
            batches.append(Batch(first: first, count: cnt, semi: bt.blend <= 3, mode: Int(bt.blend) & 3, hasTex: hasTex))
        }
        dirty = true
    }

    // MARK: textura
    private func pipeline(_ k: PKey) -> MTLRenderPipelineState? {
        if let p = pipes[k] { return p }
        guard let lib = library else { return nil }
        do {
            let fc = MTLFunctionConstantValues()
            var f = k.filter, l = false, pass = k.pass
            fc.setConstantValue(&f, type: .bool, index: Int(VR_FC_FILTER))
            fc.setConstantValue(&l, type: .bool, index: Int(VR_FC_LINEAR_OUT))
            fc.setConstantValue(&pass, type: .uint, index: Int(VR_FC_PASS))
            let fs = try lib.makeFunction(name: "vr_fs_scene", constantValues: fc)
            guard let vs = lib.makeFunction(name: "vr_vs_hud") else { return nil }
            let d = MTLRenderPipelineDescriptor()
            d.label = "Radar textura pass\(k.pass) blend\(k.mode)"
            d.vertexFunction = vs
            d.fragmentFunction = fs
            d.rasterSampleCount = 1
            d.inputPrimitiveTopology = .triangle
            let ca = d.colorAttachments[0]!
            ca.pixelFormat = .rgba8Unorm
            if k.mode >= 0 {
                ca.isBlendingEnabled = true
                switch k.mode {                       // los 4 modos del original (B = destino, F = fragmento); ver RenderRG.pipeline
                case 0:
                    ca.rgbBlendOperation = .add
                    ca.sourceRGBBlendFactor = .blendColor; ca.destinationRGBBlendFactor = .oneMinusBlendColor
                case 1:
                    ca.rgbBlendOperation = .add
                    ca.sourceRGBBlendFactor = .one; ca.destinationRGBBlendFactor = .one
                case 2:
                    ca.rgbBlendOperation = .reverseSubtract
                    ca.sourceRGBBlendFactor = .one; ca.destinationRGBBlendFactor = .one
                default:
                    ca.rgbBlendOperation = .add
                    ca.sourceRGBBlendFactor = .blendColor; ca.destinationRGBBlendFactor = .one
                }
                ca.alphaBlendOperation = .add
                ca.sourceAlphaBlendFactor = .zero; ca.destinationAlphaBlendFactor = .one   // el alfa de la textura no se usa
            }
            let p = try device.makeRenderPipelineState(descriptor: d)
            pipes[k] = p
            return p
        } catch {
            let key = "\(k)"
            if !failed.contains(key) { failed.insert(key); print("[RadarPanel] pipeline: \(error)") }
            return nil
        }
    }

    /// Dibuja el radar en su textura (si llegó una escena nueva). `vram`: la VRAM de RGRenderer en su estado final de la lista.
    func renderTexture(_ cb: MTLCommandBuffer, vram: MTLTexture) {
        defer { advanced = false }
        guard enabled, present, dirty, let tex = texture, let buf = slots[cur].buf else { return }
        dirty = false
        let pd = MTLRenderPassDescriptor()
        pd.colorAttachments[0].texture = tex
        pd.colorAttachments[0].loadAction = .clear
        pd.colorAttachments[0].clearColor = backdrop
        pd.colorAttachments[0].storeAction = .store
        if let enc = cb.makeRenderCommandEncoder(descriptor: pd) {
            enc.label = "Radar de muñeca"
            enc.setCullMode(.none)
            enc.setViewport(MTLViewport(originX: 0, originY: 0, width: Double(texPixW), height: Double(texPixH), znear: 0, zfar: 1))
            var hu = VrHudUniforms(); hu.rect = rect          // ventana del radar en coordenadas de VRAM
            enc.setVertexBuffer(buf, offset: 0, index: Int(VR_BUF_VERTS))
            enc.setVertexBuffer(buf, offset: slots[cur].stOff, index: Int(VR_BUF_STATES))
            enc.setVertexBytes(&hu, length: MemoryLayout<VrHudUniforms>.stride, index: Int(VR_BUF_UNI))
            enc.setFragmentTexture(vram, index: 0)
            let stride = MemoryLayout<VrGpuVtx>.stride
            for bt in batches {
                enc.setVertexBufferOffset(bt.first * stride, index: Int(VR_BUF_VERTS))
                func draw(_ pass: UInt32, _ mode: Int) {
                    guard let p = pipeline(PKey(pass: pass, mode: mode, filter: false)) else { return }
                    enc.setRenderPipelineState(p)
                    enc.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: bt.count)
                }
                if bt.semi {
                    let a: Float = bt.mode == 3 ? 0.25 : 0.5
                    enc.setBlendColor(red: a, green: a, blue: a, alpha: a)
                    draw(bt.hasTex ? 1 : 0, bt.mode)
                    if bt.hasTex { draw(2, -1) }              // texels sin bit 15: opacos
                } else {
                    draw(0, -1)
                }
            }
            enc.endEncoding()
        }
        if let blit = cb.makeBlitCommandEncoder() {
            blit.generateMipmaps(for: tex)
            blit.endEncoding()
        }
        textureReady = true
    }

    // MARK: anclaje
    /// Calcula el quad en la muñeca. Prioridad (vrradar_pick_arm): `forearm` (codo, muñeca y dorso del antebrazo tras la IK, espacio VR, metros: es el brazo que se
    /// dibuja, el panel queda a lift_m de su eje) si es válido; si no, `leftGrip` (pose del mando izquierdo). `headForward` unitario.
    func updatePose(forearm: (elbow: SIMD3<Float>, wrist: SIMD3<Float>, dorsal: SIMD3<Float>)?, leftGrip: simd_float4x4?,
                    headPosition: SIMD3<Float>, headForward: SIMD3<Float>) {
        var arm = VrRadarArm()
        var gripArr: [Float] = []
        if let g = leftGrip {
            for col in [g.columns.0, g.columns.1, g.columns.2, g.columns.3] { gripArr += [col.x, col.y, col.z, col.w] }
        }
        let ikArr: [VrRadarArm] = forearm.map { f in
            [VrRadarArm(elbow: (f.elbow.x, f.elbow.y, f.elbow.z), wrist: (f.wrist.x, f.wrist.y, f.wrist.z), dorsal: (f.dorsal.x, f.dorsal.y, f.dorsal.z))]
        } ?? []
        // arrays vacios -> baseAddress nil -> el parametro NULL de C
        let pick: Int32 = gripArr.withUnsafeBufferPointer { gp in
            ikArr.withUnsafeBufferPointer { ip in vrradar_pick_arm(gp.baseAddress, ip.baseAddress, &arm) }
        }
        let ok = pick != 0
        let source = pick == 1 ? "IK" : (pick == 2 ? "mando" : "ninguna")
        if pick == 2 && flipDorsal { arm.dorsal = (-arm.dorsal.0, -arm.dorsal.1, -arm.dorsal.2) }
        var q = VrRadarQuad()
        let aspect = rect.w > 0 && rect.z > 0 ? rect.z / rect.w : 71.0 / 54.0
        let eye: [Float] = [headPosition.x, headPosition.y, headPosition.z]
        quadValid = ok && vrradar_wrist_quad_eye(&arm, &wrist, aspect, eye, &q) != 0      // la cara del panel gira alrededor del antebrazo hacia la cabeza
        let now = ProcessInfo.processInfo.systemUptime
        if now - lastRadarLog >= 1.0 {      // una vez por segundo: el hueso invisible (centro del panel) y su normal; la oclusion la decide la GPU (sin consulta de visibilidad: n/d)
            lastRadarLog = now
            func f3(_ v: Float) -> String { String(format: "%.3f", v) }
            let pos = quadValid ? "(\(f3(q.center.0)) \(f3(q.center.1)) \(f3(q.center.2)))" : "n/d"
            let nrm = quadValid ? "(\(f3(q.normal.0)) \(f3(q.normal.1)) \(f3(q.normal.2)))" : "n/d"
            var segD = "n/d"      // distancia del centro del panel al segmento codo-muñeca de la fuente usada
            if quadValid {
                let c = SIMD3<Float>(q.center.0, q.center.1, q.center.2), w = SIMD3<Float>(arm.wrist.0, arm.wrist.1, arm.wrist.2)
                let ab = SIMD3<Float>(arm.elbow.0, arm.elbow.1, arm.elbow.2) - w
                let t = min(max(simd_dot(c - w, ab) / max(simd_dot(ab, ab), 1e-9), 0), 1)
                segD = f3(simd_length(c - (w + ab * t)))
            }
            Storage.log("[radar] fuente=\(source) distCentroSegmentoCodoMuneca=\(segD) hueso=\(pos) normal=\(nrm) panel=\(quadValid ? "valido" : "invalido") presente=\(present) visible/ocluido=n/d flipDorsal=\(flipDorsal)")
        }
        guard quadValid else { glow = 0; return }
        let fwd: [Float] = [headForward.x, headForward.y, headForward.z]
        glow = vrradar_gaze(&q, eye, fwd, &wrist)
        quadU.center = SIMD4<Float>(q.center.0, q.center.1, q.center.2, q.half_w)
        quadU.right = SIMD4<Float>(q.right.0, q.right.1, q.right.2, q.half_h)
        quadU.up = SIMD4<Float>(q.up.0, q.up.1, q.up.2, q.bezel)
        quadU.params = SIMD4<Float>(1 + glow, q.bezel_back, 0, 0)
        quadU.bezelColor = SIMD4<Float>(bezelColor.x, bezelColor.y, bezelColor.z, 1)
    }

    /// Marco del quad del radar (espacio VR/recentrado) tras el último `updatePose`; nil sin pose válida. Lo usa el HUD de muñeca (XRPresent).
    var wristFrame: (center: SIMD3<Float>, right: SIMD3<Float>, up: SIMD3<Float>, halfW: Float, halfH: Float)? {
        guard quadValid else { return nil }
        return (SIMD3<Float>(quadU.center.x, quadU.center.y, quadU.center.z), SIMD3<Float>(quadU.right.x, quadU.right.y, quadU.right.z),
                SIMD3<Float>(quadU.up.x, quadU.up.y, quadU.up.z), quadU.center.w, quadU.right.w)
    }

    // MARK: quad en el pase de escena
    private func quadPipeline(_ k: QKey) -> MTLRenderPipelineState? {
        if let p = quadPipes[k] { return p }
        guard let lib = library else { return nil }
        do {
            let fc = MTLFunctionConstantValues()
            var l = linearOutput
            fc.setConstantValue(&l, type: .bool, index: Int(VR_FC_LINEAR_OUT))
            let fs = try lib.makeFunction(name: "vr_fs_radar", constantValues: fc)
            guard let vs = lib.makeFunction(name: k.layered ? "vr_vs_radar_amp" : "vr_vs_radar") else { return nil }
            let d = MTLRenderPipelineDescriptor()
            d.label = "Radar quad"
            d.vertexFunction = vs
            d.fragmentFunction = fs
            d.rasterSampleCount = k.samples
            if k.layered { d.maxVertexAmplificationCount = viewCount }
            d.colorAttachments[0].pixelFormat = k.color
            d.depthAttachmentPixelFormat = k.depth
            if k.depth == .depth32Float_stencil8 { d.stencilAttachmentPixelFormat = k.depth }
            let p = try device.makeRenderPipelineState(descriptor: d)
            quadPipes[k] = p
            return p
        } catch {
            let key = "\(k)"
            if !failed.contains(key) { failed.insert(key); print("[RadarPanel] pipeline del quad: \(error)") }
            return nil
        }
    }

    private func makeSampler() -> MTLSamplerState? {
        let d = MTLSamplerDescriptor()
        d.sAddressMode = .clampToEdge; d.tAddressMode = .clampToEdge
        if smooth {
            d.minFilter = .linear; d.magFilter = .linear; d.mipFilter = .linear; d.maxAnisotropy = 4
        } else {
            d.minFilter = .nearest; d.magFilter = .nearest; d.mipFilter = .notMipmapped
        }
        return device.makeSamplerState(descriptor: d)
    }

    /// Dibuja marco y pantalla dentro del codificador del último segmento del pase de escena (MSAA, Z invertida greaterEqual;
    /// con `layered` usa la amplificación que el codificador ya tiene activada). Siempre se dibuja si hay pose válida y radar en el
    /// juego: es un objeto del brazo; el brillo extra (`glow`) solo sube la luminosidad de la pantalla cuando se mira.
    func encodeQuad(_ enc: MTLRenderCommandEncoder, uniforms: VrSceneUniforms, layered: Bool, color: MTLPixelFormat,
                    depth: MTLPixelFormat, samples: Int) {
        guard enabled, present, quadValid, textureReady, let tex = texture,
              let p = quadPipeline(QKey(layered: layered, color: color, depth: depth, samples: samples)) else { return }
        if sampler == nil { sampler = makeSampler() }
        guard let smp = sampler else { return }
        var u = uniforms
        enc.setRenderPipelineState(p)
        enc.setDepthStencilState(depthState)
        enc.setCullMode(.none)
        enc.setDepthBias(0, slopeScale: 0, clamp: 0)
        enc.setVertexBytes(&u, length: MemoryLayout<VrSceneUniforms>.stride, index: Int(VR_BUF_UNI))
        enc.setFragmentTexture(tex, index: 0)
        enc.setFragmentSamplerState(smp, index: 0)
        var q = quadU
        q.params.z = 1                                 // marco (detrás)
        enc.setVertexBytes(&q, length: MemoryLayout<RadarQuadU>.stride, index: 3)
        enc.setFragmentBytes(&q, length: MemoryLayout<RadarQuadU>.stride, index: 3)
        enc.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
        q.params.z = 0                                 // pantalla
        enc.setVertexBytes(&q, length: MemoryLayout<RadarQuadU>.stride, index: 3)
        enc.setFragmentBytes(&q, length: MemoryLayout<RadarQuadU>.stride, index: 3)
        enc.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
    }
}
