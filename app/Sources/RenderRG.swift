import Metal
import simd
import Foundation

// Renderer Metal de la escena VR. SIN COMPILAR (escrito sin toolchain Apple): revisar en la primera compilación.
// Consume la API C de vr_scene.h (mv_host_scene) y dibuja: (1) la VRAM de la consola mantenida en GPU, (2) el panel 2D
// (HUD) en hudTexture y (3) el mundo 3D en estéreo.
//
// UN SOLO MODO DE 3D (mv_vrhost_render_mode devuelve siempre 1, vrhost.h): el nativo. El camino `gpuMode` (3D desde la lista de dibujo de la GPU con los vértices de vista de la GTE; vr_scene.c,
// planPost, encodeStereoGPU...) ya no se alcanza salvo en el banco MV_BENCH sin render nativo: es código muerto que se conserva sin tocar.
//  - nativo (único): Native/NativeRenderer.swift, escena leída de la RAM del motor: el mundo, sus efectos y los efectos de pantalla completa. La lista de la GPU solo aporta los
//    eventos de VRAM, la capa 2D, el radar y el mapa de coherencia. Si el nativo no está listo en un fotograma se REPITE el último fotograma nativo válido o se funde a negro
//    (vr/native/ns_present.h). Las cinemáticas del motor se pintan con el mismo render desde la cámara del juego, en estéreo, en la textura de la pantalla 3D.
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
    /// Rect UV (u0, v0, u1, v1; margen del 1 %) de la ventana de menú dentro de hudTexture: las primitivas 2D nuevas respecto al HUD de antes de abrir el menú. nil = usar la capa entera (créditos incluidos).
    private(set) var hudMenuUV: SIMD4<Float>?
    private var menuUVBuf = [Float](repeating: 0, count: 4)   // destino reutilizado de mv_vrhost_hud2d_menu_rect
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
    /// Umbral (m) para partir en CPU los triángulos con una arista más larga en el render nativo. 0 = no partir (el modo gpu no parte).
    var diagSplitMeters: Float = 0
    /// Sesgo de profundidad de las semitransparentes del modo gpu (evita z-fighting con decales coplanares).
    var semiDepthBias: Float = 4
    var semiSlopeBias: Float = 2
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
    // modo gpu: Z invertida (cerca = 1, compare >=). Opaco: prueba + escritura; semitransparente: solo prueba. NoZ = interruptor de diagnóstico.
    private var dsOpaque: MTLDepthStencilState?
    private var dsSemi: MTLDepthStencilState?
    private var dsOpaqueNoZ: MTLDepthStencilState?
    private var dsSemiNoZ: MTLDepthStencilState?
    /// Modo de 3D de este fotograma: false = render nativo (el único camino del mundo; mv_vrhost_render_mode() devuelve siempre 1). `true` solo en el banco MV_BENCH sin render nativo.
    private(set) var gpuMode = false

    private struct PKey: Hashable {
        var hud: Bool, layered: Bool, filter: Bool, linear: Bool
        var pass: UInt32, mode: Int          // mode -1 = sin mezcla
        var color: MTLPixelFormat, depth: MTLPixelFormat, samples: Int
    }
    private var pipes: [PKey: MTLRenderPipelineState] = [:]
    private var pipeFull: MTLRenderPipelineState?
    private var failed = Set<String>()

    // filtro de pantalla (post-proceso por ojo, SOLO modo gpu: bajo el agua las tiras de la clase 3 de vr_scene no se dibujan, las sustituye este pase)
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
    private var fxTex: [MTLTexture] = []
    private var fxKey = ""
    private var postPipes: [String: MTLRenderPipelineState] = [:]
    private var statPost = 0                        // pases de post-proceso (ojos) codificados en el último encode (registro)

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
    private var gpu3DTris = 0                           // triángulos 3D de la lista de la GPU de la última escena (se dibujan solo en el modo gpu; registro)
    private var has3D = false
    private var haveScene = false
    private var hudRect = simd_float4(0, 0, 320, 240)
    private var lastIndex: UInt32?
    private var hudDone = false
    // Capa 2D fiel (vr/scene/vr_layer2d): la rasteriza la CPU con la aritmética de la GPU original y llega ya como RGBA8; sustituye al pase HUD del shader mientras exista.
    /// Fotograma completo (pantalla de cine / vídeo ampliado) a alta resolución; nil si la última escena no lo trae. Lo consume XRPresenter.hqFull.
    private(set) var hqFull: (bytes: [UInt8], w: Int, h: Int, seq: UInt32)?
    /// Resultado de la última lectura de la capa 2D fiel (takeLayer2D): "ok" o "descartada(motivo)". Solo para el diagnóstico [radio-src].
    private(set) var layerNote = "ok"
    /// Línea [radio-src] (la pide XRFrameEncoder 1 vez por segundo mientras la vista es la radio): si la pantalla de cine usa la capa 2D fiel o el fotograma del emulador.
    func radioSrcLine(hqActive: Bool, xbrActive: Bool) -> String {
        "[radio-src] hqActive=\(hqActive) hqFull=\(hqFull != nil) xbrActive=\(xbrActive) layerView=\(layerView) takeLayer2D=\(layerNote)"
    }
    private var hudLegacy: MTLTexture?, hudLegacySRGB: MTLTexture?
    private var hudHQRing: [MTLTexture] = [], hudHQRingSRGB: [MTLTexture] = []
    private var hudHQIdx = 0, hudHQW = 0, hudHQH = 0, hudHQSeq: UInt32 = 0
    private var hudHQ = false
    /// xBR-lv2 x3 de la capa 2D (XRPixelScaler, XR2DFilter.xbr): anillo de destinos paralelo a hudHQRing (mismo índice); hudTexture apunta a él en vez de a la capa nativa.
    private var hudScaler: XRPixelScaler?
    private var hudXbr: [MTLTexture] = [], hudXbrSRGB: [MTLTexture] = []
    /// Tamaño LÓGICO (píxeles del juego) de la capa que muestra hudTexture: la textura puede llevar además la ampliación xBR (x3) o la escala del HUD de reserva (x4).
    var hudLogical: SIMD2<Float> { hudHQ ? SIMD2<Float>(Float(hudHQW), Float(hudHQH)) : SIMD2<Float>(320, 240) }
    private var hudHQMode: Int32 = 0, hudHQView: Int32 = 0   // modo (VL_*) y vista ((view_kind << 8) | view_reason) del trabajo que produjo la capa mostrada
    /// Vista vigente y si la ventana 2D del menú (L2/R2, pausa) va sobre el mundo: las fija XRFrameEncoder antes de `encode`; takeLayer2D descarta con ellas las capas de otra vista o de otro modo.
    var layerView: Int32 = 0 { didSet { dropStaleLayer() } }
    var layerOverlay = false { didSet { dropStaleLayer() } }
    /// La capa mostrada (anillo) ya no vale si la vista cambió (créditos -> menú) o la ventana del menú no admite su modo: fuera, vuelve el HUD de siempre.
    private func dropStaleLayer() {
#if !MV_BENCH
        if hudHQ && mv_vrhost_hud2d_accept(hudHQMode, hudHQView, layerView, layerOverlay ? 1 : 0) == 0 {
            hudHQ = false; hudDone = false; hudTexture = hudLegacy; hudTextureSRGB = hudLegacySRGB; hudMenuUV = nil
        }
#endif
    }
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
        if XR2DFilter.current == .xbr { hudScaler = XRPixelScaler(device: device) }
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
        "modo3D=\(gpuMode ? "gpu" : "nativo") tiene3D=\(has3D) hayEscena=\(haveScene) lotes=\(batches.count) (3D de la lista\(gpuMode ? "" : ", NO se dibujan")=\(batches.filter { $0.kind != 2 }.count) 2D=\(batches.filter { $0.kind == 2 }.count)) pases=\(statPasses) dibujos=\(statDraws) post=\(statPost) filtro=\(fx.needsPost) drawWorld=\(drawWorld) drawBody=\(drawBody) msaa=\(sampleCount) lineal=\(linearOutput) radar=\(radar != nil)" + nativeLog()
    }

    /// Línea de diagnóstico del render nativo (1 vez por segundo, justo tras xr-diag): qué se presentó (actual / último válido repetido / negro), triángulos de la lista de la GPU
    /// que NO se dibujan (siempre), cámaras y coherencia con la lista (solo registro). nil si no hay renderer nativo.
    func nativeDiagLine() -> String? {
#if MV_BENCH && !MV_BENCH_NATIVE
        return nil
#else
        guard !gpuMode, let n = native else { return nil }
        return "nat-diag: 3D de la lista de la GPU (no se dibuja jamas): tris=\(gpu3DTris) | nativo dibujado=\(nativeDraw) | " + n.logLine() + " | " + n.drawnSummary(gpuEye: gpuEyeVR)
#endif
    }

    /// Línea de la red de seguridad de vértices del render nativo (1 vez por segundo; reinicia sus contadores). nil si no hay renderer nativo.
    func nativeNetLine() -> String? {
#if MV_BENCH && !MV_BENCH_NATIVE
        return nil
#else
        return gpuMode ? nil : native?.netLine()
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
        if gpuMode { return " | gpu3D: dibujos=\(statDraws) tris3D=\(gpu3DTris)" }
        guard let n = native else { return " | nativo: renderer=no (sin 3D)" }
        return " | listo=\(mv_vrhost_native_available() != 0) dibuja=\(nativeDraw) " + n.logLine()
#endif
    }


    // MARK: pipelines
    private func pipeline(_ k: PKey) -> MTLRenderPipelineState? {
        if let p = pipes[k] { return p }
        guard let lib = library else { return nil }
        do {
            let fc = MTLFunctionConstantValues()
            var f = k.filter, l = k.linear, pass = k.pass, bm = Int32(k.mode)
            fc.setConstantValue(&f, type: .bool, index: Int(VR_FC_FILTER))
            fc.setConstantValue(&l, type: .bool, index: Int(VR_FC_LINEAR_OUT))
            fc.setConstantValue(&pass, type: .uint, index: Int(VR_FC_PASS))
            // modo 0..3 (semitransparente): vr_fs_scene_blend mezcla en gamma leyendo el destino ([[color(0)]], sin mezcla de función fija); el resto, vr_fs_scene (no lee el destino)
            if k.mode >= 0 { fc.setConstantValue(&bm, type: .int, index: Int(VR_FC_BLEND)) }
            let fs = try lib.makeFunction(name: k.mode >= 0 ? "vr_fs_scene_blend" : "vr_fs_scene", constantValues: fc)
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

    /// Resumen del filtro de pantalla de la escena (VrScene.filter, contrato en vr_scene.h "Clase 3"): se conserva hasta la siguiente escena. Solo en el modo gpu.
    private func storeFilter(_ f: VrFilter) {
        func clampOr(_ v: Float, _ lo: Float, _ hi: Float, _ d: Float) -> Float { v.isFinite ? min(max(v, lo), hi) : d }
        var n = FilterFx()
        defer { fx = n }
        guard gpuMode, screenFilterEnabled, f.active != 0 else { return }
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
           f.scan_rgb.0.isFinite, f.scan_rgb.1.isFinite, f.scan_rgb.2.isFinite {
            n.scan = SIMD4<Float>(f.scan_pitch, f.scan_phase, min(f.scan_h, f.scan_pitch), Float(f.scan_abr & 3))
            n.scanRGB = SIMD4<Float>(clampOr(f.scan_rgb.0, 0, 1, 0), clampOr(f.scan_rgb.1, 0, 1, 0), clampOr(f.scan_rgb.2, 0, 1, 0), 1)   // ganancia 1 = fiel al original
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

    private func takeLayer2D(_ commandBuffer: MTLCommandBuffer) {
#if MV_BENCH
        return
#else
        var px: UnsafePointer<UInt8>?
        var w: Int32 = 0, h: Int32 = 0, full: Int32 = 0, seq: UInt32 = 0
        let r = mv_vrhost_hud2d_acquire(&px, &w, &h, &full, &seq)
        defer { if r >= 0 { mv_vrhost_hud2d_release() } }
        let ov: Int32 = layerOverlay ? 1 : 0
        dropStaleLayer()
        let usable = r >= 0 && px != nil && w > 0 && h > 0
        var lMode: Int32 = 0, lView: Int32 = 0
        // La capa nueva (retrasada: el trabajador va por detrás de la vista) se rechaza si es de otra vista o, en el menú, no es VL_PANEL: se conserva la anterior válida (arriba) o el HUD de siempre.
        let accepted = usable && mv_vrhost_hud2d_layer(&lMode, &lView) != 0 && mv_vrhost_hud2d_accept(lMode, lView, layerView, ov) != 0
        layerNote = accepted ? "ok" : usable ? "descartada(vista/modo lMode=\(lMode) lView=\(lView) vigente=\(layerView) ov=\(ov))" : "descartada(sin-capa r=\(r))"
        if !accepted {
            if hudHQ && !usable { hudHQ = false; hudDone = false; hudTexture = hudLegacy; hudTextureSRGB = hudLegacySRGB; hudMenuUV = nil }
            hqFull = nil
            return
        }
        let W = Int(w), H = Int(h)
        if full != 0 {
            if r == 1 || hqFull == nil { hqFull = (Array(UnsafeBufferPointer(start: px!, count: W * H * 4)), W, H, seq) }
            if hudHQ { hudHQ = false; hudDone = false; hudTexture = hudLegacy; hudTextureSRGB = hudLegacySRGB; hudMenuUV = nil }
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
            hudXbr = []; hudXbrSRGB = []
            if hudScaler != nil {
                for i in 0..<3 {
                    guard let t = XRPixelScaler.makeTarget(device, width: W, height: H, label: "HUD 2D xBR \(i)") else { hudXbr = []; hudXbrSRGB = []; break }
                    hudXbr.append(t.tex); hudXbrSRGB.append(t.srgb)
                }
            }
        }
        let fresh = r == 1 || !hudHQ || hudHQSeq != seq
        if fresh {
            hudHQIdx = (hudHQIdx + 1) % hudHQRing.count
            hudHQRing[hudHQIdx].replace(region: MTLRegionMake2D(0, 0, W, H), mipmapLevel: 0, withBytes: px!, bytesPerRow: W * 4)
            hudHQSeq = seq
            if let sc = hudScaler, hudXbr.count == hudHQRing.count { sc.encode(commandBuffer, src: hudHQRing[hudHQIdx], dst: hudXbr[hudHQIdx], width: W, height: H) }
        }
        if hudXbr.count == hudHQRing.count {
            hudTexture = hudXbr[hudHQIdx]; hudTextureSRGB = hudXbrSRGB[hudHQIdx]
        } else {
            hudTexture = hudHQRing[hudHQIdx]; hudTextureSRGB = hudHQRingSRGB[hudHQIdx]
        }
        hudHQ = true; hudHQMode = lMode; hudHQView = lView
        if XRLayer2D.split && lMode == 0 && mv_vrhost_hud2d_menu_rect(&menuUVBuf) != 0 {   // rect de la ventana del menú: solo con el troceo del 2D (apagado por defecto: la capa va entera)
            hudMenuUV = SIMD4<Float>(max(0, menuUVBuf[0] - 0.01), max(0, menuUVBuf[1] - 0.01), min(1, menuUVBuf[2] + 0.01), min(1, menuUVBuf[3] + 0.01))
        } else if XRLayer2D.split && lMode == 0 && (lView & 255) == 10 && hudMenuUV != nil {
            // selector (VRV_R_SELECTOR = 10) sin rect en esta capa: se conserva el último válido (nil solo al salir del selector)
        } else { hudMenuUV = nil }
#endif
    }


    /// Máscara de visión de la caja para el render nativo (recubrimiento de ojo NSG_OV_FRAME): solo con el jugador en la caja (VrFilter.frame_in_box) y la máscara que calcula vrhost (frame, 160x128). Se copia aquí
    /// (el puntero solo vale durante la llamada) y la consume encodeScene. nil = sin máscara. En el modo gpu la aplica el post-proceso (planPost).
    private struct BoxMaskIn { var frame: [UInt8]; var area: (Float, Float); var cxCol: Float, cyRow: Float, projH: Float; var rgb: (Float, Float, Float) }
    private var boxMaskIn: BoxMaskIn?
    private func storeBoxMask(_ f: VrFilter) {
        boxMaskIn = nil
        guard !gpuMode, f.frame_in_box != 0, f.frame_on != 0, let fp = f.frame, f.frame_w == Int32(VR_FRAME_W), f.frame_h == Int32(VR_FRAME_H), f.area.2 > 0, f.area.3 > 0 else { return }
        func fin(_ v: Float, _ lo: Float, _ hi: Float, _ d: Float) -> Float { v.isFinite ? min(max(v, lo), hi) : d }
        boxMaskIn = BoxMaskIn(frame: Array(UnsafeBufferPointer(start: fp, count: Int(VR_FRAME_W) * Int(VR_FRAME_H))), area: (Float(f.area.2), Float(f.area.3)),
                              cxCol: fin(f.cx_col, 0, 4096, 160), cyRow: fin(f.cy_row, 0, 4096, 128), projH: f.proj_h.isFinite && f.proj_h > 1 ? f.proj_h : 160,
                              rgb: (fin(f.frame_rgb.0, 0, 0.5, 0), fin(f.frame_rgb.1, 0, 0.5, 0), fin(f.frame_rgb.2, 0, 0.5, 0)))
    }

    /// Guarda la escena de la lista de la GPU: vértices y estados para el panel 2D (HUD), los lotes con su `ev_end` (orden de los eventos de VRAM), la cámara del juego y los datos de
    /// cinemática. Los lotes de 3D (kind 0/1) NO se dibujan: el mundo sale del render nativo.
    private func storeScene(_ sc: VrScene, _ newSlot: inout Bool) {
        storeFilter(sc.filter)
        storeBoxMask(sc.filter)
        var scFilter = sc.filter
        let win = vr_scene_screen_window(&scFilter)
        stereoWindow = win.ok != 0 ? SIMD4<Float>(win.left, win.right, win.up, win.down) : nil   // la de ESTA lista (la misma que pareja con hudRect y los lotes); una lista sin proyección no hereda la de otra
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
            if kind == 3 { continue }   // filtro de pantalla: no es geometría ni HUD; en el modo gpu lo aplica el post-proceso por ojo con VrScene.filter (storeFilter / planPost), en el nativo los efectos salen del render nativo
            var hasTex = false
            if kind != 0 {   // 2D y 3D semitransparente: decide las dos pasadas por texel (bit 15); el opaco no lo necesita
                for j in first..<(first + cnt) where (v[j].flags & texBit) != 0 { hasTex = true; break }
            }
            let semi = kind == 1 || (kind == 2 && bt.blend <= 3)   // blend 0xFF = opaco
            batches.append(DrawBatch(first: first, count: cnt, kind: kind, mode: Int(bt.blend) & 3, semi: semi, hasTex: hasTex, evEnd: Int(bt.ev_end)))
            if kind != 2 { gpu3DTris += cnt / 3 }   // registro: 3D de la lista (se dibuja solo en el modo gpu)
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
        statDraws = 0; statPasses = 0; statPost = 0
#if !MV_BENCH || MV_BENCH_NATIVE
        gpuMode = mv_vrhost_render_mode() == 0      // 0 = gpu (defecto), 1 = nativo
#else
        gpuMode = true
#endif

        // 1) escenas nuevas: eventos de VRAM + capa 2D. Si llegan varias listas, los eventos de las anteriores se
        //    aplican enteros al principio y solo se usa la última.
        var preCmds: [IdxCmd] = []
        var listCmds: [IdxCmd] = []
        var newSlot = false
        var gotNew = false
        diagLists = 0; diagList3D = nil; diagList2D = nil
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
            diagLists += 1
            if has3D { diagList3D = idx }
            if batches.contains(where: { $0.kind == 2 }) { diagList2D = idx }
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
        takeLayer2D(commandBuffer)
        let doHud = (gotNew || !hudDone) && drawWorld && !hudHQ
        // Render nativo: se prepara SIEMPRE (reconoce épocas del almacén aunque no se dibuje: menús) y lo que no se pueda dibujar se REPITE (último válido) o se funde a negro.
        // Modo gpu: no hay nada nativo que preparar; la pantalla 3D se considera sana (el 3D sale de la lista) salvo que encodeStereo falle.
        if gpuMode { nativeDraw = false; nativeDrawnLast = false }
        else { prepareNative(commandBuffer: commandBuffer, colorTargets: colorTargets, depthTargets: depthTargets, sceneV: sceneV, projections: projections, head: head, layered: layered) }
        // Filtro de pantalla (modo gpu): la escena se resuelve en texturas intermedias y un pase por ojo escribe el drawable (ver planPost). Nativo: nunca (plan = nil).
        let plan = gpuMode ? planPost(colorTargets: colorTargets, resolveTargets: resolveTargets, layered: layered) : nil
        let sceneColor = plan?.sceneColor ?? colorTargets
        let sceneResolve = plan?.sceneResolve ?? resolveTargets
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
            // El mundo de los ojos. Nativo: UN pase tras el último grupo (la VRAM ya tiene todos los eventos de la lista). Modo gpu: un pase por grupo de lotes con los mismos
            // eventos de VRAM (el primero limpia, el último resuelve el MSAA y dibuja radar y arma), con las vistas por ojo.
            if gpuMode {
                encodeScenePasses(commandBuffer, range: g.0..<g.1, first: firstG, last: lastG, colorTargets: sceneColor, depthTargets: depthTargets,
                                  resolveTargets: sceneResolve, views: sceneV, projections: projections, head: head, layered: layered)
            } else if lastG {
                encodeScenePasses(commandBuffer, colorTargets: colorTargets, depthTargets: depthTargets, resolveTargets: resolveTargets,
                                  views: sceneV, projections: projections, head: head, layered: layered)
            }
        }
        if let p = plan { encodePost(commandBuffer, p, projections: projections, layered: layered) }
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
        n.vramList = lastIndex ?? 0; n.vramKnown = lastIndex != nil      // índice de la última lista aplicada a la VRAM: sincronía de época de las mallas (NativeRenderer.drain)
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
        // Cámara del juego: SOLO en la cinemática del motor (view_kind ENGINE) en cualquier modo de cinemática (pantalla 3D, diorama, dentro de la escena). En el mundo (cámara VR activa) nunca, aunque la
        // escena no sea apta para primera persona (sceneUnfit): ahí se dibuja el mundo VR normal.
        n.screenMode = screenMode
        n.cinemaGameView = (engineView && Self.viewOK(gameView)) ? gameView : nil
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
        didSet {
            if stereoRequest != oldValue { stereoDirty = true }
            // Al salir de la pantalla 3D se olvida su imagen: la siguiente escena del motor no debe empezar enseñando el último fotograma de la anterior (caché obsoleta).
            if stereoRequest == nil { stereoRendered = false; stereoBaseValid = false; stereoBaseStale = false; stereoHas2D = false; stereoComposed = false }
        }
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
    /// Ventana de la proyección del motor de la ÚLTIMA lista recibida (tangentes izquierda, derecha, arriba, abajo; vr_scene_screen_window; nil si esa lista no trae proyección): la pantalla 3D del modo gpu la usa tal
    /// cual para que el 3D registre con los lotes 2D de la misma lista, que cubren la misma imagen mostrada (H real y centro OFX/OFY, no la ventana virtual de la petición).
    private var stereoWindow: SIMD4<Float>?
    /// Diagnóstico `[estereo]`: listas recibidas en este fotograma y el índice de la lista que trae el 3D y de la que trae lotes 2D (nil = ninguna); último registro (ns).
    private var diagLists = 0
    private var diagList3D: UInt32?
    private var diagList2D: UInt32?
    private var stereoLogNs: UInt64 = 0
    /// Modo gpu: los lotes 2D (bandas, subtítulos, alarma) de la lista se componen DENTRO de las texturas de la pantalla 3D; XRPresenter no pone entonces el panel 2D encima.
    /// Solo si ESTA lista se compuso de verdad dentro (hay 3D o base 3D guardada y el estéreo no falló): si no, XRPresent pone el panel 2D aparte como antes (vrhost_stereo2d_inside).
    var stereo2DInside: Bool { vrhost_stereo2d_inside(gpuMode ? 1 : 0, (has3D || stereoBaseValid) ? 1 : 0, stereoComposed ? 1 : 0) != 0 }
    private var stereoComposed = false          // el estéreo del modo gpu compuso (3D o base + 2D) la última lista; false si falló la textura o la cámara
    private var stereoBaseValid = false         // stereoBase guarda el 3D compuesto (tras 3D + onda) de la última vez que se dibujó
    private var stereoBaseStale = false         // el 3D vigente (sin 2D) está solo en las texturas estéreo: stereoBase aún no lo guarda (copia perezosa en la siguiente lista solo-2D)
    private var stereoHas2D = false             // las texturas estéreo llevan 2D encima de la base
    private var stereoBase: [MTLTexture] = []   // por ojo: el 3D compuesto SIN 2D; cada lista nueva solo-2D parte de ella para pintar SU 2D (ver stereoBaseStale)
    /// Imagen estéreo (array de 2 capas, valores tal como se guardaron: gamma en los formatos sRGB) o nil si aún no se dibujó.
    var stereoImage: MTLTexture? { stereoRendered ? stereoRaw : nil }
    /// Capas de la textura de la pantalla 3D ya reservada (aunque aún no se haya dibujado nada en ella; 0 = sin reservar). El banco comprueba que es un array de 2.
    var stereoAllocatedLength: Int { stereoTex?.arrayLength ?? 0 }
    /// Diagnóstico de la pantalla de cine 3D ([cine3d], lo pide XRFrameEncoder 1/s mientras la vista es una cinemática del motor): secuencia de la escena nativa actual frente a la de la última
    /// imagen dibujada en las texturas (si divergen o no suben, la pantalla está congelada), cámara del juego usada (posición en el mundo VR y rumbo) y campo de visión horizontal de la ventana del motor.
    func cine3dDiag() -> String {
        var cur: UInt32 = 0, camH: Float = 0, cgv = false
#if !MV_BENCH || MV_BENCH_NATIVE
        if let n = native { cur = n.sceneSeq; camH = n.camH; cgv = n.cinemaGameView != nil }
#endif
        let p = gameView.columns.3, f = gameView.columns.2
        let yaw = atan2(f.x, f.z) * 180 / Float.pi
        let fov = camH > 8 ? 2 * atan(160 / camH) * 180 / Float.pi : 0
        return "seqNativo=\(cur) seqEstereo=\(stereoSeq) estereoDibujado=\(stereoRendered) stereoImage=\(stereoImage == nil ? "nil" : "ok") engineView=\(engineView) cinemaGameView=\(cgv) drawWorld=\(drawWorld) drawEyes=\(drawEyes) pasada=[\(stereoPassDiag)] camara=(\(String(format: "%.2f %.2f %.2f", p.x, p.y, p.z))) rumbo=\(Int(yaw.rounded()))gr fov=\(Int(fov.rounded()))gr"
    }
    /// Los valores de `stereoImage` ya son lineales (destino de coma flotante).
    var stereoStoredLinear: Bool { colorFormat == .rgba16Float }
    /// Nativo: los efectos de pantalla de la cinemática (fundidos, destello...) los dibuja el render nativo dentro de la propia pantalla 3D (recubrimientos del ojo): aquí no hay tinte ni
    /// recubrimientos. Modo gpu: la escena de una cinemática no pasa por el post-proceso de los ojos, así que se devuelven el tinte y los recubrimientos de la última escena (espacio gamma).
    var screenFx: (tint: SIMD3<Float>, covers: [SIMD4<Float>]) {
        if gpuMode { return (fx.tint, fx.covers) }
        return (SIMD3<Float>(1, 1, 1), [])
    }

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
        var base: [MTLTexture] = []
        for e in 0..<2 {
            let bd = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: colorFormat, width: w, height: h, mipmapped: false)
            bd.usage = [.shaderRead]
            bd.storageMode = .private
            guard let bt = device.makeTexture(descriptor: bd) else { return false }
            bt.label = "VR pantalla 3D (base ojo \(e))"
            base.append(bt)
        }
        stereoBase = base; stereoBaseValid = false; stereoBaseStale = false; stereoHas2D = false
        stereoTex = t; stereoRaw = raw ?? t; stereoSlices = slices; stereoMSAAColor = msaa; stereoDepth = d; stereoKey = key
        stereoDirty = true
        return true
    }


    /// La escena es una cinemática del motor (VRV_K_ENGINE, view_kind 2): la fija XRFrameEncoder antes de `encode`. Fuerza la colocación desde la cámara del juego.
    var engineView = false
    /// La escena nativa se muestra en una pantalla (pantalla 3D o cine): único caso con descarte de reversos (ver NativeRenderer.screenMode).
    var screenMode = false
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
        if !gpuMode, let n = native, n.camH > 8 {
            tx = 160 / n.camH; ty = 120 / n.camH
            let dx = (n.camOfx - 160) / n.camH, dy = (n.camOfy - 120) / n.camH
            sh = SIMD4<Float>(dx, -dx, dy, -dy)
        }
#endif
        if gpuMode, let w = stereoWindow {
            // Modo gpu: la ventana REAL de la escena (H y OFX/OFY de la lista sobre la imagen mostrada), la misma que cubre el panel 2D: el 3D registra con él.
            tx = 0.5 * (w.x + w.y); ty = 0.5 * (w.z + w.w)
            let dx = 0.5 * (w.x - w.y), dy = 0.5 * (w.z - w.w)
            sh = SIMD4<Float>(dx, -dx, dy, -dy)
        }
        guard let cam = XRStereo.eye(gameView: gameView, eye: e, ipd: r.ipd, conv: r.convergence, tanX: tx, tanY: ty) else { return nil }
        let t = cam.tangents + sh
        guard t.x > 0.01, t.y > 0.01, t.z > 0.01, t.w > 0.01 else { return nil }
        return (cam.view, t)
    }

    /// Pantalla 3D: el render nativo desde la cámara del juego (A, t, H, OFX/OFY, sobredimensión 66/64: ns_cinema), un pase por ojo con cámaras paralelas y ventana desplazada. Se vuelve
    /// a dibujar solo si cambió la escena nativa, la petición o el fundido de la presentación; si no, las texturas valen. Con el render nativo no listo no se toca la imagen anterior.
    private func encodeStereo(_ cb: MTLCommandBuffer, _ r: StereoRequest, gotNew: Bool) {
        if gpuMode { encodeStereoGPU(cb, r, gotNew: gotNew); return }
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
        stereoPassDiag = "\(auxPassDiag) tris/ojo=\(n.statTrisEye) dibujos=\(n.statDraws)"
        stereoSeq = n.sceneSeq
        stereoFade = fade
#endif
        stereoRendered = true
        stereoDirty = false
    }

    /// Pantalla 3D en el modo gpu: la lista de la GPU (todos los lotes 3D, con la VRAM tal como queda tras los eventos de la lista) desde las dos cámaras estéreo de la petición. Una lista
    /// sin 3D (fundido, texto) conserva la imagen anterior; si no hay cámara o texturas, `stereoComposed` baja (no se compone nada nuevo).
    private func encodeStereoGPU(_ cb: MTLCommandBuffer, _ r: StereoRequest, gotNew: Bool) {
        guard ensureStereo(r.width, r.height), let d = stereoDepth, stereoSlices.count == 2, stereoBase.count == 2 else { stereoComposed = false; return }
        logStereo()
        let has2D = batches.contains { $0.kind == 2 }
        // Lista SIN 3D con una base guardada: el 3D de la base vale (no se pinta una escena vacía encima); solo se rehace SU 2D (o se quita el de la lista anterior).
        let useBase = !has3D && stereoBaseValid
        let redraw3D = (gotNew && has3D) || ((stereoDirty || !stereoRendered) && !useBase)
        let redo2D = !redraw3D && useBase && ((gotNew && (has2D || stereoHas2D)) || stereoDirty)
        if !redraw3D && !redo2D { return }
        // Sin ventana de ESTA lista no se dibuja un fotograma estéreo nuevo con 3D (registraría mal con los lotes 2D): se conserva el anterior entero (3D + 2D juntos).
        if redraw3D && has3D && stereoWindow == nil { if !stereoRendered { stereoComposed = false }; return }
        if !redraw3D {
            // base -> texturas estéreo (3D sin 2D) y encima el 2D de ESTA lista; si la base aún no se guardó, las texturas ya son el 3D sin 2D: se copian a la base y se pinta encima
            if stereoBaseStale { copyStereoBase(cb, toBase: true); stereoBaseStale = false } else { copyStereoBase(cb, toBase: false) }
            encodeStereo2D(cb)
            stereoHas2D = has2D
            stereoComposed = true
            stereoDirty = false
            return
        }
        // Onda por fila del agua (filter.dv): la escena se resuelve en texturas intermedias y un pase por ojo la copia a la pantalla desplazando las filas (encodePost con warpOnly).
        // El tinte y los recubrimientos NO se hornean aquí: los aplica la pantalla al presentar (screenFx), también en listas sin 3D (fundidos).
        let pre: [MTLTexture]? = (fx.nrows > 0 && screenFilterEnabled && ensureFrameTextures() && postPipeline(format: colorFormat, layered: false) != nil) ? stereoPreTextures() : nil
        var projs: [simd_float4x4] = []
        for e in 0..<2 {
            guard let cam = stereoCamera(e, r) else { stereoComposed = false; return }
            var u = VrSceneUniforms()
            let proj = XRMath.projection(tangents: cam.tangents, near: 0.05, far: 800)
            projs.append(proj)
            let m = proj * cam.view
            u.view_proj = (m, m)
            u.head = SIMD4<Float>(0, 0, 0, 1)
            let target = pre?[e] ?? stereoSlices[e]
            let color = stereoMSAAColor ?? target
            _ = encodeScene(cb, color: color, depth: d, resolve: stereoMSAAColor != nil ? target : nil, uniforms: u, amplify: 0, viewIndex: 0,
                            views: [], projections: [], isAux: true, last: true, cams: nil, first: true, gpuRange: 0..<batches.count)
        }
        if let p = pre { encodePost(cb, PostPlan(sceneColor: p, sceneResolve: [], fxTex: p, dst: stereoSlices), projections: projs, layered: false, warpOnly: true) }
        if has2D { copyStereoBase(cb, toBase: true); stereoBaseStale = false } else { stereoBaseStale = true }   // base = 3D compuesto (con onda) sin 2D: solo se copia si esta lista pinta 2D encima; si no, la textura ya es la base
        stereoBaseValid = true
        encodeStereo2D(cb)
        stereoHas2D = has2D
        stereoComposed = true
        stereoRendered = true
        stereoDirty = false
    }

    /// Copia por ojo entre la capa de la textura de la pantalla 3D y `stereoBase` (toBase: capa -> base; si no, base -> capa).
    private func copyStereoBase(_ cb: MTLCommandBuffer, toBase: Bool) {
        guard let tex = stereoTex, stereoBase.count == 2, let blit = cb.makeBlitCommandEncoder() else { return }
        let size = MTLSize(width: tex.width, height: tex.height, depth: 1)
        let o0 = MTLOrigin(x: 0, y: 0, z: 0)
        for e in 0..<2 {
            if toBase {
                blit.copy(from: tex, sourceSlice: e, sourceLevel: 0, sourceOrigin: o0, sourceSize: size, to: stereoBase[e], destinationSlice: 0, destinationLevel: 0, destinationOrigin: o0)
            } else {
                blit.copy(from: stereoBase[e], sourceSlice: 0, sourceLevel: 0, sourceOrigin: o0, sourceSize: size, to: tex, destinationSlice: e, destinationLevel: 0, destinationOrigin: o0)
            }
        }
        blit.endEncoding()
    }

    /// Lotes 2D (kind 2) de la lista actual, dentro de las dos texturas de la pantalla 3D, tras el 3D y la onda del agua (el 2D no se deforma): vr_vs_hud con hudRect de la MISMA lista que los lotes 3D cubre
    /// toda la textura, porque la ventana del 3D es exactamente la imagen mostrada. Un pase por ojo sobre la textura ya resuelta (1 muestra, sin profundidad: un pipeline sin profundidad no puede
    /// ir en el pase con profundidad de la escena). Semitransparentes: mezcla en gamma en el shader (vr_fs_scene_blend).
    private func encodeStereo2D(_ cb: MTLCommandBuffer) {
        let seg = batches.indices.filter { batches[$0].kind == 2 }
        guard haveScene, !seg.isEmpty, stereoSlices.count == 2, let buf = vtxSlots[curSlot].buf else { return }
        let stride = MemoryLayout<VrGpuVtx>.stride
        for e in 0..<2 {
            let tex = stereoSlices[e]
            let pd = MTLRenderPassDescriptor()
            pd.colorAttachments[0].texture = tex
            pd.colorAttachments[0].loadAction = .load
            pd.colorAttachments[0].storeAction = .store
            guard let enc = cb.makeRenderCommandEncoder(descriptor: pd) else { continue }
            enc.label = "pantalla 3D: 2D (ojo \(e))"
            statPasses += 1
            enc.setCullMode(.none)
            enc.setViewport(MTLViewport(originX: 0, originY: 0, width: Double(tex.width), height: Double(tex.height), znear: 0, zfar: 1))
            var hu = VrHudUniforms(); hu.rect = hudRect
            enc.setVertexBuffer(buf, offset: 0, index: Int(VR_BUF_VERTS))
            enc.setVertexBuffer(buf, offset: vtxSlots[curSlot].stOff, index: Int(VR_BUF_STATES))
            enc.setVertexBytes(&hu, length: MemoryLayout<VrHudUniforms>.stride, index: Int(VR_BUF_UNI))
            enc.setFragmentTexture(vram, index: 0)
            for i in seg {
                let bt = batches[i]
                enc.setVertexBufferOffset(bt.first * stride, index: Int(VR_BUF_VERTS))
                func draw(_ pass: UInt32, _ mode: Int) {
                    let k = PKey(hud: true, layered: false, filter: textureFilter, linear: linearOutput, pass: pass, mode: mode,
                                 color: tex.pixelFormat, depth: .invalid, samples: 1)
                    guard let p = pipeline(k) else { return }
                    enc.setRenderPipelineState(p)
                    enc.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: bt.count)
                    statDraws += 1
                }
                if bt.semi {
                    draw(bt.hasTex ? 1 : 0, bt.mode)
                    if bt.hasTex { draw(2, -1) }   // texels sin bit15: opacos
                } else {
                    draw(0, -1)
                }
            }
            enc.endEncoding()
        }
    }

    /// Registro `[estereo]` (1 por segundo, mientras haya pantalla 3D en el modo gpu): ventana L,R,U,D de la última lista, si es válida, H de la proyección, hudRect y qué listas llegaron en el fotograma.
    private func logStereo() {
        let now = DispatchTime.now().uptimeNanoseconds
        if now - stereoLogNs < 1_000_000_000 { return }
        stereoLogNs = now
        let w = stereoWindow
        func f(_ x: Float?) -> String { x.map { String(format: "%.4f", $0) } ?? "-" }
        func id(_ x: UInt32?) -> String { x.map { String($0) } ?? "-" }
        print("[estereo] L=\(f(w?.x)) R=\(f(w?.y)) U=\(f(w?.z)) D=\(f(w?.w)) ok=\(w != nil) proj_h=\(String(format: "%.1f", fx.projH)) hudRect=(\(hudRect.x),\(hudRect.y),\(hudRect.z),\(hudRect.w)) listas=\(diagLists) lista3D=\(id(diagList3D)) lista2D=\(id(diagList2D)) tiene3D=\(has3D) dentro2D=\(gpuMode)")
    }

    private var stereoPre: [MTLTexture] = []
    private var stereoPreKey = ""
    /// Dos texturas 2D (una por ojo) con la forma y el formato de las capas de la pantalla 3D: la escena sin onda, antes del pase de post. Se recrean si cambia la pantalla 3D.
    private func stereoPreTextures() -> [MTLTexture]? {
        if stereoPreKey == stereoKey, stereoPre.count == 2 { return stereoPre }
        guard let ref = stereoSlices.first else { return nil }
        var out: [MTLTexture] = []
        for e in 0..<2 {
            let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: colorFormat, width: ref.width, height: ref.height, mipmapped: false)
            d.usage = [.renderTarget, .shaderRead]
            d.storageMode = .private
            guard let t = device.makeTexture(descriptor: d) else { return nil }
            t.label = "VR pantalla 3D (ojo \(e), sin onda)"
            out.append(t)
        }
        stereoPre = out
        stereoPreKey = stereoKey
        return out
    }

    /// Segunda vista (mira óptica / cámara del misil): el render nativo (o, en el modo gpu, la lista de la GPU) desde la cámara pedida, a la textura auxiliar.
    private func encodeAux(_ cb: MTLCommandBuffer, _ a: AuxView) {
        if gpuMode {
            guard haveScene, has3D, !batches.isEmpty, ensureAux(a.width, a.height), let c = auxColor, let d = auxDepth else { return }
            var u = VrSceneUniforms()
            let m = a.proj * a.view
            u.view_proj = (m, m)
            u.head = a.pos
            _ = encodeScene(cb, color: c, depth: d, resolve: nil, uniforms: u, amplify: 0, viewIndex: 0, views: [], projections: [], isAux: true, last: false, cams: nil,
                            first: true, gpuRange: 0..<batches.count)
            return
        }
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
    private var auxPassDiag = ""
    /// Diagnóstico [cine3d]: pase de la pantalla 3D (cull, clear/compare de profundidad, viewport) y triángulos/dibujos enviados por ojo.
    private(set) var stereoPassDiag = "sin pase"

    /// El mundo de los ojos: un pase por ojo (o uno con amplificación a 2 vistas), SOLO con el render nativo.
    private func encodeScenePasses(_ cb: MTLCommandBuffer, range: Range<Int>? = nil, first: Bool = true, last: Bool = true,
                                   colorTargets: [MTLTexture], depthTargets: [MTLTexture], resolveTargets: [MTLTexture],
                                   views: [simd_float4x4], projections: [simd_float4x4], head: simd_float4x4, layered: Bool) {
        let n = min(views.count, projections.count)
        // Modo gpu: los lotes 3D de `range` (nil = todos) se dibujan solo si hay mundo y ojos; si no, el pase se limpia igual (primer grupo) y no dibuja.
        let gr: Range<Int>? = (gpuMode && drawWorld && drawEyes) ? (range ?? 0..<batches.count) : nil
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
                            isAux: false, last: last, cams: cams, first: first, gpuRange: gr)
        } else {
            for i in 0..<min(colorTargets.count, depthTargets.count) where i < n {
                var u = VrSceneUniforms()
                let m = projections[i] * views[i]
                u.view_proj = (m, m)
                u.head = head.columns.3
                let cams = draw ? NativeCams(viewProj: [m], projections: [projections[i]], eyes: [eyePos(views[i])], eyeIndex: i) : nil
                _ = encodeScene(cb, color: colorTargets[i], depth: depthTargets[i], resolve: i < resolveTargets.count ? resolveTargets[i] : nil, uniforms: u,
                                amplify: 0, viewIndex: i, views: views, projections: projections, isAux: false, last: last, cams: cams, first: first, gpuRange: gr)
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
        guard fx.needsPost, screenFilterEnabled, drawWorld, drawEyes, drawBody, haveScene, has3D, viewportOverride == nil, rasterizationRateMap == nil else { return nil }
        let msaa = (colorTargets.first?.sampleCount ?? 1) > 1
        let dst = msaa ? resolveTargets : colorTargets
        guard let first = dst.first, postPipeline(format: first.pixelFormat, layered: layered) != nil,
              ensureFrameTextures(), let tex = filterTextures(like: dst) else { return nil }
        if msaa { return PostPlan(sceneColor: colorTargets, sceneResolve: tex, fxTex: tex, dst: dst) }
        return PostPlan(sceneColor: tex, sceneResolve: [], fxTex: tex, dst: dst)
    }

    /// Pase de post-proceso por ojo: lee la escena resuelta (fxTex) y escribe el drawable con la onda por fila, el tinte y el recubrimiento.
    /// Parámetros por ojo a partir de la proyección: fila del punto principal = H/2*(1+P[2][1]) y focal vertical = H/2*P[1][1] (px).
    /// `warpOnly`: pantalla 3D de las cinemáticas (2 ojos sueltos): solo la onda por fila; sin tinte, recubrimientos, líneas ni máscara (los pone la pantalla al presentar).
    private func encodePost(_ cb: MTLCommandBuffer, _ plan: PostPlan, projections: [simd_float4x4], layered: Bool, warpOnly: Bool = false) {
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
        if warpOnly {
            u.tint = SIMD4<Float>(1, 1, 1, 0)
            let off = SIMD4<Float>(0, 0, 0, -1)
            u.covers = (off, off, off, off)
            u.scan = SIMD4<Float>(0, 0, 0, -1)
            u.frame = SIMD4<Float>(0, 0, 0, 0)
        }
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
            statPost += 1
        } else {
            for i in 0..<min(plan.dst.count, plan.fxTex.count, warpOnly ? 2 : viewCount, projections.count) {
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
                statPost += 1
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
                             views: [simd_float4x4] = [], projections: [simd_float4x4] = [], isAux: Bool = false, last: Bool, cams: NativeCams?,
                             first: Bool = true, gpuRange: Range<Int>? = nil) -> Bool {
        // Modo gpu: lotes 3D (kind 0/1) del rango, en el orden de envío. Un grupo intermedio sin lotes 3D no abre pase.
        let seg: [Int] = gpuRange.map { r in r.filter { batches.indices.contains($0) && batches[$0].kind != 2 } } ?? []
        if gpuMode && !first && !last && seg.isEmpty { return false }
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
        if isAux {   // diagnóstico [cine3d]: cómo se abrió el pase de la pantalla 3D / segunda vista (el compare de profundidad es el de NativeRenderer.dsOpaque)
            auxPassDiag = "cull=none color=\(ca.loadAction == .clear ? "clear" : "load") prof=\(da.loadAction == .clear ? "clear" : "load")(\(da.clearDepth)) compare=greaterEqual(Z invertida) viewport=\(Int(vp0.width))x\(Int(vp0.height)) muestras=\(samples)"
        }
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
        if gpuRange != nil {   // modo gpu: la lista de la GPU, con las mismas vistas y proyecciones por ojo (profundidad inversa del sistema: no se escribe depthRange)
            drawGPU(enc, seg, color: color, depth: depth, uniforms: uniforms, amplify: amplify)
            return true
        }
#if !MV_BENCH || MV_BENCH_NATIVE
        // El mundo: render nativo con las mismas vistas y proyecciones por ojo y la profundidad inversa del sistema (no se escribe depthRange). Si falla, el pase queda limpio y el
        // fotograma siguiente se repite (ns_present: último válido) o se funde a negro; se cuenta para el registro.
        if let cm = cams, let n = native {
            if let b = boxMaskIn { n.setBoxMask(frame: b.frame, w: Int(VR_FRAME_W), h: Int(VR_FRAME_H), area: b.area, cxCol: b.cxCol, cyRow: b.cyRow, projH: b.projH, rgb: b.rgb) } else { n.clearBoxMask() }
            let ok = n.encode(encoder: enc, vram: vram, viewProj: cm.viewProj, projections: cm.projections, eyes: cm.eyes, eyeIndex: cm.eyeIndex, layered: amplify > 0,
                              colorFormat: color.pixelFormat, depthFormat: depth.pixelFormat, samples: samples, linear: linearOutput)
            if !ok { nativeEncodeFails += 1; nativeDrawnLast = false }
            return ok
        }
#endif
        return false
    }

    /// Modo gpu: dibuja los lotes 3D `seg` (índices de `batches`, en el orden de envío) en el codificador del pase. Opaco (kind 0): profundidad con prueba y escritura. Semitransparente (kind 1):
    /// prueba sin escritura, mezcla del modo en el shader (VR_FC_BLEND, en gamma) y, si el lote lleva texturas, segunda pasada para los texels sin bit 15 (opacos). Sin descarte de reversos (cull .none, ya fijado).
    private func drawGPU(_ enc: MTLRenderCommandEncoder, _ seg: [Int], color: MTLTexture, depth: MTLTexture, uniforms: VrSceneUniforms, amplify: Int) {
        guard haveScene, has3D, !seg.isEmpty, let buf = vtxSlots[curSlot].buf else { return }
        var u = uniforms
        enc.setVertexBuffer(buf, offset: 0, index: Int(VR_BUF_VERTS))
        enc.setVertexBuffer(buf, offset: vtxSlots[curSlot].stOff, index: Int(VR_BUF_STATES))
        enc.setVertexBytes(&u, length: MemoryLayout<VrSceneUniforms>.stride, index: Int(VR_BUF_UNI))
        enc.setFragmentTexture(vram, index: 0)
        let stride = MemoryLayout<VrGpuVtx>.stride
        let samples = color.sampleCount
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
        var curKind = -1
        for i in seg {
            let bt = batches[i]
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
                draw(bt, key(bt.hasTex ? 1 : 0, bt.mode))
                if bt.hasTex { draw(bt, key(2, -1)) }
            }
        }
    }
}
