import Foundation
import Metal
import simd

// Renderer nativo: dibuja la escena leída de la RAM del motor (NsScene, vr/native/ns_types.h) con las cámaras VR, en lugar de la salida de la GPU.
//
// Flujo por fotograma de visor (100 Hz):
//   prepareFrame(sortView:)  toma las dos últimas NsScene del puente (ns_bridge_acquire; interpola entre ellas) y sube a la GPU las mallas nuevas, construye
//                            la tabla de piezas y ordena de atrás adelante los triángulos semitransparentes respecto a la cabeza.
//   encode(encoder:...)      dentro del pase de escena ya abierto (el llamador fija viewports/amplificación): pase opaco con z (una llamada por pieza) y pase
//                            semitransparente (lista ordenada, 4 modos de mezcla de función fija). Sin ningún culling.
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

/// Malla en la caché de GPU.
private struct NativeMeshKey: Hashable { let id: UInt64; let ver: UInt32 }

private final class NativeGpuMesh {
    let id: UInt64
    var vertStart = 0, nverts = 0
    var triStart = 0, ntri = 0
    var flags: UInt32 = 0
    var depthBias: Int16 = 0
    var semi = false
    var opaquePass = true
    var centroid: UnsafeMutablePointer<Float>?   // 3 por triángulo (solo semitransparentes)
    var abr: UnsafeMutablePointer<UInt8>?        // modo de mezcla por triángulo
    var lastUsed: UInt32 = 0
    init(id: UInt64) { self.id = id }
    deinit { centroid?.deallocate(); abr?.deallocate() }
}

final class NativeRenderer {
    // MARK: configuración
    static let maxSemiTris = 1 << 17
    private let device: MTLDevice
    private let library: MTLLibrary
    private let ringSize: Int

    // MARK: estado de la caché
    private var vertPool: NativePool
    private var triPool: NativePool            // unidad = triángulo (3 esquinas)
    private var meshes: [NativeMeshKey: NativeGpuMesh] = [:]
    private var pendingFree: [(frame: Int, vs: Int, nv: Int, ts: Int, nt: Int)] = []
    private var lastEpoch: UInt32 = 0
    private var lastSeq: UInt32 = 0
    private var meshFailures = 0

    // MARK: escena preparada
    private struct DrawItem { var piece: Int; var triStart: Int; var ntri: Int }
    private struct SemiItem { var piece: Int; var mesh: NativeGpuMesh }
    private struct Prepared {
        var gen = 0
        var pieces: [NsGpuPiece] = []
        var draws: [DrawItem] = []
        var semis: [SemiItem] = []
        var toVR = matrix_identity_float4x4
        var fogFwd = SIMD4<Float>(0, 0, 0, 0)
        var fogPar = SIMD4<Float>(0, 1, 0, 0)
    }
    private var prepared = Prepared()
    private var genCounter = 0

    // MARK: anillo por fotograma
    private final class Slot {
        var pieceBuf: MTLBuffer?
        var pieceGen = -1
        var orderBuf: MTLBuffer?
        var orderCap = 0
        var runs: [(start: Int, count: Int, abr: Int)] = []
    }
    private var ring: [Slot]
    private var current: Slot?
    private var frameNo = 0
    private var abrScratch: [UInt8] = []

    // MARK: pipelines
    private struct PKey: Hashable {
        var layered: Bool, pass: UInt32, mode: Int, color: MTLPixelFormat, depth: MTLPixelFormat, samples: Int, linear: Bool
    }
    private var pipes: [PKey: MTLRenderPipelineState] = [:]
    private var failed = Set<String>()
    private let dsOpaque: MTLDepthStencilState
    private let dsSemi: MTLDepthStencilState

    // MARK: estadísticas
    private(set) var isActive = false
    private(set) var statDraws = 0
    private(set) var statSemiTris = 0
    private(set) var statPieces = 0
    private(set) var statMeshes = 0
    private(set) var statScenes = 0
    var enabled = true

    init?(device: MTLDevice, library: MTLLibrary, ringSize: Int = 3) {
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
        self.device = device; self.library = library
        self.ringSize = max(2, ringSize)
        vertPool = vp; triPool = tp
        dsOpaque = s1; dsSemi = s2
        ring = (0..<max(2, ringSize)).map { _ in Slot() }
    }

    // MARK: puente

    /// Puente de escenas (NsBridge de vr/native/ns_bridge.h; la app usa `mv_vrhost_native_bridge()`). Sin puente no se dibuja nada.
    var bridge: OpaquePointer?
    /// bit NS_SV_TOVR_OK de NsScene.valid (ns_bridge.h): la escena está en el mundo VR del usuario (cámara VR en primera persona).
    private static let svToVrOK: UInt32 = 64

    /// Toma la escena del puente (cada fotograma: la interpolación entre las dos últimas escenas depende de la hora) y prepara la tabla de piezas.
    @discardableResult
    func refresh() -> Bool {
        guard let b = bridge else { return false }
        var view = NsView()
        guard ns_bridge_acquire(b, ns_now_ns(), &view) != 0, let cur = view.cur else { return false }
        var ip = NsInterp()
        ns_interp_begin(b, &view, &ip)
        ingest(scene: cur, store: view.store, interp: &ip, fresh: view.fresh != 0)
        return true
    }

    // MARK: ingestión de una escena

    private func dropAll() {
        // Pools nuevos: los fotogramas en vuelo siguen usando los búferes antiguos (Metal los retiene).
        meshes.removeAll()
        pendingFree.removeAll()
        if let vp = NativePool(device: device, stride: MemoryLayout<NsGpuVert>.stride, initial: 1 << 15, maxElems: 1 << 21, label: "native.verts"),
           let tp = NativePool(device: device, stride: 3 * MemoryLayout<NsGpuCorner>.stride, initial: 1 << 15, maxElems: 1 << 20, label: "native.corners") {
            vertPool = vp; triPool = tp
        }
        prepared = Prepared()
    }

    private func releaseMesh(_ m: NativeGpuMesh) {
        if m.ntri > 0 { pendingFree.append((frameNo, m.vertStart, m.nverts, m.triStart, m.ntri)) }
    }

    private func reclaim() {
        guard !pendingFree.isEmpty else { return }
        let limit = frameNo - ringSize - 1
        var keep: [(frame: Int, vs: Int, nv: Int, ts: Int, nt: Int)] = []
        for p in pendingFree {
            if p.frame <= limit { vertPool.release(p.vs, p.nv); triPool.release(p.ts, p.nt) } else { keep.append(p) }
        }
        pendingFree = keep
    }

    private func upload(_ pm: UnsafePointer<NsMesh>, baked: UnsafePointer<NsBaked>?, key: NativeMeshKey, seq: UInt32) -> NativeGpuMesh? {
        let id = key.id
        let gm = NativeGpuMesh(id: id)
        gm.lastUsed = seq
        let nv = Int(pm.pointee.n_verts), nt = Int(ns_gpu_tri_count(pm))
        gm.flags = pm.pointee.flags
        gm.depthBias = pm.pointee.depth_bias
        if nv == 0 || nt == 0 || pm.pointee.verts == nil || pm.pointee.faces == nil { meshes[key] = gm; return nil }
        guard let vs = vertPool.allocate(nv) else { meshFailures += 1; return nil }
        guard let ts = triPool.allocate(nt) else { vertPool.release(vs, nv); meshFailures += 1; return nil }
        gm.vertStart = vs; gm.nverts = nv; gm.triStart = ts; gm.ntri = nt
        gm.semi = (gm.flags & UInt32(truncatingIfNeeded: NS_MF_BLEND)) != 0
        if gm.semi {
            gm.centroid = UnsafeMutablePointer<Float>.allocate(capacity: 3 * nt)
            gm.abr = UnsafeMutablePointer<UInt8>.allocate(capacity: nt)
        }
        var textured = false
        for f in 0..<Int(pm.pointee.n_faces) where pm.pointee.faces[f].texid != 0 { textured = true; break }
        gm.opaquePass = !gm.semi || textured
        let vptr = vertPool.buffer.contents().assumingMemoryBound(to: NsGpuVert.self) + vs
        let cptr = triPool.buffer.contents().assumingMemoryBound(to: NsGpuCorner.self) + ts * 3
        ns_gpu_pack(pm, baked, vptr, cptr, UInt32(vs), gm.centroid, gm.abr)
        meshes[key] = gm
        return gm
    }

    /// Malla (y juego de colores precalculados de la versión `ver`, si la malla los usa y existen) en la caché de GPU; la sube si aún no está.
    private func acquireMesh(id: UInt64, ver: UInt32, store: OpaquePointer?, seq: UInt32) -> NativeGpuMesh? {
        if let m = meshes[NativeMeshKey(id: id, ver: ver)] { m.lastUsed = seq; return m.ntri > 0 ? m : nil }
        guard let st = store, let pm = ns_store_get(st, id) else { return nil }
        var baked: UnsafePointer<NsBaked>? = nil
        var kver: UInt32 = 0
        if ver != 0 && (pm.pointee.flags & UInt32(truncatingIfNeeded: NS_MF_BAKED)) != 0, let b = ns_store_get_baked(st, id, ver) { baked = b; kver = ver }
        let key = NativeMeshKey(id: id, ver: kver)
        if kver != ver, let m = meshes[key] { m.lastUsed = seq; return m.ntri > 0 ? m : nil }
        return upload(pm, baked: baked, key: key, seq: seq)
    }

    /// Prepara la escena: sube mallas nuevas, construye la tabla de piezas y las listas de dibujo. Copia todo lo necesario (no guarda punteros de `sc`).
    func ingest(scene sc: UnsafePointer<NsScene>, store: OpaquePointer?, interp ip: inout NsInterp, fresh: Bool) {
        let seq = sc.pointee.seq
        lastSeq = seq
        if fresh { statScenes += 1 }
        lastEpoch = sc.pointee.mesh_epoch
        guard sc.pointee.version == UInt32(truncatingIfNeeded: NS_VERSION), sc.pointee.size == UInt32(MemoryLayout<NsScene>.size),
              sc.pointee.inst != nil, sc.pointee.piece != nil else { return }
        let inWorld = (sc.pointee.valid & Self.svToVrOK) != 0
        let nInst = Int(min(sc.pointee.n_inst, sc.pointee.cap_inst))
        let nPiece = Int(min(sc.pointee.n_piece, sc.pointee.cap_piece))

        var table = [NsGpuPiece](repeating: NsGpuPiece(), count: nPiece)
        var meshOf = [NativeGpuMesh?](repeating: nil, count: nPiece)
        let groupMask = UInt32(sc.pointee.group_mask)   // NsScene.group_mask frente a NsInstance.zone_mask
        let hidden = UInt16(truncatingIfNeeded: NS_PF_HIDDEN)
        let drawFlag = UInt32(truncatingIfNeeded: NSG_PF_DRAW)

        // 1) mallas de todas las piezas (también las ocultas: pueden ser el padre de una soldadura)
        for ii in 0..<nInst {
            let inst = sc.pointee.inst[ii]
            let first = Int(inst.first_piece), cnt = Int(inst.n_pieces)
            guard first + cnt <= nPiece else { continue }
            for k in 0..<cnt { meshOf[first + k] = acquireMesh(id: sc.pointee.piece[first + k].mesh_id, ver: sc.pointee.piece[first + k].baked_ver, store: store, seq: seq) }
        }
        // 2) tabla de piezas y listas
        var p = Prepared()
        p.pieces.reserveCapacity(nPiece)
        for ii in 0..<nInst {
            let instPtr = UnsafePointer(sc.pointee.inst + ii)
            let inst = instPtr.pointee
            let first = Int(inst.first_piece), cnt = Int(inst.n_pieces)
            guard first + cnt <= nPiece else { continue }
            let inGroup = groupMask == 0 || inst.zone_mask == 0 || (UInt32(inst.zone_mask) & groupMask) != 0
            for k in 0..<cnt {
                let gi = first + k
                guard let gm = meshOf[gi] else { continue }
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
                if gm.opaquePass { p.draws.append(DrawItem(piece: gi, triStart: gm.triStart, ntri: gm.ntri)) }
                if gm.semi { p.semis.append(SemiItem(piece: gi, mesh: gm)) }
            }
        }
        p.pieces = table
        // transformación mundo del motor -> VR y niebla
        var t = [Float](repeating: 0, count: 12)
        if ip.enabled != 0 { ns_interp_to_vr(&ip, &t) } else { let src = ns_scene_to_vr(sc); for i in 0..<12 { t[i] = src[i] } }
        var tv = matrix_identity_float4x4
        var finite = inWorld
        for i in 0..<12 where !t[i].isFinite { finite = false }
        if finite {
            tv = simd_float4x4(columns: (SIMD4<Float>(t[0], t[4], t[8], 0), SIMD4<Float>(t[1], t[5], t[9], 0), SIMD4<Float>(t[2], t[6], t[10], 0), SIMD4<Float>(t[3], t[7], t[11], 1)))
        } else { p.draws.removeAll(); p.semis.removeAll() }
        p.toVR = tv
        let rot = ns_scene_cam_rot(sc), eye = ns_scene_cam_eye(sc)
        let fwd = SIMD3<Float>(rot[2], rot[5], rot[8])
        let eyeP = SIMD3<Float>(eye[0], eye[1], eye[2])
        p.fogFwd = SIMD4<Float>(fwd.x, fwd.y, fwd.z, -simd_dot(fwd, eyeP))
        let upm = sc.pointee.units_per_m > 1 ? sc.pointee.units_per_m : 1000
        p.fogPar = SIMD4<Float>(sc.pointee.fog.start, max(sc.pointee.fog.len, 1e-3), sc.pointee.fog.active != 0 ? 1 : 0, 0.05 / upm)
        genCounter += 1
        p.gen = genCounter
        prepared = p
        statPieces = p.draws.count
        if fresh { gcMeshes(seq: seq, store: store) }
        statMeshes = meshes.count
        // Todas las mallas de la época de esta escena están ya copiadas en la GPU: el productor puede liberar la época anterior (protocolo de ns_types.h).
        if fresh, let st = store { ns_store_ack(st, sc.pointee.mesh_epoch) }
    }

    /// Libera las mallas que ya no están en el almacén (id desaparecido) y, con la caché llena, las que llevan mucho sin usarse.
    private func gcMeshes(seq: UInt32, store: OpaquePointer?) {
        let pressure = vertPool.alloc.used * 4 > vertPool.alloc.capacity * 3 || triPool.alloc.used * 4 > triPool.alloc.capacity * 3
        var dead: [NativeMeshKey] = []
        for (key, m) in meshes where m.lastUsed != seq {
            let gone = store == nil || ns_store_get(store, key.id) == nil || (key.ver != 0 && ns_store_get_baked(store, key.id, key.ver) == nil)
            if gone || (pressure && seq &- m.lastUsed > 600) { dead.append(key) }
        }
        for k in dead { if let m = meshes.removeValue(forKey: k) { releaseMesh(m) } }
    }

    // MARK: fotograma

    private func world4(_ p: NsGpuPiece) -> simd_float4x4 {
        let w = p.w
        return simd_float4x4(columns: (SIMD4<Float>(w.0, w.4, w.8, 0), SIMD4<Float>(w.1, w.5, w.9, 0), SIMD4<Float>(w.2, w.6, w.10, 0), SIMD4<Float>(w.3, w.7, w.11, 1)))
    }

    /// Prepara el fotograma: escena nueva del puente (si la hay), tabla de piezas en el búfer del anillo y orden de los semitransparentes respecto a `sortView`
    /// (matriz de vista del mundo VR al ojo de referencia, convención de Metal: -Z delante). Devuelve true si hay algo que dibujar.
    @discardableResult
    func prepareFrame(sortView: simd_float4x4) -> Bool {
        frameNo += 1
        reclaim()
        if enabled { refresh() }
        guard enabled, !prepared.pieces.isEmpty, !(prepared.draws.isEmpty && prepared.semis.isEmpty) else { isActive = false; current = nil; return false }
        let slot = ring[frameNo % ring.count]
        let bytes = prepared.pieces.count * MemoryLayout<NsGpuPiece>.stride
        if slot.pieceBuf == nil || slot.pieceBuf!.length < bytes {
            slot.pieceBuf = device.makeBuffer(length: max(bytes * 5 / 4, 4096), options: .storageModeShared)
            slot.pieceBuf?.label = "native.pieces"
            slot.pieceGen = -1
        }
        guard let pb = slot.pieceBuf else { isActive = false; current = nil; return false }
        if slot.pieceGen != prepared.gen {
            prepared.pieces.withUnsafeBytes { raw in _ = memcpy(pb.contents(), raw.baseAddress!, bytes) }
            slot.pieceGen = prepared.gen
        }
        sortSemis(slot, sortView)
        current = slot
        isActive = true
        return true
    }

    private func sortSemis(_ slot: Slot, _ view: simd_float4x4) {
        slot.runs.removeAll(keepingCapacity: true)
        statSemiTris = 0
        let items = prepared.semis
        if items.isEmpty { return }
        var total = 0
        for it in items { total += it.mesh.ntri }
        total = min(total, Self.maxSemiTris)
        if total == 0 { return }
        if slot.orderBuf == nil || slot.orderCap < total {
            let cap = max(total * 5 / 4, 1024)
            slot.orderBuf = device.makeBuffer(length: cap * MemoryLayout<NsGpuOrder>.stride, options: .storageModeShared)
            slot.orderBuf?.label = "native.order"
            slot.orderCap = cap
        }
        guard let ob = slot.orderBuf else { return }
        var srcs = [NsGpuSemiSrc]()
        srcs.reserveCapacity(items.count)
        let vt = view * prepared.toVR
        for it in items {
            let m = vt * world4(prepared.pieces[it.piece])
            // profundidad positiva hacia delante = -z de Metal
            let z = (-m.columns.0.z, -m.columns.1.z, -m.columns.2.z, -m.columns.3.z)
            srcs.append(NsGpuSemiSrc(piece: UInt32(it.piece), tri_base: UInt32(it.mesh.triStart), ntri: UInt32(it.mesh.ntri),
                                     centroid: UnsafePointer(it.mesh.centroid!), abr: UnsafePointer(it.mesh.abr!), zrow: (z.0, z.1, z.2, z.3)))
        }
        if abrScratch.count < total { abrScratch = [UInt8](repeating: 0, count: total) }
        let n = abrScratch.withUnsafeMutableBufferPointer { ap -> Int in
            Int(ns_gpu_sort_semi(srcs, srcs.count, ob.contents().assumingMemoryBound(to: NsGpuOrder.self), ap.baseAddress, total))
        }
        statSemiTris = n
        var i = 0
        while i < n {
            var j = i + 1
            while j < n && abrScratch[j] == abrScratch[i] { j += 1 }
            slot.runs.append((i, j - i, Int(abrScratch[i] & 3)))
            i = j
        }
    }

    // MARK: pipelines

    private func pipeline(_ k: PKey) -> MTLRenderPipelineState? {
        if let p = pipes[k] { return p }
        do {
            let fc = MTLFunctionConstantValues()
            var pass = k.pass, lin = k.linear
            fc.setConstantValue(&pass, type: .uint, index: Int(NSG_FC_PASS))
            fc.setConstantValue(&lin, type: .bool, index: Int(NSG_FC_LINEAR))
            let fs = try library.makeFunction(name: "ns_fs", constantValues: fc)
            let vsName = (k.pass == 0 ? "ns_vs_piece" : "ns_vs_order") + (k.layered ? "_amp" : "")
            guard let vs = library.makeFunction(name: vsName) else {
                if !failed.contains(vsName) { failed.insert(vsName); print("[NativeRenderer] falta \(vsName)") }
                return nil
            }
            let d = MTLRenderPipelineDescriptor()
            d.label = "\(vsName) blend\(k.mode)"
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

    // MARK: dibujo

    /// Dibuja la escena en el pase abierto `enc` (el llamador ya fijó viewports y, si `layered`, la amplificación de vértices a 2 vistas y la limpieza de profundidad a 0).
    /// `viewProj[i]` = proyección * vista del ojo i (Z invertida); `eyes[i]` = posición del ojo en el mundo VR (m). Con un solo pase por ojo se pasa el ojo en [0].
    /// `linear`: el destino es lineal/sRGB (convierte de gamma a lineal al escribir).
    func encode(encoder enc: MTLRenderCommandEncoder, vram: MTLTexture, viewProj: [simd_float4x4], eyes: [SIMD3<Float>], layered: Bool,
                colorFormat: MTLPixelFormat, depthFormat: MTLPixelFormat, samples: Int, linear: Bool) {
        statDraws = 0
        guard isActive, let slot = current, let pb = slot.pieceBuf, !viewProj.isEmpty, !eyes.isEmpty else { return }
        var u = NsGpuUniforms()
        let vp1 = viewProj.count > 1 ? viewProj[1] : viewProj[0]
        let e1 = eyes.count > 1 ? eyes[1] : eyes[0]
        u.view_proj = (viewProj[0], vp1)
        u.eye = (SIMD4<Float>(eyes[0].x, eyes[0].y, eyes[0].z, 1), SIMD4<Float>(e1.x, e1.y, e1.z, 1))
        let c = prepared.toVR.columns
        u.tovr = (SIMD4<Float>(c.0.x, c.1.x, c.2.x, c.3.x), SIMD4<Float>(c.0.y, c.1.y, c.2.y, c.3.y), SIMD4<Float>(c.0.z, c.1.z, c.2.z, c.3.z))
        u.fog_fwd = prepared.fogFwd
        u.fog_par = prepared.fogPar
        enc.setCullMode(.none)
        enc.setFragmentTexture(vram, index: 0)
        enc.setVertexBuffer(vertPool.buffer, offset: 0, index: Int(NSG_BUF_VERTS))
        enc.setVertexBuffer(triPool.buffer, offset: 0, index: Int(NSG_BUF_CORNERS))
        enc.setVertexBuffer(pb, offset: 0, index: Int(NSG_BUF_PIECES))
        enc.setVertexBytes(&u, length: MemoryLayout<NsGpuUniforms>.stride, index: Int(NSG_BUF_UNI))

        // opacos
        if !prepared.draws.isEmpty,
           let pso = pipeline(PKey(layered: layered, pass: 0, mode: -1, color: colorFormat, depth: depthFormat, samples: samples, linear: linear)) {
            enc.setRenderPipelineState(pso)
            enc.setDepthStencilState(dsOpaque)
            for d in prepared.draws {
                var ix = UInt32(d.piece)
                enc.setVertexBytes(&ix, length: 4, index: Int(NSG_BUF_PIECEIX))
                enc.drawPrimitives(type: .triangle, vertexStart: d.triStart * 3, vertexCount: d.ntri * 3)
                statDraws += 1
            }
        }
        // semitransparentes, de atrás adelante
        if !slot.runs.isEmpty, let ob = slot.orderBuf {
            enc.setDepthStencilState(dsSemi)
            enc.setVertexBuffer(ob, offset: 0, index: Int(NSG_BUF_ORDER))
            var curMode = -1
            for r in slot.runs {
                if r.abr != curMode {
                    curMode = r.abr
                    guard let pso = pipeline(PKey(layered: layered, pass: 1, mode: curMode, color: colorFormat, depth: depthFormat, samples: samples, linear: linear)) else { curMode = -2; continue }
                    enc.setRenderPipelineState(pso)
                    let a: Float = curMode == 3 ? 0.25 : 0.5
                    enc.setBlendColor(red: a, green: a, blue: a, alpha: a)
                }
                if curMode < 0 { continue }
                enc.drawPrimitives(type: .triangle, vertexStart: r.start * 3, vertexCount: r.count * 3)
                statDraws += 1
            }
        }
    }

    func diagnostics() -> String {
        "nativo: activo=\(isActive) escenas=\(statScenes) piezas=\(statPieces) mallas=\(statMeshes) semi=\(statSemiTris) dibujos=\(statDraws) fallos-malla=\(meshFailures) " +
        "verts=\(vertPool.alloc.used)/\(vertPool.alloc.capacity) tris=\(triPool.alloc.used)/\(triPool.alloc.capacity)"
    }
}
