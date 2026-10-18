import ARKit
import Foundation
import GameController
import simd

/// Pose en el espacio ARKit/inmersivo (Y arriba, adelante = -Z) de los mandos PS VR2 Sense, vía
/// AccessoryTrackingProvider (visionOS 26). Sin permiso, sin soporte o antes de visionOS 26 las manos quedan
/// como "no seguidas" y la locomoción cae a la cabeza. Hilo: las actualizaciones llegan en una Task propia;
/// `pose(hand:)` se puede llamar desde cualquier hilo.
final class VRInputPose: @unchecked Sendable {
    struct Pose {
        var tracked = false
        var position = SIMD3<Float>(0, 0, 0)
        var rotation = SIMD4<Float>(0, 0, 0, 1)   // x, y, z, w
    }

    static let shared = VRInputPose()

    private let lock = NSLock()
    private var poses = [Pose(), Pose()]          // 0 izquierda, 1 derecha
    private var session: ARKitSession?
    private var task: Task<Void, Never>?
    private var requested: Set<ObjectIdentifier> = []   // último conjunto pedido a track()
    private var generation = 0                            // invalida arranques en curso cuando cambia el conjunto

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
        var accessories: [Accessory] = []
        for controller in controllers {
            do {
                accessories.append(try await Accessory(device: controller))
            } catch {
                Storage.log("seguimiento de Sense: sin accesorio (\(error.localizedDescription))")
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
            let r = simd_quatf(simd_float3x3(
                SIMD3<Float>(m.columns.0.x, m.columns.0.y, m.columns.0.z),
                SIMD3<Float>(m.columns.1.x, m.columns.1.y, m.columns.1.z),
                SIMD3<Float>(m.columns.2.x, m.columns.2.y, m.columns.2.z)))
            p = Pose(tracked: true, position: SIMD3(m.columns.3.x, m.columns.3.y, m.columns.3.z), rotation: r.vector)
        }
        lock.withLock { poses[hand] = p }
    }
}
