import ARKit
import Foundation
import GameController
import simd

/// Pose en el espacio ARKit/inmersivo (Y arriba, adelante = -Z) de los mandos PS VR2 Sense, vía
/// AccessoryTrackingProvider (visionOS 26). Sin permiso, sin soporte o antes de visionOS 26 las manos quedan
/// como "no seguidas" y la locomoción cae a la cabeza. Hilo: las actualizaciones llegan en una Task propia;
/// `pose(hand:)` se puede llamar desde cualquier hilo.
///
/// MARCOS. La pose del anclaje (`position/rotation`) es la del DISPOSITIVO (su origen no es la mano). El sistema da además ubicaciones con
/// nombre del accesorio (`Accessory.LocationName`: `.grip` = centro de la palma, `.aim` = punto de apuntado; convención OpenXR: -Z hacia
/// delante, +Y arriba): `grip*` es lo que debe mover la mano del personaje y `aim*` el arma. Si el sistema no las entrega, `grip*` cae a la
/// pose del dispositivo y `aim*` queda sin validar (`hasLocations` = false).
final class VRInputPose: @unchecked Sendable {
    struct Pose {
        var tracked = false
        var position = SIMD3<Float>(0, 0, 0)
        var rotation = SIMD4<Float>(0, 0, 0, 1)   // x, y, z, w
        /// Ubicaciones del accesorio (mismo espacio que `position`).
        var hasLocations = false
        var gripPosition = SIMD3<Float>(0, 0, 0)
        var gripRotation = SIMD4<Float>(0, 0, 0, 1)
        var aimPosition = SIMD3<Float>(0, 0, 0)
        var aimRotation = SIMD4<Float>(0, 0, 0, 1)
    }

    static let shared = VRInputPose()

    private let lock = NSLock()
    private var poses = [Pose(), Pose()]          // 0 izquierda, 1 derecha
    private var session: ARKitSession?
    private var task: Task<Void, Never>?
    private var requested: Set<ObjectIdentifier> = []   // último conjunto pedido a track()
    private var generation = 0                            // invalida arranques en curso cuando cambia el conjunto
    private var locationsLogged = [false, false]
    private var locationFail = [0, 0]

    private init() {}

    func pose(hand: Int) -> Pose {
        lock.lock()
        defer { lock.unlock() }
        return poses[min(max(hand, 0), 1)]
    }

    /// Sigue estos mandos (y ningún otro). Vuelve enseguida; el permiso y la carga siguen en segundo plano.
    func track(_ controllers: [GCController]) {
        let wanted = Set(controllers.map(ObjectIdentifier.init))
        lock.lock()
        if wanted == requested {
            lock.unlock()
            return
        }
        // Un cambio de conjunto durante un arranque en curso (p. ej. el segundo Sense conecta justo después del
        // primero) ya no se descarta: sube la generación, el arranque viejo se aborta y se lanza uno nuevo.
        requested = wanted
        generation += 1
        let gen = generation
        lock.unlock()
        teardown()
        guard !controllers.isEmpty else { return }
        guard #available(visionOS 26.0, *) else {
            Storage.log("seguimiento de Sense: requiere visionOS 26")
            return
        }
        Task.detached(priority: .userInitiated) { [weak self] in
            await self?.start(controllers, generation: gen)
        }
    }

    @available(visionOS 26.0, *)
    private func start(_ controllers: [GCController], generation gen: Int) async {
        func current() -> Bool { lock.withLock { generation == gen } }
        guard AccessoryTrackingProvider.isSupported else {
            Storage.log("seguimiento de Sense: no soportado")
            return
        }
        // Un mando recién conectado puede tardar en ser "rastreable" para el sistema (o serlo solo tras el permiso de
        // seguimiento): se reintenta con espera creciente mientras este conjunto siga pedido, sin bloquear el resto.
        var accessories: [Accessory] = []
        var pending = controllers
        var attempt = 0
        while !pending.isEmpty, current() {
            var still: [GCController] = []
            for controller in pending {
                do {
                    accessories.append(try await Accessory(device: controller))
                    Storage.log("seguimiento de Sense: accesorio listo (\(controller.vendorName ?? "?"))")
                } catch {
                    still.append(controller)
                    if attempt == 0 || attempt % 10 == 9 {
                        Storage.log("seguimiento de Sense: sin accesorio todavía (\(error.localizedDescription)); intento \(attempt + 1)")
                    }
                }
            }
            pending = still
            // Con al menos uno listo se arranca ya; los que falten se reintentan aparte (más abajo).
            if !accessories.isEmpty || pending.isEmpty { break }
            attempt += 1
            try? await Task.sleep(nanoseconds: attempt < 10 ? 2_000_000_000 : 10_000_000_000)
        }
        if !pending.isEmpty, !accessories.isEmpty {
            let missing = pending
            Task.detached(priority: .utility) { [weak self] in
                for i in 0..<60 {
                    try? await Task.sleep(nanoseconds: i < 10 ? 3_000_000_000 : 10_000_000_000)
                    guard let self, self.lock.withLock({ self.generation == gen }) else { return }
                    for c in missing {
                        if (try? await Accessory(device: c)) != nil {
                            Storage.log("seguimiento de Sense: el otro mando ya es rastreable; se reinicia el seguimiento")
                            self.lock.withLock { self.requested = [] }
                            self.track(controllers)
                            return
                        }
                    }
                }
            }
        }
        guard !accessories.isEmpty, current() else { return }
        let session = ARKitSession()
        _ = await session.requestAuthorization(for: AccessoryTrackingProvider.requiredAuthorizations)
        let provider = AccessoryTrackingProvider(accessories: accessories)
        do {
            try await session.run([provider])
        } catch {
            Storage.log("seguimiento de Sense: no arranca (\(error.localizedDescription))")
            return
        }
        let keep: Bool = lock.withLock {
            guard generation == gen else { return false }
            self.session = session
            return true
        }
        guard keep else { session.stop(); return }
        Storage.log("seguimiento de Sense: activo (\(accessories.count))")
        let updates = Task.detached(priority: .userInitiated) { [weak self] in
            for await update in provider.anchorUpdates {
                if Task.isCancelled { break }
                self?.receive(update.anchor)
            }
        }
        let stale: Bool = lock.withLock {
            if generation == gen { task = updates; return false }
            return true
        }
        if stale { updates.cancel(); session.stop() }
    }

    func stop() {
        lock.withLock { requested = []; generation += 1 }
        teardown()
    }

    private func teardown() {
        lock.lock()
        let session = self.session
        let task = self.task
        self.session = nil
        self.task = nil
        poses = [Pose(), Pose()]
        locationsLogged = [false, false]
        lock.unlock()
        task?.cancel()
        session?.stop()
    }

    @available(visionOS 26.0, *)
    private func receive(_ anchor: AccessoryAnchor) {
        // Mano que lo sujeta; si no se sabe, la mano para la que está hecho (mismo criterio que el motor propio).
        let held = String(describing: anchor.heldChirality).lowercased()
        let inherent = "\(anchor.accessory.inherentChirality)".lowercased()
        let isLeft = held.contains("left") || (!held.contains("right") && inherent.contains("left"))
        let hand = isLeft ? 0 : 1
        var p = Pose()
        if anchor.isTracked {
            let m = anchor.originFromAnchorTransform
            let (pos, rot) = Self.split(m)
            p = Pose(tracked: true, position: pos, rotation: rot)
            // Agarre y apuntado: ubicaciones del propio accesorio (sin corrección: posiciones reales, sin el ajuste de pantalla de paso).
            if let g = Self.location(anchor, .grip), let a = Self.location(anchor, .aim) {
                let (gp, gr) = Self.split(g), (ap, ar) = Self.split(a)
                p.hasLocations = true
                p.gripPosition = gp; p.gripRotation = gr
                p.aimPosition = ap; p.aimRotation = ar
                if !lock.withLock({ locationsLogged[hand] }) {
                    lock.withLock { locationsLogged[hand] = true; locationFail[hand] = 0 }
                    // Desplazamientos medidos (marco del dispositivo): sirven para documentar el mando.
                    let inv = m.inverse
                    let dg = inv * g, da = inv * a
                    Storage.log("seguimiento de Sense: ubicaciones de \(hand == 0 ? "izq" : "der"): agarre desde el dispositivo (m) \(String(format: "%.3f %.3f %.3f", dg.columns.3.x, dg.columns.3.y, dg.columns.3.z)), apuntado \(String(format: "%.3f %.3f %.3f", da.columns.3.x, da.columns.3.y, da.columns.3.z)); ejes del agarre en el dispositivo X=(\(String(format: "%.2f %.2f %.2f", dg.columns.0.x, dg.columns.0.y, dg.columns.0.z))) Y=(\(String(format: "%.2f %.2f %.2f", dg.columns.1.x, dg.columns.1.y, dg.columns.1.z))) Z=(\(String(format: "%.2f %.2f %.2f", dg.columns.2.x, dg.columns.2.y, dg.columns.2.z))); ejes del apuntado Z=(\(String(format: "%.2f %.2f %.2f", da.columns.2.x, da.columns.2.y, da.columns.2.z)))")
                }
            } else {
                // Sin ubicaciones: se usa la pose del dispositivo como agarre. Se avisa una vez y luego cada ~10 s.
                let n: Int = lock.withLock { locationFail[hand] += 1; return locationFail[hand] }
                if n == 1 || n % 1000 == 0 {
                    Storage.log("seguimiento de Sense: el sistema no entrega las ubicaciones agarre/apuntado de la mano \(hand == 0 ? "izquierda" : "derecha") (\(n)); se usa la pose del dispositivo")
                }
                p.gripPosition = pos; p.gripRotation = rot
            }
        }
        lock.withLock { poses[hand] = p }
    }

    /// (posición, cuaternión x,y,z,w) de una matriz rígida.
    static func split(_ m: simd_float4x4) -> (SIMD3<Float>, SIMD4<Float>) {
        let r = simd_quatf(simd_float3x3(
            SIMD3<Float>(m.columns.0.x, m.columns.0.y, m.columns.0.z),
            SIMD3<Float>(m.columns.1.x, m.columns.1.y, m.columns.1.z),
            SIMD3<Float>(m.columns.2.x, m.columns.2.y, m.columns.2.z)))
        return (SIMD3(m.columns.3.x, m.columns.3.y, m.columns.3.z), r.vector)
    }

    /// Transformación origen <- ubicación del accesorio (`Accessory.LocationName`), o nil si el accesorio no la tiene o no se puede convertir.
    @available(visionOS 26.0, *)
    private static func location(_ anchor: AccessoryAnchor, _ name: Accessory.LocationName) -> simd_float4x4? {
        let space = anchor.coordinateSpace(for: name, correction: .none)
        guard let t = try? space.ancestorFromSpaceTransformFloat().matrix, XRMath.isFinite(t) else { return nil }
        return t
    }
}
