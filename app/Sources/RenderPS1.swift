import Metal
import simd
import Foundation

// Renderer Metal de la escena VR. SIN COMPILAR (escrito sin toolchain Apple): revisar en la primera compilación.
// Consume la API C de vr_scene.h (mv_host_scene) y dibuja: (1) la VRAM de la consola mantenida en GPU, (2) el panel 2D
// (HUD) en hudTexture y (3) la geometría 3D en estéreo. Shaders: render/metal/vr_scene.metal, vr_vram.metal.
//
// Convenciones (contrato del bucle inmersivo): Z invertida (cerca = 1): profundidad limpiada a 0 y compare >=;
// views = mundo->ojo, projections = ojo->clip (clip = P * V * mundo); mundo en metros, Y arriba, -Z delante.
// layered: 1 textura de array (2 vistas) con amplificación de vértices; si no, una textura por ojo y un pase por ojo.
// MSAA: color/profundidad multimuestra propios; el color se resuelve en resolveTargets; la profundidad se guarda (.store).
// Filtro de pantalla (efecto submarino, clase 3 de vr_scene): con `VrScene.filter.active` la escena de cada ojo se resuelve (MSAA) en una textura
// intermedia propia (`fxTex`, mismo formato/forma que el drawable) y un pase de post-proceso por ojo (`vr_fs_post*`) la lee y escribe el drawable con la onda
// vertical por fila (dv) y el tinte; el panel 2D (XRFinalPass) se dibuja después, sin distorsionar. El radar y el arma, que van dentro del pase de escena, sí se distorsionan/tintan.

final class PS1Renderer {
    // MARK: API pública
    let device: MTLDevice
    /// Panel 2D: RGBA8 NO premultiplicado (gamma, valores de la PS1). Se redibuja solo cuando llega una escena nueva.
    private(set) var hudTexture: MTLTexture?
    /// Vista sRGB de hudTexture por si el pase que la muestrea escribe en un destino lineal (decodifica al muestrear).
    private(set) var hudTextureSRGB: MTLTexture?
    /// Suavizado bilineal de texels (respeta transparencia). Por defecto nearest.
    var textureFilter = false
    /// true si el destino es sRGB/lineal (se deduce del formato; ajustable antes del primer encode).
    var linearOutput: Bool
    var backgroundColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
    /// Mapa de tasas de rasterización del drawable (foveación), opcional.
    var rasterizationRateMap: MTLRasterizationRateMap?
    /// Sustituye el viewport por defecto (textura completa). Un elemento por vista.
    var viewportOverride: [MTLViewport]?
    /// Los vértices 2D (kind 2) vienen en coordenadas de VRAM (offset de dibujo sumado) -> se resta el origen del display.
    var hudInVRAMCoords = true
    /// Sesgo de profundidad de las semitransparentes (evita z-fighting con decales coplanares).
    var semiDepthBias: Float = 4
    var semiSlopeBias: Float = 2

    /// false: no se aplica el filtro de pantalla (la escena va directa al drawable, sin onda ni tinte).
    var screenFilterEnabled = true

    // MARK: estado
    private let colorFormat: MTLPixelFormat
    private let depthFormat: MTLPixelFormat
    private let sampleCount: Int
    private let viewCount: Int
    private let library: MTLLibrary?

    private var vram: MTLTexture
    /// Radar de muñeca y arma en mano (integración VR: RadarPanel.swift, RenderWeapon.swift). Se crean al final de `init`.
    var radar: RadarPanel?
    var weapon: WeaponRenderer?
    /// La VRAM de la consola en GPU (el radar de muñeca la muestrea al dibujar su textura).
    var vramTexture: MTLTexture { vram }
    private var hudPre: MTLTexture?
    private let hudW = 1280, hudH = 960   // 4x 320x240

    private var csFill: MTLComputePipelineState?
    private var csUpload: MTLComputePipelineState?
    private var csCopy: MTLComputePipelineState?
    private var dsOpaque: MTLDepthStencilState?
    private var dsSemi: MTLDepthStencilState?

    private struct PKey: Hashable {
        var hud: Bool, layered: Bool, filter: Bool, linear: Bool
        var pass: UInt32, mode: Int          // mode -1 = sin mezcla
        var color: MTLPixelFormat, depth: MTLPixelFormat, samples: Int
    }
    private var pipes: [PKey: MTLRenderPipelineState] = [:]
    // filtro de pantalla (post-proceso)
    private struct FilterFx {
        var needsPost = false
        var tint = SIMD3<Float>(1, 1, 1)
        var cover = SIMD4<Float>(0, 0, 0, -1)       // xyz = color/255, w = modo de mezcla original o -1
        var projH: Float = 160, cyRow: Float = 128
        var nrows = 0
        var dv = [Float](repeating: 0, count: Int(VR_POST_ROWS))   // desplazamiento por fila de la imagen original (0 donde no hay tira)
    }
    private struct PostPlan { var sceneColor: [MTLTexture], sceneResolve: [MTLTexture], fxTex: [MTLTexture], dst: [MTLTexture] }
    private var fx = FilterFx()
    private var fxTex: [MTLTexture] = []
    private var fxKey = ""
    private var postPipes: [String: MTLRenderPipelineState] = [:]
    private var pipeFull: MTLRenderPipelineState?
    private var failed = Set<String>()

    // escena persistente (se repite hasta que llegue otra)
    private struct DrawBatch { var first: Int, count: Int, kind: Int, mode: Int, semi: Bool, hasTex: Bool, evEnd: Int }
    private final class VtxSlot { var buf: MTLBuffer?; var cap = 0; var stOff = 0 }   // stOff: offset de VrGpuState[] tras los vértices
    // Invariante del anillo (revisado): `inflight` (3) cuenta comandos en vuelo INCLUIDO el que se codifica, así que al volver de
    // `inflight.wait()` hay como mucho 2 anteriores sin terminar (la cola Metal los completa en orden). `storeScene` avanza `curSlot`
    // como mucho UNA vez por encode, de modo que el slot que se escribe (curSlot+1) nunca es el de los dos fotogramas previos
    // (curSlot y curSlot-1 como mucho). Hace falta slots >= valor del semáforo: si se sube uno, subir el otro (memoria compartida
    // escrita por CPU: Metal no hace seguimiento de peligros ahí).
    private var vtxSlots = [VtxSlot(), VtxSlot(), VtxSlot()]
    private var curSlot = 2
    private var batches: [DrawBatch] = []
    private var has3D = false
    private var haveScene = false
    private var hudRect = simd_float4(0, 0, 320, 240)
    private var lastIndex: UInt32?
    private var hudDone = false

    // por frame (anillo de 3 con semáforo)
    private final class FrameSlot { var buf: MTLBuffer?; var cap = 0; var used = 0 }
    private var frames = [FrameSlot(), FrameSlot(), FrameSlot()]
    private var frameNo = 0
    private let inflight = DispatchSemaphore(value: 3)

    private enum VramCmd {
        case initBlit(byteOffset: Int)
        case fill(VrVramEvent), upload(VrVramEvent), copy(VrVramEvent)
    }
    /// Evento con su índice en src->ops (initBlit = -1): VrBatch.ev_end manda aplicar los de índice < ev_end antes del batch.
    private struct IdxCmd { var op: Int; var cmd: VramCmd }

    // MARK: init
    init(device: MTLDevice, colorFormat: MTLPixelFormat, depthFormat: MTLPixelFormat, sampleCount: Int, viewCount: Int) {
        self.device = device
        self.colorFormat = colorFormat
        self.depthFormat = depthFormat
        self.sampleCount = sampleCount
        self.viewCount = max(1, min(2, viewCount))
        switch colorFormat {
        case .bgra8Unorm_srgb, .rgba8Unorm_srgb, .rgba16Float: linearOutput = true
        default: linearOutput = false
        }
        let lib0 = device.makeDefaultLibrary()
        library = lib0
        if lib0 == nil { print("[PS1Renderer] sin biblioteca Metal por defecto: faltan vr_*.metal en el target") }
        if MemoryLayout<VrVtx>.stride != MemoryLayout<VrGpuVtx>.stride {
            print("[PS1Renderer] VrVtx (\(MemoryLayout<VrVtx>.stride) B) != VrGpuVtx: layout incompatible")
        }

        let td = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .r16Uint, width: 1024, height: 512, mipmapped: false)
        td.usage = [.shaderRead, .shaderWrite]
        td.storageMode = .private
        vram = device.makeTexture(descriptor: td)!
        vram.label = "PS1 VRAM"

        let hd = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm, width: hudW, height: hudH, mipmapped: false)
        hd.usage = [.renderTarget, .shaderRead, .pixelFormatView]
        hd.storageMode = .private
        hudTexture = device.makeTexture(descriptor: hd)
        hudTexture?.label = "PS1 HUD"
        hudTextureSRGB = hudTexture?.makeTextureView(pixelFormat: .rgba8Unorm_srgb)
        let pd = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba16Unorm, width: hudW, height: hudH, mipmapped: false)
        pd.usage = [.renderTarget, .shaderRead]
        pd.storageMode = .private
        hudPre = device.makeTexture(descriptor: pd)
        hudPre?.label = "PS1 HUD premultiplicado (unorm: satura a 0..1 como la PS1)"

        if let lib = lib0 {
            func cp(_ n: String) -> MTLComputePipelineState? {
                guard let f = lib.makeFunction(name: n) else { print("[PS1Renderer] falta \(n)"); return nil }
                return try? device.makeComputePipelineState(function: f)
            }
            csFill = cp("vr_k_fill"); csUpload = cp("vr_k_upload"); csCopy = cp("vr_k_copy")
        }
        let d1 = MTLDepthStencilDescriptor()
        d1.depthCompareFunction = .greaterEqual; d1.isDepthWriteEnabled = true
        dsOpaque = device.makeDepthStencilState(descriptor: d1)
        let d2 = MTLDepthStencilDescriptor()
        d2.depthCompareFunction = .greaterEqual; d2.isDepthWriteEnabled = false
        dsSemi = device.makeDepthStencilState(descriptor: d2)
        radar = RadarPanel(device: device, library: library, colorFormat: colorFormat, depthFormat: depthFormat,
                           sampleCount: sampleCount, viewCount: self.viewCount, linearOutput: linearOutput)
        weapon = WeaponRenderer(device: device, colorFormat: colorFormat, depthFormat: depthFormat,
                                sampleCount: sampleCount, viewCount: self.viewCount)
    }

    // MARK: pipelines
    private func pipeline(_ k: PKey) -> MTLRenderPipelineState? {
        if let p = pipes[k] { return p }
        guard let lib = library else { return nil }
        do {
            let fc = MTLFunctionConstantValues()
            var f = k.filter, l = k.linear, pass = k.pass
            fc.setConstantValue(&f, type: .bool, index: Int(VR_FC_FILTER))
            fc.setConstantValue(&l, type: .bool, index: Int(VR_FC_LINEAR_OUT))
            fc.setConstantValue(&pass, type: .uint, index: Int(VR_FC_PASS))
            let fs = try lib.makeFunction(name: "vr_fs_scene", constantValues: fc)
            let vsName = k.hud ? "vr_vs_hud" : (k.layered ? "vr_vs_scene_amp" : "vr_vs_scene")
            guard let vs = lib.makeFunction(name: vsName) else { return nil }
            let d = MTLRenderPipelineDescriptor()
            d.label = "PS1 \(vsName) pass\(k.pass) blend\(k.mode)"
            d.vertexFunction = vs
            d.fragmentFunction = fs
            d.rasterSampleCount = k.samples
            d.inputPrimitiveTopology = .triangle
            if k.layered { d.maxVertexAmplificationCount = viewCount }
            let ca = d.colorAttachments[0]!
            ca.pixelFormat = k.color
            if !k.hud {
                d.depthAttachmentPixelFormat = k.depth
                if k.depth == .depth32Float_stencil8 { d.stencilAttachmentPixelFormat = k.depth }
            }
            if k.mode >= 0 {
                ca.isBlendingEnabled = true
                // Los 4 modos de la PS1 (B = destino, F = fragmento). blendColor = 0,5 (modo 0) o 0,25 (modo 3).
                switch k.mode {
                case 0: // 0,5B + 0,5F  (alfa: 0,5·1 + 0,5·Ad -> cobertura de panel premultiplicado)
                    ca.rgbBlendOperation = .add
                    ca.sourceRGBBlendFactor = .blendColor; ca.destinationRGBBlendFactor = .oneMinusBlendColor
                    ca.alphaBlendOperation = .add
                    ca.sourceAlphaBlendFactor = .blendAlpha; ca.destinationAlphaBlendFactor = .oneMinusBlendAlpha
                case 1: // B + F
                    ca.rgbBlendOperation = .add
                    ca.sourceRGBBlendFactor = .one; ca.destinationRGBBlendFactor = .one
                    ca.alphaBlendOperation = .add
                    ca.sourceAlphaBlendFactor = .zero; ca.destinationAlphaBlendFactor = .one
                case 2: // B - F
                    ca.rgbBlendOperation = .reverseSubtract
                    ca.sourceRGBBlendFactor = .one; ca.destinationRGBBlendFactor = .one
                    ca.alphaBlendOperation = .add
                    ca.sourceAlphaBlendFactor = .zero; ca.destinationAlphaBlendFactor = .one
                default: // B + 0,25F
                    ca.rgbBlendOperation = .add
                    ca.sourceRGBBlendFactor = .blendColor; ca.destinationRGBBlendFactor = .one
                    ca.alphaBlendOperation = .add
                    ca.sourceAlphaBlendFactor = .zero; ca.destinationAlphaBlendFactor = .one
                }
            }
            let p = try device.makeRenderPipelineState(descriptor: d)
            pipes[k] = p
            return p
        } catch {
            let key = "\(k)"
            if !failed.contains(key) { failed.insert(key); print("[PS1Renderer] pipeline: \(error)") }
            return nil
        }
    }

    private func fullPipeline() -> MTLRenderPipelineState? {
        if let p = pipeFull { return p }
        guard let lib = library, let vs = lib.makeFunction(name: "vr_vs_full"),
              let fs = lib.makeFunction(name: "vr_fs_unpremul") else { return nil }
        let d = MTLRenderPipelineDescriptor()
        d.label = "PS1 HUD sin premultiplicar"
        d.vertexFunction = vs; d.fragmentFunction = fs
        d.colorAttachments[0].pixelFormat = .rgba8Unorm
        pipeFull = try? device.makeRenderPipelineState(descriptor: d)
        return pipeFull
    }

    // MARK: búferes
    private func ensureUpload(_ fs: FrameSlot, extra: Int) -> Bool {
        let need = fs.used + extra
        if need > fs.cap {
            let cap = max(need, fs.cap * 2, 1 << 20)
            guard let nb = device.makeBuffer(length: cap, options: [.storageModeShared, .cpuCacheModeWriteCombined]) else { return false }
            nb.label = "PS1 subidas VRAM"
            if let ob = fs.buf, fs.used > 0 { memcpy(nb.contents(), ob.contents(), fs.used) }
            fs.buf = nb; fs.cap = cap
        }
        return true
    }

    /// Eventos de VRAM de una lista, en orden. Los píxeles de las subidas se copian ahora (src.data solo vale hasta la
    /// siguiente llamada a mv_host_scene).
    private func collectEvents(_ src: UnsafePointer<GpuDrawList>, _ fs: FrameSlot, _ cmds: inout [IdxCmd]) {
        let l = src.pointee
        if let vi = l.vram_init {
            fs.used = (fs.used + 255) & ~255
            let n = 1024 * 512 * 2
            if ensureUpload(fs, extra: n), let b = fs.buf {
                memcpy(b.contents() + fs.used, vi, n)
                cmds.append(IdxCmd(op: -1, cmd: .initBlit(byteOffset: fs.used)))
                fs.used += n
            }
        }
        guard let ops = l.ops else { return }
        for i in 0..<Int(l.nops) {
            let op = ops[i]
            let t = Int(op.type)
            if t != Int(GPU_OP_UPLOAD) && t != Int(GPU_OP_VCOPY) && t != Int(GPU_OP_FILL) { continue }
            var ev = VrVramEvent()
            ev.x = Int32(op.rx); ev.y = Int32(op.ry); ev.w = Int32(op.rw); ev.h = Int32(op.rh)
            ev.sx = Int32(op.sx); ev.sy = Int32(op.sy)
            ev.flags = UInt32(op.flags)
            ev.fill15 = UInt32(op.fill15)
            if ev.w <= 0 || ev.h <= 0 { continue }
            if t == Int(GPU_OP_FILL) { cmds.append(IdxCmd(op: i, cmd: .fill(ev))) }
            else if t == Int(GPU_OP_VCOPY) { cmds.append(IdxCmd(op: i, cmd: .copy(ev))) }
            else {
                guard let data = l.data, op.data_off < l.ndata else { continue }
                let n = Int(min(op.data_n, l.ndata - op.data_off, UInt32(ev.w * ev.h)))
                if n == 0 { continue }
                fs.used = (fs.used + 3) & ~3
                guard ensureUpload(fs, extra: n * 2), let b = fs.buf else { continue }
                memcpy(b.contents() + fs.used, data + Int(op.data_off), n * 2)
                ev.data_off = UInt32(fs.used / 2)
                ev.data_n = UInt32(n)
                fs.used += n * 2
                cmds.append(IdxCmd(op: i, cmd: .upload(ev)))
            }
        }
    }

    /// Resumen del filtro de pantalla de la escena (VrScene.filter, contrato en vr_scene.h "Clase 3"): se conserva hasta la siguiente escena.
    private func storeFilter(_ f: VrFilter) {
        var n = FilterFx()
        defer { fx = n }
        guard screenFilterEnabled, f.active != 0 else { return }
        var warp = false
        withUnsafeBytes(of: f.dv) { (dvRaw: UnsafeRawBufferPointer) -> Void in
            withUnsafeBytes(of: f.rowcov) { (covRaw: UnsafeRawBufferPointer) -> Void in
                let dvp = dvRaw.bindMemory(to: Float.self)
                let rows = min(n.dv.count, dvp.count, covRaw.count)
                for i in 0..<rows where covRaw[i] != 0 {
                    let d = dvp[i]
                    if d.isFinite && abs(d) < 64 { n.dv[i] = d; if d != 0 { warp = true } }
                }
            }
        }
        n.nrows = warp ? max(1, Int(f.nrows)) : 0
        let tc = (f.tint.0, f.tint.1, f.tint.2)
        n.tint = SIMD3<Float>(tc.0.isFinite ? min(max(tc.0, 0), 4) : 1, tc.1.isFinite ? min(max(tc.1, 0), 4) : 1, tc.2.isFinite ? min(max(tc.2, 0), 4) : 1)
        if f.cover_abr >= 0 {
            n.cover = SIMD4<Float>(f.cover_rgb.0, f.cover_rgb.1, f.cover_rgb.2, Float(f.cover_abr & 3))
        }
        n.projH = f.proj_h.isFinite && f.proj_h > 1 ? f.proj_h : 160
        n.cyRow = f.cy_row.isFinite ? f.cy_row : 128
        let tinted = abs(n.tint.x - 1) > 1e-3 || abs(n.tint.y - 1) > 1e-3 || abs(n.tint.z - 1) > 1e-3
        n.needsPost = warp || tinted || n.cover.w >= 0
    }

    private func storeScene(_ sc: VrScene, _ newSlot: inout Bool) {
        storeFilter(sc.filter)
        let nv = Int(sc.nv)
        if !newSlot { curSlot = (curSlot + 1) % 3; newSlot = true }
        let slot = vtxSlots[curSlot]
        let stOff = (nv * MemoryLayout<VrGpuVtx>.stride + 15) & ~15
        let ns = Int(sc.nstates)
        let bytes = max(stOff + max(ns, 1) * MemoryLayout<VrGpuState>.stride, 32)
        if bytes > slot.cap {
            let cap = max(bytes, slot.cap * 2, 1 << 20)
            slot.buf = device.makeBuffer(length: cap, options: [.storageModeShared, .cpuCacheModeWriteCombined])
            slot.buf?.label = "PS1 vértices VR"
            slot.cap = slot.buf == nil ? 0 : cap
        }
        batches.removeAll(keepingCapacity: true)
        has3D = sc.has_3d != 0
        haveScene = true
        guard let buf = slot.buf, let v = sc.v, nv > 0 else { return }
        memcpy(buf.contents(), v, nv * MemoryLayout<VrGpuVtx>.stride)
        slot.stOff = stOff
        if ns > 0, let st = sc.states { memcpy(buf.contents() + stOff, st, ns * MemoryLayout<VrGpuState>.stride) }
        else { memset(buf.contents() + stOff, 0, MemoryLayout<VrGpuState>.stride) }
        guard let b = sc.b else { return }
        let texBit = UInt16(VR_VF_TEXTURED)
        for i in 0..<Int(sc.nb) {
            let bt = b[i]
            let first = Int(bt.first)
            if first >= nv { continue }
            var cnt = min(Int(bt.count), nv - first)
            cnt -= cnt % 3
            if cnt <= 0 { continue }
            let kind = Int(bt.kind)
            if kind == 3 { continue }   // filtro de pantalla: no es geometría ni HUD (lo aplica el pase de post-proceso con VrScene.filter)
            var hasTex = false
            if kind != 0 {
                for j in first..<(first + cnt) where (v[j].flags & texBit) != 0 { hasTex = true; break }
            }
            let semi = kind == 1 || (kind == 2 && bt.blend <= 3)   // blend 0xFF = opaco
            batches.append(DrawBatch(first: first, count: cnt, kind: kind, mode: Int(bt.blend) & 3, semi: semi,
                                     hasTex: hasTex, evEnd: Int(bt.ev_end)))
        }
    }

    // MARK: encode
    func encode(commandBuffer: MTLCommandBuffer, colorTargets: [MTLTexture], depthTargets: [MTLTexture],
                resolveTargets: [MTLTexture], views: [simd_float4x4], projections: [simd_float4x4],
                head: simd_float4x4, layered: Bool) {
        inflight.wait()
        let sem = inflight
        commandBuffer.addCompletedHandler { _ in sem.signal() }
        let fs = frames[frameNo % 3]
        frameNo += 1
        fs.used = 0

        // 1) escenas nuevas: eventos de VRAM + geometría. Si llegan varias listas, los eventos de las anteriores se
        //    aplican enteros al principio y solo se dibuja la última.
        var preCmds: [IdxCmd] = []
        var listCmds: [IdxCmd] = []
        var newSlot = false
        var gotNew = false
        for _ in 0..<4 {
            var sc = VrScene()
            if mv_host_scene(&sc) == 0 { break }
            var idx: UInt32?
            if let s = sc.src {
                idx = s.pointee.index
                if let l = lastIndex, l == idx!, s.pointee.vram_init == nil { break }   // (tras una resincronización la numeración vuelve a 0: la lista con vram_init nunca es repetida)   // misma lista repetida: no reaplicar eventos
            }
            lastIndex = idx
            preCmds += listCmds
            listCmds.removeAll()
            if let s = sc.src {
                collectEvents(s, fs, &listCmds)
                let d = s.pointee.disp
                let w = d.w > 0 ? Float(d.w) : 320, h = d.h > 0 ? Float(d.h) : 240
                hudRect = hudInVRAMCoords ? simd_float4(Float(d.x), Float(d.y), w, h) : simd_float4(0, 0, w, h)
            }
            storeScene(sc, &newSlot)
            radar?.ingest(device: device)       // escena del radar asociada a esta escena (mv_host_radar)
            gotNew = true
        }
        encodeVram(commandBuffer, preCmds.map { $0.cmd }, fs)

        // 2) grupos de batches con el mismo ev_end: antes de cada grupo se aplican los eventos pendientes de VRAM.
        //    Repetición de una escena ya vista (reproyección): un solo grupo, sin eventos (la VRAM ya está en su estado final).
        var groups: [(Int, Int, Int)] = []   // (inicio, fin, ev_end)
        if gotNew && !batches.isEmpty {
            var s = 0
            for i in 1...batches.count where i == batches.count || batches[i].evEnd != batches[s].evEnd {
                groups.append((s, i, batches[s].evEnd)); s = i
            }
        } else {
            groups.append((0, batches.count, Int.max))
        }
        let doHud = gotNew || !hudDone
        // Filtro de pantalla: la escena se resuelve en texturas intermedias y un pase por ojo escribe el drawable (ver planPost).
        let plan = planPost(colorTargets: colorTargets, resolveTargets: resolveTargets, layered: layered)
        let sceneColor = plan?.sceneColor ?? colorTargets
        let sceneResolve = plan?.sceneResolve ?? resolveTargets
        var nextCmd = 0
        for (gi, g) in groups.enumerated() {
            let firstG = gi == 0, lastG = gi == groups.count - 1
            var pend: [VramCmd] = []
            while nextCmd < listCmds.count && (listCmds[nextCmd].op < g.2) { pend.append(listCmds[nextCmd].cmd); nextCmd += 1 }
            encodeVram(commandBuffer, pend, fs)
            if doHud { encodeHud(commandBuffer, range: g.0..<g.1, first: firstG) }
            encodeScenePasses(commandBuffer, range: g.0..<g.1, first: firstG, last: lastG, colorTargets: sceneColor,
                              depthTargets: depthTargets, resolveTargets: sceneResolve, views: views,
                              projections: projections, head: head, layered: layered)
        }
        if let p = plan { encodePost(commandBuffer, p, projections: projections, layered: layered) }
        // eventos restantes de la lista (tras el último batch) para dejar la VRAM en sincronía con el oráculo
        if nextCmd < listCmds.count { encodeVram(commandBuffer, listCmds[nextCmd...].map { $0.cmd }, fs) }
        radar?.renderTexture(commandBuffer, vram: vram)      // VRAM ya en su estado final de la lista
        if doHud { finishHud(commandBuffer); hudDone = true }
    }

    private func encodeScenePasses(_ cb: MTLCommandBuffer, range: Range<Int>, first: Bool, last: Bool,
                                   colorTargets: [MTLTexture], depthTargets: [MTLTexture], resolveTargets: [MTLTexture],
                                   views: [simd_float4x4], projections: [simd_float4x4], head: simd_float4x4, layered: Bool) {
        let n = min(views.count, projections.count)
        if layered {
            guard let c = colorTargets.first, let d = depthTargets.first, n >= 1 else { return }
            let cnt = min(n, viewCount)
            var u = VrSceneUniforms()
            u.view_proj = (projections[0] * views[0], projections[min(1, cnt - 1)] * views[min(1, cnt - 1)])
            u.head = head.columns.3
            encodeScene(cb, range: range, first: first, last: last, color: c, depth: d, resolve: resolveTargets.first,
                        uniforms: u, amplify: cnt, views: views, projections: projections)
        } else {
            for i in 0..<min(colorTargets.count, depthTargets.count) where i < n {
                var u = VrSceneUniforms()
                let m = projections[i] * views[i]
                u.view_proj = (m, m)
                u.head = head.columns.3
                encodeScene(cb, range: range, first: first, last: last, color: colorTargets[i], depth: depthTargets[i],
                            resolve: i < resolveTargets.count ? resolveTargets[i] : nil, uniforms: u, amplify: 0, viewIndex: i,
                            views: views, projections: projections)
            }
        }
    }

    // MARK: filtro de pantalla (post-proceso)
    private func postPipeline(format: MTLPixelFormat, layered: Bool) -> MTLRenderPipelineState? {
        let key = "\(format.rawValue)-\(layered)-\(linearOutput)"
        if let p = postPipes[key] { return p }
        if failed.contains(key) { return nil }      // ya falló: no reintentar (compilar un pipeline por fotograma)
        guard let lib = library else { failed.insert(key); return nil }
        do {
            let fc = MTLFunctionConstantValues()
            var lin = linearOutput
            fc.setConstantValue(&lin, type: .bool, index: Int(VR_FC_LINEAR_OUT))
            let fs = try lib.makeFunction(name: layered ? "vr_fs_post_layered" : "vr_fs_post", constantValues: fc)
            guard let vs = lib.makeFunction(name: layered ? "vr_vs_post_layered" : "vr_vs_post") else { failed.insert(key); return nil }
            let d = MTLRenderPipelineDescriptor()
            d.label = "VR post filtro de pantalla"
            d.vertexFunction = vs
            d.fragmentFunction = fs
            d.colorAttachments[0].pixelFormat = format
            if layered { d.inputPrimitiveTopology = .triangle }   // requerido al escribir render_target_array_index
            let p = try device.makeRenderPipelineState(descriptor: d)
            postPipes[key] = p
            return p
        } catch {
            if !failed.contains(key) { failed.insert(key); print("[Renderer] pipeline post: \(error)") }
            return nil
        }
    }

    /// Texturas intermedias (una muestra) con la forma y el formato de los destinos; se recrean si cambian.
    private func filterTextures(like dst: [MTLTexture]) -> [MTLTexture]? {
        let key = dst.map { "\($0.textureType.rawValue)/\($0.width)x\($0.height)x\($0.arrayLength)/\($0.pixelFormat.rawValue)" }.joined(separator: ",")
        if key == fxKey && fxTex.count == dst.count { return fxTex }
        var out: [MTLTexture] = []
        for t in dst {
            guard t.textureType == .type2D || t.textureType == .type2DArray else { return nil }
            let d = MTLTextureDescriptor()
            d.textureType = t.textureType
            d.pixelFormat = t.pixelFormat
            d.width = t.width
            d.height = t.height
            d.arrayLength = t.arrayLength
            d.mipmapLevelCount = 1
            d.sampleCount = 1
            d.usage = [.renderTarget, .shaderRead]
            d.storageMode = .private
            guard let nt = device.makeTexture(descriptor: d) else { return nil }
            nt.label = "VR escena sin filtrar"
            out.append(nt)
        }
        fxTex = out
        fxKey = key
        return out
    }

    /// Plan del filtro: con MSAA la escena se resuelve en `fxTex` en vez de en el drawable; sin MSAA se dibuja directa en `fxTex`. nil = sin filtro
    /// (no hace falta, está desactivado, o no es viable: viewport propio o mapa de tasas de rasterización, texturas que no son 2D, falta el pipeline).
    private func planPost(colorTargets: [MTLTexture], resolveTargets: [MTLTexture], layered: Bool) -> PostPlan? {
        guard fx.needsPost, screenFilterEnabled, viewportOverride == nil, rasterizationRateMap == nil else { return nil }
        let msaa = (colorTargets.first?.sampleCount ?? 1) > 1
        let dst = msaa ? resolveTargets : colorTargets
        guard let first = dst.first, postPipeline(format: first.pixelFormat, layered: layered) != nil,
              let tex = filterTextures(like: dst) else { return nil }
        if msaa { return PostPlan(sceneColor: colorTargets, sceneResolve: tex, fxTex: tex, dst: dst) }
        return PostPlan(sceneColor: tex, sceneResolve: [], fxTex: tex, dst: dst)
    }

    /// Pase de post-proceso por ojo: lee la escena resuelta (fxTex) y escribe el drawable con la onda por fila, el tinte y el recubrimiento.
    /// Parámetros por ojo a partir de la proyección: fila del punto principal = H/2*(1+P[2][1]) y focal vertical = H/2*P[1][1] (px).
    private func encodePost(_ cb: MTLCommandBuffer, _ plan: PostPlan, projections: [simd_float4x4], layered: Bool) {
        guard let first = plan.dst.first, let pso = postPipeline(format: first.pixelFormat, layered: layered) else { return }
        func eyeParams(_ i: Int, _ height: Int) -> SIMD4<Float> {
            let h = Float(height)
            guard !projections.isEmpty else { return SIMD4<Float>(h * 0.5, h * 0.5, 0, 0) }
            let p = projections[min(i, projections.count - 1)]
            return SIMD4<Float>(h * 0.5 * (1 + p.columns.2.y), h * 0.5 * p.columns.1.y, 0, 0)
        }
        var u = VrPostUniforms()
        u.tint = SIMD4<Float>(fx.tint.x, fx.tint.y, fx.tint.z, 0)
        u.cover = fx.cover
        u.cfg = SIMD4<Float>(fx.projH, fx.cyRow, Float(fx.nrows), 0)
        let dvCopy = fx.dv
        if layered {
            guard let src = plan.fxTex.first else { return }
            let pd = MTLRenderPassDescriptor()
            pd.colorAttachments[0].texture = first
            pd.colorAttachments[0].loadAction = .dontCare      // el triángulo cubre todo
            pd.colorAttachments[0].storeAction = .store
            let n = max(1, min(viewCount, first.arrayLength))
            pd.renderTargetArrayLength = n
            u.eye = (eyeParams(0, first.height), eyeParams(1, first.height))
            guard let enc = cb.makeRenderCommandEncoder(descriptor: pd) else { return }
            enc.label = "VR post filtro (estéreo)"
            enc.setRenderPipelineState(pso)
            enc.setViewport(MTLViewport(originX: 0, originY: 0, width: Double(first.width), height: Double(first.height), znear: 0, zfar: 1))
            enc.setFragmentTexture(src, index: 0)
            enc.setFragmentBytes(&u, length: MemoryLayout<VrPostUniforms>.stride, index: 0)
            dvCopy.withUnsafeBytes { (raw: UnsafeRawBufferPointer) -> Void in
                if let base = raw.baseAddress { enc.setFragmentBytes(base, length: raw.count, index: Int(VR_BUF_POST_DV)) }
            }
            enc.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3, instanceCount: n)
            enc.endEncoding()
        } else {
            for i in 0..<min(plan.dst.count, plan.fxTex.count, viewCount, projections.count) {
                let dst = plan.dst[i]
                let pd = MTLRenderPassDescriptor()
                pd.colorAttachments[0].texture = dst
                pd.colorAttachments[0].loadAction = .dontCare
                pd.colorAttachments[0].storeAction = .store
                let ep = eyeParams(i, dst.height)
                u.eye = (ep, ep)                                // el fragmento del ojo suelto usa eye[0]
                guard let enc = cb.makeRenderCommandEncoder(descriptor: pd) else { continue }
                enc.label = "VR post filtro (ojo \(i))"
                enc.setRenderPipelineState(pso)
                enc.setViewport(MTLViewport(originX: 0, originY: 0, width: Double(dst.width), height: Double(dst.height), znear: 0, zfar: 1))
                enc.setFragmentTexture(plan.fxTex[i], index: 0)
                enc.setFragmentBytes(&u, length: MemoryLayout<VrPostUniforms>.stride, index: 0)
                dvCopy.withUnsafeBytes { (raw: UnsafeRawBufferPointer) -> Void in
                    if let base = raw.baseAddress { enc.setFragmentBytes(base, length: raw.count, index: Int(VR_BUF_POST_DV)) }
                }
                enc.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
                enc.endEncoding()
            }
        }
    }

    // MARK: VRAM
    private func encodeVram(_ cb: MTLCommandBuffer, _ cmds: [VramCmd], _ fs: FrameSlot) {
        if cmds.isEmpty { return }
        var comp: MTLComputeCommandEncoder?
        var blit: MTLBlitCommandEncoder?
        func bind(_ enc: MTLComputeCommandEncoder, _ p: MTLComputePipelineState, _ ev: VrVramEvent) {
            enc.setComputePipelineState(p)
            enc.setTexture(vram, index: 0)
            var e = ev
            enc.setBytes(&e, length: MemoryLayout<VrVramEvent>.stride, index: 0)
        }
        for c in cmds {
            switch c {
            case .initBlit(let off):
                guard let b = fs.buf else { continue }
                if comp != nil { comp?.endEncoding(); comp = nil }
                if blit == nil { blit = cb.makeBlitCommandEncoder() }
                blit?.copy(from: b, sourceOffset: off, sourceBytesPerRow: 2048, sourceBytesPerImage: 2048 * 512,
                           sourceSize: MTLSize(width: 1024, height: 512, depth: 1), to: vram, destinationSlice: 0,
                           destinationLevel: 0, destinationOrigin: MTLOrigin(x: 0, y: 0, z: 0))
            case .fill(let ev), .upload(let ev), .copy(let ev):
                if blit != nil { blit?.endEncoding(); blit = nil }
                if comp == nil { comp = cb.makeComputeCommandEncoder() }
                guard let enc = comp else { continue }
                let tg = MTLSize(width: 16, height: 16, depth: 1)
                let grid = MTLSize(width: Int(ev.w), height: Int(ev.h), depth: 1)
                switch c {
                case .fill:
                    guard let p = csFill else { continue }
                    bind(enc, p, ev)
                    enc.dispatchThreads(grid, threadsPerThreadgroup: tg)
                case .upload:
                    guard let p = csUpload, let b = fs.buf else { continue }
                    bind(enc, p, ev)
                    enc.setBuffer(b, offset: 0, index: 1)
                    enc.dispatchThreads(grid, threadsPerThreadgroup: tg)
                default: // copia fila a fila: un único threadgroup (el kernel asume w <= 8·hilos)
                    guard let p = csCopy else { continue }
                    bind(enc, p, ev)
                    let t = min(256, p.maxTotalThreadsPerThreadgroup)
                    enc.dispatchThreadgroups(MTLSize(width: 1, height: 1, depth: 1),
                                             threadsPerThreadgroup: MTLSize(width: t, height: 1, depth: 1))
                }
            }
        }
        comp?.endEncoding()
        blit?.endEncoding()
    }

    // MARK: HUD
    /// Segmento de HUD (batches kind 2 del rango) en hudPre (premultiplicado, rgba16f); `first` limpia a transparente.
    private func encodeHud(_ cb: MTLCommandBuffer, range: Range<Int>, first: Bool) {
        guard let pre = hudPre else { return }
        let segment = range.filter { batches.indices.contains($0) && batches[$0].kind == 2 }
        if !first && segment.isEmpty { return }
        let pd = MTLRenderPassDescriptor()
        pd.colorAttachments[0].texture = pre
        pd.colorAttachments[0].loadAction = first ? .clear : .load
        pd.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 0)
        pd.colorAttachments[0].storeAction = .store
        guard let enc = cb.makeRenderCommandEncoder(descriptor: pd) else { return }
        enc.label = "PS1 HUD"
        defer { enc.endEncoding() }
        enc.setCullMode(.none)
        enc.setViewport(MTLViewport(originX: 0, originY: 0, width: Double(hudW), height: Double(hudH), znear: 0, zfar: 1))
        guard haveScene, let b = vtxSlots[curSlot].buf else { return }
        var hu = VrHudUniforms(); hu.rect = hudRect
        enc.setVertexBuffer(b, offset: 0, index: Int(VR_BUF_VERTS))
        enc.setVertexBuffer(b, offset: vtxSlots[curSlot].stOff, index: Int(VR_BUF_STATES))
        enc.setVertexBytes(&hu, length: MemoryLayout<VrHudUniforms>.stride, index: Int(VR_BUF_UNI))
        enc.setFragmentTexture(vram, index: 0)
        for i in segment {
            let bt = batches[i]
            enc.setVertexBufferOffset(bt.first * MemoryLayout<VrGpuVtx>.stride, index: Int(VR_BUF_VERTS))
            func draw(_ pass: UInt32, _ mode: Int) {
                let k = PKey(hud: true, layered: false, filter: textureFilter, linear: false, pass: pass, mode: mode,
                             color: .rgba16Unorm, depth: .invalid, samples: 1)
                guard let p = pipeline(k) else { return }
                enc.setRenderPipelineState(p)
                enc.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: bt.count)
            }
            if bt.semi {
                let a: Float = bt.mode == 3 ? 0.25 : 0.5
                enc.setBlendColor(red: a, green: a, blue: a, alpha: a)
                draw(bt.hasTex ? 1 : 0, bt.mode)
                if bt.hasTex { draw(2, -1) }   // texels sin bit15: opacos
            } else {
                draw(0, -1)
            }
        }
    }

    /// premultiplicado (rgba16f) -> no premultiplicado (rgba8) = hudTexture
    private func finishHud(_ cb: MTLCommandBuffer) {
        guard let pre = hudPre, let out = hudTexture else { return }
        let p2 = MTLRenderPassDescriptor()
        p2.colorAttachments[0].texture = out
        p2.colorAttachments[0].loadAction = .dontCare
        p2.colorAttachments[0].storeAction = .store
        guard let enc = cb.makeRenderCommandEncoder(descriptor: p2) else { return }
        enc.label = "PS1 HUD sin premultiplicar"
        if let fp = fullPipeline() {
            enc.setRenderPipelineState(fp)
            enc.setFragmentTexture(pre, index: 0)
            enc.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
        }
        enc.endEncoding()
    }

    // MARK: escena 3D
    /// amplify > 0: un solo pase con amplificación de vértices a `amplify` capas del array; 0: un pase de un ojo.
    /// first: limpia color/profundidad; si no, carga. last: resuelve el color (MSAA); si no, lo guarda sin resolver.
    private func encodeScene(_ cb: MTLCommandBuffer, range: Range<Int>, first: Bool, last: Bool, color: MTLTexture,
                             depth: MTLTexture, resolve: MTLTexture?, uniforms: VrSceneUniforms, amplify: Int, viewIndex: Int = 0,
                             views: [simd_float4x4] = [], projections: [simd_float4x4] = []) {
        let seg = range.filter { batches.indices.contains($0) && batches[$0].kind != 2 }
        if !first && seg.isEmpty && !last { return }
        let samples = color.sampleCount
        let pd = MTLRenderPassDescriptor()
        let ca = pd.colorAttachments[0]!
        ca.texture = color
        ca.loadAction = first ? .clear : .load
        ca.clearColor = backgroundColor
        if samples > 1, last, let r = resolve {
            ca.resolveTexture = r
            ca.storeAction = .multisampleResolve
        } else {
            ca.storeAction = .store
        }
        let da = pd.depthAttachment!
        da.texture = depth
        da.loadAction = first ? .clear : .load
        da.clearDepth = 0            // Z invertida
        da.storeAction = .store      // la profundidad MSAA la resuelve el pase final del bucle
        if depth.pixelFormat == .depth32Float_stencil8 {
            pd.stencilAttachment.texture = depth
            pd.stencilAttachment.loadAction = .dontCare
            pd.stencilAttachment.storeAction = .dontCare
        }
        if amplify > 0 { pd.renderTargetArrayLength = amplify }
        if let m = rasterizationRateMap { pd.rasterizationRateMap = m }
        guard let enc = cb.makeRenderCommandEncoder(descriptor: pd) else { return }
        enc.label = amplify > 0 ? "PS1 escena (estéreo, amplificación)" : "PS1 escena (ojo \(viewIndex))"
        defer { enc.endEncoding() }
        // Los defer corren al revés: este va ANTES de cerrar el codificador, tras dibujar el 3D (también si el guard de más abajo
        // retorna). Solo en el último segmento (el que resuelve el MSAA): el quad del radar y el arma se prueban contra la profundidad del 3D.
        defer {
            if last {
                radar?.encodeQuad(enc, uniforms: uniforms, layered: amplify > 0, color: color.pixelFormat, depth: depth.pixelFormat, samples: samples)
                weapon?.encode(encoder: enc, views: views, projections: projections, layered: amplify > 0, viewIndex: viewIndex)
            }
        }
        enc.setCullMode(.none)
        let vp0 = MTLViewport(originX: 0, originY: 0, width: Double(color.width), height: Double(color.height), znear: 0, zfar: 1)
        if amplify > 0 {
            var vps = viewportOverride ?? [MTLViewport](repeating: vp0, count: amplify)
            while vps.count < amplify { vps.append(vps.last ?? vp0) }
            enc.setViewports(vps)
            var maps = (0..<amplify).map {
                MTLVertexAmplificationViewMapping(viewportArrayIndexOffset: UInt32($0), renderTargetArrayIndexOffset: UInt32($0))
            }
            enc.setVertexAmplificationCount(amplify, viewMappings: &maps)
        } else {
            enc.setViewport(viewportOverride.flatMap { viewIndex < $0.count ? $0[viewIndex] : nil } ?? vp0)
        }
        guard haveScene, has3D, !seg.isEmpty, let buf = vtxSlots[curSlot].buf else { return }
        var u = uniforms
        enc.setVertexBuffer(buf, offset: 0, index: Int(VR_BUF_VERTS))
        enc.setVertexBuffer(buf, offset: vtxSlots[curSlot].stOff, index: Int(VR_BUF_STATES))
        enc.setVertexBytes(&u, length: MemoryLayout<VrSceneUniforms>.stride, index: Int(VR_BUF_UNI))
        enc.setFragmentTexture(vram, index: 0)
        let stride = MemoryLayout<VrGpuVtx>.stride
        func key(_ pass: UInt32, _ mode: Int) -> PKey {
            PKey(hud: false, layered: amplify > 0, filter: textureFilter, linear: linearOutput, pass: pass, mode: mode,
                 color: color.pixelFormat, depth: depth.pixelFormat, samples: samples)
        }
        func draw(_ bt: DrawBatch, _ k: PKey) {
            guard let p = pipeline(k) else { return }
            enc.setRenderPipelineState(p)
            enc.setVertexBufferOffset(bt.first * stride, index: Int(VR_BUF_VERTS))
            enc.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: bt.count)
        }
        // Orden del ensamblador: por época, opacos y luego semitransparentes (se respeta tal cual).
        var curKind = -1
        for i in seg {
            let bt = batches[i]
            if bt.kind != curKind {
                curKind = bt.kind
                if curKind == 0 {
                    enc.setDepthStencilState(dsOpaque)
                    enc.setDepthBias(0, slopeScale: 0, clamp: 0)
                } else {
                    enc.setDepthStencilState(dsSemi)
                    enc.setDepthBias(semiDepthBias, slopeScale: semiSlopeBias, clamp: 0)   // Z invertida: + = hacia la cámara
                }
            }
            if bt.kind == 0 {
                draw(bt, key(0, -1))
            } else {
                let a: Float = bt.mode == 3 ? 0.25 : 0.5
                enc.setBlendColor(red: a, green: a, blue: a, alpha: a)
                draw(bt, key(bt.hasTex ? 1 : 0, bt.mode))
                if bt.hasTex { draw(bt, key(2, -1)) }
            }
        }
    }
}
