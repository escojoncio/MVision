import Foundation
import Metal
import simd

// Renderer nativo: dibuja la escena leída de la RAM del motor (NsScene, vr/native/ns_types.h) con las cámaras VR, en lugar de la salida de la GPU.
//
// Flujo por fotograma de visor (100 Hz), ver la documentación de la clase NativeRenderer:
//   beginFrame(commandBuffer:sortView:draw:)  toma las dos últimas NsScene del puente (ns_bridge_acquire), sube mallas, hace ns_store_ack, tabla de piezas.
//                                             Una escena no utilizable NO se dibuja: se conserva lo preparado de la última válida (se repite con la cabeza actual).
//   present(ready:)                           ns_present_step: dibujar la actual, repetir la última válida o fundir a negro. NO existe un modo «GPU»: el 3D del mundo sale SOLO de aquí.
//   encode(encoder:...)                       escenario en 1-2 llamadas, dinámicos por pieza, UNA pasada semitransparente ordenada con mallas y efectos intercalados,
//                                             y los efectos de pantalla (agua en el mundo; fundidos, destello, modos, anillo y barras como recubrimientos del ojo).
// Layout de búferes y shaders: vr/native/ns_gpu_types.h y NativeShaders.metal; empaquetado en C: vr/native/ns_gpu.c.
// Unidades: el mundo del motor (1 ≈ 1 mm, Y arriba) pasa a metros VR con NsScene.to_vr dentro del shader; las matrices de vista/proyección son las de
// Metal (Z invertida, plano lejano infinito: la profundidad se usa tal cual la da el sistema, no se reescribe).

/// Asignador de rangos con lista libre (primer ajuste, con fusión de vecinos).
struct NativeRangeAllocator {
    private(set) var capacity: Int
    private(set) var used = 0
    private var free: [(start: Int, count: Int)]

    init(capacity: Int) {
        self.capacity = capacity
        free = [(0, capacity)]
    }

    mutating func alloc(_ n: Int) -> Int? {
        guard n > 0 else { return nil }
        for i in 0..<free.count where free[i].count >= n {
            let s = free[i].start
            if free[i].count == n { free.remove(at: i) } else { free[i] = (s + n, free[i].count - n) }
            used += n
            return s
        }
        return nil
    }

    private mutating func insertFree(_ start: Int, _ n: Int) {
        var i = 0
        while i < free.count && free[i].start < start { i += 1 }
        free.insert((start, n), at: i)
        if i + 1 < free.count && free[i].start + free[i].count == free[i + 1].start {
            free[i].count += free[i + 1].count
            free.remove(at: i + 1)
        }
        if i > 0 && free[i - 1].start + free[i - 1].count == free[i].start {
            free[i - 1].count += free[i].count
            free.remove(at: i)
        }
    }

    mutating func release(_ start: Int, _ n: Int) {
        guard n > 0 else { return }
        used -= n
        insertFree(start, n)
    }

    mutating func grow(to newCapacity: Int) {
        guard newCapacity > capacity else { return }
        insertFree(capacity, newCapacity - capacity)
        capacity = newCapacity
    }
}

/// Búfer compartido que crece (se reasigna y se copia; los fotogramas en vuelo conservan el antiguo porque Metal lo retiene).
private final class NativePool {
    let device: MTLDevice
    let stride: Int
    let maxElems: Int
    let label: String
    private(set) var buffer: MTLBuffer
    private(set) var alloc: NativeRangeAllocator

    init?(device: MTLDevice, stride: Int, initial: Int, maxElems: Int, label: String) {
        self.device = device; self.stride = stride; self.maxElems = maxElems; self.label = label
        alloc = NativeRangeAllocator(capacity: initial)
        guard let b = device.makeBuffer(length: max(stride * initial, 16), options: .storageModeShared) else { return nil }
        b.label = label
        buffer = b
    }

    func allocate(_ n: Int) -> Int? {
        if let s = alloc.alloc(n) { return s }
        var cap = alloc.capacity
        while cap < maxElems {
            cap = min(max(cap * 2, cap + n), maxElems)
            guard grow(to: cap) else { return nil }
            if let s = alloc.alloc(n) { return s }
        }
        return nil
    }

    private func grow(to cap: Int) -> Bool {
        guard let nb = device.makeBuffer(length: max(stride * cap, 16), options: .storageModeShared) else { return false }
        nb.label = label
        memcpy(nb.contents(), buffer.contents(), min(buffer.length, nb.length))
        buffer = nb
        alloc.grow(to: cap)
        return true
    }

    func release(_ start: Int, _ n: Int) { alloc.release(start, n) }
}

/// Clave de la caché de GPU: id de contenido de la malla + versión del juego de colores precalculados que pidió la pieza (0 = ninguno).
private struct NativeMeshKey: Hashable { let id: UInt64; let ver: UInt32 }

/// Malla en la caché de GPU.
private final class NativeGpuMesh {
    let id: UInt64
    var vertStart = 0, nverts = 0
    var triStart = 0, ntri = 0
    var flags: UInt32 = 0
    var depthBias: Int16 = 0
    var semi = false
    var opaquePass = true
    var flat = false          // ninguna cara texturizada ni semitransparente: pipeline sin texel ni descarte (early-z completo)
    var bakedOK = true        // lleva el juego de colores que pide la pieza (o no pide ninguno)
    var centroid: UnsafeMutablePointer<Float>?   // 3 por triángulo (solo semitransparentes)
    var abr: UnsafeMutablePointer<UInt8>?        // modo de mezcla por triángulo
    var lastUsed: UInt32 = 0
    var bmin = SIMD3<Float>(0, 0, 0), bmax = SIMD3<Float>(0, 0, 0)   // caja local (unidades del motor), solo para el registro de diagnóstico
    var restR: Float = 0      // radio de reposo (máx. |v| local, unidades del motor): referencia de la red de seguridad de vértices
    init(id: UInt64) { self.id = id }
    deinit { centroid?.deallocate(); abr?.deallocate() }
}

/// Renderer nativo: dibuja la escena leída de la RAM del motor (NsScene, vr/native/ns_types.h) con las cámaras VR.
///
/// HILO: todos los métodos se llaman desde el MISMO hilo, el que codifica el fotograma (RGRenderer.encode). Excepción: el manejador de finalización de la GPU
/// (`addCompletedHandler`), que solo toca contadores protegidos por `lock`. Con `queue` se comprueba con dispatchPrecondition; sin ella se registra el primer hilo
/// que llama y se avisa una vez si otro lo hace.
///
/// Flujo por fotograma:
///   beginFrame(commandBuffer:sortView:draw:)  SIEMPRE (también en cinemáticas y menús, para reconocer épocas): toma las dos últimas escenas del puente
///                       (ns_bridge_acquire), sube las mallas nuevas, hace ns_store_ack y construye la tabla de piezas con la interpolación. Con draw = true además
///                       reserva una ranura del anillo (espera a que la GPU termine la que se reutiliza) y ordena los semitransparentes.
///   present(ready:)     qué se enseña (ns_present_step): la escena, la última válida repetida o negro. No existe un modo GPU.
///   encode(...)         dentro del pase de escena abierto (el llamador fija viewports y amplificación): estático en 1-2 llamadas, dinámicos por pieza
///                       (índice de pieza = instance_id), semitransparentes ordenados atrás-adelante. Sin culling. Sin escribir depthRange.
/// Los rangos de los pools y las ranuras solo se reutilizan cuando la GPU terminó los comandos que los usaban (contadores de finalización, no de fotogramas).
final class NativeRenderer {
    // MARK: configuración
    static let maxSemiTris = 1 << 17
    static let maxStaticTris = 1 << 21
    static let sortHeadThreshold: Float = 0.03     // m: la cabeza debe moverse más que esto para reordenar los semitransparentes
    private let device: MTLDevice
    private let library: MTLLibrary
    private let ringSize: Int
    private let queue: DispatchQueue?

    /// Puente de escenas (NsBridge de vr/native/ns_bridge.h; la app usa `mv_vrhost_native_bridge()`). Sin puente no se dibuja nada.
    var bridge: OpaquePointer?
    var enabled = true
    /// Factor de color por canal del dibujo nativo ((1,1,1) = sin teñir). Lo pone el llamador antes de `encode` (modo de depuración «solo nativo»).
    var tint = SIMD3<Float>(1, 1, 1)
    /// Diagnóstico: umbral (m de mundo VR) para partir en CPU los triángulos con una arista más larga al subir cada malla (0 = no partir). Cambiarlo vuelve a subir todas las mallas.
    var splitMeters: Float = 0
    /// Diagnóstico: sin prueba de profundidad (opacos: siempre pasan y escriben; semitransparentes: siempre pasan, sin escribir).
    var noZTest = false
    /// Sesgo de profundidad de los efectos (decals, sombras, ondas pegados a una superficie): mismo orden que el de los semitransparentes de la ruta de la GPU (Z inversa: + = hacia la cámara).
    var fxDepthBias: Float = 4, fxSlopeBias: Float = 2
    private var appliedSplit: Float = 0
    private var splitLocalEdge: Float = 0            // el umbral en unidades locales del motor (mismo valor para todas las mallas de la escena)
    private static let splitExtraPerMesh = 50_000    // tope de seguridad: triángulos añadidos como mucho por malla
    private static let splitExtraTotal = 600_000     // y en total mientras dure el ajuste
    private var splitMeshStat = 0, splitTrisStat = 0, splitAddedStat = 0, splitWeldStat = 0, splitCapStat = 0, splitFailStat = 0

    // MARK: sincronización con la GPU
    private let lock = NSLock()
    private var completedSerial: UInt64 = 0      // lock
    private var slotBusy: [Bool]                  // lock
    private let slotSem: DispatchSemaphore
    private var serial: UInt64 = 0                // solo el hilo de render

    // MARK: caché de mallas
    private var vertPool: NativePool
    private var triPool: NativePool            // unidad = triángulo (3 esquinas)
    private var meshes: [NativeMeshKey: NativeGpuMesh] = [:]
    private var pendingFree: [(serial: UInt64, vs: Int, nv: Int, ts: Int, nt: Int)] = []
    private var meshesChanged = false
    private var frameFailures = 0                // mallas que no cupieron en este fotograma (se reintentan en el siguiente)
    private var frameMisses = 0                  // mallas pedidas por la escena y ausentes del almacén
    private var missStreak = 0                   // escenas nuevas seguidas con mallas ausentes
    private var missTolerated = false            // las ausentes llevan >= missGrace escenas seguidas: no van a llegar (otro modelo de la zona); se dibuja lo que hay y el relleno cubre el resto
    private static let missGrace = 25            // ~1 s de escenas: lo transitorio (mallas que aún se hornean) sigue conservando la escena anterior
    private var totalFailures = 0

    // MARK: red de seguridad de vértices (ver ns_gpu_types.h NSG_NET_*): contadores desde la última línea de registro y detalle por (instancia, pieza, malla)
    private struct NetKey: Hashable { let inst: UInt32; let piece: Int; let mesh: UInt64 }
    private struct NetRec { var n = 0; var causes: UInt32 = 0; var tris: UInt32 = 0; var gap: Float = 0; var far: Float = 0; var edge: Float = 0; var scale: Float = 0 }
    private var netRecs: [NetKey: NetRec] = [:]
    private var netScenes = 0, netPieces = 0, netPiecesMat = 0, netPiecesWeld = 0, netTrisCollapsed = 0, netTrisFar = 0, netTrisEdge = 0, netTrisSeen = 0

    // MARK: escena preparada (búferes persistentes: nada se reserva por fotograma salvo crecimiento)
    private struct DrawItem { var piece: Int; var triStart: Int; var ntri: Int }
    private struct SemiItem { var piece: Int; var mesh: NativeGpuMesh }
    private var table: [NsGpuPiece] = []
    private var pieceCount = 0
    private var meshOf: [NativeGpuMesh?] = []
    private var dynTex: [DrawItem] = []
    private var dynFlat: [DrawItem] = []
    private var semis: [SemiItem] = []
    private var statPcs: [(piece: Int, mesh: NativeGpuMesh)] = []
    private var staticTex: [NsGpuOrder] = []
    private var staticFlat: [NsGpuOrder] = []
    private var staticDirty = true
    private var staticGen = 0
    private var toVR = matrix_identity_float4x4
    private var camEye = SIMD3<Float>(0, 0, 0)     // ojo de la cámara del motor (mundo del motor) de la escena actual
    private var toVR12 = [Float](repeating: 0, count: 12)   // to_vr de la escena actual (3x4 fila-mayor), tal cual se usa
    private var camRot9 = [Float](repeating: 0, count: 9)   // vista -> mundo del motor de la cámara de la escena actual (fila-mayor)
    private var curValid: UInt32 = 0                         // NsScene.valid de la escena actual
    /// Cabeza (mundo VR, m) de este fotograma: la pone el llamador antes de beginFrame (solo diagnóstico).
    var headVR = SIMD3<Float>(0, 0, 0)
    /// Cinemática del motor: `VrScene.game_view` (vista del juego -> mundo VR, columnas). Si no es nil, la escena se coloca en el mundo VR desde la CÁMARA DEL JUEGO de la escena nativa
    /// (ns_cinema_to_vr) en lugar de con el to_vr de primera persona: es la que dibuja la pantalla 3D de la cinemática (dos ojos paralelos, ver `encode(eyeIndex:)`). Lo fija el llamador antes de beginFrame.
    var cinemaGameView: simd_float4x4?
    private(set) var sceneSeq: UInt32 = 0                    // secuencia de la escena con la que se preparó el fotograma (sube con cada escena válida)
    // presentación: repetir el último fotograma válido o fundir a negro (vr/native/ns_present.h)
    private var presentState = NsPresentState()
    private var presentCfg = ns_present_cfg_default()
    private(set) var lastPresent = NsPresentOut()
    private var curFit = false, curMeshesOK = false, curHaveScene = false, curSceneT: UInt64 = 0
    private var coherentOK = true                            // red de coherencia con la lista de la GPU: SOLO registro
    /// Factor de color del fundido a negro (1 = imagen entera).
    private var presentFade: Float = 1
    // efectos de pantalla (vr/native/ns_screen.h): agua en el mundo, recubrimientos del ojo
    private var screenFx = NsScreenFx()
    /// La abertura de la mira sustituye a la máscara de visión (anillo) del juego: no se dibuja el anillo.
    var suppressRing = false
    /// Interruptor de los efectos de pantalla (agua, fundidos, destello, modos de visión) para diagnóstico; false = ninguno.
    var screenEffectsEnabled = true
    private var screenCfg = ns_screen_cfg_default()
    private var screenState = NsScreenState()
    private var screenOut = NsGpuScreen()
    private var overlays: [NsGpuOverlay] = []
    // Máscara de visión de la caja (NSG_OV_FRAME): resta por celda 160x128 anclada a la cabeza; solo activa con el jugador en la caja (la pone RenderRG antes de dibujar).
    private var boxOverlays: [NsGpuOverlay] = []
    private var boxTex: [MTLTexture] = []
    private var boxTexCur = 0, boxTexNext = 0
    private var boxSum: UInt64 = 0
    private var dummyTex: MTLTexture?      // R8 1x1 a 0: la textura 0 de ns_fs_overlay nunca queda sin enlazar (solo la lee NSG_OV_FRAME)
    /// Activa la máscara de la caja: `frame` = w*h bytes (resta 0..255), `area` = imagen mostrada del motor (px), centro óptico (cxCol, cyRow), H del motor y color del interior cerrado. false = no se pudo (sin máscara).
    @discardableResult
    func setBoxMask(frame: [UInt8], w: Int, h: Int, area: (Float, Float), cxCol: Float, cyRow: Float, projH: Float, rgb: (Float, Float, Float)) -> Bool {
        boxOverlays.removeAll(keepingCapacity: true)
        guard w == 160, h == 128, frame.count >= w * h, ensureBoxTex() else { return false }
        var sum: UInt64 = 1469598103934665603      // FNV-1a: se sube a una textura nueva del anillo solo si cambió
        for b in frame[0..<(w * h)] { sum = (sum ^ UInt64(b)) &* 1099511628211 }
        if sum != boxSum {
            let idx = boxTexNext % boxTex.count
            frame.withUnsafeBytes { boxTex[idx].replace(region: MTLRegionMake2D(0, 0, w, h), mipmapLevel: 0, withBytes: $0.baseAddress!, bytesPerRow: w) }
            boxTexNext = (idx + 1) % boxTex.count
            boxTexCur = idx
            boxSum = sum
        }
        var rgbv: [Float] = [rgb.0, rgb.1, rgb.2]
        var out = [NsGpuOverlay](repeating: NsGpuOverlay(), count: 2)
        let n = rgbv.withUnsafeMutableBufferPointer { rp in out.withUnsafeMutableBufferPointer { op in ns_screen_frame_build(area.0, area.1, cxCol, cyRow, projH, rp.baseAddress, op.baseAddress) } }
        guard n == 2 else { return false }
        boxOverlays = out
        return true
    }
    /// Onda de pantalla bajo el agua para el post-proceso por ojo de RGRenderer (planWave / vr_fs_wave): (amplitud en px del ojo a 1888 de ancho, rad por fila del motor, fase en rad, H del motor), o nil si no hay onda
    /// (cabeza fuera del agua o efectos de pantalla apagados: ns_screen_build deja water_wave a 0); se calcula en beginFrame.
    var waveParams: SIMD4<Float>? {
        let w = screenOut.water_wave
        guard screenEffectsEnabled, w.0 != 0, w.0.isFinite, w.1.isFinite, w.2.isFinite, camH > 8 else { return nil }
        return SIMD4<Float>(w.0, w.1, w.2, camH)
    }
    /// Sin máscara de la caja (el jugador no está en ella).
    func clearBoxMask() { boxOverlays.removeAll(keepingCapacity: true) }
    var boxMaskActive: Bool { !boxOverlays.isEmpty }
    private func ensureDummyTex() -> MTLTexture? {
        if let d = dummyTex { return d }
        let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .r8Unorm, width: 1, height: 1, mipmapped: false)
        d.usage = .shaderRead
        d.storageMode = .shared
        guard let tx = device.makeTexture(descriptor: d) else { return nil }
        tx.label = "native.mascara respaldo"
        var z: UInt8 = 0
        tx.replace(region: MTLRegionMake2D(0, 0, 1, 1), mipmapLevel: 0, withBytes: &z, bytesPerRow: 1)
        dummyTex = tx
        return tx
    }
    private func ensureBoxTex() -> Bool {
        if !boxTex.isEmpty { return true }
        var out: [MTLTexture] = []
        for _ in 0..<4 {
            let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .r8Unorm, width: 160, height: 128, mipmapped: false)
            d.usage = .shaderRead
            d.storageMode = .shared
            guard let tx = device.makeTexture(descriptor: d) else { return false }
            tx.label = "native.mascara caja"
            out.append(tx)
        }
        boxTex = out
        return true
    }
    private var fromVR12: [Float] = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0]
    private var lastScreenNs: UInt64 = 0
    /// H REAL y centro de proyección (OFX/OFY, píxeles de la pantalla de 320x240 del motor) de la cámara de la escena preparada: los radios y filas de los efectos de pantalla y la
    /// ventana de la pantalla 3D de las cinemáticas salen de ellos (no de constantes). OFX/OFY desconocidos (0) = el centro.
    private(set) var camH: Float = 320, camOfx: Float = 160, camOfy: Float = 120
    /// Profundidades (m desde la cámara del juego) de lo que enseña la escena preparada, solo en cinemáticas y suavizadas en el tiempo (ns_cinema_depth_filter; 0 = desconocidas): convergencia y separación de la pantalla 3D (ns_cinema_stereo).
    private(set) var cineNear: Float = 0, cineSubject: Float = 0
    private var cineFilter = NsCinemaDepthFilter()
    /// La última escena del puente es VÁLIDA (partida, listas y escenario completos) pero NO apta para primera persona (sin TOVR_OK: vehículos, ascensores, zonas
    /// especiales). El jugador sin enlazar (sin PLAYER_OK) NO la hace no apta: se dibuja el mundo sin cuerpo, manos ni IK propios. Con histéresis (4 fotogramas para entrar, 3 aptos para salir): entonces se pinta con la cámara del juego.
    private(set) var sceneUnfit = false
    private var cullBack = false
    /// El llamador lo pone antes de beginFrame: la escena se dibuja en una pantalla (pantalla 3D o cine), no en el diorama, dentro de la escena ni en la vista de mundo.
    var screenMode = false
    private var unfitFrames = 0, fitFrames = 0
    private var fogScene = NsFog()
    private var lastView0 = matrix_identity_float4x4         // vista del primer ojo (solo diagnóstico)
    private(set) var statTrisEye = 0, statTrisStatic = 0, statTrisDyn = 0   // triángulos enviados por ojo en el último encode
    private var cohOut = NsCohOut()
    private var cohEvaluated = false
    private var cohWhy = ""
    // red de coherencia con la lista de la GPU (ver checkCoherence): SOLO registro, nunca cambia lo que se dibuja
    private var incoherent = false
    private var cohState = NsCohState()                       // veredicto con tolerancia al desfase de fotogramas (ns_coh_step)
    private var cohCfg: NsCohCfg = { var c = NsCohCfg(); ns_coh_cfg_default(&c); return c }()
    private var cohStats = NsCohStats()                       // acumulados desde la última línea de registro
    private(set) var cohDist: Float = -1
    private var fogFwd = SIMD4<Float>(0, 0, 0, 0)
    private var fogPar = SIMD4<Float>(0, 1, 0, 0)
    // orden de los semitransparentes (persistente; se recalcula solo si cambia la escena o la cabeza se mueve)
    private var semiOrder: UnsafeMutablePointer<NsGpuOrder>
    private var semiAbr: UnsafeMutablePointer<UInt8>
    private var semiDepth: UnsafeMutablePointer<Float>          // profundidad (positiva hacia delante) de cada triángulo ordenado: para intercalar con los efectos
    private var semiN = 0
    private var mergedRuns: [NsGpuSemiRun] = []                  // la ÚNICA pasada semitransparente: mallas (kind 0) y efectos (kind 1) intercalados de atrás adelante
    private var mergedN = 0
    private var fxItemsBuf: [NsGpuFxSemiItem] = []
    private var fxItemCount = 0
    private var semiSrcs: [NsGpuSemiSrc] = []
    private var semiScratch = NsGpuSortScratch(a: nil, b: nil, cap: 0)
    private var semiDirty = true
    private var lastSortHead = SIMD3<Float>(repeating: .infinity)

    // MARK: efectos (carteles, cintas, cuadriláteros y triángulos de mundo de NsScene.effects)
    /// Geometría de los efectos de la escena actual, en el mundo VR y orientada a la cabeza (se construye en beginFrame con ns_gpu_fx_build). El renderer solo da por
    /// dibujado (la lista de la GPU ya no dibuja nada en 3D).
    private var fxEffects: [NsEffect] = []
    private var fxCam = NsCamera()
    private var fxHas = false                      // la escena trae efectos (NS_SV_FX) y es apta para dibujar en el mundo
    private var fxVerts: UnsafeMutablePointer<NsGpuFxVert>?
    private var fxVertCap = 0
    private var fxRunsBuf: [NsGpuFxRun] = []
    private var fxKeysBuf: [UInt64] = []
    private var fxOut = NsGpuFxOut()
    // Registro de efectos cerca de la cabeza (ventana de 1 s): `fxNear` acumula, `fxNearSnap` es la ultima ventana completa (la que se escribe en el registro)
    private var fxNear = NsGpuFxNearAcc(), fxNearSnap = NsGpuFxNearAcc(), fxNearT0: Double = 0
    private var fxAllowed = false                  // este fotograma se dibujarán los efectos (pipelines listas para todos los formatos comprobados)
    private var fxTagsOK = Set<String>()           // formatos de destino con las pipelines de efectos compiladas
    private(set) var statFxItems = 0, statFxTris = 0, statFxBillboards = 0, statFxRibbons = 0, statFxPolys = 0, statFxSkipped = 0

    // MARK: vista actual del puente
    private var view = NsView()
    private var lastEpoch: UInt32 = 0
    private var sceneAbsent = 0                       // fotogramas sin escena nativa del puente (se repitió la última válida); `[nativo] escena_ausente=N` en el registro
    /// Sincronía de época (mallas y texturas): una escena de época de mallas NUEVA (cambio de zona) espera a que la VRAM del renderer traiga la lista de la GPU que contiene su dibujo (NsScene.list_index),
    /// como mucho `syncWaitMax` fotogramas; mientras espera se repite la última escena válida (con su VRAM). `vramList`/`vramKnown`: índice de la última lista aplicada (lo fija RGRenderer antes de beginFrame).
    var vramList: UInt32 = 0
    var vramKnown = false
    static let syncWaitMax = 6
    private var syncHeld = false                      // la escena `view` está retenida: no se vuelve a tomar del puente (sus punteros valen hasta la siguiente toma)
    private var syncWait = 0                          // fotogramas que lleva esperando la VRAM de la época nueva
    private var syncHeldFrames = 0                    // total de fotogramas retenidos (registro `sync_epoca=N`)
    private var syncEpoch: UInt32 = 0, syncHave = false   // época de la última escena aceptada
    private var lastSeq: UInt32 = 0

    // MARK: anillo por fotograma
    private final class Slot {
        var pieceBuf: MTLBuffer?
        var orderBuf: MTLBuffer?
        var staticBuf: MTLBuffer?
        var staticGen = -1
        var nTex = 0, nFlat = 0
        var fxBuf: MTLBuffer?
        var nFxOpaque = 0, nFxSemi = 0
    }
    private var ring: [Slot]
    private var current: Slot?
    private var currentIx = -1

    // MARK: pipelines
    private struct PKey: Hashable {
        var layered: Bool, pass: UInt32, mode: Int, order: Bool, flat: Bool, color: MTLPixelFormat, depth: MTLPixelFormat, samples: Int, linear: Bool
        var fx: Bool = false          // vértices de efectos (ns_vs_fx*) en vez de mallas
    }
    private var pipes: [PKey: MTLRenderPipelineState] = [:]
    private struct OKey: Hashable { var layered: Bool, abr: UInt32, color: MTLPixelFormat, depth: MTLPixelFormat, samples: Int, linear: Bool }
    private var opipes: [OKey: MTLRenderPipelineState] = [:]
    private var ovTagsOK = Set<String>()
    private let dsOverlay: MTLDepthStencilState
    private var failed = Set<String>()
    private var readyOK = Set<String>()
    private var badPipes = Set<PKey>()        // caché negativa: una pipeline que no compiló no se reintenta cada fotograma
    private var badTags = Set<String>()       // ídem para un formato de destino entero (prewarm)
    private let dsOpaque: MTLDepthStencilState
    private let dsSemi: MTLDepthStencilState
    private let dsOpaqueNoZ: MTLDepthStencilState
    private let dsSemiNoZ: MTLDepthStencilState

    // MARK: estadísticas (hilo de render)
    private(set) var isActive = false
    private(set) var statDraws = 0
    private(set) var statSemiTris = 0
    private(set) var statPieces = 0
    private(set) var statMeshes = 0
    private(set) var statScenes = 0
    private(set) var statInstances = 0
    private var cpuPrepareMs = 0.0, cpuEncodeMs = 0.0
    private var ownerThread: pthread_t?
    private var warnedThread = false

    init?(device: MTLDevice, library: MTLLibrary, ringSize: Int = 3, queue: DispatchQueue? = nil) {
        guard MemoryLayout<NsGpuVert>.stride == 16, MemoryLayout<NsGpuCorner>.stride == 28, MemoryLayout<NsGpuPiece>.stride == 164,
              MemoryLayout<NsGpuOrder>.stride == 8, MemoryLayout<NsGpuFxVert>.stride == 44, MemoryLayout<NsGpuOverlay>.stride == 52, MemoryLayout<NsGpuOverlayU>.stride == 116 else {
            print("[NativeRenderer] layout de estructuras inesperado: vert=\(MemoryLayout<NsGpuVert>.stride) corner=\(MemoryLayout<NsGpuCorner>.stride) piece=\(MemoryLayout<NsGpuPiece>.stride) order=\(MemoryLayout<NsGpuOrder>.stride) fx=\(MemoryLayout<NsGpuFxVert>.stride)")
            return nil
        }
        guard let vp = NativePool(device: device, stride: MemoryLayout<NsGpuVert>.stride, initial: 1 << 15, maxElems: 1 << 21, label: "native.verts"),
              let tp = NativePool(device: device, stride: 3 * MemoryLayout<NsGpuCorner>.stride, initial: 1 << 15, maxElems: 1 << 20, label: "native.corners") else { return nil }
        let d1 = MTLDepthStencilDescriptor()
        d1.depthCompareFunction = .greaterEqual; d1.isDepthWriteEnabled = true
        let d2 = MTLDepthStencilDescriptor()
        d2.depthCompareFunction = .greaterEqual; d2.isDepthWriteEnabled = false
        let d3 = MTLDepthStencilDescriptor()
        d3.depthCompareFunction = .always; d3.isDepthWriteEnabled = true
        let d4 = MTLDepthStencilDescriptor()
        d4.depthCompareFunction = .always; d4.isDepthWriteEnabled = false
        guard let s1 = device.makeDepthStencilState(descriptor: d1), let s2 = device.makeDepthStencilState(descriptor: d2),
              let s3 = device.makeDepthStencilState(descriptor: d3), let s4 = device.makeDepthStencilState(descriptor: d4),
              let s5 = device.makeDepthStencilState(descriptor: d4) else { return nil }   // (recubrimientos: siempre pasan y no escriben = d4)
        dsOpaqueNoZ = s3; dsSemiNoZ = s4; dsOverlay = s5
        self.device = device; self.library = library; self.queue = queue
        self.ringSize = max(6, ringSize)
        vertPool = vp; triPool = tp
        dsOpaque = s1; dsSemi = s2
        ring = (0..<max(6, ringSize)).map { _ in Slot() }
        slotBusy = [Bool](repeating: false, count: max(6, ringSize))
        slotSem = DispatchSemaphore(value: max(6, ringSize))
        semiOrder = UnsafeMutablePointer<NsGpuOrder>.allocate(capacity: Self.maxSemiTris)
        semiAbr = UnsafeMutablePointer<UInt8>.allocate(capacity: Self.maxSemiTris)
        semiDepth = UnsafeMutablePointer<Float>.allocate(capacity: Self.maxSemiTris)
    }

    deinit {
        fxVerts?.deallocate()
        semiOrder.deallocate(); semiAbr.deallocate(); semiDepth.deallocate()
        ns_gpu_sort_free(&semiScratch)
    }

    private func checkThread() {
        if let q = queue { dispatchPrecondition(condition: .onQueue(q)); return }
        let me = pthread_self()
        if let o = ownerThread {
            if pthread_equal(o, me) == 0 && !warnedThread { warnedThread = true; print("[NativeRenderer] AVISO: llamado desde más de un hilo (debe ser siempre el de codificación)") }
        } else { ownerThread = me }
    }

    private static func nowNs() -> UInt64 { DispatchTime.now().uptimeNanoseconds }

    // MARK: sincronización

    private func frameDone(serial s: UInt64, slot: Int) {
        lock.lock()
        if s > completedSerial { completedSerial = s }
        if slot >= 0 { slotBusy[slot] = false }
        lock.unlock()
        if slot >= 0 { slotSem.signal() }
    }

    private func completed() -> UInt64 { lock.lock(); defer { lock.unlock() }; return completedSerial }

    private func reclaim() {
        guard !pendingFree.isEmpty else { return }
        let done = completed()
        var i = 0
        while i < pendingFree.count {
            let p = pendingFree[i]
            if p.serial <= done { vertPool.release(p.vs, p.nv); triPool.release(p.ts, p.nt); pendingFree.swapAt(i, pendingFree.count - 1); pendingFree.removeLast() }
            else { i += 1 }
        }
    }

    // MARK: puente

    /// Toma las escenas del puente, interpola, sube las mallas y reconoce la época. Se llama en CADA fotograma (beginFrame).
    private func drain() {
        curHaveScene = false; curFit = false; curMeshesOK = false
        guard enabled, let b = bridge else { return }
        var v = view
        // Sin escena nativa este fotograma: no se toca nada de lo preparado (se conserva la última escena válida y ns_present decide repetirla o fundir a negro); solo se cuenta. Nunca hay caída a la lista de la GPU.
        if !syncHeld {      // (con la escena retenida no se vuelve a tomar del puente)
            v = NsView()
            guard ns_bridge_acquire(b, ns_now_ns(), &v) != 0 else { sceneAbsent += 1; return }
        }
        guard let cur = v.cur else { syncHeld = false; sceneAbsent += 1; return }
        view = v
        // Misma época de mallas y texturas: la escena de una época NUEVA no se prepara (ni se reconoce la época) hasta que la VRAM traiga su lista, con tope de espera. Dentro de una época no espera nunca.
        if view.fresh != 0, syncHave, cur.pointee.mesh_epoch != syncEpoch, ns_scene_vram_ready(cur, vramList, vramKnown ? 1 : 0) == 0, syncWait < Self.syncWaitMax {
            syncWait += 1; syncHeld = true; syncHeldFrames += 1
            return
        }
        syncHeld = false; syncWait = 0
        syncEpoch = cur.pointee.mesh_epoch; syncHave = true
        do {
            let vb = cur.pointee.valid
            let need = UInt32(truncatingIfNeeded: NS_SV_GAME) | UInt32(truncatingIfNeeded: NS_SV_LISTS_OK) | UInt32(truncatingIfNeeded: NS_SV_MAP_OK)
            let bad = UInt32(truncatingIfNeeded: NS_SV_MAP_INCOMPLETE) | UInt32(truncatingIfNeeded: NS_SV_STORE_FULL)
            if ns_scene_native_ok(vb) != 0 { fitFrames += 1; unfitFrames = 0; if fitFrames >= 3 { sceneUnfit = false } }
            else if (vb & need) == need && (vb & bad) == 0 { unfitFrames += 1; fitFrames = 0; if unfitFrames >= 4 { sceneUnfit = true } }
        }
        var ip = NsInterp()
        ns_interp_begin(b, &view, &ip)
        // Sin interpolación entre escenas: la fluidez de XRFrameEncoder ya corrige la VISTA con la muestra de cada escena (igual que para la lista de la GPU); interpolar además
        // la escena nativa (que también mueve to_vr con el jugador) compensaría dos veces el mismo movimiento. Se dibuja siempre la escena actual.
        ip.enabled = 0
        let fresh = view.fresh != 0
        ingest(scene: cur, store: view.store, interp: &ip, fresh: fresh)
        // Todas las mallas de la época de esta escena están copiadas en la GPU: el productor puede liberar la anterior (protocolo de ns_types.h). Siempre, se
        // dibuje o no (cinemáticas, menús): si no, el almacén se llenaría.
        if fresh, let st = view.store { ns_store_ack(st, cur.pointee.mesh_epoch) }
    }

    // MARK: caché de mallas

    /// Libera todas las mallas subidas (los rangos vuelven al pool cuando la GPU termina lo que los usaba) y reinicia el recuento del partido.
    private func invalidateAllMeshes() {
        for (_, m) in meshes { releaseMesh(m) }
        meshes.removeAll()
        meshesChanged = true
        splitMeshStat = 0; splitTrisStat = 0; splitAddedStat = 0; splitWeldStat = 0; splitCapStat = 0; splitFailStat = 0
    }

    private func releaseMesh(_ m: NativeGpuMesh) {
        if m.ntri > 0 { pendingFree.append((serial, m.vertStart, m.nverts, m.triStart, m.ntri)) }
        meshesChanged = true
    }

    private var lastForceGc: UInt64 = 0          // fotograma (serial) del último barrido forzado de mallas
    private func allocRetry(_ pool: NativePool, _ n: Int, seq: UInt32, store: OpaquePointer?) -> Int? {
        if let s = pool.allocate(n) { return s }
        // sin sitio: recuperar lo que la GPU ya terminó y, como mucho UNA vez cada 10 fotogramas, liberar todo lo que esta escena no usa (el barrido recorre todas las mallas); reintentar
        if serial &- lastForceGc >= 10 || lastForceGc == 0 {
            lastForceGc = serial == 0 ? 1 : serial
            gcMeshes(seq: seq, store: store, force: true)
        }
        reclaim()
        return pool.allocate(n)
    }

    private func upload(_ pm: UnsafePointer<NsMesh>, baked: UnsafePointer<NsBaked>?, key: NativeMeshKey, seq: UInt32, store: OpaquePointer?) -> NativeGpuMesh? {
        let gm = NativeGpuMesh(id: key.id)
        gm.lastUsed = seq
        gm.bakedOK = key.ver == 0 || baked != nil
        let nv = Int(pm.pointee.n_verts), nt = Int(ns_gpu_tri_count(pm))
        gm.flags = pm.pointee.flags
        gm.depthBias = pm.pointee.depth_bias
        let bb = pm.pointee.bbox
        gm.bmin = SIMD3<Float>(Float(bb.0), Float(bb.1), Float(bb.2)); gm.bmax = SIMD3<Float>(Float(bb.3), Float(bb.4), Float(bb.5))
        gm.restR = ns_gpu_mesh_radius(pm)
        if nv == 0 || nt == 0 || pm.pointee.verts == nil || pm.pointee.faces == nil { meshes[key] = gm; return nil }
        // Diagnóstico «partir triángulos grandes»: se empaqueta a memoria temporal (índices locales), se parten los triángulos de arista > umbral y el resultado (vértices originales
        // + añadidos, esquinas, centroides y modos de mezcla) es lo que se sube. Los triángulos con vértices soldados no se tocan. Con tope de seguridad por malla y en total.
        var split = NsGpuSplit()
        var splitOK = false
        var tv: UnsafeMutablePointer<NsGpuVert>?
        var tc: UnsafeMutablePointer<NsGpuCorner>?
        var tcen: UnsafeMutablePointer<Float>?
        var tabr: UnsafeMutablePointer<UInt8>?
        defer {
            tv?.deallocate(); tc?.deallocate(); tcen?.deallocate(); tabr?.deallocate()
            if splitOK { ns_gpu_split_free(&split) }
        }
        let wantSemiArrays = (gm.flags & UInt32(truncatingIfNeeded: NS_MF_BLEND)) != 0
        if splitLocalEdge > 0 && splitAddedStat < Self.splitExtraTotal {
            let tvp = UnsafeMutablePointer<NsGpuVert>.allocate(capacity: nv)
            let tcp = UnsafeMutablePointer<NsGpuCorner>.allocate(capacity: nt * 3)
            tv = tvp; tc = tcp
            if wantSemiArrays {
                tcen = UnsafeMutablePointer<Float>.allocate(capacity: 3 * nt)
                tabr = UnsafeMutablePointer<UInt8>.allocate(capacity: nt)
            }
            ns_gpu_pack(pm, baked, tvp, tcp, 0, tcen, tabr)
            let cap = UInt32(min(nt + Self.splitExtraPerMesh, Int(Int32.max / 4)))
            let rc = ns_gpu_split(tvp, UInt32(nv), tcp, UInt32(nt), tcen, tabr, splitLocalEdge, cap, &split)
            if rc == 0 { splitOK = true } else if rc < 0 { splitFailStat += 1 }
            splitWeldStat += Int(split.n_skip_weld)
        }
        let nvOut = splitOK ? Int(split.nverts) : nv
        let ntOut = splitOK ? Int(split.ntris) : nt
        guard let vs = allocRetry(vertPool, nvOut, seq: seq, store: store) else { frameFailures += 1; totalFailures += 1; return nil }
        guard let ts = allocRetry(triPool, ntOut, seq: seq, store: store) else { vertPool.release(vs, nvOut); frameFailures += 1; totalFailures += 1; return nil }
        gm.vertStart = vs; gm.nverts = nvOut; gm.triStart = ts; gm.ntri = ntOut
        gm.semi = (gm.flags & UInt32(truncatingIfNeeded: NS_MF_BLEND)) != 0
        if gm.semi {
            gm.centroid = UnsafeMutablePointer<Float>.allocate(capacity: 3 * ntOut)
            gm.abr = UnsafeMutablePointer<UInt8>.allocate(capacity: ntOut)
        }
        var textured = false
        for f in 0..<Int(pm.pointee.n_faces) where pm.pointee.faces[f].texid != 0 { textured = true; break }
        gm.opaquePass = !gm.semi || textured
        gm.flat = !gm.semi && !textured
        let vptr = vertPool.buffer.contents().assumingMemoryBound(to: NsGpuVert.self) + vs
        let cptr = triPool.buffer.contents().assumingMemoryBound(to: NsGpuCorner.self) + ts * 3
        if splitOK, let sv = split.verts, let sc = split.corners {
            memcpy(vptr, sv, nvOut * MemoryLayout<NsGpuVert>.stride)
            memcpy(cptr, sc, ntOut * 3 * MemoryLayout<NsGpuCorner>.stride)
            ns_gpu_rebase_corners(cptr, UInt32(ntOut * 3), UInt32(vs))
            if gm.semi, let c = gm.centroid, let a = gm.abr, let scen = split.centroid, let sab = split.abr {
                memcpy(c, scen, 3 * ntOut * MemoryLayout<Float>.stride)
                memcpy(a, sab, ntOut)
            }
            splitMeshStat += 1
            splitTrisStat += Int(split.n_split)
            splitAddedStat += ntOut - nt
            splitCapStat += Int(split.n_skip_cap)
        } else {
            ns_gpu_pack(pm, baked, vptr, cptr, UInt32(vs), gm.centroid, gm.abr)
        }
        meshes[key] = gm
        meshesChanged = true
        return gm
    }

    /// Malla (y juego de colores precalculados de la versión `ver`) en la caché de GPU. La clave es la que pidió la pieza; si la entrada se subió sin colores y
    /// ahora existen en el almacén, se invalida y se vuelve a subir.
    private func acquireMesh(id: UInt64, ver: UInt32, store: OpaquePointer?, seq: UInt32) -> NativeGpuMesh? {
        let key = NativeMeshKey(id: id, ver: ver)
        if let m = meshes[key] {
            if m.bakedOK || store == nil || ns_store_get_baked(store, id, ver) == nil {
                m.lastUsed = seq
                return m.ntri > 0 ? m : nil
            }
            meshes.removeValue(forKey: key)       // aparecieron los colores: se sustituye
            releaseMesh(m)
        }
        guard let st = store, let pm = ns_store_get(st, id) else { frameMisses += 1; return nil }
        var baked: UnsafePointer<NsBaked>? = nil
        if ver != 0 && (pm.pointee.flags & UInt32(truncatingIfNeeded: NS_MF_BAKED)) != 0 { baked = ns_store_get_baked(st, id, ver) }
        return upload(pm, baked: baked, key: key, seq: seq, store: st)
    }

    /// Libera las mallas que ya no están en el almacén y, si `force` o hay presión, las que esta escena no usa.
    private func gcMeshes(seq: UInt32, store: OpaquePointer?, force: Bool) {
        let pressure = force || vertPool.alloc.used * 4 > vertPool.alloc.capacity * 3 || triPool.alloc.used * 4 > triPool.alloc.capacity * 3
        var dead: [NativeMeshKey] = []
        for (key, m) in meshes where m.lastUsed != seq {
            let gone = store == nil || ns_store_get(store, key.id) == nil || (key.ver != 0 && m.bakedOK && ns_store_get_baked(store, key.id, key.ver) == nil)
            if gone || (pressure && (force || seq &- m.lastUsed > 600)) { dead.append(key) }
        }
        for k in dead { if let m = meshes.removeValue(forKey: k) { releaseMesh(m) } }
    }

    // MARK: ingestión de una escena

    /// Prepara la escena. Devuelve true si es utilizable y quedó preparada; false = NO se toca nada de lo preparado: se conserva la última escena válida (mallas, piezas,
    /// efectos, niebla) y se REPITE con la cabeza actual (el mundo está fijo en el espacio VR, así que repetir es reproyectar). Es lo que sustituye a la antigua caída a la GPU.
    @discardableResult
    private func ingest(scene sc: UnsafePointer<NsScene>, store: OpaquePointer?, interp ip: inout NsInterp, fresh: Bool) -> Bool {
        let t0 = Self.nowNs()
        let seq = sc.pointee.seq
        lastSeq = seq
        // Diagnóstico: umbral de partido en unidades locales (m por unidad del motor = norma de la primera columna de to_vr); un cambio del ajuste vuelve a subir todas las mallas.
        if splitMeters != appliedSplit {
            appliedSplit = splitMeters
            invalidateAllMeshes()
        }
        if splitMeters > 0 {
            let t3 = ns_scene_to_vr(sc)!
            let col = (t3[0] * t3[0] + t3[4] * t3[4] + t3[8] * t3[8]).squareRoot()
            let upm0 = sc.pointee.units_per_m > 1 ? sc.pointee.units_per_m : 1000
            let mpu: Float = (col.isFinite && col > 1e-9) ? col : 1 / upm0
            splitLocalEdge = splitMeters / mpu
        } else { splitLocalEdge = 0 }
        lastEpoch = sc.pointee.mesh_epoch
        if fresh { statScenes += 1 }
        frameFailures = 0; frameMisses = 0
        curHaveScene = true; curSceneT = sc.pointee.t_ns
        curFit = false; curMeshesOK = false
        // Conserva lo preparado: las mallas que usa no se liberan mientras se repita (lastUsed al día).
        func hold() { for m in meshOf { m?.lastUsed = seq } }
        guard sc.pointee.version == UInt32(truncatingIfNeeded: NS_VERSION), sc.pointee.size == UInt32(MemoryLayout<NsScene>.size),
              sc.pointee.inst != nil, sc.pointee.piece != nil else { hold(); return false }
        let validBits = sc.pointee.valid
        let cinema = cinemaGameView != nil
        do {
            let c = sc.pointee.cam
            if c.h.isFinite && c.h > 8 && c.h < 4096 { camH = c.h }
            camOfx = (c.ofx.isFinite && c.ofx > 0 && c.ofx < 320) ? c.ofx : 160
            camOfy = (c.ofy.isFinite && c.ofy > 0 && c.ofy < 240) ? c.ofy : 120
            var zn: Float = 0, zs: Float = 0
            if cinema { _ = ns_cinema_depth_filter(&cineFilter, sc, &zn, &zs) } else { cineFilter = NsCinemaDepthFilter() }
            cineNear = zn; cineSubject = zs
        }
        // Aptitud. Primera persona: la del contrato (ns_scene_native_ok; sin PLAYER_OK se dibuja el mundo sin cuerpo/manos/IK, no se retiene la escena). Cinemática: no hay cámara VR ni jugador, basta con escenario y listas completas.
        let cinemaNeed = UInt32(truncatingIfNeeded: NS_SV_GAME) | UInt32(truncatingIfNeeded: NS_SV_LISTS_OK) | UInt32(truncatingIfNeeded: NS_SV_MAP_OK)
        let cinemaBad = UInt32(truncatingIfNeeded: NS_SV_MAP_INCOMPLETE) | UInt32(truncatingIfNeeded: NS_SV_STORE_FULL)
        let fit = cinema ? ((validBits & cinemaNeed) == cinemaNeed && (validBits & cinemaBad) == 0) : ns_scene_native_ok(validBits) != 0
        curFit = fit
        guard fit else { hold(); return false }
        // mundo del motor -> mundo VR: primera persona = to_vr de la escena; cinemática = cámara del juego colocada en la vista del juego
        var t = [Float](repeating: 0, count: 12)
        var finite = true
        if cinema, let gvm = cinemaGameView {
            var cam = sc.pointee.cam
            let gv = XRMath.floats(gvm)
            finite = ns_cinema_to_vr(&cam, gv, &t) != 0
        } else {
            if ip.enabled != 0 { ns_interp_to_vr(&ip, &t) } else { let src = ns_scene_to_vr(sc)!; for i in 0..<12 { t[i] = src[i] } }
            for i in 0..<12 where !t[i].isFinite { finite = false }
        }
        guard finite else { hold(); return false }
        let nInst = Int(min(sc.pointee.n_inst, sc.pointee.cap_inst))
        let nPiece = Int(min(sc.pointee.n_piece, sc.pointee.cap_piece))
        guard nPiece > 0 else { hold(); return false }
        let groupMask = UInt32(sc.pointee.group_mask)
        let hidden = UInt16(truncatingIfNeeded: NS_PF_HIDDEN)
        let drawFlag = UInt32(truncatingIfNeeded: NSG_PF_DRAW)
        let staticFlag = UInt16(truncatingIfNeeded: NS_IF_STATIC)

        // 1) mallas de todas las piezas (también las ocultas: pueden ser el padre de una soldadura). Si falta alguna se conserva la escena anterior.
        var newMeshOf = [NativeGpuMesh?](repeating: nil, count: nPiece)
        for ii in 0..<nInst {
            let first = Int(sc.pointee.inst[ii].first_piece), cnt = Int(sc.pointee.inst[ii].n_pieces)
            guard first + cnt <= nPiece else { continue }
            for k in 0..<cnt {
                let pp = sc.pointee.piece + (first + k)
                newMeshOf[first + k] = acquireMesh(id: pp.pointee.mesh_id, ver: pp.pointee.baked_ver, store: store, seq: seq)
            }
        }
        // Mallas ausentes: una escena con piezas sin malla NO se descarta para siempre (en las cinemáticas sin partida llegaron a ser decenas de ausentes durante minutos: se repetía la última
        // escena y la pantalla 3D caía a la pantalla plana). Pasada la gracia se dibuja lo que hay (las piezas sin malla se omiten más abajo).
        if frameMisses > 0 && frameFailures == 0 { if fresh { missStreak += 1 }; missTolerated = missStreak >= Self.missGrace }
        else if frameMisses == 0 { missStreak = 0; missTolerated = false }
        guard frameFailures == 0, frameMisses == 0 || missTolerated else {
            // lo ya subido de esta escena también se conserva (la próxima vez no hará falta volver a subirlo)
            hold(); for m in newMeshOf { m?.lastUsed = seq }
            return false
        }
        curMeshesOK = true
        // --- a partir de aquí la escena es válida: se sustituye todo lo preparado ---
        statInstances = nInst
        if fresh { staticDirty = true; semiDirty = true }
        sceneSeq = seq
        fxEffects.removeAll(keepingCapacity: true); fxHas = false
        pieceCount = 0
        dynTex.removeAll(keepingCapacity: true); dynFlat.removeAll(keepingCapacity: true)
        semis.removeAll(keepingCapacity: true); statPcs.removeAll(keepingCapacity: true)
        if table.count < nPiece { table.append(contentsOf: repeatElement(NsGpuPiece(), count: nPiece - table.count)) }
        meshOf = newMeshOf
        // 2) tabla de piezas y listas. Por instancia, en tres pasos: (a) tabla de todas sus piezas (las soldaduras miran la tabla del padre), (b) red de seguridad de PIEZA (matriz
        // corrupta: no se dibuja; soldadura inconsistente: se ignoran sus soldaduras), (c) listas de dibujo y recuento de los triángulos que el shader colapsará.
        let netVerts = vertPool.buffer.contents().assumingMemoryBound(to: NsGpuVert.self)
        let netCorners = triPool.buffer.contents().assumingMemoryBound(to: NsGpuCorner.self)
        let netCol = (t[0] * t[0] + t[4] * t[4] + t[8] * t[8]).squareRoot()
        let netMpu: Float = (netCol.isFinite && netCol > 1e-7) ? netCol : 0       // metros VR por unidad del motor (0 = no medible: sin comprobación en CPU; el shader sigue vigilando)
        let netMat = UInt32(truncatingIfNeeded: NSG_NET_MAT), netWeld = UInt32(truncatingIfNeeded: NSG_NET_WELD)
        let noWeldFlag = UInt32(truncatingIfNeeded: NSG_PF_NOWELD)
        if fresh { netScenes += 1 }
        for ii in 0..<nInst {
            let instPtr = UnsafePointer(sc.pointee.inst + ii)
            let first = Int(instPtr.pointee.first_piece), cnt = Int(instPtr.pointee.n_pieces)
            guard first + cnt <= nPiece else { continue }
            let isStatic = (instPtr.pointee.flags & staticFlag) != 0
            let zm = UInt32(instPtr.pointee.zone_mask)
            let inGroup = groupMask == 0 || zm == 0 || (zm & groupMask) != 0
            let instKey = instPtr.pointee.key
            // (a) tabla
            for k in 0..<cnt {
                let gi = first + k
                guard let gm = meshOf[gi] else { table[gi] = NsGpuPiece(); continue }
                let piecePtr = UnsafePointer(sc.pointee.piece + gi)
                let pk = Int(piecePtr.pointee.parent)
                var parentGlobal: Int32 = -1
                var parentNV: UInt32 = 0
                if pk >= 0 && pk < cnt, let pm = meshOf[first + pk] { parentGlobal = Int32(first + pk); parentNV = UInt32(pm.nverts) }
                var pc = piecePtr.pointee
                var xf = NsXf()
                // cuerpo del jugador y manos propias: fijos respecto a la cabeza e interpolados entre escenas (vr_bodylive.h); 0 = la matriz de la escena
                if mv_vrhost_body_piece(sc, UInt32(ii), UInt32(k), &xf) != 0 { pc.world = xf }
                else if ip.enabled != 0 && ns_interp_piece(&ip, UInt32(ii), UInt32(k), &xf) != 0 { pc.world = xf }
                withUnsafePointer(to: &pc) { pcp in
                    ns_gpu_make_piece(sc, instPtr, pcp, gm.flags, Int32(gm.depthBias), UInt32(gm.vertStart), parentGlobal, parentNV, &table[gi])
                }
                table[gi].rest_r = gm.restR
                if !inGroup || (piecePtr.pointee.flags & hidden) != 0 { table[gi].flags &= ~drawFlag }
            }
            // (b) red de seguridad de pieza (solo objetos dinámicos que se van a dibujar)
            if !isStatic && netMpu > 0 {
                for k in 0..<cnt {
                    let gi = first + k
                    guard let gm = meshOf[gi], (table[gi].flags & drawFlag) != 0 else { continue }
                    var np = NsGpuNetPiece()
                    let causes = table.withUnsafeBufferPointer { ns_gpu_net_piece(netVerts, $0.baseAddress!, UInt32(gi), UInt32(gm.nverts), netMpu, &np) }
                    if fresh { netPieces += 1 }
                    if causes == 0 { continue }
                    if (causes & netMat) != 0 { table[gi].flags &= ~drawFlag }
                    else if (causes & netWeld) != 0 { table[gi].flags |= noWeldFlag }
                    if fresh {
                        if (causes & netMat) != 0 { netPiecesMat += 1 }
                        if (causes & netWeld) != 0 { netPiecesWeld += 1 }
                        var r = netRecs[NetKey(inst: instKey, piece: k, mesh: gm.id)] ?? NetRec()
                        r.n += 1; r.causes |= causes
                        r.gap = max(r.gap, np.max_gap_m); r.scale = max(r.scale, np.max_scale)
                        netRecs[NetKey(inst: instKey, piece: k, mesh: gm.id)] = r
                    }
                }
            }
            // (c) listas de dibujo (y recuento de lo que colapsa la red del shader)
            for k in 0..<cnt {
                let gi = first + k
                guard let gm = meshOf[gi] else { continue }
                if (table[gi].flags & drawFlag) == 0 { continue }
                if !isStatic && netMpu > 0 && fresh {
                    var acc = NsGpuNetTris()
                    let n = table.withUnsafeBufferPointer { ns_gpu_net_tris(netVerts, netCorners + gm.triStart * 3, UInt32(gm.ntri), $0.baseAddress!, UInt32(gi), netMpu, &acc) }
                    netTrisSeen += Int(acc.tris)
                    if n > 0 {
                        netTrisCollapsed += Int(n); netTrisFar += Int(acc.bad_far); netTrisEdge += Int(acc.bad_edge)
                        let key = NetKey(inst: instKey, piece: k, mesh: gm.id)
                        var r = netRecs[key] ?? NetRec()
                        r.n += 1; r.tris = max(r.tris, n)
                        if acc.bad_far > 0 { r.causes |= UInt32(truncatingIfNeeded: NSG_NET_FAR) }
                        if acc.bad_edge > 0 { r.causes |= UInt32(truncatingIfNeeded: NSG_NET_EDGE) }
                        r.far = max(r.far, acc.worst_far_m); r.edge = max(r.edge, acc.worst_edge_m)
                        netRecs[key] = r
                    }
                }
                if gm.opaquePass {
                    if isStatic { statPcs.append((gi, gm)) }
                    else if gm.flat { dynFlat.append(DrawItem(piece: gi, triStart: gm.triStart, ntri: gm.ntri)) }
                    else { dynTex.append(DrawItem(piece: gi, triStart: gm.triStart, ntri: gm.ntri)) }
                }
                if gm.semi { semis.append(SemiItem(piece: gi, mesh: gm)) }
            }
        }
        pieceCount = nPiece
        // transformación mundo del motor -> VR y niebla
        toVR = simd_float4x4(columns: (SIMD4<Float>(t[0], t[4], t[8], 0), SIMD4<Float>(t[1], t[5], t[9], 0), SIMD4<Float>(t[2], t[6], t[10], 0), SIMD4<Float>(t[3], t[7], t[11], 1)))
        // efectos y efectos de pantalla: copia propia (la escena del puente es de su ranura); solo con NS_SV_FX
        fxCam = sc.pointee.cam
        fogScene = sc.pointee.fog
        screenFx = NsScreenFx()
        if (validBits & UInt32(truncatingIfNeeded: NS_SV_FX)) != 0 {
            screenFx = sc.pointee.screen
            if let fp = sc.pointee.effects {
                let n = Int(min(sc.pointee.n_effect, sc.pointee.cap_effect))
                if n > 0 { fxEffects.append(contentsOf: UnsafeBufferPointer(start: fp, count: n)) }
            }
            fxHas = true
        }
        curValid = validBits
        for i in 0..<12 { toVR12[i] = t[i] }
        let rot = ns_scene_cam_rot(sc)!, eye = ns_scene_cam_eye(sc)!
        for i in 0..<9 { camRot9[i] = rot[i] }
        let fwd = SIMD3<Float>(rot[2], rot[5], rot[8])
        let eyeP = SIMD3<Float>(eye[0], eye[1], eye[2])
        fogFwd = SIMD4<Float>(fwd.x, fwd.y, fwd.z, -simd_dot(fwd, eyeP))
        camEye = eyeP
        // Descarte de reversos desde la cámara del juego (ver NsGpuUniforms.cull_eye): SOLO en los modos de pantalla (pantalla 3D y cine), como el PS1. En diorama, dentro de la escena y en la vista de mundo
        // no hay descarte. Se fija junto a camEye para que ojo y bandera sean siempre de la misma escena.
        cullBack = cinema && screenMode
        let upm = sc.pointee.units_per_m > 1 ? sc.pointee.units_per_m : 1000
        fogPar = SIMD4<Float>(sc.pointee.fog.start, max(sc.pointee.fog.len, 1e-3), sc.pointee.fog.active != 0 ? 1 : 0, 0.05 / upm)
        if fresh { gcMeshes(seq: seq, store: store, force: false) }
        if meshesChanged { staticDirty = true; semiDirty = true; meshesChanged = false }
        if staticDirty { rebuildStatic() }
        statPieces = dynTex.count + dynFlat.count + statPcs.count
        statMeshes = meshes.count
        cpuPrepareMs += (Double(Self.nowNs() - t0) / 1e6 - cpuPrepareMs) * 0.05
        return true
    }

    /// Lista de triángulos del escenario (una sola llamada de dibujo por clase). Solo cuando cambia la escena o la caché.
    private func rebuildStatic() {
        staticTex.removeAll(keepingCapacity: true); staticFlat.removeAll(keepingCapacity: true)
        for (gi, gm) in statPcs {
            for t in 0..<gm.ntri {
                if staticTex.count + staticFlat.count >= Self.maxStaticTris { break }
                let o = NsGpuOrder(piece: UInt32(gi), tri: UInt32(gm.triStart + t))
                if gm.flat { staticFlat.append(o) } else { staticTex.append(o) }
            }
        }
        staticGen += 1
        staticDirty = false
    }

    // MARK: fotograma

    private func world4(_ p: NsGpuPiece) -> simd_float4x4 {
        let w = p.w
        return simd_float4x4(columns: (SIMD4<Float>(w.0, w.4, w.8, 0), SIMD4<Float>(w.1, w.5, w.9, 0), SIMD4<Float>(w.2, w.6, w.10, 0), SIMD4<Float>(w.3, w.7, w.11, 1)))
    }

    /// Prepara el fotograma. SIEMPRE (drain: escenas, mallas y ack de época). Con `draw` reserva una ranura (espera a la GPU si las 3 están en vuelo; se libera
    /// cuando `commandBuffer` termina) y deja listos tabla de piezas, escenario y orden de los semitransparentes. Devuelve true si hay algo que dibujar.
    @discardableResult
    func beginFrame(commandBuffer cb: MTLCommandBuffer, sortView: simd_float4x4, draw: Bool, eyes: [SIMD3<Float>] = []) -> Bool {
        checkThread()
        let t0 = Self.nowNs()
        lastView0 = sortView
        serial += 1
        let my = serial
        var ix = -1
        if draw && enabled {
            // El anillo (ver init) tiene más ranuras que fotogramas en vuelo del bucle de render (3) más los que ya terminaron pero aún no ejecutaron su `frameDone`, así que siempre hay
            // una libre: la espera solo se agota si la GPU lleva más de 250 ms colgada. En ese caso el fotograma no tiene ranura y se REPITE lo último (ns_present); no hay ruta de GPU.
            if slotSem.wait(timeout: .now() + .milliseconds(250)) == .success {
                lock.lock()
                if let f = slotBusy.firstIndex(of: false) { slotBusy[f] = true; ix = f }
                lock.unlock()
                if ix < 0 { slotSem.signal() }
            }
        }
        let slotIx = ix
        cb.addCompletedHandler { [weak self] _ in self?.frameDone(serial: my, slot: slotIx) }
        reclaim()
        drain()
        isActive = false; current = nil; currentIx = ix
        fxAllowed = false
        guard draw, enabled, ix >= 0, pieceCount > 0, !(dynTex.isEmpty && dynFlat.isEmpty && semis.isEmpty && statPcs.isEmpty) else { return false }
        let slot = ring[ix]
        let bytes = pieceCount * MemoryLayout<NsGpuPiece>.stride
        if slot.pieceBuf == nil || slot.pieceBuf!.length < bytes {
            slot.pieceBuf = device.makeBuffer(length: max(bytes * 5 / 4, 4096), options: .storageModeShared)
            slot.pieceBuf?.label = "native.pieces"
        }
        guard let pb = slot.pieceBuf else { return false }
        table.withUnsafeBytes { raw in _ = memcpy(pb.contents(), raw.baseAddress!, bytes) }
        // escenario: copiar la lista si cambió desde la última vez que esta ranura la vio
        if slot.staticGen != staticGen {
            let nt = staticTex.count, nf = staticFlat.count
            let total = (nt + nf) * MemoryLayout<NsGpuOrder>.stride
            if total > 0 {
                if slot.staticBuf == nil || slot.staticBuf!.length < total {
                    slot.staticBuf = device.makeBuffer(length: max(total * 5 / 4, 4096), options: .storageModeShared)
                    slot.staticBuf?.label = "native.static"
                }
                if let sb = slot.staticBuf {
                    staticTex.withUnsafeBytes { r in if nt > 0 { _ = memcpy(sb.contents(), r.baseAddress!, nt * MemoryLayout<NsGpuOrder>.stride) } }
                    staticFlat.withUnsafeBytes { r in if nf > 0 { _ = memcpy(sb.contents() + nt * MemoryLayout<NsGpuOrder>.stride, r.baseAddress!, nf * MemoryLayout<NsGpuOrder>.stride) } }
                }
            }
            slot.nTex = nt; slot.nFlat = nf
            slot.staticGen = staticGen
        }
        updateSemiOrder(sortView)
        if semiN > 0 {
            let ob = semiN * MemoryLayout<NsGpuOrder>.stride
            if slot.orderBuf == nil || slot.orderBuf!.length < ob {
                slot.orderBuf = device.makeBuffer(length: max(ob * 5 / 4, 4096), options: .storageModeShared)
                slot.orderBuf?.label = "native.order"
            }
            if let o = slot.orderBuf { _ = memcpy(o.contents(), semiOrder, ob) }
        }
        current = slot
        isActive = true
        buildFx(slot: slot, sortView: sortView, eyes: eyes)
        mergeSemi(slot: slot)
        buildScreen(sortView: sortView)
        cpuPrepareMs += (Double(Self.nowNs() - t0) / 1e6 - cpuPrepareMs) * 0.05
        return true
    }

    /// Efectos de pantalla de este fotograma (ns_screen): parámetros del agua (en el mundo) y lista de recubrimientos del ojo, con el estado de suavizado del destello y el fundido.
    private func buildScreen(sortView: simd_float4x4) {
        let now = Self.nowNs()
        let dt: Float = lastScreenNs == 0 ? 0 : min(0.25, Float(Double(now &- lastScreenNs) / 1e9))
        lastScreenNs = now
        let c = sortView.inverse.columns.3
        let head: [Float] = [c.x, c.y, c.z]
        screenCfg.proj_h = camH
        var fxIn = screenFx
        if !screenEffectsEnabled { fxIn.flags = 0 }
        else if suppressRing { fxIn.flags &= ~UInt32(NS_SFX_GAS) }
        let cnt = toVR12.withUnsafeBufferPointer { tv in
            head.withUnsafeBufferPointer { hp in
                fromVR12.withUnsafeMutableBufferPointer { fv in
                    ns_screen_build(&fxIn, tv.baseAddress, hp.baseAddress, dt, &screenCfg, &screenState, &screenOut, fv.baseAddress)
                }
            }
        }
        overlays.removeAll(keepingCapacity: true)
        withUnsafePointer(to: &screenOut.ov) { p in
            p.withMemoryRebound(to: NsGpuOverlay.self, capacity: Int(NSG_MAX_OVERLAYS)) { q in
                for i in 0..<min(Int(cnt), Int(NSG_MAX_OVERLAYS)) { overlays.append(q[i]) }
            }
        }
    }

    /// Fusiona la lista ordenada de triángulos semitransparentes de las mallas con los efectos semitransparentes en UNA sola secuencia de atrás adelante (ns_gpu_semi_merge).
    private func mergeSemi(slot: Slot) {
        let nm = semiN
        let nf = slot.nFxSemi > 0 ? min(fxItemCount, fxItemsBuf.count) : 0
        let cap = nm + nf + 1
        if mergedRuns.count < cap { mergedRuns = [NsGpuSemiRun](repeating: NsGpuSemiRun(), count: cap) }
        var trunc: Int32 = 0
        let n = fxItemsBuf.withUnsafeBufferPointer { fp in
            mergedRuns.withUnsafeMutableBufferPointer { rp in
                ns_gpu_semi_merge(semiDepth, semiAbr, nm, fp.baseAddress, nf, rp.baseAddress, cap, &trunc)
            }
        }
        mergedN = Int(n)
    }

    /// Geometría de los efectos de este fotograma: carteles y cintas orientados UNA vez a la cabeza (la del primer ojo de `sortView`), polígonos de una cara con el test
    /// de cara de cada ojo. Deja los datos en la ranura; `fxAllowed` se confirma después con las pipelines (canEncode).
    private func buildFx(slot: Slot, sortView: simd_float4x4, eyes: [SIMD3<Float>]) {
        slot.nFxOpaque = 0; slot.nFxSemi = 0
        fxItemCount = 0
        statFxItems = 0; statFxTris = 0
        guard fxHas, !fxEffects.isEmpty else { fxAllowed = fxHas; return }
        let n = fxEffects.count
        let cap = 12 * n + 12
        if fxVertCap < cap {
            fxVerts?.deallocate()
            fxVerts = UnsafeMutablePointer<NsGpuFxVert>.allocate(capacity: cap)
            fxVertCap = cap
        }
        if fxRunsBuf.count < n + 1 { fxRunsBuf = [NsGpuFxRun](repeating: NsGpuFxRun(), count: n + 1) }
        if fxKeysBuf.count < n + 1 { fxKeysBuf = [UInt64](repeating: 0, count: n + 1) }
        if fxItemsBuf.count < n + 1 { fxItemsBuf = [NsGpuFxSemiItem](repeating: NsGpuFxSemiItem(), count: n + 1) }
        guard let vbuf = fxVerts else { return }
        let inv = sortView.inverse
        func axis(_ c: SIMD4<Float>) -> SIMD3<Float> { let v = SIMD3<Float>(c.x, c.y, c.z); let l = simd_length(v); return l > 1e-9 ? v / l : v }
        let head = SIMD3<Float>(inv.columns.3.x, inv.columns.3.y, inv.columns.3.z)
        let right = axis(inv.columns.0), up = axis(inv.columns.1)
        let ey = eyes.isEmpty ? [head] : Array(eyes.prefix(2))
        var out = NsGpuFxOut()
        var prm = NsGpuFxParams()
        prm.cam = nil
        // los efectos semitransparentes se ordenan por la MISMA profundidad que los triángulos de las mallas (la de la vista de ordenación), para poder intercalarlos
        prm.has_depth_row = 1
        prm.raise_m = fogPar.w        // sesgo `raise` del motor por elemento, con la misma unidad que las mallas
        prm.board_pull = 1            // carteles (halos, destellos): adelantados media extension hacia la cabeza, sin cambiar lo que se ve
        // Los efectos se colocan con la geometria del propio motor: carteles y cintas con tamaño en mundo fijo (NS_EF_WORLD_SIZE, calculado una vez con la camara del motor en ns_fx.c; el mismo para los dos
        // ojos), en su punto de mundo; no se encogen, no se retienen ni se siguen. Unica red de seguridad: un efecto con el centro a menos de 15 cm de la cabeza se descarta, y todo ON_CAMERA que no es de
        // pantalla (papel VIEW_OVERLAY) tambien. near_log_m: radio del registro `fx-cerca`.
        prm.near_log_m = 0.5; prm.drop_inside_m = 0.15
        prm.depth_row = (-sortView.columns.0.z, -sortView.columns.1.z, -sortView.columns.2.z, -sortView.columns.3.z)
        let rc: Int32 = toVR12.withUnsafeBufferPointer { (tv) -> Int32 in
            withUnsafePointer(to: &fxCam) { (camP) -> Int32 in
              withUnsafePointer(to: &fogScene) { (fogP) -> Int32 in
                prm.to_vr = tv.baseAddress
                prm.cam = camP
                prm.fog = fogP       // niebla de la escena también en los efectos (misma fórmula que las mallas)
                prm.head = (head.x, head.y, head.z)
                prm.head_right = (right.x, right.y, right.z)
                prm.head_up = (up.x, up.y, up.z)
                prm.eyes = ((ey[0].x, ey[0].y, ey[0].z), (ey[ey.count > 1 ? 1 : 0].x, ey[ey.count > 1 ? 1 : 0].y, ey[ey.count > 1 ? 1 : 0].z))
                prm.n_eyes = Int32(ey.count)
                return fxEffects.withUnsafeBufferPointer { (fp) -> Int32 in
                    fxRunsBuf.withUnsafeMutableBufferPointer { (rp) -> Int32 in
                        fxKeysBuf.withUnsafeMutableBufferPointer { (kp) -> Int32 in
                            fxItemsBuf.withUnsafeMutableBufferPointer { (ip) -> Int32 in
                                ns_gpu_fx_build_ex(fp.baseAddress, UInt32(n), &prm, vbuf, UInt32(cap), rp.baseAddress, UInt32(n + 1), kp.baseAddress, UInt32(n + 1),
                                                   ip.baseAddress, UInt32(n + 1), &out)
                            }
                        }
                    }
                }
              }
            }
        }
        guard rc == 0 else { return }
        fxOut = out
        do {   // registro de cercanos: se acumula y cada segundo se congela la ventana para el registro
            let now = ProcessInfo.processInfo.systemUptime
            var hd: [Float] = [head.x, head.y, head.z], rt: [Float] = [right.x, right.y, right.z]
            ns_gpu_fx_near_accum(&fxNear, &out, &hd, &rt, Float(now.truncatingRemainder(dividingBy: 1000)))
            if now - fxNearT0 >= 1.0 { fxNearSnap = fxNear; ns_gpu_fx_near_reset(&fxNear); fxNearT0 = now }
        }
        let total = Int(out.n_opaque + out.n_semi)
        statFxItems = Int(out.n_keys); statFxTris = total / 3
        statFxBillboards = Int(out.n_billboard); statFxRibbons = Int(out.n_ribbon); statFxPolys = Int(out.n_poly); statFxSkipped = Int(out.n_skipped)
        if total > 0 {
            let bytes = total * MemoryLayout<NsGpuFxVert>.stride
            if slot.fxBuf == nil || slot.fxBuf!.length < bytes {
                slot.fxBuf = device.makeBuffer(length: max(bytes * 5 / 4, 4096), options: .storageModeShared)
                slot.fxBuf?.label = "native.fx"
            }
            guard let fb = slot.fxBuf else { return }   // sin búfer: nada confirmado
            _ = memcpy(fb.contents(), vbuf, bytes)
            slot.nFxOpaque = Int(out.n_opaque); slot.nFxSemi = Int(out.n_semi)
            fxItemCount = Int(out.n_sitems)
        }
        fxAllowed = true
    }

    /// Orden de los semitransparentes: solo se recalcula si cambió la escena (o la caché) o la cabeza se movió más que el umbral.
    private func updateSemiOrder(_ sortView: simd_float4x4) {
        statSemiTris = 0
        if semis.isEmpty { semiN = 0; return }
        let inv = sortView.inverse.columns.3
        let head = SIMD3<Float>(inv.x, inv.y, inv.z)
        if !semiDirty && simd_distance(head, lastSortHead) <= Self.sortHeadThreshold { statSemiTris = semiN; return }
        semiDirty = false
        lastSortHead = head
        semiSrcs.removeAll(keepingCapacity: true)
        let vt = sortView * toVR
        for it in semis {
            let m = vt * world4(table[it.piece])
            // profundidad positiva hacia delante = -z de Metal
            semiSrcs.append(NsGpuSemiSrc(piece: UInt32(it.piece), tri_base: UInt32(it.mesh.triStart), ntri: UInt32(it.mesh.ntri),
                                         centroid: UnsafePointer(it.mesh.centroid!), abr: UnsafePointer(it.mesh.abr!),
                                         zrow: (-m.columns.0.z, -m.columns.1.z, -m.columns.2.z, -m.columns.3.z)))
        }
        let n = Int(ns_gpu_sort_semi_ex(semiSrcs, semiSrcs.count, semiOrder, semiAbr, semiDepth, Self.maxSemiTris, &semiScratch))
        semiN = n
        statSemiTris = n
    }

    // MARK: presentación (actual / último válido / negro)

    /// El render nativo se puede dibujar entero en este fotograma: ranura preparada, todas las mallas de la escena subidas y las pipelines listas.
    var frameReady: Bool { isActive && current != nil && frameFailures == 0 && (frameMisses == 0 || missTolerated) }

    /// Lo que la lista de la GPU declara del mundo y de la cámara (VrScene): mapa mundo del motor -> VR del espacio de usuario, origen de la lista y vista del juego.
    struct GpuMap {
        var userR: [Float]       // user_R (fila-mayor 3x3)
        var k: Float             // user_k * scale (m por unidad del motor)
        var eyeH: Float          // user_eye_h
        var pRef: [Float]        // user_p_ref
        var origin: [Float]      // origin_game
        var gameView: [Float]    // game_view (columna-mayor 4x4)
        var eyeVR: [Float]       // eye_vr
        var upm: Float           // unidades del motor por metro
    }

    /// Red de coherencia (SOLO REGISTRO: ya no cae a ninguna ruta de GPU; el resultado se anota en los bits `why` de la presentación): con el mundo fiable, la escena nativa y la lista de la GPU deben decir lo mismo del MISMO mundo. Se lleva el ojo nativo (y dos puntos de prueba) por el mapa
    /// mundo -> VR que declara la lista de la GPU (con su origen) y se compara con lo que da el to_vr de la escena nativa (ns_coherence); además se compara el giro de la cámara nativa con
    /// game_view. Comparar «to_vr * ojo» con eye_vr NO sirve: el origen de la lista está bajo el ojo, así que ambos valen siempre p_ref (la cabeza).
    /// El veredicto (ns_coh_step, vr/native/ns_bridge.c) tolera el desfase de fotogramas del motor entre la escena nativa y la lista (andando difieren hasta ~0,5 m y ~16 grados sin ser
    /// un mundo distinto) y solo cae a GPU con un salto grueso o una discrepancia continuada por encima de la tolerancia. Sin mapa de la lista (cinemática, mundo no fiable) no se
    /// evalúa y se mantiene el estado.
    private func checkCoherence(_ map: GpuMap?) -> Bool {
        cohEvaluated = false
        guard let m = map, (curValid & UInt32(truncatingIfNeeded: NS_SV_TOVR_OK)) != 0, m.userR.count == 9, m.pRef.count == 3, m.origin.count == 3, m.gameView.count == 16 else { return !incoherent }
        var out = NsCohOut()
        let ce: [Float] = [camEye.x, camEye.y, camEye.z]
        let ok = ns_coherence_flat(m.userR, m.k, m.eyeH, m.pRef, m.origin, m.gameView, toVR12, ce, camRot9, m.upm, &out) != 0 && out.ok != 0
        cohOut = out
        cohEvaluated = true
        let coherent = ns_coh_step(&cohState, &cohCfg, &out, ns_now_ns()) != 0   // out.ok == 0 (no evaluable) cuenta como salto grueso
        incoherent = cohState.incoherent != 0
        cohDist = ok && out.d_probe.isFinite ? out.d_probe : -1
        let why = Int(cohState.why)
        if why == Int(NS_COH_WHY_POS) { cohWhy = ok ? "posicion" : "no evaluable" }
        else if why == Int(NS_COH_WHY_ROT) { cohWhy = "giro" }
        else { cohWhy = "" }
        return coherent
    }

    /// Qué se enseña del mundo en este fotograma (ns_present_step): la escena actual, la ÚLTIMA VÁLIDA repetida con la cabeza actual, o negro tras un fallo prolongado
    /// (fundido suave). NO hay modo «GPU»: la lista de la GPU no se dibuja jamás en 3D. `ready`: pipelines del formato de destino listas. La antigua red de coherencia y la edad
    /// de la escena solo se REGISTRAN (bits `why`).
    func present(ready: Bool, gpuMap: GpuMap? = nil) -> NsPresentOut {
        coherentOK = checkCoherence(gpuMap)
        var inp = NsPresentIn(renderer_ok: (enabled && ready && isActive) ? 1 : 0, have_scene: curHaveScene ? 1 : 0, scene_fit: curFit ? 1 : 0,
                              meshes_ok: curMeshesOK ? 1 : 0, coherent: coherentOK ? 1 : 0, scene_t_ns: curSceneT, now_ns: ns_now_ns())
        var out = NsPresentOut()
        _ = ns_present_step(&presentState, &presentCfg, &inp, &out)
        lastPresent = out
        presentFade = out.fade
        return out
    }

    // MARK: pipelines

    private func pipeline(_ k: PKey) -> MTLRenderPipelineState? {
        if let p = pipes[k] { return p }
        if badPipes.contains(k) { return nil }
        do {
            let fc = MTLFunctionConstantValues()
            var pass = k.pass, lin = k.linear, flat = k.flat
            fc.setConstantValue(&pass, type: .uint, index: Int(NSG_FC_PASS))
            fc.setConstantValue(&lin, type: .bool, index: Int(NSG_FC_LINEAR))
            fc.setConstantValue(&flat, type: .bool, index: Int(NSG_FC_FLAT))
            let fs = try library.makeFunction(name: "ns_fs", constantValues: fc)
            let vsName = (k.fx ? "ns_vs_fx" : ((k.pass == 1 || k.order) ? "ns_vs_order" : "ns_vs_piece")) + (k.layered ? "_amp" : "")
            guard let vs = library.makeFunction(name: vsName) else {
                if !failed.contains(vsName) { failed.insert(vsName); print("[NativeRenderer] falta \(vsName)") }
                badPipes.insert(k)
                return nil
            }
            let d = MTLRenderPipelineDescriptor()
            d.label = "\(vsName) pass\(k.pass) blend\(k.mode)\(k.flat ? " flat" : "")"
            d.vertexFunction = vs
            d.fragmentFunction = fs
            d.rasterSampleCount = k.samples
            d.inputPrimitiveTopology = .triangle
            if k.layered { d.maxVertexAmplificationCount = 2 }
            let ca = d.colorAttachments[0]!
            ca.pixelFormat = k.color
            d.depthAttachmentPixelFormat = k.depth
            if k.depth == .depth32Float_stencil8 { d.stencilAttachmentPixelFormat = k.depth }
            if k.mode >= 0 {
                ca.isBlendingEnabled = true
                // Alfa = cobertura: máximo (1 donde se escribe). Color: los 4 modos del original (B = destino, F = fragmento).
                ca.alphaBlendOperation = .max
                ca.sourceAlphaBlendFactor = .one; ca.destinationAlphaBlendFactor = .one
                switch k.mode {
                case 0: // 0,5 B + 0,5 F
                    ca.rgbBlendOperation = .add
                    ca.sourceRGBBlendFactor = .blendColor; ca.destinationRGBBlendFactor = .oneMinusBlendColor
                case 1: // B + F
                    ca.rgbBlendOperation = .add
                    ca.sourceRGBBlendFactor = .one; ca.destinationRGBBlendFactor = .one
                case 2: // B - F
                    ca.rgbBlendOperation = .reverseSubtract
                    ca.sourceRGBBlendFactor = .one; ca.destinationRGBBlendFactor = .one
                default: // B + 0,25 F
                    ca.rgbBlendOperation = .add
                    ca.sourceRGBBlendFactor = .blendColor; ca.destinationRGBBlendFactor = .one
                }
            }
            let ps = try device.makeRenderPipelineState(descriptor: d)
            pipes[k] = ps
            return ps
        } catch {
            badPipes.insert(k)
            let key = "\(k)"
            if !failed.contains(key) { failed.insert(key); print("[NativeRenderer] pipeline: \(error)") }
            return nil
        }
    }

    private func overlayPipeline(_ k: OKey) -> MTLRenderPipelineState? {
        if let p = opipes[k] { return p }
        let name = "ov\(k)"
        if failed.contains(name) { return nil }
        do {
            let fc = MTLFunctionConstantValues()
            var lin = k.linear
            fc.setConstantValue(&lin, type: .bool, index: Int(NSG_FC_LINEAR))
            let fs = try library.makeFunction(name: "ns_fs_overlay", constantValues: fc)
            guard let vs = library.makeFunction(name: k.layered ? "ns_vs_overlay_amp" : "ns_vs_overlay") else { failed.insert(name); print("[NativeRenderer] falta ns_vs_overlay"); return nil }
            let d = MTLRenderPipelineDescriptor()
            d.label = "ns_overlay abr\(k.abr)"
            d.vertexFunction = vs
            d.fragmentFunction = fs
            d.rasterSampleCount = k.samples
            d.inputPrimitiveTopology = .triangle
            if k.layered { d.maxVertexAmplificationCount = 2 }
            let ca = d.colorAttachments[0]!
            ca.pixelFormat = k.color
            ca.writeMask = [.red, .green, .blue]         // la cobertura (alfa) del mundo no se toca
            d.depthAttachmentPixelFormat = k.depth
            if k.depth == .depth32Float_stencil8 { d.stencilAttachmentPixelFormat = k.depth }
            ca.isBlendingEnabled = true
            ca.alphaBlendOperation = .add; ca.sourceAlphaBlendFactor = .zero; ca.destinationAlphaBlendFactor = .one
            switch k.abr {
            case 0: // mezcla 50/50: el fragmento lleva alfa = a/2
                ca.rgbBlendOperation = .add
                ca.sourceRGBBlendFactor = .sourceAlpha; ca.destinationRGBBlendFactor = .oneMinusSourceAlpha
            case 2: // B - F
                ca.rgbBlendOperation = .reverseSubtract
                ca.sourceRGBBlendFactor = .one; ca.destinationRGBBlendFactor = .one
            default: // 1: B + F; 3: B + F/4 (el sombreador ya escala F)
                ca.rgbBlendOperation = .add
                ca.sourceRGBBlendFactor = .one; ca.destinationRGBBlendFactor = .one
            }
            let ps = try device.makeRenderPipelineState(descriptor: d)
            opipes[k] = ps
            return ps
        } catch {
            failed.insert(name)
            print("[NativeRenderer] pipeline de recubrimiento: \(error)")
            return nil
        }
    }

    /// Compila (si faltan) las 8 pipelines del formato de destino: opacos (por pieza / lista, con y sin texel) y los 4 modos de mezcla de los semitransparentes.
    /// Devuelve true si están todas. Se llama al crear el renderer con los formatos conocidos y de nuevo con los del pase real (no hace nada si ya estaban).
    @discardableResult
    func prewarm(layered: Bool, colorFormat: MTLPixelFormat, depthFormat: MTLPixelFormat, samples: Int, linear: Bool) -> Bool {
        let tag = "\(layered)/\(colorFormat.rawValue)/\(depthFormat.rawValue)/\(samples)/\(linear)"
        if readyOK.contains(tag) { return true }
        if badTags.contains(tag) { return false }
        var ok = true
        for order in [false, true] {
            for flat in [false, true] {
                if pipeline(PKey(layered: layered, pass: 0, mode: -1, order: order, flat: flat, color: colorFormat, depth: depthFormat, samples: samples, linear: linear)) == nil { ok = false }
            }
        }
        for m in 0..<4 {
            if pipeline(PKey(layered: layered, pass: 1, mode: m, order: true, flat: false, color: colorFormat, depth: depthFormat, samples: samples, linear: linear)) == nil { ok = false }
        }
        if ok { readyOK.insert(tag) } else { badTags.insert(tag) }
        // Efectos: pipelines propias; si no compilan, los efectos siguen por GPU (fxAllowed) y las mallas nativas no se ven afectadas.
        if ok && !fxTagsOK.contains(tag) {
            var fxOK = pipeline(PKey(layered: layered, pass: 0, mode: -1, order: false, flat: false, color: colorFormat, depth: depthFormat, samples: samples, linear: linear, fx: true)) != nil
            for m in 0..<4 {
                if pipeline(PKey(layered: layered, pass: 1, mode: m, order: true, flat: false, color: colorFormat, depth: depthFormat, samples: samples, linear: linear, fx: true)) == nil { fxOK = false }
            }
            if fxOK { fxTagsOK.insert(tag) }
        }
        // Recubrimientos de pantalla (fundidos, destello, modos de visión, anillo, barras): si no compilan se sigue dibujando el mundo sin ellos (se registra una vez).
        if ok && !ovTagsOK.contains(tag) {
            var ovOK = true
            for a in 0..<4 where overlayPipeline(OKey(layered: layered, abr: UInt32(a), color: colorFormat, depth: depthFormat, samples: samples, linear: linear)) == nil { ovOK = false }
            if ovOK { ovTagsOK.insert(tag) }
        }
        return ok
    }

    // MARK: dibujo

    /// Comprobación previa a `encode` (sin dibujar): hay ranura preparada y las pipelines de este formato están listas. Si es true, `encode` no falla por estas causas.
    func canEncode(layered: Bool, colorFormat: MTLPixelFormat, depthFormat: MTLPixelFormat, samples: Int, linear: Bool) -> Bool {
        guard isActive, current?.pieceBuf != nil else { return false }
        let ok = prewarm(layered: layered, colorFormat: colorFormat, depthFormat: depthFormat, samples: samples, linear: linear)
        // los efectos solo se dan por dibujados si TODOS los formatos de destino del fotograma tienen sus pipelines
        if ok && !fxTagsOK.contains("\(layered)/\(colorFormat.rawValue)/\(depthFormat.rawValue)/\(samples)/\(linear)") { fxAllowed = false }
        return ok
    }

    /// Dibuja la escena en el pase abierto `enc` (el llamador ya fijó viewports y, si `layered`, la amplificación de vértices a 2 vistas; profundidad inversa, limpia a 0;
    /// NO se escribe depthRange). `viewProj[i]` = proyección * vista del ojo i; `projections[i]` = solo la proyección del ojo i (los recubrimientos de pantalla reconstruyen
    /// las tangentes del ojo con ella); `eyes[i]` = posición del ojo en el mundo VR (m). Con un solo pase por ojo se pasa el ojo en [0] y `eyeIndex` dice cuál es (0/1: máscara
    /// de caras de los efectos de una cara). `linear`: el destino es lineal/sRGB. Devuelve false si no pudo dibujar (pipeline ausente o ranura no preparada).
    /// Orden: opacos (escenario, dinámicos, efectos opacos) -> UNA pasada semitransparente de atrás adelante con mallas y efectos intercalados -> recubrimientos de pantalla.
    @discardableResult
    func encode(encoder enc: MTLRenderCommandEncoder, vram: MTLTexture, viewProj: [simd_float4x4], projections: [simd_float4x4], eyes: [SIMD3<Float>], eyeIndex: Int = 0, layered: Bool,
                colorFormat: MTLPixelFormat, depthFormat: MTLPixelFormat, samples: Int, linear: Bool) -> Bool {
        checkThread()
        statDraws = 0
        guard isActive, let slot = current, let pb = slot.pieceBuf, !viewProj.isEmpty, !eyes.isEmpty else { return false }
        guard prewarm(layered: layered, colorFormat: colorFormat, depthFormat: depthFormat, samples: samples, linear: linear) else { return false }
        let t0 = Self.nowNs()
        var drawFx = fxAllowed && (slot.nFxOpaque + slot.nFxSemi) > 0 && slot.fxBuf != nil
        func psoFx(_ pass: UInt32, _ mode: Int) -> MTLRenderPipelineState? {
            pipes[PKey(layered: layered, pass: pass, mode: mode, order: pass == 1, flat: false, color: colorFormat, depth: depthFormat, samples: samples, linear: linear, fx: true)]
        }
        if drawFx, slot.nFxOpaque > 0 || slot.nFxSemi > 0 {
            // si faltara alguna pipeline de efectos, SOLO se omiten los efectos (el mundo se dibuja entero): nunca se pierde el fotograma por eso
            if psoFx(0, -1) == nil || !(0..<4).allSatisfy({ psoFx(1, $0) != nil }) { drawFx = false }
        }
        func pso(_ pass: UInt32, _ mode: Int, order: Bool, flat: Bool) -> MTLRenderPipelineState? {
            pipes[PKey(layered: layered, pass: pass, mode: mode, order: order, flat: flat, color: colorFormat, depth: depthFormat, samples: samples, linear: linear)]
        }
        var u = NsGpuUniforms()
        let vp1 = viewProj.count > 1 ? viewProj[1] : viewProj[0]
        let e1 = eyes.count > 1 ? eyes[1] : eyes[0]
        u.view_proj = (viewProj[0], vp1)
        u.eye = (SIMD4<Float>(eyes[0].x, eyes[0].y, eyes[0].z, 1), SIMD4<Float>(e1.x, e1.y, e1.z, 1))
        let c = toVR.columns
        u.tovr = (SIMD4<Float>(c.0.x, c.1.x, c.2.x, c.3.x), SIMD4<Float>(c.0.y, c.1.y, c.2.y, c.3.y), SIMD4<Float>(c.0.z, c.1.z, c.2.z, c.3.z))
        u.fog_fwd = fogFwd
        u.fog_par = fogPar
        // fundido a negro de la presentación (repetir el último válido durante demasiado tiempo): multiplica el color de todo lo del mundo
        u.tint = SIMD4<Float>(tint.x * presentFade, tint.y * presentFade, tint.z * presentFade, 0)
        // agua: el tinte vive en el mundo (el sombreador de fragmentos lo aplica por posición del mundo del motor); la onda es un post-proceso de pantalla (waveParams, lo codifica RGRenderer)
        u.water_min = SIMD4<Float>(screenOut.water_min.0, screenOut.water_min.1, screenOut.water_min.2, screenOut.water_min.3)
        u.water_max = SIMD4<Float>(screenOut.water_max.0, screenOut.water_max.1, screenOut.water_max.2, screenOut.water_max.3)
        u.water_tint = SIMD4<Float>(screenOut.water_tint.0, screenOut.water_tint.1, screenOut.water_tint.2, screenOut.water_tint.3)
        let fv = fromVR12
        u.fromvr = (SIMD4<Float>(fv[0], fv[1], fv[2], fv[3]), SIMD4<Float>(fv[4], fv[5], fv[6], fv[7]), SIMD4<Float>(fv[8], fv[9], fv[10], fv[11]))
        u.misc = SIMD4<Float>(layered ? 0 : Float(max(0, min(1, eyeIndex))), 0, 0, 0)
        // Descarte de reversos: cinemáticas = todas las caras de una cara (NSG_CULL_ALL); con la cabeza bajo el agua solo las semitransparentes (NSG_CULL_SEMI: la superficie del agua vista desde abajo
        // no se dibuja, como en el motor); si no, doble cara.
        let underwater = screenOut.water_tint.3 > 0.5 && screenOut.water_min.3 > 0.5
        u.cull_eye = cullBack ? SIMD4<Float>(camEye.x, camEye.y, camEye.z, Float(NSG_CULL_ALL))
                   : underwater ? SIMD4<Float>(camEye.x, camEye.y, camEye.z, Float(NSG_CULL_SEMI)) : SIMD4<Float>(0, 0, 0, 0)
        enc.setCullMode(.none)
        enc.setFragmentTexture(vram, index: 0)
        enc.setVertexBuffer(vertPool.buffer, offset: 0, index: Int(NSG_BUF_VERTS))
        enc.setVertexBuffer(triPool.buffer, offset: 0, index: Int(NSG_BUF_CORNERS))
        enc.setVertexBuffer(pb, offset: 0, index: Int(NSG_BUF_PIECES))
        enc.setVertexBytes(&u, length: MemoryLayout<NsGpuUniforms>.stride, index: Int(NSG_BUF_UNI))
        enc.setFragmentBytes(&u, length: MemoryLayout<NsGpuUniforms>.stride, index: Int(NSG_BUF_UNI))

        // opacos: escenario en una llamada por clase (lista de triángulos), luego los dinámicos por pieza (índice = instance_id)
        enc.setDepthStencilState(noZTest ? dsOpaqueNoZ : dsOpaque)
        let os = MemoryLayout<NsGpuOrder>.stride
        if let sb = slot.staticBuf {
            if slot.nTex > 0, let p = pso(0, -1, order: true, flat: false) {
                enc.setRenderPipelineState(p)
                enc.setVertexBuffer(sb, offset: 0, index: Int(NSG_BUF_ORDER))
                enc.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: slot.nTex * 3)
                statDraws += 1
            }
            if slot.nFlat > 0, let p = pso(0, -1, order: true, flat: true) {
                enc.setRenderPipelineState(p)
                enc.setVertexBuffer(sb, offset: slot.nTex * os, index: Int(NSG_BUF_ORDER))
                enc.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: slot.nFlat * 3)
                statDraws += 1
            }
        }
        if !dynTex.isEmpty, let p = pso(0, -1, order: false, flat: false) {
            enc.setRenderPipelineState(p)
            for d in dynTex {
                enc.drawPrimitives(type: .triangle, vertexStart: d.triStart * 3, vertexCount: d.ntri * 3, instanceCount: 1, baseInstance: d.piece)
                statDraws += 1
            }
        }
        if !dynFlat.isEmpty, let p = pso(0, -1, order: false, flat: true) {
            enc.setRenderPipelineState(p)
            for d in dynFlat {
                enc.drawPrimitives(type: .triangle, vertexStart: d.triStart * 3, vertexCount: d.ntri * 3, instanceCount: 1, baseInstance: d.piece)
                statDraws += 1
            }
        }
        // efectos opacos (y la copia de los semitransparentes texturizados: solo sus texels con bit 15 a 0): vértices ya en el mundo VR
        func restoreMeshVerts() { enc.setVertexBuffer(vertPool.buffer, offset: 0, index: Int(NSG_BUF_VERTS)) }
        if drawFx, slot.nFxOpaque > 0, let fb = slot.fxBuf, let p = psoFx(0, -1) {
            enc.setDepthBias(fxDepthBias, slopeScale: fxSlopeBias, clamp: 0)
            enc.setRenderPipelineState(p)
            enc.setVertexBuffer(fb, offset: 0, index: Int(NSG_BUF_FX))
            enc.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: slot.nFxOpaque)
            statDraws += 1
            restoreMeshVerts()
            enc.setDepthBias(0, slopeScale: 0, clamp: 0)
        }
        // UNA pasada semitransparente de atrás adelante: triángulos de mallas (kind 0) y efectos (kind 1) intercalados por profundidad (ns_gpu_semi_merge)
        if mergedN > 0 {
            enc.setDepthStencilState(noZTest ? dsSemiNoZ : dsSemi)
            var curKind: UInt32 = 99, curMode = -1
            for i in 0..<mergedN {
                let r = mergedRuns[i]
                if r.kind != 0 && !drawFx { continue }   // tramo de efectos con los efectos no permitidos: se omite, el resto del orden sigue
                let mode = Int(r.abr & 3)
                if r.kind != curKind {
                    curKind = r.kind; curMode = -1
                    if r.kind == 0 {
                        guard let ob = slot.orderBuf else { restoreMeshVerts(); return false }
                        restoreMeshVerts()
                        enc.setVertexBuffer(ob, offset: 0, index: Int(NSG_BUF_ORDER))
                        enc.setDepthBias(0, slopeScale: 0, clamp: 0)
                    } else {
                        enc.setVertexBuffer(slot.fxBuf, offset: 0, index: Int(NSG_BUF_FX))   // (drawFx implica fxBuf)
                        enc.setDepthBias(fxDepthBias, slopeScale: fxSlopeBias, clamp: 0)
                    }
                }
                if mode != curMode {
                    curMode = mode
                    guard let p = (r.kind == 0 ? pso(1, mode, order: true, flat: false) : psoFx(1, mode)) else { restoreMeshVerts(); enc.setDepthBias(0, slopeScale: 0, clamp: 0); return false }
                    enc.setRenderPipelineState(p)
                    let a: Float = mode == 3 ? 0.25 : 0.5
                    enc.setBlendColor(red: a, green: a, blue: a, alpha: a)
                }
                if r.kind == 0 { enc.drawPrimitives(type: .triangle, vertexStart: Int(r.start) * 3, vertexCount: Int(r.count) * 3) }
                else { enc.drawPrimitives(type: .triangle, vertexStart: Int(r.start), vertexCount: Int(r.count)) }
                statDraws += 1
            }
            restoreMeshVerts()
            enc.setDepthBias(0, slopeScale: 0, clamp: 0)
        }
        // recubrimientos de pantalla del ojo (fundidos, destello, modos de visión, anillo, barras), encima de todo lo del mundo
        drawOverlays(enc, viewProj: viewProj, projections: projections, layered: layered, colorFormat: colorFormat, depthFormat: depthFormat, samples: samples, linear: linear)
        statTrisStatic = slot.staticBuf != nil ? slot.nTex + slot.nFlat : 0
        statTrisDyn = dynTex.reduce(0) { $0 + $1.ntri } + dynFlat.reduce(0) { $0 + $1.ntri }
        statTrisEye = statTrisStatic + statTrisDyn + semiN
        cpuEncodeMs += (Double(Self.nowNs() - t0) / 1e6 - cpuEncodeMs) * 0.05
        return true
    }

    /// Recubrimientos de pantalla del ojo (NsGpuScreen.ov, en orden de dibujo): un triángulo a pantalla completa por recubrimiento, sin profundidad. Las tangentes del ojo salen de su
    /// proyección (t = (ndc + P[2].xy) / (P[0][0], P[1][1])), así que no dependen de la resolución del ojo ni giran con nada que no sea la cabeza.
    private func drawOverlays(_ enc: MTLRenderCommandEncoder, viewProj: [simd_float4x4], projections: [simd_float4x4], layered: Bool, colorFormat: MTLPixelFormat, depthFormat: MTLPixelFormat, samples: Int, linear: Bool) {
        let list = boxOverlays.isEmpty ? overlays : overlays + boxOverlays
        guard !list.isEmpty, !projections.isEmpty else { return }
        // la textura de la máscara de la caja solo se exige si hay recubrimientos de caja; sin ella se omite únicamente la máscara (fundidos, tinte de agua, etc. siguen)
        let haveBoxTex = boxOverlays.isEmpty ? !boxTex.isEmpty : ensureBoxTex()
        let tag = "\(layered)/\(colorFormat.rawValue)/\(depthFormat.rawValue)/\(samples)/\(linear)"
        guard ovTagsOK.contains(tag) else { return }
        func eyeP(_ i: Int) -> (Float, Float, Float, Float) {
            let p = projections[min(i, projections.count - 1)]
            return (p.columns.0.x, p.columns.1.y, p.columns.2.x, p.columns.2.y)
        }
        // eje Y del mundo VR en el espacio de vista de cada ojo = columna 1 de la matriz de vista (vista = P^-1 * (P V)); lo usan las líneas de exploración ancladas al mundo
        func upView(_ i: Int) -> (Float, Float, Float, Float) {
            let k = min(i, viewProj.count - 1), pk = min(i, projections.count - 1)
            let v = projections[pk].inverse * viewProj[k]
            let c = v.columns.1
            let l = (c.x * c.x + c.y * c.y + c.z * c.z).squareRoot()
            guard l.isFinite, l > 1e-6 else { return (0, 1, 0, 0) }
            return (c.x / l, c.y / l, c.z / l, 0)
        }
        enc.setCullMode(.none)       // sin descarte de caras en ningun pase del mundo (doble cara, como el motor)
        enc.setDepthStencilState(dsOverlay)
        enc.setDepthBias(0, slopeScale: 0, clamp: 0)
        enc.setFragmentTexture(haveBoxTex ? boxTex[boxTexCur] : ensureDummyTex(), index: 0)       // la declara ns_fs_overlay; solo la lee NSG_OV_FRAME (sin máscara: respaldo 1x1 a 0)
        for ov in list {
            if ov.kind == UInt32(NSG_OV_FRAME) && !haveBoxTex { continue }
            guard let p = overlayPipeline(OKey(layered: layered, abr: ov.abr & 3, color: colorFormat, depth: depthFormat, samples: samples, linear: linear)) else { continue }
            enc.setRenderPipelineState(p)
            var ou = NsGpuOverlayU()
            ou.ov = ov
            ou.eye0 = eyeP(0)
            ou.eye1 = layered ? eyeP(1) : eyeP(0)
            ou.up0 = upView(0)
            ou.up1 = layered ? upView(1) : upView(0)
            enc.setFragmentBytes(&ou, length: MemoryLayout<NsGpuOverlayU>.stride, index: Int(NSG_BUF_OVERLAY))
            enc.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
            statDraws += 1
        }
    }

    /// Resumen para el registro (1 vez por segundo, una línea con el de la lista de la GPU; ver RGRenderer.nativeDiag): triángulos enviados por ojo, caja envolvente en mundo VR de lo
    /// dibujado, piezas delante de la vista, cámara nativa frente a la de la GPU y a la cabeza, y la última comprobación de coherencia. Recorre las piezas: no se llama por fotograma.
    func drawnSummary(gpuEye: SIMD3<Float>?) -> String {
        var mn = SIMD3<Float>(repeating: .greatestFiniteMagnitude), mx = SIMD3<Float>(repeating: -.greatestFiniteMagnitude)
        var total = 0, front = 0
        func add(_ gi: Int, _ gm: NativeGpuMesh) {
            guard gi >= 0, gi < table.count else { return }
            let m = toVR * world4(table[gi])
            var c = SIMD3<Float>(0, 0, 0)
            for i in 0..<8 {
                let q = m * SIMD4<Float>((i & 1) == 0 ? gm.bmin.x : gm.bmax.x, (i & 2) == 0 ? gm.bmin.y : gm.bmax.y, (i & 4) == 0 ? gm.bmin.z : gm.bmax.z, 1)
                let v = SIMD3<Float>(q.x, q.y, q.z)
                guard v.x.isFinite, v.y.isFinite, v.z.isFinite else { return }
                mn = pointwiseMin(mn, v); mx = pointwiseMax(mx, v); c += v * 0.125
            }
            total += 1
            if (lastView0 * SIMD4<Float>(c.x, c.y, c.z, 1)).z < 0 { front += 1 }   // delante de la vista (Metal mira a -z)
        }
        for it in dynTex where it.piece < meshOf.count { if let gm = meshOf[it.piece] { add(it.piece, gm) } }
        for it in dynFlat where it.piece < meshOf.count { if let gm = meshOf[it.piece] { add(it.piece, gm) } }
        for it in statPcs { add(it.piece, it.mesh) }
        for it in semis { add(it.piece, it.mesh) }
        let box = total > 0 ? String(format: "[%.1f %.1f %.1f]..[%.1f %.1f %.1f]", mn.x, mn.y, mn.z, mx.x, mx.y, mx.z) : "vacia"
        let nc = toVR * SIMD4<Float>(camEye.x, camEye.y, camEye.z, 1)
        let g = gpuEye ?? SIMD3<Float>(.nan, .nan, .nan)
        ns_coh_take_stats(&cohState, &cohStats)
        let coh = cohEvaluated
            ? String(format: "ojo=%.3fm sondas=%.3fm giro=%.2fgr | desfase_tolerado=%u/%u fuera_de_tolerancia=%u caidas_a_gpu=%u peor=%.3fm/%.2fgr", cohOut.d_eye, cohOut.d_probe, cohOut.d_rot,
                      cohStats.tolerated, cohStats.frames, cohStats.over, cohStats.trips, cohStats.worst_pos_m, cohStats.worst_rot_deg) : "no evaluada"
        var s = "tris/ojo=\(statTrisEye) (escenario=\(statTrisStatic) dinamicos=\(statTrisDyn) semi=\(semiN)) piezas=\(total) (escenario=\(statPcs.count) dinamicas=\(dynTex.count + dynFlat.count) semi=\(semis.count)) delante=\(front)/\(total) caja_VR=\(box)"
        s += String(format: " | camara: nativa=(%.2f %.2f %.2f) gpu=(%.2f %.2f %.2f) cabeza=(%.2f %.2f %.2f) d_nat_cabeza=%.2fm d_gpu_cabeza=%.2fm",
                    nc.x, nc.y, nc.z, g.x, g.y, g.z, headVR.x, headVR.y, headVR.z,
                    simd_length(SIMD3<Float>(nc.x, nc.y, nc.z) - headVR), gpuEye != nil ? simd_length(g - headVR) : Float.nan)
        s += " | coherencia: " + coh + (incoherent ? " INCOHERENTE(\(cohWhy))" : "") + " | escena nativa valid=0x" + String(curValid, radix: 16)
        return s
    }

    /// Diagnóstico (1 vez por segundo): triángulos con alguna arista > `big` m y que cruzan el plano cercano entre los que se dibujan en la escena actual (cada pieza una vez), y el
    /// recuento del partido de triángulos grandes. `view` = mundo VR -> ojo del primer ojo. Recorre las mallas subidas: no se llama por fotograma.
    func diagScan(view: simd_float4x4, near: Float, big: Float) -> String {
        var acc = NsGpuScan()
        var seen = Set<Int>()
        let verts = vertPool.buffer.contents().assumingMemoryBound(to: NsGpuVert.self)
        let corners = triPool.buffer.contents().assumingMemoryBound(to: NsGpuCorner.self)
        func scan(_ gi: Int, _ gm: NativeGpuMesh) {
            guard gi >= 0, gi < table.count, gm.ntri > 0, !seen.contains(gi) else { return }
            seen.insert(gi)
            let m = view * toVR * world4(table[gi])
            let c = m.columns
            let f: [Float] = [c.0.x, c.0.y, c.0.z, c.0.w, c.1.x, c.1.y, c.1.z, c.1.w, c.2.x, c.2.y, c.2.z, c.2.w, c.3.x, c.3.y, c.3.z, c.3.w]
            ns_gpu_scan(verts, corners + gm.triStart * 3, UInt32(gm.ntri), f, near, big, &acc)
        }
        for it in dynTex where it.piece < meshOf.count { if let gm = meshOf[it.piece] { scan(it.piece, gm) } }
        for it in dynFlat where it.piece < meshOf.count { if let gm = meshOf[it.piece] { scan(it.piece, gm) } }
        for it in statPcs { scan(it.piece, it.mesh) }
        for it in semis { scan(it.piece, it.mesh) }
        var t = String(format: "tris=%u largos=%u cruzan_cercano=%u largos_y_cruzan=%u arista_max=%.1fm (piezas=%d)", acc.tris, acc.big, acc.cross, acc.big_cross, acc.max_edge, seen.count)
        if splitMeters > 0 {
            t += " partido: mallas=\(splitMeshStat) tris_partidos=\(splitTrisStat) tris_añadidos=\(splitAddedStat) sin_partir_por_soldadura=\(splitWeldStat) sin_partir_por_tope=\(splitCapStat) fallos=\(splitFailStat)"
        } else { t += " partido=no" }
        return t
    }

    /// Red de seguridad de vértices (una vez por segundo; reinicia los contadores): piezas con matriz corrupta (no se dibujan), con soldadura inconsistente (se dibujan sin soldar) y triángulos que
    /// el shader colapsa (vértice lejos de la pieza / arista desproporcionada), y el detalle por instancia, pieza y malla con la causa y el peor valor.
    func netLine() -> String {
        func causeNames(_ c: UInt32) -> String {
            var n: [String] = []
            if (c & UInt32(truncatingIfNeeded: NSG_NET_MAT)) != 0 { n.append("matriz_corrupta") }
            if (c & UInt32(truncatingIfNeeded: NSG_NET_WELD)) != 0 { n.append("soldadura_inconsistente") }
            if (c & UInt32(truncatingIfNeeded: NSG_NET_FAR)) != 0 { n.append("vertice_lejos_de_la_pieza") }
            if (c & UInt32(truncatingIfNeeded: NSG_NET_EDGE)) != 0 { n.append("arista_desproporcionada") }
            return n.isEmpty ? "?" : n.joined(separator: "+")
        }
        var t = "red-vertices: escenas=\(netScenes) piezas_vigiladas=\(netPieces) tris_vigilados=\(netTrisSeen) | piezas: matriz_corrupta=\(netPiecesMat) soldadura_inconsistente=\(netPiecesWeld) | tris_colapsados=\(netTrisCollapsed) (lejos=\(netTrisFar) arista=\(netTrisEdge))"
        let top = netRecs.sorted { $0.value.n > $1.value.n }.prefix(8)
        for (k, r) in top {
            t += " | inst=" + String(k.inst, radix: 16) + " pieza=\(k.piece) malla=" + String(k.mesh, radix: 16) + " causa=" + causeNames(r.causes) + " veces=\(r.n) tris=\(r.tris)"
            t += String(format: " salto_soldadura=%.2fm lejos=%.2fm exceso_arista=%.2fm escala=%.2f", r.gap, r.far, r.edge, r.scale)
        }
        if netRecs.count > 8 { t += " | (+\(netRecs.count - 8) mas)" }
        netRecs.removeAll(keepingCapacity: true)
        netScenes = 0; netPieces = 0; netPiecesMat = 0; netPiecesWeld = 0; netTrisCollapsed = 0; netTrisFar = 0; netTrisEdge = 0; netTrisSeen = 0
        return t
    }

    /// Resumen de la ultima ventana de 1 s de efectos cerca de la cabeza (ver ns_gpu_fx_near_describe).
    private func fxNearText() -> String {
        var buf = [CChar](repeating: 0, count: 1536)
        _ = ns_gpu_fx_near_describe(&fxNearSnap, &buf, 1536)
        return "[" + String(cString: buf) + "] |"
    }

    /// Línea de registro (una vez por segundo): estado del renderer nativo y de la última decisión.
    func logLine() -> String {
        let lp = lastPresent
        let modo = lp.mode == Int32(NS_PR_CURRENT) ? "actual" : (lp.mode == Int32(NS_PR_HOLD) ? "repetir-ultimo" : "negro")
        var why: [String] = []
        let bits: [(UInt32, String)] = [(UInt32(NS_PRW_NO_RENDERER), "renderer"), (UInt32(NS_PRW_NO_SCENE), "sin-escena"), (UInt32(NS_PRW_UNFIT), "no-apta"),
                                        (UInt32(NS_PRW_MESHES), "mallas"), (UInt32(NS_PRW_STALE), "vieja(solo-log)"), (UInt32(NS_PRW_INCOHERENT), "incoherente(solo-log)")]
        for (b, n) in bits where (lp.why & b) != 0 { why.append(n) }
        return "nativo: presenta=\(modo) fundido=\(String(format: "%.2f", lp.fade)) motivos=[\(why.joined(separator: ","))] repetidos=\(presentState.n_hold) negros=\(presentState.n_black) coh=\(String(format: "%.3f", cohDist))m " +
               "instancias=\(statInstances) piezas=\(statPieces) mallas=\(statMeshes) semi=\(statSemiTris) tramos_semi=\(mergedN) dibujos=\(statDraws) fallos=\(frameFailures)/\(totalFailures) ausentes=\(frameMisses) " +
               "efectos=\(statFxItems) (carteles=\(statFxBillboards) cintas=\(statFxRibbons) poligonos=\(statFxPolys) descartados=\(statFxSkipped) tris=\(statFxTris) dibujados=\(fxAllowed ? "si" : "no")) " +
               "pantalla=[agua=\(screenOut.water_min.3 > 0.5 ? "si" : "no") recubrimientos=\(overlays.count)] " +
               "\(fxNearText()) [nativo] escena_ausente=\(sceneAbsent) sync_epoca=\(syncHeldFrames) " +
               "escenas=\(statScenes) epoca=\(lastEpoch) cpu(prepare)=\(String(format: "%.2f", cpuPrepareMs))ms cpu(encode)=\(String(format: "%.2f", cpuEncodeMs))ms"
    }

    func diagnostics() -> String {
        logLine() + " verts=\(vertPool.alloc.used)/\(vertPool.alloc.capacity) tris=\(triPool.alloc.used)/\(triPool.alloc.capacity)"
    }
}
