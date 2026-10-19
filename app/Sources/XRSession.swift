import SwiftUI
import CompositorServices
import Combine

/// Ajustes del modo inmersivo (UserDefaults; se leen al abrir el espacio, no en caliente).
enum XRSettings {
    static let autoEnterKey = "xr.autoEnter"        // Bool, defecto true: entrar en VR al arrancar el juego
    static let layoutKey = "xr.layout"              // "layered" (defecto) | "dedicated"
    static let msaaKey = "xr.msaa"                  // 1, 2 o 4 (defecto 4)
    static let hdrKey = "xr.hdr"                    // Bool, defecto false: rgba16Float en vez de bgra8Unorm_srgb
    static let recenterGestureKey = "xr.recenterGesture"   // Bool, defecto true: pinza mantenida 1,2 s
    static let windowModeKey = "xr.windowMode"      // "hidden" (defecto) | "closed" (prueba: cierra la ventana al entrar en VR)

    static var autoEnter: Bool { bool(autoEnterKey, true) }
    static var layered: Bool { (UserDefaults.standard.string(forKey: layoutKey) ?? "layered") != "dedicated" }
    static var hdr: Bool { bool(hdrKey, false) }
    static var recenterGesture: Bool { bool(recenterGestureKey, true) }
    /// Qué hacer con la ventana 2D al entrar en VR. "hidden": sigue viva pero invisible (la entrada del mando está garantizada);
    /// "closed": se cierra, con vuelta automática a "hidden" si los botones no llegan (ver XRInputProbe).
    static var windowClosed: Bool { (UserDefaults.standard.string(forKey: windowModeKey) ?? "hidden") == "closed" }
    static var msaa: Int {
        let v = UserDefaults.standard.integer(forKey: msaaKey)
        return (v == 1 || v == 2 || v == 4) ? v : 4
    }
    private static func bool(_ k: String, _ d: Bool) -> Bool {
        UserDefaults.standard.object(forKey: k) == nil ? d : UserDefaults.standard.bool(forKey: k)
    }
}

/// Estado compartido entre hilos del módulo inmersivo (seguro desde cualquier hilo).
/// Lo usan el bucle de render y el resto de la app (p. ej. un botón del mando puede llamar a `requestRecenter`).
final class XRControl {
    static let shared = XRControl()
    private let lock = NSLock()
    private var recenter = false
    private var pausedHook: ((Bool) -> Void)?

    /// Pide recentrar (origen = posición y rumbo actuales de la cabeza, a la altura del suelo) en el próximo frame.
    func requestRecenter() { lock.lock(); recenter = true; lock.unlock() }
    func takeRecenterRequest() -> Bool { lock.lock(); defer { lock.unlock() }; let r = recenter; recenter = false; return r }

    /// El bucle de render avisa (hilo propio) cuando el compositor pausa (true) o reanuda (false) el espacio.
    func setPausedHook(_ h: ((Bool) -> Void)?) { lock.lock(); pausedHook = h; lock.unlock() }
    func notifyPaused(_ p: Bool) { lock.lock(); let h = pausedHook; lock.unlock(); h?(p) }
}

/// Acciones de SwiftUI guardadas (valores) para usarlas cuando la ventana ya no exista o no se vea: cerrar el espacio inmersivo
/// desde el menú VR y cerrar/reabrir la ventana 2D.
struct XRWindowActions {
    var dismissSpace: () async -> Void
    var closeWindow: () -> Void
    var openWindow: () -> Void
}

/// En VR la ventana 2D NO debe verse nunca. Pero en visionOS los botones del mando (salvo Menú) solo llegan si una vista viva
/// y con foco declara `.handlesGameControllerEvents(matching: .gamepad)`, así que la ventana sigue viva y se vuelve invisible:
/// estilo `.plain` (sin cristal), contenido casi transparente de 1x1 (el sistema impone su tamaño mínimo), sin gestos,
/// sin barra/controles del sistema (`persistentSystemOverlays(.hidden)`) y sin imagen del juego ni botones.
/// Fuera de VR no restringe el tamaño (ideal 1024x768). Requiere `.windowResizability(.contentSize)` en el WindowGroup.
struct XRCompactModifier: ViewModifier {
    @ObservedObject private var xr = XRSession.shared
    func body(content: Content) -> some View {
        let c = xr.state == .open
        content
            .frame(minWidth: c ? 1 : 480, idealWidth: c ? 1 : 1024, maxWidth: c ? 1 : .infinity,
                   minHeight: c ? 1 : 360, idealHeight: c ? 1 : 768, maxHeight: c ? 1 : .infinity)
            .persistentSystemOverlays(c ? .hidden : .automatic)
    }
}
extension View {
    func xrCompactWhenImmersive() -> some View { modifier(XRCompactModifier()) }
}

/// Contenido de la ventana mientras dura el espacio inmersivo: nada visible. Alfa 0,01 y no 0 porque el motor de foco
/// descarta las vistas totalmente transparentes y la ventana perdería el foco que necesita para recibir el mando.
struct XRGhostView: View {
    var body: some View {
        Color.black.opacity(0.01)
            .frame(width: 1, height: 1)
            .allowsHitTesting(false)
            .accessibilityHidden(true)
    }
}

/// Comprobación de que, con la ventana CERRADA (modo de prueba), los botones del mando siguen llegando. El botón de Menú
/// llega siempre; si tras él pasan 3 s sin ningún otro elemento, se vuelve al modo "ventana invisible" y se reabre.
/// Un falso positivo es barato (la ventana de respaldo es invisible). `note(signature:)` es seguro desde cualquier hilo;
/// lo llama el gancho `ControllerHub.onInputSignature` (conectado en `arm()`) con la firma de elementos pulsados (la misma que escribe en el registro `entrada ...`).
final class XRInputProbe {
    static let shared = XRInputProbe()
    private let lock = NSLock()
    private var armed = false
    private var menuAt: TimeInterval?
    private var sawOther = false

    func arm() {
        lock.lock(); armed = true; menuAt = nil; sawOther = false; lock.unlock()
        // ControllerHub.diagnose avisa de cada cambio de la firma de botones pulsados (el mismo texto del registro `entrada ...`).
        ControllerHub.onInputSignature = { XRInputProbe.shared.note(signature: $0) }
    }
    func disarm() {
        lock.lock(); armed = false; lock.unlock()
        ControllerHub.onInputSignature = nil
    }

    func note(signature: String) {
        let names = signature.split(separator: ",").map { $0.trimmingCharacters(in: .whitespaces) }
            .map { $0.hasPrefix("nuevo:") ? String($0.dropFirst(6)) : $0 }
            .filter { !$0.isEmpty }
        guard !names.isEmpty else { return }
        let menuOnly = names.allSatisfy { $0 == "Button Menu" }
        lock.lock()
        guard armed else { lock.unlock(); return }
        if !menuOnly {
            sawOther = true
            armed = false
            lock.unlock()
            Storage.log("ventana cerrada: los botones del mando llegan sin ventana (\(names.joined(separator: ", ")))")
            return
        }
        let first = menuAt == nil
        if first { menuAt = ProcessInfo.processInfo.systemUptime }
        lock.unlock()
        if first {
            DispatchQueue.main.asyncAfter(deadline: .now() + 3.0) { [weak self] in self?.evaluate() }
        }
    }

    private func evaluate() {
        lock.lock()
        let fail = armed && !sawOther
        if fail { armed = false }
        lock.unlock()
        if fail { Task { @MainActor in XRSession.shared.windowFallback("solo llegó Menú en 3 s con la ventana cerrada") } }
    }
}

/// Estado del espacio inmersivo para la interfaz (MainActor).
@MainActor
final class XRSession: ObservableObject {
    static let shared = XRSession()
    static let spaceID = "mv-immersive"
    static let windowID = "mv-main"
    /// Las registra la ventana (XRControlsView) al aparecer.
    var windowActions: XRWindowActions?

    enum State { case closed, opening, open }
    @Published var state: State = .closed
    @Published var lastError: String?
    /// Evita reabrir solo tras salir a mano: la entrada automática solo ocurre una vez por arranque.
    var autoEntered = false
    private var loop: XRRenderLoop?
    /// La ventana se cerró a propósito al entrar en VR (modo "closed"); al salir hay que reabrirla.
    private var windowClosed = false
    private var windowClosedAt: TimeInterval = 0

    var isActive: Bool { state != .closed }

    /// Llamado por CompositorLayer cuando el sistema entrega el LayerRenderer.
    func attach(_ layerRenderer: LayerRenderer) {
        loop?.stop()
        loop = nil
        // XRRenderLoop.init es fallable; onEnded recibe el bucle que termina para ignorar el aviso de un bucle ya sustituido.
        guard let l = XRRenderLoop(layerRenderer: layerRenderer, onEnded: { [weak self] ended in
            Task { @MainActor in
                guard let self = self, self.loop === ended else { return }
                self.detach()
            }
        }) else {
            state = .closed
            lastError = "No se pudo iniciar el renderizado inmersivo."
            return
        }
        loop = l
        state = .open
        lastError = nil
        // Mientras el espacio está abierto manda VRInput (Sense o mando normal); el mando de la ventana se calla.
        InputManager.shared.setSuspended(true)
        VRInput.shared.start()
        ControllerHub.shared.attachEventInteractions()
        logScenes()
        l.start()
        applyWindowPolicy()
    }

    /// Diagnóstico: escenas de la app y sus ventanas UIKit (¿existe una UIWindowScene del espacio inmersivo con ventana?).
    private func logScenes() {
        for sc in UIApplication.shared.connectedScenes {
            let role = sc.session.role.rawValue.split(separator: ".").last.map(String.init) ?? "?"
            let n = (sc as? UIWindowScene)?.windows.count ?? -1
            Storage.log("[pad] escena \(role): estado=\(sc.activationState.rawValue) ventanas=\(n)")
        }
    }

    /// Ventana 2D en VR: por defecto sigue viva e invisible (XRGhostView). En modo de prueba se cierra con `dismissWindow`
    /// y XRInputProbe vigila que los botones del mando sigan llegando.
    private func applyWindowPolicy() {
        guard XRSettings.windowClosed, !windowClosed, let a = windowActions else { return }
        XRInputProbe.shared.arm()
        Task { @MainActor [weak self] in
            // Margen para que la vista ya sea invisible y el GCEventInteraction esté puesto antes de cerrarla.
            try? await Task.sleep(nanoseconds: 600_000_000)
            guard let self = self, self.state == .open, !self.windowClosed else { return }
            self.windowClosed = true
            self.windowClosedAt = ProcessInfo.processInfo.systemUptime
            Storage.log("ventana: cerrada al entrar en VR (modo de prueba)")
            a.closeWindow()
        }
    }

    /// Los botones no llegan sin ventana (o el sistema cerró también el espacio): vuelve a "ventana invisible" para siempre.
    func windowFallback(_ why: String) {
        XRInputProbe.shared.disarm()
        UserDefaults.standard.set("hidden", forKey: XRSettings.windowModeKey)
        Storage.log("ventana: respaldo a ventana invisible (\(why))")
        reopenWindowIfClosed()
    }

    private func reopenWindowIfClosed() {
        guard windowClosed else { return }
        windowClosed = false
        windowActions?.openWindow()
    }

    /// El espacio se cerró (LayerRenderer invalidado o descartado).
    func detach() {
        loop?.stop()
        loop = nil
        state = .closed
        mv_vr_clear_head()
        VRInput.shared.releaseAll()
        VRInputPose.shared.stop()
        InputManager.shared.setSuspended(false)
        // Si el espacio murió poco después de cerrar la ventana, probablemente fue por cerrarla: se abandona ese modo.
        if windowClosed, ProcessInfo.processInfo.systemUptime - windowClosedAt < 5 {
            UserDefaults.standard.set("hidden", forKey: XRSettings.windowModeKey)
            Storage.log("ventana: el espacio se cerró <5 s tras cerrar la ventana; modo invisible a partir de ahora")
        }
        XRInputProbe.shared.disarm()
        reopenWindowIfClosed()
    }

    func open(_ action: OpenImmersiveSpaceAction) async {
        guard state == .closed else { return }
        state = .opening
        let r = await action(id: Self.spaceID)
        switch r {
        case .opened: break                      // state pasa a .open en attach(); la ventana se reduce sola (XRCompactModifier)
        case .userCancelled: state = .closed
        case .error: state = .closed; lastError = "No se pudo abrir el espacio inmersivo."
        @unknown default: state = .closed
        }
    }

    /// Salir de VR desde el menú del inmersivo (la ventana puede estar oculta): cierra el espacio con la acción guardada.
    func exitVR() async {
        guard state != .closed else { return }
        if let a = windowActions { await a.dismissSpace() }
        detach()
    }

    func close(_ action: DismissImmersiveSpaceAction) async {
        guard state != .closed else { return }
        await action()
        detach()
    }
}

/// Botón «Entrar en VR» / «Salir de VR» + entrada automática. Se coloca sobre la ventana existente.
struct XRControlsView: View {
    let running: Bool
    @ObservedObject private var xr = XRSession.shared
    @EnvironmentObject private var model: GameModel
    @Environment(\.openImmersiveSpace) private var openSpace
    @Environment(\.dismissImmersiveSpace) private var dismissSpace
    @Environment(\.dismissWindow) private var dismissWindow
    @Environment(\.openWindow) private var openWindow
    @AppStorage(XRSettings.windowModeKey) private var windowMode = "hidden"
    @AppStorage(XRSettings.autoEnterKey) private var autoEnter = true
    @AppStorage(XRSettings.layoutKey) private var layout = "layered"

    var body: some View {
        // El contenedor (y con él el registro de acciones y la entrada automática) sigue vivo en VR, pero sin nada visible:
        // los botones solo existen fuera del espacio inmersivo (salir de VR y las opciones van en el menú VR).
        ZStack {
            Color.clear.frame(width: 0, height: 0)
            if xr.state != .open {
                HStack(spacing: 12) {
                    Button(xr.state == .open ? "Salir de VR" : "Entrar en VR") {
                        Task {
                            if xr.state == .open { await xr.close(dismissSpace) }
                            else { await xr.open(openSpace) }
                        }
                    }
                    .disabled(!running || xr.state == .opening)
                    Menu("Opciones VR") {
                        Toggle("Entrar en VR al arrancar", isOn: $autoEnter)
                        Picker("Estéreo", selection: $layout) {
                            Text("Una pasada (capas)").tag("layered")
                            Text("Dos pasadas").tag("dedicated")
                        }
                        Picker("Ventana en VR", selection: $windowMode) {
                            Text("Invisible (segura)").tag("hidden")
                            Text("Cerrada (prueba)").tag("closed")
                        }
                        Button("Recentrar") { XRControl.shared.requestRecenter() }
                        Button("Cambiar carpeta") { model.changeFolder() }
                    }
                }
                .padding(12)
                .background(.regularMaterial, in: Capsule())
            }
        }
        .onChange(of: running) { _, now in autoEnterIfNeeded(now) }
        .onAppear {
            // Se copian las acciones (valores) para poder usarlas cuando la ventana ya no exista.
            let ds = dismissSpace, dw = dismissWindow, ow = openWindow
            xr.windowActions = XRWindowActions(
                dismissSpace: { await ds() },
                closeWindow: { dw(id: XRSession.windowID) },
                openWindow: { ow(id: XRSession.windowID) })
            autoEnterIfNeeded(running)
        }
    }

    private func autoEnterIfNeeded(_ isRunning: Bool) {
        guard isRunning, autoEnter, !xr.autoEntered, xr.state == .closed else { return }
        xr.autoEntered = true
        Task { await xr.open(openSpace) }
    }
}

/// Configuración del LayerRenderer: color, profundidad y layout.
struct XRLayerConfiguration: CompositorLayerConfiguration {
    func makeConfiguration(capabilities: LayerRenderer.Capabilities,
                           configuration: inout LayerRenderer.Configuration) {
        configuration.colorFormat = XRSettings.hdr ? .rgba16Float : .bgra8Unorm_srgb
        configuration.depthFormat = .depth32Float

        // Sin foveación: el renderer de escena no la usa (la VRAM y el panel se muestrean a resolución plena).
        configuration.isFoveationEnabled = false
        let layouts = capabilities.supportedLayouts(options: [])
        if XRSettings.layered && layouts.contains(.layered) {
            configuration.layout = .layered
        } else {
            configuration.layout = .dedicated
        }
    }
}

/// Escena inmersiva (estilo .full) que registra MVApp.
struct XRImmersiveScene: Scene {
    /// Ocultar las manos y brazos reales del usuario durante el espacio inmersivo (se ven los del personaje). Preferencia del sistema
    /// (`upperLimbVisibility`: solo una petición, SwiftUI visionOS 1+ en Scene y View). Se cambia desde el menú VR (`xr.view.hideHands`).
    @AppStorage("xr.view.hideHands") private var hideRealHands = true

    var body: some Scene {
        /* Indicador de inicio del sistema oculto (visionOS 2+, `Scene.persistentSystemOverlays`): al girar la palma no aparece la barra/botón
         * de inicio sin intención expresa. Es una petición: el gesto de la palma es del sistema y no existe API pública para anularlo
         * (con visionOS 1 el modificador no existe: de ahí la comprobación de disponibilidad). */
        if #available(visionOS 2.0, *) {
            XRImmersiveSpaceBase(hideRealHands: hideRealHands)
                .persistentSystemOverlays(.hidden)
        } else {
            XRImmersiveSpaceBase(hideRealHands: hideRealHands)
        }
    }
}

/// Espacio inmersivo completo con el compositor; las manos reales se ocultan si se pide.
struct XRImmersiveSpaceBase: Scene {
    let hideRealHands: Bool
    var body: some Scene {
        ImmersiveSpace(id: XRSession.spaceID) {
            CompositorLayer(configuration: XRLayerConfiguration()) { layerRenderer in
                Task { @MainActor in XRSession.shared.attach(layerRenderer) }
            }
        }
        .immersionStyle(selection: .constant(.full), in: .full)
        .upperLimbVisibility(hideRealHands ? .hidden : .automatic)
    }
}
