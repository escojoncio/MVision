import SwiftUI
import GameController
import UniformTypeIdentifiers

@main
struct MVApp: App {
    @StateObject private var model = GameModel()
    @Environment(\.scenePhase) private var scenePhase

    var body: some Scene {
        WindowGroup(id: XRSession.windowID) {
            ContentView()
                .environmentObject(model)
                .xrCompactWhenImmersive()   // en VR la ventana queda viva pero invisible (ver XRSession.swift)
                .onAppear {
                    model.start()
                    // VR: el compositor pausa/reanuda el espacio inmersivo (quitarse el visor, menús del sistema).
                    XRControl.shared.setPausedHook { paused in
                        Task { @MainActor in
                            guard model.phase == .running else { return }
                            if paused { mv_host_pause(); model.audio.pause() }
                            else { mv_host_resume(); model.audio.resume() }
                        }
                    }
                }
                .onReceive(NotificationCenter.default.publisher(for: UIApplication.willTerminateNotification)) { _ in
                    model.shutdown()
                }
                .onChange(of: scenePhase) { _, p in
                    // Con el espacio inmersivo abierto la ventana pasa a segundo plano: no pausar (lo gestiona el bucle XR).
                    if XRSession.shared.isActive { return }
                    model.scenePhaseChanged(p)
                }
        }
        .windowStyle(.plain)            // sin cristal: en VR la ventana (invisible) no deja ningún fondo
        .defaultSize(width: 1024, height: 768)
        .windowResizability(.contentSize)
        XRImmersiveScene()
    }
}

struct ContentView: View {
    @EnvironmentObject var model: GameModel
    @FocusState private var controllerFocus: Bool
    @ObservedObject private var xr = XRSession.shared
    @Environment(\.scenePhase) private var scenePhase

    var body: some View {
        Group {
            // En VR no se ve nada de la ventana: ni la imagen del juego (ya sale en el espacio inmersivo) ni botones.
            if xr.state == .open { XRGhostView() } else { windowContent }
        }
        // visionOS entrega los botones del mando por GameController solo a la app que lo declara en una vista con foco.
        .modifier(GameControllerEventsModifier())
        .focusable()
        .focusEffectDisabled()
        .focused($controllerFocus)
        .onAppear { controllerFocus = true }
        // Al abrir/cerrar el espacio inmersivo o reactivarse la ventana el foco puede perderse: se reasigna.
        .onChange(of: xr.state) { _, _ in refocus() }
        .onChange(of: scenePhase) { _, _ in refocus() }
        .fileImporter(isPresented: $model.showPicker, allowedContentTypes: [.folder], allowsMultipleSelection: false) { model.folderPicked($0) }
        .alert("Carpeta", isPresented: Binding(get: { model.aviso != nil }, set: { if !$0 { model.aviso = nil } })) {
            Button("Aceptar", role: .cancel) {}
        } message: { Text(model.aviso ?? "") }
        .overlay(alignment: .bottom) {
            XRControlsView(running: model.phase == .running)
                .padding(.bottom, xr.state == .open ? 0 : 24)
        }
    }

    private var windowContent: some View {
        ZStack {
            Color.black.ignoresSafeArea()
            switch model.phase {
            case .loading:
                ProgressView("Iniciando…")
            case .running:
                GameView()
                    .aspectRatio(4.0 / 3.0, contentMode: .fit)
            case .needFolder(let aviso):
                FolderPromptView(aviso: aviso)
            case .noDisc:
                MessageView(
                    titulo: "No se encontró ningún disco",
                    texto: "En la carpeta elegida no hay ninguna imagen de disco (.cue/.bin) ni carpeta con SYSTEM.CNF dentro. Cópiala ahí con la app Archivos y pulsa Reintentar, o elige otra carpeta desde el menú.",
                    reintentar: { model.start() })
            case .failed(let msg):
                MessageView(
                    titulo: "No se pudo ejecutar",
                    texto: "\(msg)\n\nRegistro: carpeta Logs dentro de la carpeta elegida (o mvision.log en la carpeta de documentos de la app). Cierra y vuelve a abrir la app para reintentarlo.",
                    reintentar: nil)
            }
        }
    }
}

struct MessageView: View {
    let titulo: String
    let texto: String
    let reintentar: (() -> Void)?

    var body: some View {
        VStack(spacing: 20) {
            Text(titulo).font(.title)
            Text(texto)
                .multilineTextAlignment(.center)
                .frame(maxWidth: 560)
            if let r = reintentar {
                Button("Reintentar", action: r)
            }
        }
        .padding(40)
    }
}

/// Pantalla de elegir la carpeta del juego (primera vez, permiso perdido o nombre incorrecto).
extension ContentView {
    func refocus() {
        controllerFocus = false
        DispatchQueue.main.async { controllerFocus = true }
    }
}

struct FolderPromptView: View {
    @EnvironmentObject var model: GameModel
    let aviso: String?

    var body: some View {
        VStack(spacing: 20) {
            Text("Elige la carpeta del juego").font(.title)
                // Pulsación larga: ajuste oculto de desarrollo (buscar también en Documents de la app).
                .onLongPressGesture(minimumDuration: 3) {
                    Storage.devDocuments.toggle()
                    model.aviso = Storage.devDocuments ? "Modo desarrollo: activado." : "Modo desarrollo: desactivado."
                }
            Text("1. Abre la app Archivos y, en \"En mi Apple Vision Pro\", crea una carpeta con el nombre indicado en las instrucciones.\n2. Mete dentro la imagen del disco (.cue/.bin) o su carpeta.\n3. Pulsa \"Elegir carpeta\" y selecciónala.\n\nLas partidas guardadas quedarán en una subcarpeta Saves dentro de esa misma carpeta.")
                .multilineTextAlignment(.leading)
                .frame(maxWidth: 560)
            if let a = aviso {
                Text(a).foregroundStyle(.red).multilineTextAlignment(.center).frame(maxWidth: 560)
            }
            Button("Elegir carpeta") { model.showPicker = true }
            if Storage.devDocuments {
                Button("Buscar en Documents de la app") { model.start() }
            }
        }
        .padding(40)
    }
}

/// `handlesGameControllerEvents` existe desde visionOS 2; el destino mínimo del proyecto es anterior.
struct GameControllerEventsModifier: ViewModifier {
    func body(content: Content) -> some View {
        if #available(visionOS 2.0, *) {
            content.handlesGameControllerEvents(matching: .gamepad)
        } else {
            content
        }
    }
}
