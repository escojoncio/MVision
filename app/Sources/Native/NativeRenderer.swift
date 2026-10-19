import Foundation
import Metal
import simd

// Renderer nativo: dibuja la escena leída de la RAM del motor (NsScene, vr/native/ns_types.h) con las cámaras VR, en lugar de la salida de la GPU.
//
// Flujo por fotograma de visor (100 Hz), ver la documentación de la clase NativeRenderer:
//   beginFrame(commandBuffer:sortView:draw:)  toma las dos últimas NsScene del puente (ns_bridge_acquire; interpola), sube mallas, hace ns_store_ack, tabla de piezas.
//   decide(...)                               ns_native_decide con el estado real.
//   encode(encoder:...)                       escenario en 1-2 llamadas, dinámicos por pieza, semitransparentes ordenados. Sin culling.
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
///   decide(...)         decisión nativo/GPU del fotograma (ns_native_decide) con el estado real.
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
    private var totalFailures = 0

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
    private var fogFwd = SIMD4<Float>(0, 0, 0, 0)
    private var fogPar = SIMD4<Float>(0, 1, 0, 0)
    // orden de los semitransparentes (persistente; se recalcula solo si cambia la escena o la cabeza se mueve)
    private var semiOrder: UnsafeMutablePointer<NsGpuOrder>
    private var semiAbr: UnsafeMutablePointer<UInt8>
    private var semiN = 0
    private var semiRuns: [(start: Int, count: Int, abr: Int)] = []
    private var semiSrcs: [NsGpuSemiSrc] = []
    private var semiScratch = NsGpuSortScratch(a: nil, b: nil, cap: 0)
    private var semiDirty = true
    private var lastSortHead = SIMD3<Float>(repeating: .infinity)

    // MARK: vista actual del puente
    private var view = NsView()
    private var haveView = false
    private var lastEpoch: UInt32 = 0
    private var lastSeq: UInt32 = 0

    // MARK: anillo por fotograma
    private final class Slot {
        var pieceBuf: MTLBuffer?
        var orderBuf: MTLBuffer?
        var staticBuf: MTLBuffer?
        var staticGen = -1
        var nTex = 0, nFlat = 0
    }
    private var ring: [Slot]
    private var current: Slot?
    private var currentIx = -1

    // MARK: pipelines
    private struct PKey: Hashable {
        var layered: Bool, pass: UInt32, mode: Int, order: Bool, flat: Bool, color: MTLPixelFormat, depth: MTLPixelFormat, samples: Int, linear: Bool
    }
    private var pipes: [PKey: MTLRenderPipelineState] = [:]
    private var failed = Set<String>()
    private var readyOK = Set<String>()
    private let dsOpaque: MTLDepthStencilState
    private let dsSemi: MTLDepthStencilState

    // MARK: estadísticas (hilo de render)
    private(set) var isActive = false
    private(set) var statDraws = 0
    private(set) var statSemiTris = 0
    private(set) var statPieces = 0
    private(set) var statMeshes = 0
    private(set) var statScenes = 0
    private(set) var statInstances = 0
    private(set) var lastDecision = NsDecision()
    private var cpuPrepareMs = 0.0, cpuEncodeMs = 0.0
    private var ownerThread: pthread_t?
    private var warnedThread = false

    init?(device: MTLDevice, library: MTLLibrary, ringSize: Int = 3, queue: DispatchQueue? = nil) {
        guard MemoryLayout<NsGpuVert>.stride == 16, MemoryLayout<NsGpuCorner>.stride == 28, MemoryLayout<NsGpuPiece>.stride == 164,
              MemoryLayout<NsGpuOrder>.stride == 8 else {
            print("[NativeRenderer] layout de estructuras inesperado: vert=\(MemoryLayout<NsGpuVert>.stride) corner=\(MemoryLayout<NsGpuCorner>.stride) piece=\(MemoryLayout<NsGpuPiece>.stride) order=\(MemoryLayout<NsGpuOrder>.stride)")
            return nil
        }
        guard let vp = NativePool(device: device, stride: MemoryLayout<NsGpuVert>.stride, initial: 1 << 15, maxElems: 1 << 21, label: "native.verts"),
              let tp = NativePool(device: device, stride: 3 * MemoryLayout<NsGpuCorner>.stride, initial: 1 << 15, maxElems: 1 << 20, label: "native.corners") else { return nil }
        let d1 = MTLDepthStencilDescriptor()
        d1.depthCompareFunction = .greaterEqual; d1.isDepthWriteEnabled = true
        let d2 = MTLDepthStencilDescriptor()
        d2.depthCompareFunction = .greaterEqual; d2.isDepthWriteEnabled = false
        guard let s1 = device.makeDepthStencilState(descriptor: d1), let s2 = device.makeDepthStencilState(descriptor: d2) else { return nil }
        self.device = device; self.library = library; self.queue = queue
        self.ringSize = max(2, ringSize)
        vertPool = vp; triPool = tp
        dsOpaque = s1; dsSemi = s2
        ring = (0..<max(2, ringSize)).map { _ in Slot() }
        slotBusy = [Bool](repeating: false, count: max(2, ringSize))
        slotSem = DispatchSemaphore(value: max(2, ringSize))
        semiOrder = UnsafeMutablePointer<NsGpuOrder>.allocate(capacity: Self.maxSemiTris)
        semiAbr = UnsafeMutablePointer<UInt8>.allocate(capacity: Self.maxSemiTris)
    }

    deinit {
        semiOrder.deallocate(); semiAbr.deallocate()
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
        haveView = false
        guard enabled, let b = bridge else { return }
        var v = NsView()
        guard ns_bridge_acquire(b, ns_now_ns(), &v) != 0, let cur = v.cur else { return }
        view = v
        haveView = true
        var ip = NsInterp()
        ns_interp_begin(b, &view, &ip)
        let fresh = view.fresh != 0
        ingest(scene: cur, store: view.store, interp: &ip, fresh: fresh)
        // Todas las mallas de la época de esta escena están copiadas en la GPU: el productor puede liberar la anterior (protocolo de ns_types.h). Siempre, se
        // dibuje o no (cinemáticas, menús): si no, el almacén se llenaría.
        if fresh, let st = view.store { ns_store_ack(st, cur.pointee.mesh_epoch) }
    }

    // MARK: caché de mallas

    private func releaseMesh(_ m: NativeGpuMesh) {
        if m.ntri > 0 { pendingFree.append((serial, m.vertStart, m.nverts, m.triStart, m.ntri)) }
        meshesChanged = true
    }

    private func allocRetry(_ pool: NativePool, _ n: Int, seq: UInt32, store: OpaquePointer?) -> Int? {
        if let s = pool.allocate(n) { return s }
        // sin sitio: liberar todo lo que esta escena no usa y recuperar lo que la GPU ya terminó, y reintentar una vez
        gcMeshes(seq: seq, store: store, force: true)
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
        if nv == 0 || nt == 0 || pm.pointee.verts == nil || pm.pointee.faces == nil { meshes[key] = gm; return nil }
        guard let vs = allocRetry(vertPool, nv, seq: seq, store: store) else { frameFailures += 1; totalFailures += 1; return nil }
        guard let ts = allocRetry(triPool, nt, seq: seq, store: store) else { vertPool.release(vs, nv); frameFailures += 1; totalFailures += 1; return nil }
        gm.vertStart = vs; gm.nverts = nv; gm.triStart = ts; gm.ntri = nt
        gm.semi = (gm.flags & UInt32(truncatingIfNeeded: NS_MF_BLEND)) != 0
        if gm.semi {
            gm.centroid = UnsafeMutablePointer<Float>.allocate(capacity: 3 * nt)
            gm.abr = UnsafeMutablePointer<UInt8>.allocate(capacity: nt)
        }
        var textured = false
        for f in 0..<Int(pm.pointee.n_faces) where pm.pointee.faces[f].texid != 0 { textured = true; break }
        gm.opaquePass = !gm.semi || textured
        gm.flat = !gm.semi && !textured
        let vptr = vertPool.buffer.contents().assumingMemoryBound(to: NsGpuVert.self) + vs
        let cptr = triPool.buffer.contents().assumingMemoryBound(to: NsGpuCorner.self) + ts * 3
        ns_gpu_pack(pm, baked, vptr, cptr, UInt32(vs), gm.centroid, gm.abr)
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

    private func ingest(scene sc: UnsafePointer<NsScene>, store: OpaquePointer?, interp ip: inout NsInterp, fresh: Bool) {
        let t0 = Self.nowNs()
        let seq = sc.pointee.seq
        lastSeq = seq
        lastEpoch = sc.pointee.mesh_epoch
        if fresh { statScenes += 1 }
        frameFailures = 0; frameMisses = 0
        pieceCount = 0
        dynTex.removeAll(keepingCapacity: true); dynFlat.removeAll(keepingCapacity: true)
        semis.removeAll(keepingCapacity: true); statPcs.removeAll(keepingCapacity: true)
        guard sc.pointee.version == UInt32(truncatingIfNeeded: NS_VERSION), sc.pointee.size == UInt32(MemoryLayout<NsScene>.size),
              sc.pointee.inst != nil, sc.pointee.piece != nil else { return }
        let inWorld = (sc.pointee.valid & UInt32(truncatingIfNeeded: NS_SV_TOVR_OK)) != 0
        let nInst = Int(min(sc.pointee.n_inst, sc.pointee.cap_inst))
        let nPiece = Int(min(sc.pointee.n_piece, sc.pointee.cap_piece))
        statInstances = nInst
        if fresh { staticDirty = true; semiDirty = true }
        if table.count < nPiece { table.append(contentsOf: repeatElement(NsGpuPiece(), count: nPiece - table.count)) }
        if meshOf.count < nPiece { meshOf.append(contentsOf: repeatElement(nil, count: nPiece - meshOf.count)) }
        for i in 0..<nPiece { meshOf[i] = nil }
        let groupMask = UInt32(sc.pointee.group_mask)
        let hidden = UInt16(truncatingIfNeeded: NS_PF_HIDDEN)
        let drawFlag = UInt32(truncatingIfNeeded: NSG_PF_DRAW)
        let staticFlag = UInt16(truncatingIfNeeded: NS_IF_STATIC)

        // 1) mallas de todas las piezas (también las ocultas: pueden ser el padre de una soldadura)
        for ii in 0..<nInst {
            let first = Int(sc.pointee.inst[ii].first_piece), cnt = Int(sc.pointee.inst[ii].n_pieces)
            guard first + cnt <= nPiece else { continue }
            for k in 0..<cnt {
                let pp = sc.pointee.piece + (first + k)
                meshOf[first + k] = acquireMesh(id: pp.pointee.mesh_id, ver: pp.pointee.baked_ver, store: store, seq: seq)
            }
        }
        // 2) tabla de piezas y listas
        for ii in 0..<nInst {
            let instPtr = UnsafePointer(sc.pointee.inst + ii)
            let first = Int(instPtr.pointee.first_piece), cnt = Int(instPtr.pointee.n_pieces)
            guard first + cnt <= nPiece else { continue }
            let isStatic = (instPtr.pointee.flags & staticFlag) != 0
            let zm = UInt32(instPtr.pointee.zone_mask)
            let inGroup = groupMask == 0 || zm == 0 || (zm & groupMask) != 0
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
                if ip.enabled != 0 && ns_interp_piece(&ip, UInt32(ii), UInt32(k), &xf) != 0 { pc.world = xf }
                withUnsafePointer(to: &pc) { pcp in
                    ns_gpu_make_piece(sc, instPtr, pcp, gm.flags, Int32(gm.depthBias), UInt32(gm.vertStart), parentGlobal, parentNV, &table[gi])
                }
                if !inGroup || (piecePtr.pointee.flags & hidden) != 0 { table[gi].flags &= ~drawFlag; continue }
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
        var t = [Float](repeating: 0, count: 12)
        if ip.enabled != 0 { ns_interp_to_vr(&ip, &t) } else { let src = ns_scene_to_vr(sc)!; for i in 0..<12 { t[i] = src[i] } }
        var tv = matrix_identity_float4x4
        var finite = inWorld
        for i in 0..<12 where !t[i].isFinite { finite = false }
        if finite {
            tv = simd_float4x4(columns: (SIMD4<Float>(t[0], t[4], t[8], 0), SIMD4<Float>(t[1], t[5], t[9], 0), SIMD4<Float>(t[2], t[6], t[10], 0), SIMD4<Float>(t[3], t[7], t[11], 1)))
        } else { dynTex.removeAll(); dynFlat.removeAll(); semis.removeAll(); statPcs.removeAll(); pieceCount = 0 }
        toVR = tv
        let rot = ns_scene_cam_rot(sc)!, eye = ns_scene_cam_eye(sc)!
        let fwd = SIMD3<Float>(rot[2], rot[5], rot[8])
        let eyeP = SIMD3<Float>(eye[0], eye[1], eye[2])
        fogFwd = SIMD4<Float>(fwd.x, fwd.y, fwd.z, -simd_dot(fwd, eyeP))
        let upm = sc.pointee.units_per_m > 1 ? sc.pointee.units_per_m : 1000
        fogPar = SIMD4<Float>(sc.pointee.fog.start, max(sc.pointee.fog.len, 1e-3), sc.pointee.fog.active != 0 ? 1 : 0, 0.05 / upm)
        if fresh { gcMeshes(seq: seq, store: store, force: false) }
        if meshesChanged { staticDirty = true; semiDirty = true; meshesChanged = false }
        if staticDirty { rebuildStatic() }
        statPieces = dynTex.count + dynFlat.count + statPcs.count
        statMeshes = meshes.count
        cpuPrepareMs += (Double(Self.nowNs() - t0) / 1e6 - cpuPrepareMs) * 0.05
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
    func beginFrame(commandBuffer cb: MTLCommandBuffer, sortView: simd_float4x4, draw: Bool) -> Bool {
        checkThread()
        let t0 = Self.nowNs()
        serial += 1
        let my = serial
        var ix = -1
        if draw && enabled {
            slotSem.wait()
            lock.lock()
            if let f = slotBusy.firstIndex(of: false) { slotBusy[f] = true; ix = f }
            lock.unlock()
        }
        let slotIx = ix
        cb.addCompletedHandler { [weak self] _ in self?.frameDone(serial: my, slot: slotIx) }
        reclaim()
        drain()
        isActive = false; current = nil; currentIx = ix
        guard draw, enabled, ix >= 0, haveView, pieceCount > 0, !(dynTex.isEmpty && dynFlat.isEmpty && semis.isEmpty && statPcs.isEmpty) else { return false }
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
        cpuPrepareMs += (Double(Self.nowNs() - t0) / 1e6 - cpuPrepareMs) * 0.05
        return true
    }

    /// Orden de los semitransparentes: solo se recalcula si cambió la escena (o la caché) o la cabeza se movió más que el umbral.
    private func updateSemiOrder(_ sortView: simd_float4x4) {
        statSemiTris = 0
        if semis.isEmpty { semiN = 0; semiRuns.removeAll(keepingCapacity: true); return }
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
        let n = Int(ns_gpu_sort_semi(semiSrcs, semiSrcs.count, semiOrder, semiAbr, Self.maxSemiTris, &semiScratch))
        semiN = n
        statSemiTris = n
        semiRuns.removeAll(keepingCapacity: true)
        var i = 0
        while i < n {
            var j = i + 1
            while j < n && semiAbr[j] == semiAbr[i] { j += 1 }
            semiRuns.append((i, j - i, Int(semiAbr[i] & 3)))
            i = j
        }
    }

    // MARK: decisión nativo / GPU

    /// El render nativo se puede dibujar entero en este fotograma: ranura preparada, todas las mallas de la escena subidas y las pipelines listas.
    var frameReady: Bool { isActive && current != nil && frameFailures == 0 && frameMisses == 0 }

    /// Decisión del fotograma (ns_native_decide): modo pedido, estado real del renderer, escena de la lista marcada y escena nativa apta y reciente.
    /// Nunca deja un fotograma sin 3D: si algo falta, native = 0 y skip = 0.
    func decide(wantNative: Bool, sceneMarked: Bool, pipelinesReady: Bool) -> NsDecision {
        var d = NsDecision()
        if !haveView { d.why = Int32(NS_WHY_NOSCENE); lastDecision = d; return d }
        var inp = NsDecideIn(want_native: wantNative ? 1 : 0, renderer_ok: (enabled && pipelinesReady && isActive) ? 1 : 0,
                             meshes_ok: frameReady ? 1 : 0, scene_marked: sceneMarked ? 1 : 0, now_ns: ns_now_ns(), max_age_ns: 250_000_000)
        ns_native_decide(&view, &inp, &d)
        lastDecision = d
        return d
    }

    /// El dibujo nativo falló a mitad de fotograma: se pinta todo por GPU (las partes ya saltadas de pasadas anteriores no se recuperan; ver RGRenderer).
    func fail() {
        ns_native_fail(&lastDecision)
    }

    // MARK: pipelines

    private func pipeline(_ k: PKey) -> MTLRenderPipelineState? {
        if let p = pipes[k] { return p }
        do {
            let fc = MTLFunctionConstantValues()
            var pass = k.pass, lin = k.linear, flat = k.flat
            fc.setConstantValue(&pass, type: .uint, index: Int(NSG_FC_PASS))
            fc.setConstantValue(&lin, type: .bool, index: Int(NSG_FC_LINEAR))
            fc.setConstantValue(&flat, type: .bool, index: Int(NSG_FC_FLAT))
            let fs = try library.makeFunction(name: "ns_fs", constantValues: fc)
            let vsName = ((k.pass == 1 || k.order) ? "ns_vs_order" : "ns_vs_piece") + (k.layered ? "_amp" : "")
            guard let vs = library.makeFunction(name: vsName) else {
                if !failed.contains(vsName) { failed.insert(vsName); print("[NativeRenderer] falta \(vsName)") }
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
            let key = "\(k)"
            if !failed.contains(key) { failed.insert(key); print("[NativeRenderer] pipeline: \(error)") }
            return nil
        }
    }

    /// Compila (si faltan) las 8 pipelines del formato de destino: opacos (por pieza / lista, con y sin texel) y los 4 modos de mezcla de los semitransparentes.
    /// Devuelve true si están todas. Se llama al crear el renderer con los formatos conocidos y de nuevo con los del pase real (no hace nada si ya estaban).
    @discardableResult
    func prewarm(layered: Bool, colorFormat: MTLPixelFormat, depthFormat: MTLPixelFormat, samples: Int, linear: Bool) -> Bool {
        let tag = "\(layered)/\(colorFormat.rawValue)/\(depthFormat.rawValue)/\(samples)/\(linear)"
        if readyOK.contains(tag) { return true }
        var ok = true
        for order in [false, true] {
            for flat in [false, true] {
                if pipeline(PKey(layered: layered, pass: 0, mode: -1, order: order, flat: flat, color: colorFormat, depth: depthFormat, samples: samples, linear: linear)) == nil { ok = false }
            }
        }
        for m in 0..<4 {
            if pipeline(PKey(layered: layered, pass: 1, mode: m, order: true, flat: false, color: colorFormat, depth: depthFormat, samples: samples, linear: linear)) == nil { ok = false }
        }
        if ok { readyOK.insert(tag) }
        return ok
    }

    // MARK: dibujo

    /// Dibuja la escena en el pase abierto `enc` (el llamador ya fijó viewports y, si `layered`, la amplificación de vértices a 2 vistas; profundidad inversa, limpia a 0;
    /// NO se escribe depthRange). `viewProj[i]` = proyección * vista del ojo i; `eyes[i]` = posición del ojo en el mundo VR (m). Con un solo pase por ojo se pasa el ojo en [0].
    /// `linear`: el destino es lineal/sRGB. Devuelve false si no pudo dibujar (pipeline ausente o ranura no preparada).
    @discardableResult
    func encode(encoder enc: MTLRenderCommandEncoder, vram: MTLTexture, viewProj: [simd_float4x4], eyes: [SIMD3<Float>], layered: Bool,
                colorFormat: MTLPixelFormat, depthFormat: MTLPixelFormat, samples: Int, linear: Bool) -> Bool {
        checkThread()
        statDraws = 0
        guard isActive, let slot = current, let pb = slot.pieceBuf, !viewProj.isEmpty, !eyes.isEmpty else { return false }
        guard prewarm(layered: layered, colorFormat: colorFormat, depthFormat: depthFormat, samples: samples, linear: linear) else { return false }
        let t0 = Self.nowNs()
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
        enc.setCullMode(.none)
        enc.setFragmentTexture(vram, index: 0)
        enc.setVertexBuffer(vertPool.buffer, offset: 0, index: Int(NSG_BUF_VERTS))
        enc.setVertexBuffer(triPool.buffer, offset: 0, index: Int(NSG_BUF_CORNERS))
        enc.setVertexBuffer(pb, offset: 0, index: Int(NSG_BUF_PIECES))
        enc.setVertexBytes(&u, length: MemoryLayout<NsGpuUniforms>.stride, index: Int(NSG_BUF_UNI))

        // opacos: escenario en una llamada por clase (lista de triángulos), luego los dinámicos por pieza (índice = instance_id)
        enc.setDepthStencilState(dsOpaque)
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
        // semitransparentes, de atrás adelante
        if !semiRuns.isEmpty, let ob = slot.orderBuf {
            enc.setDepthStencilState(dsSemi)
            enc.setVertexBuffer(ob, offset: 0, index: Int(NSG_BUF_ORDER))
            var curMode = -1
            for r in semiRuns {
                if r.abr != curMode {
                    curMode = r.abr
                    guard let p = pso(1, curMode, order: true, flat: false) else { curMode = -2; continue }
                    enc.setRenderPipelineState(p)
                    let a: Float = curMode == 3 ? 0.25 : 0.5
                    enc.setBlendColor(red: a, green: a, blue: a, alpha: a)
                }
                if curMode < 0 { continue }
                enc.drawPrimitives(type: .triangle, vertexStart: r.start * 3, vertexCount: r.count * 3)
                statDraws += 1
            }
        }
        cpuEncodeMs += (Double(Self.nowNs() - t0) / 1e6 - cpuEncodeMs) * 0.05
        return true
    }

    /// Línea de registro (una vez por segundo): estado del renderer nativo y de la última decisión.
    func logLine() -> String {
        let why = ["ok", "modo", "renderer", "mallas", "lista-sin-marca", "sin-escena", "escena-vieja", "escena-no-apta", "fallo-dibujo"]
        let w = Int(lastDecision.why)
        return "nativo: decision=\(lastDecision.native != 0 ? "nativo" : "gpu") motivo=\(w >= 0 && w < why.count ? why[w] : "\(w)") skip=\(lastDecision.skip) " +
               "instancias=\(statInstances) piezas=\(statPieces) mallas=\(statMeshes) semi=\(statSemiTris) dibujos=\(statDraws) fallos=\(frameFailures)/\(totalFailures) ausentes=\(frameMisses) " +
               "escenas=\(statScenes) epoca=\(lastEpoch) cpu(prepare)=\(String(format: "%.2f", cpuPrepareMs))ms cpu(encode)=\(String(format: "%.2f", cpuEncodeMs))ms"
    }

    func diagnostics() -> String {
        logLine() + " verts=\(vertPool.alloc.used)/\(vertPool.alloc.capacity) tris=\(triPool.alloc.used)/\(triPool.alloc.capacity)"
    }
}
