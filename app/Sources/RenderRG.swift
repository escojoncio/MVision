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
    /// Sesgo de profundidad de las semitransparentes (evita z-fighting con decales coplanares).
    var semiDepthBias: Float = 4
    var semiSlopeBias: Float = 2

    /// false: no se aplica el filtro de pantalla (la escena va directa al drawable, sin onda ni tinte).
    var screenFilterEnabled = true

    // MARK: diagnóstico del hueco (submenú «Diagnóstico»; XRDiag). Lo fija XRFrameEncoder cada fotograma.
    /// Sin prueba de profundidad en el mundo (opacos: siempre pasan y escriben; semitransparentes: siempre pasan, sin escribir).
    var diagNoZ = false
    /// Umbral (m) para partir en CPU los triángulos con una arista más larga, en las dos rutas. 0 = no partir.
    var diagSplitMeters: Float = 0
    /// Tope de triángulos de salida de los batches reescritos por el partido de la lista de la GPU (tope de seguridad).
    private static let splitTriCap: UInt32 = 600_000
    private var diagView0 = matrix_identity_float4x4          // vista (mundo VR -> ojo) del primer ojo de la escena del último fotograma
    private var scanWanted = false, scanNear: Float = 0.1, scanBig: Float = 12
    private var gpuScanText = "sin medir aun"
    private var gpuSplitText = "partido=no"

    /// Presentación (XRPresent.swift). false = pantalla de cine: se siguen aplicando los eventos de VRAM (la VRAM debe quedar al día) pero no se dibuja
    /// el mundo 3D ni el panel 2D; el color y la profundidad se limpian igual.
    var drawWorld = true
    /// false = no se dibujan el radar de muñeca ni el arma en la mano (diorama: las vistas llevan otra transformación y el cuerpo no tiene sentido).
    var drawBody = true
    /// false = el pase de los ojos limpia color y profundidad pero no dibuja la geometría 3D de la escena (pantalla 3D: la escena se dibuja en las texturas
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
    /// La última escena traía geometría 3D.
    var sceneHas3D: Bool { has3D }

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
    private var dsOpaque: MTLDepthStencilState?
    private var dsSemi: MTLDepthStencilState?
    private var dsOpaqueNoZ: MTLDepthStencilState?
    private var dsSemiNoZ: MTLDepthStencilState?

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
        // recubrimientos en orden de dibujo (xyz = color/255, w = modo de mezcla original o -1 = sin usar; los usados van primero)
        var covers = [SIMD4<Float>](repeating: SIMD4<Float>(0, 0, 0, -1), count: Int(VR_POST_COVERS))
        // líneas de exploración de las gafas (x = periodo en filas originales, y = fase, z = alto, w = modo o -1) y su color (w = ganancia)
        var scan = SIMD4<Float>(0, 0, 0, -1)
        var scanRGB = SIMD4<Float>(0, 0, 0, 1)
        var scanAfter: Float = 0                    // recubrimientos dibujados antes de las líneas
        // máscara de visión (máscara de gas, caja): tamaño de la imagen mostrada del motor y textura (anillo `frameTex`)
        var frameArea = SIMD2<Float>(0, 0)
        var frameOn = false
        var frameRGB = SIMD3<Float>(0, 0, 0)        // color que se suma donde la máscara cierra del todo
        var frameIdx = 0
        var cxCol: Float = 160
        var projH: Float = 160, cyRow: Float = 128
        var nrows = 0
        var dv = [Float](repeating: 0, count: Int(VR_POST_ROWS))   // desplazamiento por fila de la imagen original (0 donde no hay tira)
    }
    private struct PostPlan { var sceneColor: [MTLTexture], sceneResolve: [MTLTexture], fxTex: [MTLTexture], dst: [MTLTexture] }
    private var fx = FilterFx()
    /// Anillo de texturas R8 con la resta de la máscara de visión (VR_FRAME_W x VR_FRAME_H, una por escena distinta: la GPU puede seguir leyendo la anterior).
    private var frameTex: [MTLTexture] = []
    private var frameSum: UInt64 = 0
    private var frameTexNext = 0
    private var frameTexCur = -1
    /// Ganancia de las líneas de exploración de las gafas (1 = fiel al original, 0 = sin líneas).
    var scanlineGain: Float = 1
    private var fxTex: [MTLTexture] = []
    private var fxKey = ""
    private var postPipes: [String: MTLRenderPipelineState] = [:]
    private var pipeFull: MTLRenderPipelineState?
    private var failed = Set<String>()

    // escena persistente (se repite hasta que llegue otra)
    private struct DrawBatch { var first: Int, count: Int, kind: Int, mode: Int, semi: Bool, hasTex: Bool, evEnd: Int, skip: UInt32 }   // skip: VrBatch.skip_if_native
    private final class VtxSlot { var buf: MTLBuffer?; var cap = 0; var stOff = 0 }   // stOff: offset de VrGpuState[] tras los vértices
    // Invariante del anillo (revisado): `inflight` (3) cuenta comandos en vuelo INCLUIDO el que se codifica, así que al volver de
    // `inflight.wait()` hay como mucho 2 anteriores sin terminar (la cola Metal los completa en orden). `storeScene` avanza `curSlot`
    // como mucho UNA vez por encode, de modo que el slot que se escribe (curSlot+1) nunca es el de los dos fotogramas previos
    // (curSlot y curSlot-1 como mucho). Hace falta slots >= valor del semáforo: si se sube uno, subir el otro (memoria compartida
    // escrita por CPU: Metal no hace seguimiento de peligros ahí).
    private var vtxSlots = [VtxSlot(), VtxSlot(), VtxSlot()]
    private var curSlot = 2
    private var batches: [DrawBatch] = []
    /// VrScene.native_3d de la última lista: la escena admite el 3D nativo y sus batches 3D vienen marcados (skip_if_native).
    private var nativeSceneMarked = false
#if !MV_BENCH || MV_BENCH_NATIVE
    /// Renderer nativo (vr/native): lee la escena de la RAM del motor del puente. nil si no hay biblioteca Metal. La decisión nativo/GPU se toma en cada fotograma.
    private(set) var native: NativeRenderer?
#endif
    private var nativeUse = false          // decisión de este fotograma: se dibuja la escena nativa
    private var nativeSkip: UInt32 = 0     // batches marcados que NO se dibujan (NS_SKIP_*); 0 tras un fallo de este fotograma
    private var decidedSkip: UInt32 = 0    // la máscara decidida al principio del fotograma (para recuperar los grupos ya saltados si el dibujo nativo falla)
    private var nativeFailed = false       // el dibujo nativo falló en este fotograma: todo lo marcado vuelve a GPU
    private var nativeOnly = false         // modo de depuración «solo nativo»: los batches 3D marcados NO se dibujan nunca por GPU (aunque la decisión sea GPU o el nativo falle) y el nativo va teñido
    static let nativeOnlyTint = SIMD3<Float>(0.75, 1.0, 0.75)
#if !MV_BENCH || MV_BENCH_NATIVE
    private var nativeMap: NativeRenderer.GpuMap?       // mapa mundo -> VR y cámara que declara la lista de la GPU (nil en cinemáticas / mundo no fiable): para la red de coherencia
#endif
    private var gpuEyeVR: SIMD3<Float>?                 // VrScene.eye_vr de la lista actual
    // Diagnóstico nativo frente a GPU (ver nativeDiagLine): triángulos 3D de la lista (marcados = los que el nativo sustituye) y batches 3D saltados / dibujados en el último encode.
    private var gpuMarkedTris = 0, gpuOtherTris = 0
    private var gpuMarkedMin = SIMD3<Float>(repeating: .greatestFiniteMagnitude), gpuMarkedMax = SIMD3<Float>(repeating: -.greatestFiniteMagnitude)
    private var statSkipB = 0, statDrawB = 0, statSkipTris = 0, statDrawTris = 0
    private var has3D = false
    private var haveScene = false
    private var hudRect = simd_float4(0, 0, 320, 240)
    private var lastIndex: UInt32?
    private var hudDone = false
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
        let d1 = MTLDepthStencilDescriptor()
        d1.depthCompareFunction = .greaterEqual; d1.isDepthWriteEnabled = true
        dsOpaque = device.makeDepthStencilState(descriptor: d1)
        let d2 = MTLDepthStencilDescriptor()
        d2.depthCompareFunction = .greaterEqual; d2.isDepthWriteEnabled = false
        dsSemi = device.makeDepthStencilState(descriptor: d2)
        let d3 = MTLDepthStencilDescriptor()
        d3.depthCompareFunction = .always; d3.isDepthWriteEnabled = true
        dsOpaqueNoZ = device.makeDepthStencilState(descriptor: d3)
        let d4 = MTLDepthStencilDescriptor()
        d4.depthCompareFunction = .always; d4.isDepthWriteEnabled = false
        dsSemiNoZ = device.makeDepthStencilState(descriptor: d4)
        radar = RadarPanel(device: device, library: library, colorFormat: colorFormat, depthFormat: depthFormat,
                           sampleCount: sampleCount, viewCount: self.viewCount, linearOutput: linearOutput)
        weapon = WeaponRenderer(device: device, colorFormat: colorFormat, depthFormat: depthFormat,
                                sampleCount: sampleCount, viewCount: self.viewCount)
#if !MV_BENCH || MV_BENCH_NATIVE
        if let lib = lib0, let n = NativeRenderer(device: device, library: lib, ringSize: 3) {
            n.bridge = mv_vrhost_native_bridge()
            // pipelines de los formatos conocidos (con y sin amplificación): nada se compila en el primer fotograma nativo
            var warmed = true
            for layered in [false, true] where layered == false || self.viewCount > 1 {
                if !n.prewarm(layered: layered, colorFormat: colorFormat, depthFormat: depthFormat, samples: sampleCount, linear: linearOutput) { warmed = false }
            }
            native = n
            mv_vrhost_native_renderer_ready(warmed ? 1 : 0)
        } else { print("[RGRenderer] renderer nativo no disponible: se dibuja por la lista de la GPU") }
#endif
    }

#if !MV_BENCH || MV_BENCH_NATIVE
    deinit { mv_vrhost_native_renderer_ready(0) }
#endif

    /// Resumen para el registro (1 vez por segundo): estado de la escena y trabajo del último `encode`.
    func diagnostics() -> String {
        "tiene3D=\(has3D) hayEscena=\(haveScene) lotes=\(batches.count) (3D=\(batches.filter { $0.kind != 2 }.count) 2D=\(batches.filter { $0.kind == 2 }.count)) pases=\(statPasses) dibujos3D=\(statDraws) drawWorld=\(drawWorld) drawBody=\(drawBody) msaa=\(sampleCount) lineal=\(linearOutput) radar=\(radar != nil)" + nativeLog()
    }

    /// Línea de diagnóstico del render nativo frente a la lista de la GPU (1 vez por segundo, justo tras xr-diag): decisión, batches de la GPU saltados / dibujados, triángulos y caja VR de
    /// cada ruta, cámaras y coherencia. nil si no hay renderer nativo.
    func nativeDiagLine() -> String? {
#if MV_BENCH && !MV_BENCH_NATIVE
        return nil
#else
        guard let n = native else { return nil }
        let box = gpuMarkedTris > 0
            ? String(format: "[%.1f %.1f %.1f]..[%.1f %.1f %.1f]", gpuMarkedMin.x, gpuMarkedMin.y, gpuMarkedMin.z, gpuMarkedMax.x, gpuMarkedMax.y, gpuMarkedMax.z) : "vacia"
        return "nat-diag: modo=\(Self.modeName()) marcada=\(nativeSceneMarked) usa=\(nativeUse) saltar=\(nativeSkip) recuperado_por_fallo=\(nativeFailed) | " +
            "lista GPU: tris_marcados=\(gpuMarkedTris) tris_no_marcados=\(gpuOtherTris) caja_VR_marcados=\(box) batches_3D_saltados=\(statSkipB) (tris \(statSkipTris)) batches_3D_dibujados=\(statDrawB) (tris \(statDrawTris)) | " +
            "nativo: " + n.drawnSummary(gpuEye: gpuEyeVR)
#endif
    }

    /// Línea de diagnóstico del hueco (1 vez por segundo): triángulos con arista > bigEdge m y que cruzan el plano cercano en cada ruta. La lista de la GPU se mide al
    /// llegar la siguiente escena (con la vista del último fotograma; no se lee el búfer de la GPU, que es de escritura combinada); la nativa, ahora, recorriendo las mallas
    /// subidas (memoria compartida normal).
    func diagLine(near: Float, bigEdge: Float) -> String {
        scanWanted = true; scanNear = near; scanBig = bigEdge
        var t = String(format: "arista>%.0fm y cruce del plano cercano (%.3f m) por ruta: ", bigEdge, near)
        t += gpuScanText + " [" + gpuSplitText + "]"
#if MV_BENCH && !MV_BENCH_NATIVE
        t += " | nativa: no compilada"
#else
        if let n = native { t += " | nativa: " + n.diagScan(view: diagView0, near: near, big: bigEdge) } else { t += " | nativa: no disponible" }
#endif
        return t
    }

    private static func modeName() -> String {
#if MV_BENCH && !MV_BENCH_NATIVE
        return "gpu"
#else
        switch mv_vrhost_render_mode() { case 0: return "gpu"; case 2: return "solo-nativo"; default: return "nativo" }
#endif
    }

    private func nativeLog() -> String {
#if MV_BENCH && !MV_BENCH_NATIVE
        return ""
#else
        let mode = Self.modeName()
        guard let n = native else { return " | nativo: modo=\(mode) renderer=no" }
        return " | modo=\(mode) listo=\(mv_vrhost_native_available() != 0) marcada=\(nativeSceneMarked) usa=\(nativeUse) " + n.logLine()
#endif
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
            d.label = "\(vsName) pass\(k.pass) blend\(k.mode)"
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
    private func storeFilter(_ f: VrFilter) {
        func clampOr(_ v: Float, _ lo: Float, _ hi: Float, _ d: Float) -> Float { v.isFinite ? min(max(v, lo), hi) : d }
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
        n.tint = SIMD3<Float>(clampOr(tc.0, 0, 4, 1), clampOr(tc.1, 0, 4, 1), clampOr(tc.2, 0, 4, 1))
        // recubrimientos en orden (covers_rgb[4][3], covers_abr[4]); sin lista, el último (cover_rgb/cover_abr)
        var nc = 0
        withUnsafeBytes(of: f.covers_rgb) { (rgbRaw: UnsafeRawBufferPointer) -> Void in
            withUnsafeBytes(of: f.covers_abr) { (abrRaw: UnsafeRawBufferPointer) -> Void in
                let rgb = rgbRaw.bindMemory(to: Float.self)
                let abr = abrRaw.bindMemory(to: Int32.self)
                let cnt = min(Int(f.n_covers), Int(VR_POST_COVERS), abr.count, rgb.count / 3)
                for i in 0..<max(cnt, 0) where abr[i] >= 0 {
                    let c = SIMD3<Float>(rgb[3 * i], rgb[3 * i + 1], rgb[3 * i + 2])
                    if c.x.isFinite && c.y.isFinite && c.z.isFinite {
                        n.covers[nc] = SIMD4<Float>(clampOr(c.x, 0, 1, 0), clampOr(c.y, 0, 1, 0), clampOr(c.z, 0, 1, 0), Float(Int(abr[i]) & 3))
                        nc += 1
                    }
                }
            }
        }
        if nc == 0 && f.cover_abr >= 0 {
            n.covers[0] = SIMD4<Float>(clampOr(f.cover_rgb.0, 0, 1, 0), clampOr(f.cover_rgb.1, 0, 1, 0), clampOr(f.cover_rgb.2, 0, 1, 0), Float(Int(f.cover_abr) & 3))
            nc = 1
        }
        // líneas de exploración (gafas): solo con >= 8 y un periodo coherente; no se usan datos no finitos
        var scanned = false
        if f.n_scan >= 8, f.scan_pitch.isFinite, f.scan_pitch >= 1, f.scan_pitch <= 64, f.scan_h.isFinite, f.scan_h > 0, f.scan_phase.isFinite,
           f.scan_rgb.0.isFinite, f.scan_rgb.1.isFinite, f.scan_rgb.2.isFinite, scanlineGain > 0 {
            n.scan = SIMD4<Float>(f.scan_pitch, f.scan_phase, min(f.scan_h, f.scan_pitch), Float(f.scan_abr & 3))
            n.scanRGB = SIMD4<Float>(clampOr(f.scan_rgb.0, 0, 1, 0), clampOr(f.scan_rgb.1, 0, 1, 0), clampOr(f.scan_rgb.2, 0, 1, 0), scanlineGain)
            n.scanAfter = Float(min(max(Int(f.scan_after), 0), Int(VR_POST_COVERS)))
            scanned = true
        }
        // máscara de visión: se sube a la siguiente textura del anillo (solo si cambió)
        var framed = false
        if f.frame_on != 0, !suppressFrame, let fp = f.frame, f.frame_w == Int32(VR_FRAME_W), f.frame_h == Int32(VR_FRAME_H), f.area.2 > 0, f.area.3 > 0 {
            if uploadFrame(fp, Int(f.frame_w), Int(f.frame_h)) {
                n.frameIdx = frameTexCur
                n.frameArea = SIMD2<Float>(Float(f.area.2), Float(f.area.3))
                n.frameOn = true
                let fr = (f.frame_rgb.0, f.frame_rgb.1, f.frame_rgb.2)
                n.frameRGB = SIMD3<Float>(clampOr(fr.0, 0, 0.5, 0), clampOr(fr.1, 0, 0.5, 0), clampOr(fr.2, 0, 0.5, 0))
                framed = true
            }
        }
        n.cxCol = f.cx_col.isFinite ? f.cx_col : 160
        n.projH = f.proj_h.isFinite && f.proj_h > 1 ? f.proj_h : 160
        n.cyRow = f.cy_row.isFinite ? f.cy_row : 128
        let tinted = abs(n.tint.x - 1) > 1e-3 || abs(n.tint.y - 1) > 1e-3 || abs(n.tint.z - 1) > 1e-3
        n.needsPost = warp || tinted || nc > 0 || scanned || framed
    }

    /// Sube la resta de la máscara de visión (w*h bytes) al anillo de texturas R8. Devuelve false si no se pudo (sin máscara). Reutiliza la última si el contenido no cambió.
    private func uploadFrame(_ p: UnsafePointer<UInt8>, _ w: Int, _ h: Int) -> Bool {
        guard ensureFrameTextures(), w == Int(VR_FRAME_W), h == Int(VR_FRAME_H) else { return false }
        var sum: UInt64 = 1469598103934665603      // FNV-1a
        for i in 0..<(w * h) { sum = (sum ^ UInt64(p[i])) &* 1099511628211 }
        if sum == frameSum && frameTexCur >= 0 { return true }
        let idx = frameTexNext % frameTex.count
        frameTex[idx].replace(region: MTLRegionMake2D(0, 0, w, h), mipmapLevel: 0, withBytes: p, bytesPerRow: w)
        frameTexNext = (idx + 1) % frameTex.count
        frameTexCur = idx
        frameSum = sum
        return true
    }

    /// Crea el anillo de texturas de la máscara de visión (también hace de textura ficticia cuando no hay máscara: el shader siempre la enlaza).
    private func ensureFrameTextures() -> Bool {
        if !frameTex.isEmpty { return true }
        var out: [MTLTexture] = []
        for _ in 0..<8 {
            let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .r8Unorm, width: Int(VR_FRAME_W), height: Int(VR_FRAME_H), mipmapped: false)
            d.usage = .shaderRead
            d.storageMode = .shared
            guard let t = device.makeTexture(descriptor: d) else { return false }
            t.label = "VR mascara de vision"
            out.append(t)
        }
        frameTex = out
        return true
    }

    private func storeScene(_ sc: VrScene, _ newSlot: inout Bool) {
        storeFilter(sc.filter)
        let nv = Int(sc.nv)
        if !newSlot { curSlot = (curSlot + 1) % 3; newSlot = true }
        let slot = vtxSlots[curSlot]
        // Diagnóstico «partir triángulos grandes»: los batches 3D con triángulos de arista > umbral se reescriben (todos sus triángulos, en orden) tras los vértices originales;
        // los demás batches (y el 2D) conservan su rango. Con tope de seguridad (splitTriCap).
        var split = VrSplit()
        var splitOK = false
        gpuSplitText = "partido=no"
        if diagSplitMeters > 0, nv > 0, sc.nb > 0, sc.has_3d != 0, let v0 = sc.v, let b0 = sc.b {
            let rc = vr_split_run(v0, sc.nv, b0, sc.nb, diagSplitMeters, Self.splitTriCap, &split)
            if rc == 0 {
                splitOK = true
                gpuSplitText = "partido=\(Int(split.split_tris)) tris (arista>\(XRDiag.metersText(diagSplitMeters))) salida=\(Int(split.out_tris)) tris sin_partir_por_tope=\(Int(split.cap_skipped))"
            } else if rc == 1 {
                gpuSplitText = "partido=nada que partir (examinados \(Int(split.src_tris)) tris)"
            } else {
                gpuSplitText = "partido=ERROR (memoria o parametros)"
            }
        }
        defer { if splitOK { vr_split_free(&split) } }
        let nvTotal = nv + (splitOK ? Int(split.n) : 0)
        let stOff = (nvTotal * MemoryLayout<VrGpuVtx>.stride + 15) & ~15
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
        nativeSceneMarked = sc.native_3d != 0
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
        gpuMarkedTris = 0; gpuOtherTris = 0
        gpuMarkedMin = SIMD3<Float>(repeating: .greatestFiniteMagnitude); gpuMarkedMax = SIMD3<Float>(repeating: -.greatestFiniteMagnitude)
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
        if splitOK, split.n > 0, let sv = split.v { memcpy(buf.contents() + nv * MemoryLayout<VrGpuVtx>.stride, sv, Int(split.n) * MemoryLayout<VrGpuVtx>.stride) }
        slot.stOff = stOff
        if ns > 0, let st = sc.states { memcpy(buf.contents() + stOff, st, ns * MemoryLayout<VrGpuState>.stride) }
        else { memset(buf.contents() + stOff, 0, MemoryLayout<VrGpuState>.stride) }
        guard let b = sc.b else { return }
        let texBit = UInt16(VR_VF_TEXTURED)
        var scan = VrScan(), scanMarked = VrScan()
        let doScan = scanWanted
        scanWanted = false
        var scanView = [Float](repeating: 0, count: 16)
        if doScan {
            let c = diagView0.columns
            scanView = [c.0.x, c.0.y, c.0.z, c.0.w, c.1.x, c.1.y, c.1.z, c.1.w, c.2.x, c.2.y, c.2.z, c.2.w, c.3.x, c.3.y, c.3.z, c.3.w]
        }
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
            var drawFirst = first, drawCnt = cnt
            var scanPtr: UnsafePointer<VrVtx> = v + first
            if splitOK, i < Int(split.nb), split.first[i] != 0xFFFF_FFFF, let sv = split.v {
                let nf = Int(split.first[i]), nc = Int(split.count[i]) - Int(split.count[i]) % 3
                if nc > 0 && nf >= nv && nf + nc <= nvTotal { drawFirst = nf; drawCnt = nc; scanPtr = UnsafePointer(sv) + (nf - nv) }
            }
            if doScan && kind != 2 {
                vr_scan_tris(scanPtr, UInt32(drawCnt), scanView, scanNear, scanBig, &scan)
                if bt.skip_if_native != 0 { vr_scan_tris(scanPtr, UInt32(drawCnt), scanView, scanNear, scanBig, &scanMarked) }
            }
            batches.append(DrawBatch(first: drawFirst, count: drawCnt, kind: kind, mode: Int(bt.blend) & 3, semi: semi,
                                     hasTex: hasTex, evEnd: Int(bt.ev_end), skip: UInt32(bt.skip_if_native)))
            if kind != 2 {   // diagnóstico: triángulos 3D de la lista y caja (mundo VR) de los marcados, que son los que el nativo debe sustituir
                if bt.skip_if_native != 0 {
                    gpuMarkedTris += cnt / 3
                    for j in first..<(first + cnt) {
                        let q = SIMD3<Float>(v[j].pos.0, v[j].pos.1, v[j].pos.2)
                        if q.x.isFinite && q.y.isFinite && q.z.isFinite { gpuMarkedMin = pointwiseMin(gpuMarkedMin, q); gpuMarkedMax = pointwiseMax(gpuMarkedMax, q) }
                    }
                } else { gpuOtherTris += cnt / 3 }
            }
        }
        if doScan {
            gpuScanText = String(format: "lista GPU (tras partir): tris=%u largos=%u (marcados %u) cruzan_cercano=%u (marcados %u) largos_y_cruzan=%u (marcados %u) arista_max=%.1fm",
                                 scan.tris, scan.big, scanMarked.big, scan.cross, scanMarked.cross, scan.big_cross, scanMarked.big_cross, scan.max_edge)
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
        statSkipB = 0; statDrawB = 0; statSkipTris = 0; statDrawTris = 0

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
        let sceneV = viewsHook?() ?? views
        if let v0 = sceneV.first { diagView0 = v0 }

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
        let doHud = (gotNew || !hudDone) && drawWorld
        // Filtro de pantalla: la escena se resuelve en texturas intermedias y un pase por ojo escribe el drawable (ver planPost).
        let plan = planPost(colorTargets: colorTargets, resolveTargets: resolveTargets, layered: layered)
        let sceneColor = plan?.sceneColor ?? colorTargets
        let sceneResolve = plan?.sceneResolve ?? resolveTargets
        // Render nativo: se prepara SIEMPRE (reconoce épocas del almacén aunque no se dibuje: cinemáticas, menús) y se decide AHORA, con el estado real de este fotograma.
        // Si falta cualquier condición, nativeUse = false y no se salta ningún batch: nunca un fotograma sin 3D.
        nativeUse = false; nativeSkip = 0; decidedSkip = 0; nativeFailed = false
#if !MV_BENCH || MV_BENCH_NATIVE
        if let n = native {
            let want = mv_vrhost_render_mode() != 0
            nativeOnly = mv_vrhost_render_only_native() != 0
            n.tint = nativeOnly ? Self.nativeOnlyTint : SIMD3<Float>(1, 1, 1)
            n.splitMeters = diagSplitMeters      // diagnóstico: partir triángulos grandes (cambiarlo vuelve a subir las mallas)
            n.noZTest = diagNoZ
            let c0 = sceneColor.first, d0 = depthTargets.first
            // Validación previa: todo lo que haría fallar a n.encode se comprueba aquí, antes de saltar ningún batch.
            let nTargets = layered ? viewCount : min(sceneColor.count, depthTargets.count)
            let viewsOK = nTargets >= 1 && sceneV.count >= nTargets && projections.count >= nTargets
                && sceneV.prefix(nTargets).allSatisfy { m in (0..<4).allSatisfy { c in (0..<4).allSatisfy { m[c][$0].isFinite } } }
            let canTry = want && drawWorld && drawEyes && nativeSceneMarked && c0 != nil && d0 != nil && !sceneV.isEmpty && viewsOK
            n.headVR = SIMD3<Float>(head.columns.3.x, head.columns.3.y, head.columns.3.z)
            n.beginFrame(commandBuffer: commandBuffer, sortView: sceneV.first ?? matrix_identity_float4x4, draw: canTry)
            var ready = false
            if canTry, let c = c0, let d = d0 {
                ready = n.prewarm(layered: layered, colorFormat: c.pixelFormat, depthFormat: d.pixelFormat, samples: c.sampleCount, linear: linearOutput)
            }
            // Con el formato de cada destino (el pase por ojo usa el de su textura) y la ranura ya preparada.
            if ready, let c = c0, let d = d0 {
                ready = n.canEncode(layered: layered, colorFormat: c.pixelFormat, depthFormat: d.pixelFormat, samples: c.sampleCount, linear: linearOutput)
                if ready && !layered {
                    for i in 0..<nTargets where sceneColor[i].pixelFormat != c.pixelFormat || depthTargets[i].pixelFormat != d.pixelFormat || sceneColor[i].sampleCount != c.sampleCount {
                        ready = ready && n.canEncode(layered: false, colorFormat: sceneColor[i].pixelFormat, depthFormat: depthTargets[i].pixelFormat,
                                                     samples: sceneColor[i].sampleCount, linear: linearOutput)
                    }
                }
            }
            let dec = n.decide(wantNative: want, sceneMarked: nativeSceneMarked, pipelinesReady: ready, gpuMap: nativeMap)
            if dec.native != 0 && canTry { nativeUse = true; nativeSkip = dec.skip; decidedSkip = dec.skip }
            if nativeOnly && nativeSceneMarked { nativeSkip = UInt32.max }   // solo nativo: ningún batch 3D marcado por GPU, se decida lo que se decida
        }
#endif
        var nextCmd = 0
        for (gi, g) in groups.enumerated() {
            let firstG = gi == 0, lastG = gi == groups.count - 1
            var pend: [VramCmd] = []
            while nextCmd < listCmds.count && (listCmds[nextCmd].op < g.2) { pend.append(listCmds[nextCmd].cmd); nextCmd += 1 }
            encodeVram(commandBuffer, pend, fs)
            // Segunda vista (mira / misil): una sola vez, con la VRAM tal como queda tras los eventos del último grupo, ANTES del pase del ojo que la muestrea.
            if lastG, let a = auxRequest, drawWorld, drawBody { encodeAux(commandBuffer, a) }
            // Pantalla 3D: la escena a las dos texturas estéreo, solo cuando llega una escena nueva (la cabeza no interviene: la imagen es la misma hasta entonces).
            if lastG, let sr = stereoRequest, drawWorld, gotNew || stereoDirty || stereoRendered == false { encodeStereo(commandBuffer, sr) }
            if doHud { encodeHud(commandBuffer, range: g.0..<g.1, first: firstG) }
            encodeScenePasses(commandBuffer, range: g.0..<g.1, first: firstG, last: lastG, colorTargets: sceneColor,
                              depthTargets: depthTargets, resolveTargets: sceneResolve, views: sceneV,
                              projections: projections, head: head, layered: layered)
        }
        if let p = plan { encodePost(commandBuffer, p, projections: projections, layered: layered) }
        // eventos restantes de la lista (tras el último batch) para dejar la VRAM en sincronía con el oráculo
        if nextCmd < listCmds.count { encodeVram(commandBuffer, listCmds[nextCmd...].map { $0.cmd }, fs) }
        radar?.renderTexture(commandBuffer, vram: vram)      // VRAM ya en su estado final de la lista
        if doHud { finishHud(commandBuffer); hudDone = true }
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
    private(set) var stereoRendered = false
    private var stereoTex: MTLTexture?          // array de 2 capas (ojo izquierdo, derecho) en el formato de color del renderer
    private var stereoRaw: MTLTexture?          // la misma sin decodificación sRGB (el shader aplica tinte y recubrimientos en gamma)
    private var stereoSlices: [MTLTexture] = []
    private var stereoMSAAColor: MTLTexture?
    private var stereoDepth: MTLTexture?
    private var stereoKey = ""
    /// Imagen estéreo (array de 2 capas, valores tal como se guardaron: gamma en los formatos sRGB) o nil si aún no se dibujó.
    var stereoImage: MTLTexture? { stereoRendered ? stereoRaw : nil }
    /// Los valores de `stereoImage` ya son lineales (destino de coma flotante).
    var stereoStoredLinear: Bool { colorFormat == .rgba16Float }
    /// Tinte y recubrimientos de pantalla de la última escena (espacio gamma) para aplicarlos a la pantalla 3D: la escena de una cinemática no pasa por el post-proceso de los ojos.
    var screenFx: (tint: SIMD3<Float>, covers: [SIMD4<Float>]) { (fx.tint, fx.covers) }

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

    private func encodeStereo(_ cb: MTLCommandBuffer, _ r: StereoRequest) {
        guard ensureStereo(r.width, r.height), let d = stereoDepth, stereoSlices.count == 2 else { return }
        for e in 0..<2 {
            guard let cam = XRStereo.eye(gameView: gameView, eye: e, ipd: r.ipd, conv: r.convergence, tanX: r.tanX, tanY: r.tanY) else { return }
            var u = VrSceneUniforms()
            let m = XRMath.projection(tangents: cam.tangents, near: 0.05, far: 800) * cam.view
            u.view_proj = (m, m)
            u.head = SIMD4<Float>(0, 0, 0, 1)
            let color = stereoMSAAColor ?? stereoSlices[e]
            encodeScene(cb, range: 0..<batches.count, first: true, last: true, color: color, depth: d,
                        resolve: stereoMSAAColor != nil ? stereoSlices[e] : nil, uniforms: u, amplify: 0, viewIndex: 0, views: [], projections: [], isAux: true)
        }
        stereoRendered = true
        stereoDirty = false
    }

    private func encodeAux(_ cb: MTLCommandBuffer, _ a: AuxView) {
        guard haveScene, has3D, !batches.isEmpty, ensureAux(a.width, a.height), let c = auxColor, let d = auxDepth else { return }
        var u = VrSceneUniforms()
        let m = a.proj * a.view
        u.view_proj = (m, m)
        u.head = a.pos
        encodeScene(cb, range: 0..<batches.count, first: true, last: false, color: c, depth: d, resolve: nil, uniforms: u, amplify: 0,
                    viewIndex: 0, views: [], projections: [], isAux: true)
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
        // Sin mundo dibujado (pantalla de cine, diorama o escena sin 3D) no hay nada que filtrar: el pase solo pintaría tinte/marco sobre el fondo limpio.
        guard fx.needsPost, screenFilterEnabled, drawWorld, drawBody, haveScene, has3D, viewportOverride == nil, rasterizationRateMap == nil else { return nil }
        let msaa = (colorTargets.first?.sampleCount ?? 1) > 1
        let dst = msaa ? resolveTargets : colorTargets
        guard let first = dst.first, postPipeline(format: first.pixelFormat, layered: layered) != nil,
              ensureFrameTextures(), let tex = filterTextures(like: dst) else { return nil }
        if msaa { return PostPlan(sceneColor: colorTargets, sceneResolve: tex, fxTex: tex, dst: dst) }
        return PostPlan(sceneColor: tex, sceneResolve: [], fxTex: tex, dst: dst)
    }

    /// Pase de post-proceso por ojo: lee la escena resuelta (fxTex) y escribe el drawable con la onda por fila, el tinte y el recubrimiento.
    /// Parámetros por ojo a partir de la proyección: fila del punto principal = H/2*(1+P[2][1]) y focal vertical = H/2*P[1][1] (px).
    private func encodePost(_ cb: MTLCommandBuffer, _ plan: PostPlan, projections: [simd_float4x4], layered: Bool) {
        guard let first = plan.dst.first, let pso = postPipeline(format: first.pixelFormat, layered: layered) else { return }
        // por ojo: x = fila del punto principal, y = focal vertical, z = columna del punto principal, w = focal horizontal (todo en píxeles de la textura)
        func eyeParams(_ i: Int, _ width: Int, _ height: Int) -> SIMD4<Float> {
            let h = Float(height), w = Float(width)
            guard !projections.isEmpty else { return SIMD4<Float>(h * 0.5, h * 0.5, w * 0.5, h * 0.5) }
            let p = projections[min(i, projections.count - 1)]
            return SIMD4<Float>(h * 0.5 * (1 + p.columns.2.y), h * 0.5 * p.columns.1.y, w * 0.5 * (1 - p.columns.2.x), w * 0.5 * p.columns.0.x)
        }
        var u = VrPostUniforms()
        u.tint = SIMD4<Float>(fx.tint.x, fx.tint.y, fx.tint.z, 0)
        u.covers = (fx.covers[0], fx.covers[1], fx.covers[2], fx.covers[3])
        u.cfg = SIMD4<Float>(fx.projH, fx.cyRow, Float(fx.nrows), fx.cxCol)
        u.scan = fx.scan
        u.scan_rgb = fx.scanRGB
        u.misc = SIMD4<Float>(fx.scanAfter, fx.frameRGB.x, fx.frameRGB.y, fx.frameRGB.z)
        u.frame = SIMD4<Float>(fx.frameArea.x, fx.frameArea.y, fx.frameOn ? 1 : 0, 0)
        guard !frameTex.isEmpty else { return }
        let frameTexture = frameTex[min(max(fx.frameIdx, 0), frameTex.count - 1)]
        let dvCopy = fx.dv
        if layered {
            guard let src = plan.fxTex.first else { return }
            let pd = MTLRenderPassDescriptor()
            pd.colorAttachments[0].texture = first
            pd.colorAttachments[0].loadAction = .dontCare      // el triángulo cubre todo
            pd.colorAttachments[0].storeAction = .store
            let n = max(1, min(viewCount, first.arrayLength))
            pd.renderTargetArrayLength = n
            u.eye = (eyeParams(0, first.width, first.height), eyeParams(1, first.width, first.height))
            guard let enc = cb.makeRenderCommandEncoder(descriptor: pd) else { return }
            enc.label = "VR post filtro (estéreo)"
            enc.setRenderPipelineState(pso)
            enc.setViewport(MTLViewport(originX: 0, originY: 0, width: Double(first.width), height: Double(first.height), znear: 0, zfar: 1))
            enc.setFragmentTexture(src, index: 0)
            enc.setFragmentTexture(frameTexture, index: 1)
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
                let ep = eyeParams(i, dst.width, dst.height)
                u.eye = (ep, ep)                                // el fragmento del ojo suelto usa eye[0]
                guard let enc = cb.makeRenderCommandEncoder(descriptor: pd) else { continue }
                enc.label = "VR post filtro (ojo \(i))"
                enc.setRenderPipelineState(pso)
                enc.setViewport(MTLViewport(originX: 0, originY: 0, width: Double(dst.width), height: Double(dst.height), znear: 0, zfar: 1))
                enc.setFragmentTexture(plan.fxTex[i], index: 0)
                enc.setFragmentTexture(frameTexture, index: 1)
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
    /// amplify > 0: un solo pase con amplificación de vértices a `amplify` capas del array; 0: un pase de un ojo.
    /// first: limpia color/profundidad; si no, carga. last: resuelve el color (MSAA); si no, lo guarda sin resolver.
    private func encodeScene(_ cb: MTLCommandBuffer, range: Range<Int>, first: Bool, last: Bool, color: MTLTexture,
                             depth: MTLTexture, resolve: MTLTexture?, uniforms: VrSceneUniforms, amplify: Int, viewIndex: Int = 0,
                             views: [simd_float4x4] = [], projections: [simd_float4x4] = [], isAux: Bool = false) {
        let seg = range.filter { batches.indices.contains($0) && batches[$0].kind != 2 }
        if !first && seg.isEmpty && !last { return }
        let samples = color.sampleCount
        statPasses += 1
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
        if let m = rasterizationRateMap, !isAux { pd.rasterizationRateMap = m }
        guard let enc = cb.makeRenderCommandEncoder(descriptor: pd) else { return }
        enc.label = isAux ? "escena (segunda vista)" : amplify > 0 ? "escena (estéreo, amplificación)" : "escena (ojo \(viewIndex))"
        defer { enc.endEncoding() }
        // Los defer corren al revés: este va ANTES de cerrar el codificador, tras dibujar el 3D (también si el guard de más abajo
        // retorna). Solo en el último segmento (el que resuelve el MSAA): el quad del radar y el arma se prueban contra la profundidad del 3D.
        defer {
            if last && drawBody {
                radar?.encodeQuad(enc, uniforms: uniforms, layered: amplify > 0, color: color.pixelFormat, depth: depth.pixelFormat, samples: samples)
                weapon?.sampleCount = samples      // las muestras del pase pueden cambiar en vivo (diagnóstico MSAA 1x)
                weapon?.encode(encoder: enc, views: views, projections: projections, layered: amplify > 0, viewIndex: viewIndex)
                if let sv = special {
                    // Con el filtro de pantalla el HUD se muestrea por la vista que corresponda al destino: aquí el panel del HUD en bruto (gamma)
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
        // Escena nativa: en el último segmento del fotograma (la VRAM ya tiene todos los eventos de la lista), con las mismas vistas y proyecciones por ojo que la ruta
        // de la GPU y la profundidad inversa del sistema (no se escribe depthRange). Si falla, ns_native_fail y los batches de este segmento se dibujan por GPU.
        var skipMask = isAux ? 0 : nativeSkip   // la segunda vista y la pantalla 3D no llevan escena nativa: se dibujan enteras por GPU
        if last, !isAux, nativeUse, drawWorld, drawEyes, let n = native {
            var vps: [simd_float4x4] = [], eyes: [SIMD3<Float>] = []
            func eyePos(_ v: simd_float4x4) -> SIMD3<Float> { let t = v.inverse.columns.3; return SIMD3<Float>(t.x, t.y, t.z) }
            if amplify > 0 {
                for i in 0..<min(amplify, min(views.count, 2)) { vps.append(i == 0 ? uniforms.view_proj.0 : uniforms.view_proj.1); eyes.append(eyePos(views[i])) }
            } else if viewIndex < views.count {
                vps.append(uniforms.view_proj.0); eyes.append(eyePos(views[viewIndex]))
            }
            let ok = !vps.isEmpty && n.encode(encoder: enc, vram: vram, viewProj: vps, eyes: eyes, layered: amplify > 0, colorFormat: color.pixelFormat,
                                              depthFormat: depth.pixelFormat, samples: samples, linear: linearOutput)
            if !ok { n.fail(); if !nativeOnly { skipMask = 0; nativeSkip = 0 }; nativeUse = false; nativeFailed = true }
        }
#else
        let skipMask: UInt32 = 0
#endif
        // Recuperación: si el dibujo nativo falló, los batches marcados de los grupos anteriores (ya saltados en sus pases) se dibujan ahora, en este mismo pase.
        var segDraw = seg
        if last, !isAux, nativeFailed, !nativeOnly, decidedSkip != 0, range.lowerBound > 0 {
            let lost = (0..<range.lowerBound).filter { batches.indices.contains($0) && batches[$0].kind != 2 && (batches[$0].skip & decidedSkip) != 0 }
            segDraw = lost + seg
        }
        guard haveScene, has3D, drawWorld, drawEyes || isAux, !segDraw.isEmpty, let buf = vtxSlots[curSlot].buf else { return }
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
            statDraws += 1
        }
        // Orden del ensamblador: por época, opacos y luego semitransparentes (se respeta tal cual).
        var curKind = -1
        for i in segDraw {
            let bt = batches[i]
            if (bt.skip & skipMask) != 0 { statSkipB += 1; statSkipTris += bt.count / 3; continue }   // 3D que se pinta desde la escena nativa (VrBatch.skip_if_native & NsDecision.skip)
            statDrawB += 1; statDrawTris += bt.count / 3
            if bt.kind != curKind {
                curKind = bt.kind
                if curKind == 0 {
                    enc.setDepthStencilState(diagNoZ ? dsOpaqueNoZ : dsOpaque)
                    enc.setDepthBias(0, slopeScale: 0, clamp: 0)
                } else {
                    enc.setDepthStencilState(diagNoZ ? dsSemiNoZ : dsSemi)
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
