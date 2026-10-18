import AVFoundation

/// Salida de audio: AVAudioSourceNode a 44100 Hz estéreo que tira de mv_host_audio_pull.
final class AudioOut {
    private let engine = AVAudioEngine()
    private var node: AVAudioSourceNode?
    private let scratchFrames = 4096
    private let scratch: UnsafeMutablePointer<Int16>
    private var observer: NSObjectProtocol?

    init() {
        scratch = UnsafeMutablePointer<Int16>.allocate(capacity: scratchFrames * 2)
        scratch.initialize(repeating: 0, count: scratchFrames * 2)
    }

    deinit {
        scratch.deallocate()
    }

    func start() {
        let session = AVAudioSession.sharedInstance()
        try? session.setCategory(.playback, mode: .default)
        try? session.setActive(true)

        guard let fmt = AVAudioFormat(commonFormat: .pcmFormatFloat32, sampleRate: 44100, channels: 2, interleaved: false) else { return }
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
                _ = mv_host_audio_pull(buf, Int32(chunk))   // rellena con ceros lo que falte
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
    }

    func pause() { engine.pause() }

    func resume() {
        if !engine.isRunning {
            try? AVAudioSession.sharedInstance().setActive(true)
            do { try engine.start() } catch { Storage.log("audio: \(error)") }
        }
    }

    func stop() { engine.stop() }
}
