import SwiftUI

@main
struct MVApp: App {
    @StateObject private var model = GameModel()
    @Environment(\.scenePhase) private var scenePhase

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(model)
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
                .onChange(of: scenePhase) { _, p in
                    // Con el espacio inmersivo abierto la ventana pasa a segundo plano: no pausar (lo gestiona el bucle XR).
                    if XRSession.shared.isActive { return }
                    model.scenePhaseChanged(p)
                }
        }
        .defaultSize(width: 1024, height: 768)
        XRImmersiveScene()
    }
}

struct ContentView: View {
    @EnvironmentObject var model: GameModel

    var body: some View {
        ZStack {
            Color.black.ignoresSafeArea()
            switch model.phase {
            case .loading:
                ProgressView("Iniciando…")
            case .running:
                GameView()
                    .aspectRatio(4.0 / 3.0, contentMode: .fit)
            case .noDisc:
                MessageView(
                    titulo: "No se encontró ningún disco",
                    texto: "Copia la imagen del disco (.cue/.bin) o su carpeta (con SYSTEM.CNF dentro) a cualquier carpeta dentro de los documentos de esta app; por ejemplo, una carpeta creada por ti con el nombre que quieras.\n\nDesde la app Archivos: En mi Apple Vision Pro > MVision. Desde un Mac: Finder > dispositivo > Archivos.",
                    reintentar: { model.start() })
            case .failed(let msg):
                MessageView(
                    titulo: "No se pudo ejecutar",
                    texto: "\(msg)\n\nRegistro: mvision.log en la carpeta de documentos de la app. Cierra y vuelve a abrir la app para reintentarlo.",
                    reintentar: nil)
            }
        }
        .overlay(alignment: .bottom) {
            XRControlsView(running: model.phase == .running)
                .padding(.bottom, 24)
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
