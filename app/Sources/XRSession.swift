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

    static var autoEnter: Bool { bool(autoEnterKey, true) }
    static var layered: Bool { (UserDefaults.standard.string(forKey: layoutKey) ?? "layered") != "dedicated" }
    static var hdr: Bool { bool(hdrKey, false) }
    static var recenterGesture: Bool { bool(recenterGestureKey, true) }
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

/// Estado del espacio inmersivo para la interfaz (MainActor).
@MainActor
final class XRSession: ObservableObject {
    static let shared = XRSession()
    static let spaceID = "mv-immersive"

    enum State { case closed, opening, open }
    @Published var state: State = .closed
    @Published var lastError: String?
    /// Evita reabrir solo tras salir a mano: la entrada automática solo ocurre una vez por arranque.
    var autoEntered = false
    private var loop: XRRenderLoop?

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
        l.start()
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
    }

    func open(_ action: OpenImmersiveSpaceAction) async {
        guard state == .closed else { return }
        state = .opening
        let r = await action(id: Self.spaceID)
        switch r {
        case .opened: break                      // state pasa a .open en attach()
        case .userCancelled: state = .closed
        case .error: state = .closed; lastError = "No se pudo abrir el espacio inmersivo."
        @unknown default: state = .closed
        }
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
    @Environment(\.openImmersiveSpace) private var openSpace
    @Environment(\.dismissImmersiveSpace) private var dismissSpace
    @AppStorage(XRSettings.autoEnterKey) private var autoEnter = true
    @AppStorage(XRSettings.layoutKey) private var layout = "layered"

    var body: some View {
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
                Button("Recentrar") { XRControl.shared.requestRecenter() }
            }
        }
        .padding(12)
        .background(.regularMaterial, in: Capsule())
        .onChange(of: running) { _, now in autoEnterIfNeeded(now) }
        .onAppear { autoEnterIfNeeded(running) }
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
    var body: some Scene {
        ImmersiveSpace(id: XRSession.spaceID) {
            CompositorLayer(configuration: XRLayerConfiguration()) { layerRenderer in
                Task { @MainActor in XRSession.shared.attach(layerRenderer) }
            }
        }
        .immersionStyle(selection: .constant(.full), in: .full)
    }
}
