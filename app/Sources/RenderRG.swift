import Metal
import simd
import Foundation

// Renderer Metal de la escena VR. SIN COMPILAR (escrito sin toolchain Apple): revisar en la primera compilación.
// Consume la API C de vr_scene.h (mv_host_scene) y dibuja: (1) la VRAM de la consola mantenida en GPU, (2) el panel 2D
// (HUD) en hudTexture y (3) el mundo 3D en estéreo.
//
// EL 3D SALE SOLO DEL RENDER NATIVO (Native/NativeRenderer.swift, escena leída de la RAM del motor): el mundo, sus efectos y los efectos de pantalla completa (agua, fundidos,
// destello, modos de visión, anillo de gas, barras). La lista de la GPU NUNCA se dibuja en 3D: solo aporta los eventos de VRAM, la capa 2D (HUD, menús, radio, códec, textos),
// el radar y el mapa de coherencia (solo registro). Si el nativo no está listo en un fotograma se REPITE el último fotograma nativo válido (con la cabeza actual) o se funde
// suavemente a negro (vr/native/ns_present.h). Las cinemáticas del motor se pintan con el mismo render nativo desde la cámara del juego, en estéreo, en la textura de la pantalla 3D.
//
// Convenciones (contrato del bucle inmersivo): Z invertida (cerca = 1): profundidad limpiada a 0 y compare >=;
// views = mundo->ojo, projections = ojo->clip (clip = P * V * mundo); mundo en metros, Y arriba, -Z delante.
// layered: 1 textura de array (2 vistas) con amplificación de vértices; si no, una textura por ojo y un pase por ojo.
// MSAA: color/profundidad multimuestra propios; el color se resuelve en resolveTargets; la profundidad se guarda (.store).

final class RGRenderer {
    // MARK: API pública
    let device: MTLDevice
    /// Panel 2D: RGBA8 NO premultiplicado (gamma, valores del original). Se redibuja solo cuando llega una escena nueva.
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

    /// false: no se dibujan los efectos de pantalla del render nativo (agua, fundidos, destello, modos de visión, anillo, barras): el mundo va tal cual.
    var screenFilterEnabled = true

    // MARK: diagnóstico del hueco (submenú «Diagnóstico»; XRDiag). Lo fija XRFrameEncoder cada fotograma.
    /// Sin prueba de profundidad en el mundo (opacos: siempre pasan y escriben; semitransparentes: siempre pasan, sin escribir).
    var diagNoZ = false
    /// Umbral (m) para partir en CPU los triángulos con una arista más larga en el render nativo. 0 = no partir.
    var diagSplitMeters: Float = 0
    private var diagView0 = matrix_identity_float4x4          // vista (mundo VR -> ojo) del primer ojo de la escena del último fotograma

    /// Presentación (XRPresent.swift). false = pantalla de cine: se siguen aplicando los eventos de VRAM (la VRAM debe quedar al día) pero no se dibuja
    /// el mundo 3D ni el panel 2D; el color y la profundidad se limpian igual.
    var drawWorld = true
    /// false = no se dibujan el radar de muñeca ni el arma en la mano (diorama: las vistas llevan otra transformación y el cuerpo no tiene sentido).
    var drawBody = true
    /// false = el pase de los ojos limpia color y profundidad pero no dibuja el mundo 3D (pantalla 3D: la escena se dibuja en las texturas
    /// estéreo de `stereoRequest`, no en los ojos). El panel 2D del HUD sí se sigue dibujando (`drawWorld`).
    var drawEyes = true
    /// Cámara del juego de la última escena -> mundo VR (VrScene.game_view, columnas; vista del juego en unidades del motor -> metros).
    private(set) var gameView = matrix_identity_float4x4
    /// Cinemática del motor (VrScene.cine_*): la escena es de una cinemática con 3D, nº de plano (sube en cada corte), si es el primer fotograma de un plano
    /// y cuántos fundidos/destellos a pantalla completa trae.
    private(set) var sceneCine = false
    private(set) var sceneShot: UInt32 = 0
    private(set) var sceneCut = false
    private(set) var sceneCover = 0
    /// La última escena traía geometría 3D, o el render nativo dibujó el mundo en el último fotograma.
    var sceneHas3D: Bool { has3D || nativeDrawnLast }

    // MARK: estado
    private let colorFormat: MTLPixelFormat
    private let depthFormat: MTLPixelFormat
    /// Muestras del pase de escena. Lo fija XRFrameEncoder cada fotograma (el interruptor de diagnóstico «MSAA 1x» lo baja a 1 sin reiniciar).
    var sampleCount: Int
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

    private struct PKey: Hashable {
        var hud: Bool, layered: Bool, filter: Bool, linear: Bool
        var pass: UInt32, mode: Int          // mode -1 = sin mezcla
        var color: MTLPixelFormat, depth: MTLPixelFormat, samples: Int
    }
    private var pipes: [PKey: MTLRenderPipelineState] = [:]
    private var pipeFull: MTLRenderPipelineState?
    private var failed = Set<String>()

    // escena persistente (se repite hasta que llegue otra)
    /// Lote de la lista de la GPU. Solo los de HUD (kind 2) se dibujan; los de 3D (kind 0/1) se conservan únicamente por su `evEnd` (orden de los eventos de VRAM) y para el diagnóstico.
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
#if !MV_BENCH || MV_BENCH_NATIVE
    /// Renderer nativo (vr/native): lee la escena de la RAM del motor del puente. nil si no hay biblioteca Metal (entonces no se dibuja 3D: la lista de la GPU no se usa para eso).
    private(set) var native: NativeRenderer?
#endif
    private var nativeDraw = false          // este fotograma se dibuja el mundo con el render nativo (la escena actual o la última válida repetida)
    private var nativeDrawnLast = false     // el último fotograma dibujó el mundo nativo
#if !MV_BENCH || MV_BENCH_NATIVE
    private var nativeMap: NativeRenderer.GpuMap?       // mapa mundo -> VR y cámara que declara la lista de la GPU (nil en cinemáticas / mundo no fiable): SOLO para el registro de coherencia
#endif
    private var gpuEyeVR: SIMD3<Float>?                 // VrScene.eye_vr de la lista actual (registro)
    private var gpu3DTris = 0                           // triángulos 3D de la lista de la GPU de la última escena: NO se dibujan (registro)
    private var has3D = false
    private var haveScene = false
    private var hudRect = simd_float4(0, 0, 320, 240)
    private var lastIndex: UInt32?
    private var hudDone = false
    // Capa 2D fiel (vr/scene/vr_layer2d): la rasteriza la CPU con la aritmética de la GPU original y llega ya como RGBA8; sustituye al pase HUD del shader mientras exista.
    /// Fotograma completo (pantalla de cine / vídeo ampliado) a alta resolución; nil si la última escena no lo trae. Lo consume XRPresenter.hqFull.
    private(set) var hqFull: (bytes: [UInt8], w: Int, h: Int, seq: UInt32)?
    private var hudLegacy: MTLTexture?, hudLegacySRGB: MTLTexture?
    private var hudHQRing: [MTLTexture] = [], hudHQRingSRGB: [MTLTexture] = []
    private var hudHQIdx = 0, hudHQW = 0, hudHQH = 0, hudHQSeq: UInt32 = 0
    private var hudHQ = false
    private var statDraws = 0, statPasses = 0, statGroups = 0

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
        let lib0 = MVLib.library(device)
        library = lib0
        if lib0 == nil { print("[RGRenderer] sin biblioteca Metal por defecto: faltan vr_*.metal en el target") }
        if MemoryLayout<VrVtx>.stride != MemoryLayout<VrGpuVtx>.stride {
            print("[RGRenderer] VrVtx (\(MemoryLayout<VrVtx>.stride) B) != VrGpuVtx: layout incompatible")
        }

        let td = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .r16Uint, width: 1024, height: 512, mipmapped: false)
        td.usage = [.shaderRead, .shaderWrite]
        td.storageMode = .private
        vram = device.makeTexture(descriptor: td)!
        vram.label = "VRAM"

        let hd = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm, width: hudW, height: hudH, mipmapped: false)
        hd.usage = [.renderTarget, .shaderRead, .pixelFormatView]
        hd.storageMode = .private
        hudTexture = device.makeTexture(descriptor: hd)
        hudTexture?.label = "HUD"
        hudTextureSRGB = hudTexture?.makeTextureView(pixelFormat: .rgba8Unorm_srgb)
        hudLegacy = hudTexture; hudLegacySRGB = hudTextureSRGB
        let pd = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba16Unorm, width: hudW, height: hudH, mipmapped: false)
        pd.usage = [.renderTarget, .shaderRead]
        pd.storageMode = .private
        hudPre = device.makeTexture(descriptor: pd)
        hudPre?.label = "HUD premultiplicado (unorm: satura a 0..1 como el original)"

        if let lib = lib0 {
            func cp(_ n: String) -> MTLComputePipelineState? {
                guard let f = lib.makeFunction(name: n) else { print("[RGRenderer] falta \(n)"); return nil }
                return try? device.makeComputePipelineState(function: f)
            }
            csFill = cp("vr_k_fill"); csUpload = cp("vr_k_upload"); csCopy = cp("vr_k_copy")
        }
        radar = RadarPanel(device: device, library: library, colorFormat: colorFormat, depthFormat: depthFormat,
                           sampleCount: sampleCount, viewCount: self.viewCount, linearOutput: linearOutput)
        weapon = WeaponRenderer(device: device, colorFormat: colorFormat, depthFormat: depthFormat,
                                sampleCount: sampleCount, viewCount: self.viewCount)
#if !MV_BENCH || MV_BENCH_NATIVE
        if let lib = lib0, let n = NativeRenderer(device: device, library: lib, ringSize: 6) {
            n.bridge = mv_vrhost_native_bridge()
            // pipelines de los formatos conocidos (con y sin amplificación): nada se compila en el primer fotograma nativo
            var warmed = true
            for layered in [false, true] where layered == false || self.viewCount > 1 {
                if !n.prewarm(layered: layered, colorFormat: colorFormat, depthFormat: depthFormat, samples: sampleCount, linear: linearOutput) { warmed = false }
            }
            native = n
            mv_vrhost_native_renderer_ready(warmed ? 1 : 0)
        } else { print("[RGRenderer] renderer nativo no disponible: no se dibuja el mundo 3D (la lista de la GPU no se usa para eso)") }
#endif
    }

#if !MV_BENCH || MV_BENCH_NATIVE
    deinit { mv_vrhost_native_renderer_ready(0) }
#endif

    /// Resumen para el registro (1 vez por segundo): estado de la escena y trabajo del último `encode`.

    /// Resumen para el registro (1 vez por segundo): estado de la escena y trabajo del último `encode`.
    func diagnostics() -> String {
        "tiene3D=\(has3D) hayEscena=\(haveScene) lotes=\(batches.count) (3D de la lista, NO se dibujan=\(batches.filter { $0.kind != 2 }.count) 2D=\(batches.filter { $0.kind == 2 }.count)) pases=\(statPasses) dibujos=\(statDraws) drawWorld=\(drawWorld) drawBody=\(drawBody) msaa=\(sampleCount) lineal=\(linearOutput) radar=\(radar != nil)" + nativeLog()
    }

    /// Línea de diagnóstico del render nativo (1 vez por segundo, justo tras xr-diag): qué se presentó (actual / último válido repetido / negro), triángulos de la lista de la GPU
    /// que NO se dibujan (siempre), cámaras y coherencia con la lista (solo registro). nil si no hay renderer nativo.
    func nativeDiagLine() -> String? {
#if MV_BENCH && !MV_BENCH_NATIVE
        return nil
#else
        guard let n = native else { return nil }
        return "nat-diag: 3D de la lista de la GPU (no se dibuja jamas): tris=\(gpu3DTris) | nativo dibujado=\(nativeDraw) | " + n.logLine() + " | " + n.drawnSummary(gpuEye: gpuEyeVR)
#endif
    }

    /// Línea de diagnóstico del hueco (1 vez por segundo): triángulos con arista > bigEdge m y que cruzan el plano cercano en el render nativo (recorre las mallas subidas).
    func diagLine(near: Float, bigEdge: Float) -> String {
        var t = String(format: "arista>%.0fm y cruce del plano cercano (%.3f m): ", bigEdge, near)
#if MV_BENCH && !MV_BENCH_NATIVE
        t += "nativa: no compilada"
#else
        if let n = native { t += "nativa: " + n.diagScan(view: diagView0, near: near, big: bigEdge) } else { t += "nativa: no disponible" }
#endif
        return t
    }

    private func nativeLog() -> String {
#if MV_BENCH && !MV_BENCH_NATIVE
        return ""
#else
        guard let n = native else { return " | nativo: renderer=no (sin 3D)" }
        return " | listo=\(mv_vrhost_native_available() != 0) dibuja=\(nativeDraw) " + n.logLine()
#endif
    }


    // MARK: pipelines
    private func pipeline(_ k: PKey) -> MTLRenderPipelineState? {
        if let p = pipes[k] { return p }
        guard k.hud, let lib = library else { return nil }   // solo el HUD de la lista de la GPU: el 3D es del render nativo
        do {
            let fc = MTLFunctionConstantValues()
            var f = k.filter, l = k.linear, pass = k.pass
            fc.setConstantValue(&f, type: .bool, index: Int(VR_FC_FILTER))
            fc.setConstantValue(&l, type: .bool, index: Int(VR_FC_LINEAR_OUT))
            fc.setConstantValue(&pass, type: .uint, index: Int(VR_FC_PASS))
            let fs = try lib.makeFunction(name: "vr_fs_scene", constantValues: fc)
            let vsName = "vr_vs_hud"
            guard let vs = lib.makeFunction(name: vsName) else { return nil }
            let d = MTLRenderPipelineDescriptor()
            d.label = "\(vsName) pass\(k.pass) blend\(k.mode)"
            d.vertexFunction = vs
            d.fragmentFunction = fs
            d.rasterSampleCount = k.samples
            d.inputPrimitiveTopology = .triangle
            let ca = d.colorAttachments[0]!
            ca.pixelFormat = k.color
            if k.mode >= 0 {
                ca.isBlendingEnabled = true
                // Los 4 modos del original (B = destino, F = fragmento). blendColor = 0,5 (modo 0) o 0,25 (modo 3).
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
            if !failed.contains(key) { failed.insert(key); print("[RGRenderer] pipeline: \(error)") }
            return nil
        }
    }

    private func fullPipeline() -> MTLRenderPipelineState? {
        if let p = pipeFull { return p }
        guard let lib = library, let vs = lib.makeFunction(name: "vr_vs_full"),
              let fs = lib.makeFunction(name: "vr_fs_unpremul") else { return nil }
        let d = MTLRenderPipelineDescriptor()
        d.label = "HUD sin premultiplicar"
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
            nb.label = "subidas VRAM"
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

    private func takeLayer2D() {
#if MV_BENCH
        return
#else
        var px: UnsafePointer<UInt8>?
        var w: Int32 = 0, h: Int32 = 0, full: Int32 = 0, seq: UInt32 = 0
        let r = mv_vrhost_hud2d_acquire(&px, &w, &h, &full, &seq)
        defer { if r >= 0 { mv_vrhost_hud2d_release() } }
        if r < 0 || px == nil || w <= 0 || h <= 0 {
            if hudHQ { hudHQ = false; hudDone = false; hudTexture = hudLegacy; hudTextureSRGB = hudLegacySRGB }
            hqFull = nil
            return
        }
        let W = Int(w), H = Int(h)
        if full != 0 {
            if r == 1 || hqFull == nil { hqFull = (Array(UnsafeBufferPointer(start: px!, count: W * H * 4)), W, H, seq) }
            if hudHQ { hudHQ = false; hudDone = false; hudTexture = hudLegacy; hudTextureSRGB = hudLegacySRGB }
            return
        }
        hqFull = nil
        if hudHQRing.isEmpty || hudHQW != W || hudHQH != H {
            let td = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm, width: W, height: H, mipmapped: false)
            td.usage = [.shaderRead, .pixelFormatView]
            td.storageMode = .shared
            var a: [MTLTexture] = [], b: [MTLTexture] = []
            for i in 0..<3 {
                guard let t = device.makeTexture(descriptor: td), let v = t.makeTextureView(pixelFormat: .rgba8Unorm_srgb) else { return }
                t.label = "HUD 2D fiel \(i)"; a.append(t); b.append(v)
            }
            hudHQRing = a; hudHQRingSRGB = b; hudHQW = W; hudHQH = H; hudHQSeq = 0
        }
        if r == 1 || !hudHQ || hudHQSeq != seq {
            hudHQIdx = (hudHQIdx + 1) % hudHQRing.count
            hudHQRing[hudHQIdx].replace(region: MTLRegionMake2D(0, 0, W, H), mipmapLevel: 0, withBytes: px!, bytesPerRow: W * 4)
            hudHQSeq = seq
        }
        hudTexture = hudHQRing[hudHQIdx]; hudTextureSRGB = hudHQRingSRGB[hudHQIdx]
        hudHQ = true
#endif
    }


    /// Guarda la escena de la lista de la GPU: vértices y estados para el panel 2D (HUD), los lotes con su `ev_end` (orden de los eventos de VRAM), la cámara del juego y los datos de
    /// cinemática. Los lotes de 3D (kind 0/1) NO se dibujan: el mundo sale del render nativo.
    private func storeScene(_ sc: VrScene, _ newSlot: inout Bool) {
        let nv = Int(sc.nv)
        if !newSlot { curSlot = (curSlot + 1) % 3; newSlot = true }
        let slot = vtxSlots[curSlot]
        let stOff = (nv * MemoryLayout<VrGpuVtx>.stride + 15) & ~15
        let ns = Int(sc.nstates)
        let bytes = max(stOff + max(ns, 1) * MemoryLayout<VrGpuState>.stride, 32)
        if bytes > slot.cap {
            let cap = max(bytes, slot.cap * 2, 1 << 20)
            slot.buf = device.makeBuffer(length: cap, options: [.storageModeShared, .cpuCacheModeWriteCombined])
            slot.buf?.label = "vértices VR"
            slot.cap = slot.buf == nil ? 0 : cap
        }
        batches.removeAll(keepingCapacity: true)
        has3D = sc.has_3d != 0
        gpuEyeVR = SIMD3<Float>(sc.eye_vr.0, sc.eye_vr.1, sc.eye_vr.2)
#if !MV_BENCH || MV_BENCH_NATIVE
        nativeMap = nil
        if sc.user_space != 0 && sc.world_valid != 0 && sc.has_3d != 0 && sc.scale > 0 && sc.user_k > 0 {
            func flat<T>(_ t: T) -> [Float] { withUnsafeBytes(of: t) { Array($0.bindMemory(to: Float.self)) } }
            let k = sc.user_k * sc.scale
            nativeMap = NativeRenderer.GpuMap(userR: flat(sc.user_R), k: k, eyeH: sc.user_eye_h, pRef: flat(sc.user_p_ref), origin: flat(sc.origin_game),
                                              gameView: flat(sc.game_view), eyeVR: flat(sc.eye_vr), upm: 1 / k)
        }
#endif
        gpu3DTris = 0
        haveScene = true
        sceneCine = sc.cine_active != 0
        sceneShot = sc.cine_shot
        sceneCut = sc.cine_cut != 0
        sceneCover = Int(sc.cine_cover)
        let gv: [Float] = withUnsafeBytes(of: sc.game_view) { Array($0.bindMemory(to: Float.self)) }
        if gv.count >= 16 && gv.allSatisfy({ $0.isFinite }) {
            gameView = simd_float4x4(columns: (SIMD4<Float>(gv[0], gv[1], gv[2], gv[3]), SIMD4<Float>(gv[4], gv[5], gv[6], gv[7]),
                                               SIMD4<Float>(gv[8], gv[9], gv[10], gv[11]), SIMD4<Float>(gv[12], gv[13], gv[14], gv[15])))
        }
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
            if kind == 3 { continue }   // filtro de pantalla de la lista: ya no se usa (los efectos de pantalla salen del render nativo)
            var hasTex = false
            if kind == 2 {
                for j in first..<(first + cnt) where (v[j].flags & texBit) != 0 { hasTex = true; break }
            }
            let semi = kind == 1 || (kind == 2 && bt.blend <= 3)   // blend 0xFF = opaco
            batches.append(DrawBatch(first: first, count: cnt, kind: kind, mode: Int(bt.blend) & 3, semi: semi, hasTex: hasTex, evEnd: Int(bt.ev_end)))
            if kind != 2 { gpu3DTris += cnt / 3 }   // registro: 3D de la lista que NO se dibuja
        }
    }

    /// Fluidez: vistas del pase de la escena, calculadas tras tomar las escenas nuevas (ver XRFrameEncoder). nil = las recibidas.
    var viewsHook: (() -> [simd_float4x4])?

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
        statDraws = 0; statPasses = 0

        // 1) escenas nuevas: eventos de VRAM + capa 2D. Si llegan varias listas, los eventos de las anteriores se
        //    aplican enteros al principio y solo se usa la última.
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
        let sceneV = viewsHook?() ?? views
        if let v0 = sceneV.first { diagView0 = v0 }

        // 2) grupos de lotes con el mismo ev_end: antes de cada grupo se aplican los eventos pendientes de VRAM (el orden de los eventos respecto a los lotes de 2D se conserva).
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
        takeLayer2D()
        let doHud = (gotNew || !hudDone) && drawWorld && !hudHQ
        // Render nativo: se prepara SIEMPRE (reconoce épocas del almacén aunque no se dibuje: menús) y lo que no se pueda dibujar se REPITE (último válido) o se funde a negro.
        prepareNative(commandBuffer: commandBuffer, colorTargets: colorTargets, depthTargets: depthTargets, sceneV: sceneV, projections: projections, head: head, layered: layered)
        var nextCmd = 0
        for (gi, g) in groups.enumerated() {
            let firstG = gi == 0, lastG = gi == groups.count - 1
            var pend: [VramCmd] = []
            while nextCmd < listCmds.count && (listCmds[nextCmd].op < g.2) { pend.append(listCmds[nextCmd].cmd); nextCmd += 1 }
            encodeVram(commandBuffer, pend, fs)
            // Segunda vista (mira / misil): una sola vez, con la VRAM tal como queda tras los eventos del último grupo, ANTES del pase del ojo que la muestrea.
            if lastG, let a = auxRequest, drawWorld, drawBody { encodeAux(commandBuffer, a) }
            // Pantalla 3D (cinemáticas del motor): el render nativo desde la cámara del juego, en estéreo, en las texturas de la pantalla. Se repite solo si cambió la escena,
            // la petición o el fundido de la presentación (la cabeza no interviene: la imagen es la misma hasta entonces).
            if lastG, let sr = stereoRequest, drawWorld { encodeStereo(commandBuffer, sr, gotNew: gotNew) }
            if doHud { encodeHud(commandBuffer, range: g.0..<g.1, first: firstG) }
            // El mundo de los ojos: UN pase tras el último grupo (la VRAM ya tiene todos los eventos de la lista), con las vistas por ojo.
            if lastG {
                encodeScenePasses(commandBuffer, colorTargets: colorTargets, depthTargets: depthTargets, resolveTargets: resolveTargets,
                                  views: sceneV, projections: projections, head: head, layered: layered)
            }
        }
        // eventos restantes de la lista (tras el último lote) para dejar la VRAM en sincronía con el oráculo
        if nextCmd < listCmds.count { encodeVram(commandBuffer, listCmds[nextCmd...].map { $0.cmd }, fs) }
        radar?.renderTexture(commandBuffer, vram: vram)      // VRAM ya en su estado final de la lista
        if doHud { finishHud(commandBuffer); hudDone = true }
    }

    // MARK: render nativo del fotograma
    /// Prepara el render nativo de este fotograma y decide qué se enseña (ns_present): `nativeDraw` = se dibuja el mundo (la escena actual o la última válida repetida).
    /// Con pantalla 3D (cinemática) el renderer coloca la escena desde la cámara del juego (`cinemaGameView`) y la ordenación/los carteles usan esa cámara.
    private func prepareNative(commandBuffer: MTLCommandBuffer, colorTargets: [MTLTexture], depthTargets: [MTLTexture], sceneV: [simd_float4x4],
                               projections: [simd_float4x4], head: simd_float4x4, layered: Bool) {
        nativeDraw = false
#if !MV_BENCH || MV_BENCH_NATIVE
        guard let n = native else { nativeDrawnLast = false; return }
        n.tint = SIMD3<Float>(1, 1, 1)
        n.splitMeters = diagSplitMeters      // diagnóstico: partir triángulos grandes (cambiarlo vuelve a subir las mallas)
        n.noZTest = diagNoZ
        n.suppressRing = suppressFrame         // la abertura de la mira sustituye a la máscara de visión del juego
        n.screenEffectsEnabled = screenFilterEnabled
        let c0 = colorTargets.first, d0 = depthTargets.first
        let nTargets = layered ? viewCount : min(colorTargets.count, depthTargets.count)
        let viewsOK = nTargets >= 1 && sceneV.count >= nTargets && projections.count >= nTargets
            && sceneV.prefix(nTargets).allSatisfy { m in (0..<4).allSatisfy { c in (0..<4).allSatisfy { m[c][$0].isFinite } } }
        func eyeOf(_ v: simd_float4x4) -> SIMD3<Float> { let t = v.inverse.columns.3; return SIMD3<Float>(t.x, t.y, t.z) }
        let eyesWanted = drawWorld && drawEyes && c0 != nil && d0 != nil && viewsOK
        var stereoCams: [(view: simd_float4x4, tangents: SIMD4<Float>)] = []
        if drawWorld, let sr = stereoRequest, ensureStereo(sr.width, sr.height) {
            for e in 0..<2 {
                if let cam = stereoCamera(e, sr) { stereoCams.append((view: cam.view, tangents: cam.tangents)) }
            }
        }
        let stereoWanted = stereoCams.count == 2
        let auxWanted = drawWorld && drawBody && auxRequest != nil && haveScene
        let want = eyesWanted || stereoWanted || auxWanted
        n.headVR = SIMD3<Float>(head.columns.3.x, head.columns.3.y, head.columns.3.z)
        // Cámara del juego: cinemática del motor (view_kind ENGINE) o escena válida pero no apta para primera persona (sin TOVR_OK/PLAYER_OK: vehículos, ascensores, zonas especiales,
        // jugador sin enlazar tras un cambio de zona), en CUALQUIER modo de cinemática (pantalla 3D, diorama, dentro de la escena). Nunca negro por eso.
        n.cinemaGameView = ((engineView || n.sceneUnfit) && Self.viewOK(gameView)) ? gameView : nil
        var sortView = sceneV.first ?? matrix_identity_float4x4
        var eyeList: [SIMD3<Float>] = eyesWanted ? sceneV.prefix(min(2, nTargets)).map(eyeOf) : []
        if stereoWanted {
            sortView = stereoCams[0].view
            eyeList = stereoCams.map { eyeOf($0.view) }
        } else if !eyesWanted, let a = auxRequest, auxWanted {
            sortView = a.view
            eyeList = [SIMD3<Float>(a.pos.x, a.pos.y, a.pos.z)]
        }
        n.beginFrame(commandBuffer: commandBuffer, sortView: sortView, draw: want, eyes: eyeList)
        guard want else { nativeDrawnLast = false; return }
        // pipelines listas para el formato de CADA destino de este fotograma
        var ready = true
        if eyesWanted, let c = c0, let d = d0 {
            ready = n.canEncode(layered: layered, colorFormat: c.pixelFormat, depthFormat: d.pixelFormat, samples: c.sampleCount, linear: linearOutput)
            if ready && !layered {
                for i in 0..<nTargets where colorTargets[i].pixelFormat != c.pixelFormat || depthTargets[i].pixelFormat != d.pixelFormat || colorTargets[i].sampleCount != c.sampleCount {
                    ready = ready && n.canEncode(layered: false, colorFormat: colorTargets[i].pixelFormat, depthFormat: depthTargets[i].pixelFormat,
                                                 samples: colorTargets[i].sampleCount, linear: linearOutput)
                }
            }
        }
        if ready && stereoWanted {
            ready = n.canEncode(layered: false, colorFormat: colorFormat, depthFormat: depthFormat, samples: sampleCount, linear: linearOutput)
        }
        if ready && auxWanted {
            ready = n.canEncode(layered: false, colorFormat: colorFormat, depthFormat: depthFormat, samples: 1, linear: linearOutput)
        }
        let out = n.present(ready: ready, gpuMap: nativeMap)
        nativeDraw = ready && out.mode != Int32(NS_PR_BLACK)
        nativeDrawnLast = nativeDraw
#endif
    }

    // MARK: segunda vista de la escena (mira óptica del francotirador, cámara del misil guiado)
    /// Petición de la segunda vista de este fotograma: matrices en el espacio recentrado (el de la cabeza), tamaño de la textura y posición de la cámara.
    struct AuxView {
        var view: simd_float4x4
        var proj: simd_float4x4
        var width: Int
        var height: Int
        var pos: SIMD4<Float>
    }
    var auxRequest: AuxView?
    /// Vista especial vigente (la fija el bucle de render antes de encode); el arma la dibuja con auxImage / el HUD.
    var special: MvVrSpecialView?
    /// No aplicar la máscara de visión del juego (la abertura de la mira la dibuja la lente).
    var suppressFrame = false
    private var auxColor: MTLTexture?
    private var auxDepth: MTLTexture?
    private var auxRaw: MTLTexture?        // vista de auxColor sin decodificación sRGB: el shader del arma convierte a lineal una sola vez
    private var auxKey = ""
    /// true si los valores de auxRaw ya son lineales (destino de coma flotante): el shader los lleva a gamma antes de mezclar.
    var auxStoredLinear: Bool { colorFormat == .rgba16Float }
    /// Imagen de la segunda vista para el shader de las vistas especiales (nil si no hay).
    var auxImage: MTLTexture? { auxRaw }

    private func ensureAux(_ w: Int, _ h: Int) -> Bool {
        let key = "\(w)x\(h)/\(colorFormat.rawValue)/\(depthFormat.rawValue)"
        if key == auxKey, auxColor != nil, auxDepth != nil { return true }
        guard w >= 16, h >= 16, w <= 2048, h <= 2048 else { return false }
        let cd = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: colorFormat, width: w, height: h, mipmapped: false)
        cd.usage = [.renderTarget, .shaderRead, .pixelFormatView]
        cd.storageMode = .private
        let dd = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: depthFormat, width: w, height: h, mipmapped: false)
        dd.usage = .renderTarget
        dd.storageMode = .private
        guard let c = device.makeTexture(descriptor: cd), let d = device.makeTexture(descriptor: dd) else { return false }
        c.label = "VR segunda vista (color)"; d.label = "VR segunda vista (profundidad)"
        var raw: MTLTexture? = c
        switch colorFormat {
        case .bgra8Unorm_srgb: raw = c.makeTextureView(pixelFormat: .bgra8Unorm)
        case .rgba8Unorm_srgb: raw = c.makeTextureView(pixelFormat: .rgba8Unorm)
        default: break
        }
        auxColor = c; auxDepth = d; auxRaw = raw ?? c; auxKey = key
        return true
    }

    // MARK: pantalla 3D estereoscópica (cinemáticas del motor)
    /// Petición de la pantalla 3D de este fotograma: parámetros de las cámaras estéreo (XRStereo) y tamaño de las texturas. Las vistas se calculan en `encodeStereo` con la
    /// cámara del juego de la escena que se va a dibujar (la recién llegada; si se calcularan antes, cada escena se dibujaría con la cámara de la anterior: un fotograma de
    /// desfase y un fotograma erróneo en cada corte de plano). No dependen de la cabeza.
    struct StereoRequest: Equatable {
        var ipd: Float
        var convergence: Float
        var tanX: Float
        var tanY: Float
        var width: Int
        var height: Int
    }
    var stereoRequest: StereoRequest? {
        didSet { if stereoRequest != oldValue { stereoDirty = true } }
    }
    private var stereoDirty = true
    private var stereoSeq: UInt32 = 0
    private var stereoFade: Float = 1
    private(set) var stereoRendered = false
    private var stereoTex: MTLTexture?          // array de 2 capas (ojo izquierdo, derecho) en el formato de color del renderer
    private var stereoRaw: MTLTexture?          // la misma sin decodificación sRGB (el shader aplica tinte y recubrimientos en gamma)
    private var stereoSlices: [MTLTexture] = []
    private var stereoMSAAColor: MTLTexture?
    private var stereoDepth: MTLTexture?
    private var stereoKey = ""
    /// Imagen estéreo (array de 2 capas, valores tal como se guardaron: gamma en los formatos sRGB) o nil si aún no se dibujó.
    var stereoImage: MTLTexture? { stereoRendered ? stereoRaw : nil }
    /// Capas de la textura de la pantalla 3D ya reservada (aunque aún no se haya dibujado nada en ella; 0 = sin reservar). El banco comprueba que es un array de 2.
    var stereoAllocatedLength: Int { stereoTex?.arrayLength ?? 0 }
    /// Los valores de `stereoImage` ya son lineales (destino de coma flotante).
    var stereoStoredLinear: Bool { colorFormat == .rgba16Float }
    /// Los efectos de pantalla de la cinemática (fundidos, destello...) los dibuja el render nativo dentro de la propia pantalla 3D (recubrimientos del ojo): aquí no hay tinte ni recubrimientos.
    var screenFx: (tint: SIMD3<Float>, covers: [SIMD4<Float>]) { (SIMD3<Float>(1, 1, 1), []) }

    func prepareStereo(width: Int, height: Int) { _ = ensureStereo(width, height) }

    private func ensureStereo(_ w: Int, _ h: Int) -> Bool {
        let key = "\(w)x\(h)/\(colorFormat.rawValue)/\(depthFormat.rawValue)/\(sampleCount)"
        if key == stereoKey, stereoTex != nil, stereoDepth != nil { return true }
        guard w >= 64, h >= 64, w <= 4096, h <= 4096 else { return false }
        stereoRendered = false
        let cd = MTLTextureDescriptor()
        cd.textureType = .type2DArray
        cd.pixelFormat = colorFormat
        cd.width = w; cd.height = h; cd.arrayLength = 2
        cd.usage = [.renderTarget, .shaderRead, .pixelFormatView]
        cd.storageMode = .private
        guard let t = device.makeTexture(descriptor: cd) else { return false }
        t.label = "VR pantalla 3D (ojos)"
        var slices: [MTLTexture] = []
        for e in 0..<2 {
            guard let v = t.makeTextureView(pixelFormat: colorFormat, textureType: .type2D, levels: 0..<1, slices: e..<(e + 1)) else { return false }
            v.label = "VR pantalla 3D (ojo \(e))"
            slices.append(v)
        }
        var raw: MTLTexture? = t
        switch colorFormat {
        case .bgra8Unorm_srgb: raw = t.makeTextureView(pixelFormat: .bgra8Unorm)
        case .rgba8Unorm_srgb: raw = t.makeTextureView(pixelFormat: .rgba8Unorm)
        default: break
        }
        let dd = MTLTextureDescriptor()
        dd.pixelFormat = depthFormat
        dd.width = w; dd.height = h
        dd.usage = .renderTarget
        dd.storageMode = .private
        var msaa: MTLTexture?
        if sampleCount > 1 {
            cd.textureType = .type2DMultisample
            cd.arrayLength = 1
            cd.sampleCount = sampleCount
            cd.usage = .renderTarget
            msaa = device.makeTexture(descriptor: cd)
            msaa?.label = "VR pantalla 3D (color MSAA)"
            dd.textureType = .type2DMultisample
            dd.sampleCount = sampleCount
            guard msaa != nil else { return false }
        } else {
            dd.textureType = .type2D
        }
        guard let d = device.makeTexture(descriptor: dd) else { return false }
        d.label = "VR pantalla 3D (profundidad)"
        stereoTex = t; stereoRaw = raw ?? t; stereoSlices = slices; stereoMSAAColor = msaa; stereoDepth = d; stereoKey = key
        stereoDirty = true
        return true
    }


    /// La escena es una cinemática del motor (VRV_K_ENGINE, view_kind 2): la fija XRFrameEncoder antes de `encode`. Fuerza la colocación desde la cámara del juego.
    var engineView = false
    /// La escena nativa actual pide la pantalla 3D porque es válida pero no apta para primera persona (con histéresis): XRPresenter la muestra en estéreo hasta que vuelva a ser apta.
    var sceneNeedsGameCam: Bool {
#if !MV_BENCH || MV_BENCH_NATIVE
        return native?.sceneUnfit ?? false
#else
        return false
#endif
    }
    private static func viewOK(_ m: simd_float4x4) -> Bool {
        guard XRMath.isFinite(m) else { return false }
        let det = simd_determinant(m)
        return det.isFinite && abs(det) > 1e-13
    }
    /// Cámara de un ojo de la pantalla 3D: ventana del motor con la H REAL de la escena (tangentes 160/H y 120/H) y su centro de proyección OFX/OFY (ns_cinema_window); sin escena nativa,
    /// las tangentes de la petición. Réplica de XRStereo.eye + desplazamiento del centro.
    private func stereoCamera(_ e: Int, _ r: StereoRequest) -> (view: simd_float4x4, tangents: SIMD4<Float>)? {
        var tx = r.tanX, ty = r.tanY
        var sh = SIMD4<Float>(0, 0, 0, 0)
#if !MV_BENCH || MV_BENCH_NATIVE
        if let n = native, n.camH > 8 {
            tx = 160 / n.camH; ty = 120 / n.camH
            let dx = (n.camOfx - 160) / n.camH, dy = (n.camOfy - 120) / n.camH
            sh = SIMD4<Float>(dx, -dx, dy, -dy)
        }
#endif
        guard let cam = XRStereo.eye(gameView: gameView, eye: e, ipd: r.ipd, conv: r.convergence, tanX: tx, tanY: ty) else { return nil }
        let t = cam.tangents + sh
        guard t.x > 0.01, t.y > 0.01, t.z > 0.01, t.w > 0.01 else { return nil }
        return (cam.view, t)
    }

    /// Pantalla 3D: el render nativo desde la cámara del juego (A, t, H, OFX/OFY, sobredimensión 66/64: ns_cinema), un pase por ojo con cámaras paralelas y ventana desplazada. Se vuelve
    /// a dibujar solo si cambió la escena nativa, la petición o el fundido de la presentación; si no, las texturas valen. Con el render nativo no listo no se toca la imagen anterior.
    private func encodeStereo(_ cb: MTLCommandBuffer, _ r: StereoRequest, gotNew: Bool) {
        guard nativeDraw, ensureStereo(r.width, r.height), let d = stereoDepth, stereoSlices.count == 2 else { return }
#if !MV_BENCH || MV_BENCH_NATIVE
        guard let n = native else { return }
        let fade = n.lastPresent.fade
        if !(gotNew || stereoDirty || !stereoRendered || n.sceneSeq != stereoSeq || fade != stereoFade) { return }
        var rendered = 0
        for e in 0..<2 {
            guard let cam = stereoCamera(e, r) else { return }
            var u = VrSceneUniforms()
            let proj = XRMath.projection(tangents: cam.tangents, near: 0.05, far: 800)
            let m = proj * cam.view
            u.view_proj = (m, m)
            u.head = SIMD4<Float>(0, 0, 0, 1)
            let t = cam.view.inverse.columns.3
            let color = stereoMSAAColor ?? stereoSlices[e]
            let cams = NativeCams(viewProj: [m], projections: [proj], eyes: [SIMD3<Float>(t.x, t.y, t.z)], eyeIndex: e)
            if encodeScene(cb, color: color, depth: d, resolve: stereoMSAAColor != nil ? stereoSlices[e] : nil, uniforms: u, amplify: 0, viewIndex: 0,
                           views: [], projections: [], isAux: true, last: true, cams: cams) { rendered += 1 }
        }
        guard rendered == 2 else { return }
        stereoSeq = n.sceneSeq
        stereoFade = fade
#endif
        stereoRendered = true
        stereoDirty = false
    }

    /// Segunda vista (mira óptica / cámara del misil): el render nativo desde la cámara pedida, a la textura auxiliar.
    private func encodeAux(_ cb: MTLCommandBuffer, _ a: AuxView) {
        guard nativeDraw, haveScene, ensureAux(a.width, a.height), let c = auxColor, let d = auxDepth else { return }
        var u = VrSceneUniforms()
        let m = a.proj * a.view
        u.view_proj = (m, m)
        u.head = a.pos
        let cams = NativeCams(viewProj: [m], projections: [a.proj], eyes: [SIMD3<Float>(a.pos.x, a.pos.y, a.pos.z)], eyeIndex: 0)
        _ = encodeScene(cb, color: c, depth: d, resolve: nil, uniforms: u, amplify: 0, viewIndex: 0, views: [], projections: [], isAux: true, last: false, cams: cams)
    }

    /// Cámaras con las que dibuja el render nativo un pase (nil = no se dibuja el mundo: solo limpieza, radar y arma).
    private struct NativeCams {
        var viewProj: [simd_float4x4]
        var projections: [simd_float4x4]
        var eyes: [SIMD3<Float>]
        var eyeIndex: Int
    }
    private var nativeEncodeFails = 0

    /// El mundo de los ojos: un pase por ojo (o uno con amplificación a 2 vistas), SOLO con el render nativo.
    private func encodeScenePasses(_ cb: MTLCommandBuffer, colorTargets: [MTLTexture], depthTargets: [MTLTexture], resolveTargets: [MTLTexture],
                                   views: [simd_float4x4], projections: [simd_float4x4], head: simd_float4x4, layered: Bool) {
        let n = min(views.count, projections.count)
        func eyePos(_ v: simd_float4x4) -> SIMD3<Float> { let t = v.inverse.columns.3; return SIMD3<Float>(t.x, t.y, t.z) }
        let draw = nativeDraw && drawEyes
        if layered {
            guard let c = colorTargets.first, let d = depthTargets.first, n >= 1 else { return }
            let cnt = min(n, viewCount)
            var u = VrSceneUniforms()
            u.view_proj = (projections[0] * views[0], projections[min(1, cnt - 1)] * views[min(1, cnt - 1)])
            u.head = head.columns.3
            let cams = draw ? NativeCams(viewProj: (0..<cnt).map { projections[$0] * views[$0] }, projections: Array(projections.prefix(cnt)),
                                         eyes: (0..<cnt).map { eyePos(views[$0]) }, eyeIndex: 0) : nil
            _ = encodeScene(cb, color: c, depth: d, resolve: resolveTargets.first, uniforms: u, amplify: cnt, views: views, projections: projections,
                            isAux: false, last: true, cams: cams)
        } else {
            for i in 0..<min(colorTargets.count, depthTargets.count) where i < n {
                var u = VrSceneUniforms()
                let m = projections[i] * views[i]
                u.view_proj = (m, m)
                u.head = head.columns.3
                let cams = draw ? NativeCams(viewProj: [m], projections: [projections[i]], eyes: [eyePos(views[i])], eyeIndex: i) : nil
                _ = encodeScene(cb, color: colorTargets[i], depth: depthTargets[i], resolve: i < resolveTargets.count ? resolveTargets[i] : nil, uniforms: u,
                                amplify: 0, viewIndex: i, views: views, projections: projections, isAux: false, last: true, cams: cams)
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
        enc.label = "HUD"
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
        enc.label = "HUD sin premultiplicar"
        if let fp = fullPipeline() {
            enc.setRenderPipelineState(fp)
            enc.setFragmentTexture(pre, index: 0)
            enc.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
        }
        enc.endEncoding()
    }

    // MARK: escena 3D
    /// Un pase de escena: limpia color y profundidad, dibuja el mundo con el render nativo (si hay `cams`), y encima el quad del radar, el arma y las vistas especiales. La lista de la GPU
    /// no se dibuja jamás en 3D. amplify > 0: un solo pase con amplificación de vértices a `amplify` capas del array; 0: un pase de un ojo.
    /// `last`: resuelve el color (MSAA); si no, lo guarda sin resolver y no dibuja radar ni arma (segunda vista). Devuelve true si el render nativo dibujó (false si no había que dibujar o falló).
    @discardableResult
    private func encodeScene(_ cb: MTLCommandBuffer, color: MTLTexture, depth: MTLTexture, resolve: MTLTexture?, uniforms: VrSceneUniforms, amplify: Int, viewIndex: Int = 0,
                             views: [simd_float4x4] = [], projections: [simd_float4x4] = [], isAux: Bool = false, last: Bool, cams: NativeCams?) -> Bool {
        let samples = color.sampleCount
        statPasses += 1
        let pd = MTLRenderPassDescriptor()
        let ca = pd.colorAttachments[0]!
        ca.texture = color
        ca.loadAction = .clear
        ca.clearColor = backgroundColor
        if samples > 1, last, let r = resolve {
            ca.resolveTexture = r
            ca.storeAction = .multisampleResolve
        } else {
            ca.storeAction = .store
        }
        let da = pd.depthAttachment!
        da.texture = depth
        da.loadAction = .clear
        da.clearDepth = 0            // Z invertida
        da.storeAction = .store      // la profundidad MSAA la resuelve el pase final del bucle
        if depth.pixelFormat == .depth32Float_stencil8 {
            pd.stencilAttachment.texture = depth
            pd.stencilAttachment.loadAction = .dontCare
            pd.stencilAttachment.storeAction = .dontCare
        }
        if amplify > 0 { pd.renderTargetArrayLength = amplify }
        if let m = rasterizationRateMap, !isAux { pd.rasterizationRateMap = m }
        guard let enc = cb.makeRenderCommandEncoder(descriptor: pd) else { return false }
        enc.label = isAux ? "escena (segunda vista / pantalla 3D)" : amplify > 0 ? "escena (estéreo, amplificación)" : "escena (ojo \(viewIndex))"
        defer { enc.endEncoding() }
        // Los defer corren al revés: este va ANTES de cerrar el codificador, tras dibujar el mundo. Solo en el pase que resuelve el MSAA: el quad del radar y el arma se prueban
        // contra la profundidad del mundo.
        defer {
            if last && drawBody {
                radar?.encodeQuad(enc, uniforms: uniforms, layered: amplify > 0, color: color.pixelFormat, depth: depth.pixelFormat, samples: samples)
                weapon?.sampleCount = samples      // las muestras del pase pueden cambiar en vivo (diagnóstico MSAA 1x)
                weapon?.encode(encoder: enc, views: views, projections: projections, layered: amplify > 0, viewIndex: viewIndex)
                if let sv = special {
                    weapon?.encodeSpecial(encoder: enc, view: sv, aux: auxImage, auxLinear: auxStoredLinear, hud: hudTexture, views: views,
                                          projections: projections, layered: amplify > 0, viewIndex: viewIndex)
                }
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
            enc.setViewport(isAux ? vp0 : (viewportOverride.flatMap { viewIndex < $0.count ? $0[viewIndex] : nil } ?? vp0))
        }
#if !MV_BENCH || MV_BENCH_NATIVE
        // El mundo: render nativo con las mismas vistas y proyecciones por ojo y la profundidad inversa del sistema (no se escribe depthRange). Si falla, el pase queda limpio y el
        // fotograma siguiente se repite (ns_present: último válido) o se funde a negro; se cuenta para el registro.
        if let cm = cams, let n = native {
            let ok = n.encode(encoder: enc, vram: vram, viewProj: cm.viewProj, projections: cm.projections, eyes: cm.eyes, eyeIndex: cm.eyeIndex, layered: amplify > 0,
                              colorFormat: color.pixelFormat, depthFormat: depth.pixelFormat, samples: samples, linear: linearOutput)
            if !ok { nativeEncodeFails += 1; nativeDrawnLast = false }
            return ok
        }
#endif
        return false
    }
}
