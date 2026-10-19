import AVFoundation

/// Salida de audio: AVAudioSourceNode estéreo a la frecuencia de la sesión (48000 Hz normalmente) que tira de mv_host_audio_pull_rate:
/// el remuestreo 44,1 -> sesión lo hace el propio emulador (polifásico de calidad, reloj ajustado al nivel del anillo), no el mezclador del sistema.
/// El callback es de tiempo real: solo llama a mv_host_audio_pull (anillo sin candados, siempre rellena de forma continua
/// lo que falte), convierte a Float en un búfer reservado de antemano y no toma candados, no reserva memoria ni escribe registros.
final class AudioOut {
    private let engine = AVAudioEngine()
    private var node: AVAudioSourceNode?
    private let scratchFrames = 4096
    private let scratch: UnsafeMutablePointer<Int16>
    private var observer: NSObjectProtocol?
    private var observers: [NSObjectProtocol] = []
    private var started = false
    private var nodeRate = 0.0

    init() {
        scratch = UnsafeMutablePointer<Int16>.allocate(capacity: scratchFrames * 2)
        scratch.initialize(repeating: 0, count: scratchFrames * 2)
    }

    deinit {
        scratch.deallocate()
    }

    func start() {
        if started { resume(); return }   // idempotente: sin segundo nodo ni observadores duplicados
        started = true
        let session = AVAudioSession.sharedInstance()
        try? session.setCategory(.playback, mode: .default)
        try? session.setActive(true)

        let sr = session.sampleRate
        let rate = (sr >= 44100 && sr <= 192000) ? sr : 48000   // por debajo de 44,1 kHz el remuestreo lo hace el sistema
        nodeRate = rate
        let rateI = Int32(rate.rounded())
        guard let fmt = AVAudioFormat(commonFormat: .pcmFormatFloat32, sampleRate: rate, channels: 2, interleaved: false) else { started = false; return }
        let buf = scratch
        let cap = scratchFrames
        let n = AVAudioSourceNode(format: fmt) { _, _, frameCount, abl -> OSStatus in
            let list = UnsafeMutableAudioBufferListPointer(abl)
            guard list.count >= 2,
                  let l = list[0].mData?.assumingMemoryBound(to: Float.self),
                  let r = list[1].mData?.assumingMemoryBound(to: Float.self) else { return noErr }
            var done = 0
            let total = Int(frameCount)
            while done < total {
                let chunk = min(cap, total - done)
                _ = mv_host_audio_pull_rate(buf, Int32(chunk), rateI)   // siempre rellena (extensión continua si faltan datos)
                for i in 0..<chunk {
                    l[done + i] = Float(buf[2 * i]) * (1.0 / 32768.0)
                    r[done + i] = Float(buf[2 * i + 1]) * (1.0 / 32768.0)
                }
                done += chunk
            }
            return noErr
        }
        node = n
        engine.attach(n)
        engine.connect(n, to: engine.mainMixerNode, format: fmt)
        observer = NotificationCenter.default.addObserver(
            forName: .AVAudioEngineConfigurationChange, object: engine, queue: .main) { [weak self] _ in
            self?.resume()
        }
        do { try engine.start() } catch { Storage.log("audio: \(error)") }
        Storage.log("audio: motor \(engine.isRunning ? "en marcha" : "PARADO"), sesión \(Int(session.sampleRate)) Hz, búfer E/S \(String(format: "%.1f", session.ioBufferDuration * 1000)) ms, latencia de salida \(String(format: "%.1f", session.outputLatency * 1000)) ms")
        let nc = NotificationCenter.default
        observers.append(nc.addObserver(forName: AVAudioSession.interruptionNotification, object: nil, queue: .main) { [weak self] n in
            let t = (n.userInfo?[AVAudioSessionInterruptionTypeKey] as? UInt) ?? 0
            Storage.log("audio: interrupción \(t == AVAudioSession.InterruptionType.began.rawValue ? "inicio" : "fin")")
            if t == AVAudioSession.InterruptionType.ended.rawValue { self?.resume() }
        })
        observers.append(nc.addObserver(forName: AVAudioSession.routeChangeNotification, object: nil, queue: .main) { [weak self] n in
            Storage.log("audio: cambio de ruta (motivo \((n.userInfo?[AVAudioSessionRouteChangeReasonKey] as? UInt) ?? 0))")
            self?.rebuildIfRateChanged()
        })
    }

    /// Si la frecuencia de la sesión cambió (otra ruta de salida), se rehace el nodo a la nueva frecuencia.
    private func rebuildIfRateChanged() {
        let sr = AVAudioSession.sharedInstance().sampleRate
        let rate = (sr >= 44100 && sr <= 192000) ? sr : 48000
        guard started, rate != nodeRate else { return }
        Storage.log("audio: frecuencia de sesión \(Int(nodeRate)) -> \(Int(rate)) Hz, se rehace el nodo")
        engine.stop()
        if let n = node { engine.detach(n); node = nil }
        if let o = observer { NotificationCenter.default.removeObserver(o); observer = nil }
        for o in observers { NotificationCenter.default.removeObserver(o) }
        observers.removeAll()
        started = false
        start()
    }

    func pause() { engine.pause() }

    func resume() {
        rebuildIfRateChanged()
        if !started { return }
        if !engine.isRunning {
            Storage.log("audio: reanudando el motor")
            try? AVAudioSession.sharedInstance().setActive(true)
            do { try engine.start() } catch { Storage.log("audio: \(error)") }
        }
    }

    func stop() {
        engine.stop()
        if let o = observer { NotificationCenter.default.removeObserver(o); observer = nil }
        for o in observers { NotificationCenter.default.removeObserver(o) }
        observers.removeAll()
        started = false
    }
}
