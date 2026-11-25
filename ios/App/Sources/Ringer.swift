import AVFoundation
import Foundation
import UserNotifications

/// Makes the phone ring when the pager's "Find my phone" tool asks for it.
final class Ringer: NSObject, ObservableObject {
    @Published var notificationsAllowed = false
    @Published var ringing = false
    private var player: AVAudioPlayer?

    func refresh() {
        UNUserNotificationCenter.current().getNotificationSettings { settings in
            DispatchQueue.main.async { self.notificationsAllowed = settings.authorizationStatus == .authorized }
        }
    }

    func askPermission() {
        UNUserNotificationCenter.current().requestAuthorization(options: [.alert, .sound]) { _, _ in self.refresh() }
    }

    func ring() {
        // A notification, in case iOS will not let the sound start while the app is in the background.
        let content = UNMutableNotificationContent()
        content.title = "Inkfob"
        content.body = "Your pager is looking for this phone."
        content.sound = .default
        UNUserNotificationCenter.current().add(UNNotificationRequest(identifier: "inkfob.find", content: content, trigger: nil))

        // The loud part: plays even with the ring switch on silent.
        let session = AVAudioSession.sharedInstance()
        try? session.setCategory(.playback, mode: .default, options: [.duckOthers])
        try? session.setActive(true)
        player = try? AVAudioPlayer(data: Self.tone())
        player?.volume = 1
        if player?.play() == true {
            ringing = true
            DispatchQueue.main.asyncAfter(deadline: .now() + 13) { [weak self] in self?.stop() }
        }
    }

    func stop() {
        player?.stop()
        player = nil
        ringing = false
        try? AVAudioSession.sharedInstance().setActive(false, options: [.notifyOthersOnDeactivation])
    }

    /// Twelve seconds of beeps as a WAV file, built in memory so the app ships no sound files.
    static func tone() -> Data {
        let rate = 22050
        let seconds = 12
        var samples = [Int16](repeating: 0, count: rate * seconds)
        for i in 0..<samples.count {
            let t = Double(i) / Double(rate)
            let beat = t.truncatingRemainder(dividingBy: 1.0)          // one pattern a second
            let on = beat < 0.15 || (beat > 0.25 && beat < 0.40)       // two short beeps
            if on {
                let wave = sin(2 * Double.pi * 2200 * t) + 0.5 * sin(2 * Double.pi * 3300 * t)
                samples[i] = Int16(max(-1, min(1, wave * 0.6)) * 32000)
            }
        }
        var data = Data()
        func put32(_ value: UInt32) { var v = value.littleEndian; withUnsafeBytes(of: &v) { data.append(contentsOf: $0) } }
        func put16(_ value: UInt16) { var v = value.littleEndian; withUnsafeBytes(of: &v) { data.append(contentsOf: $0) } }
        let bytes = UInt32(samples.count * 2)
        data.append(contentsOf: Array("RIFF".utf8)); put32(36 + bytes)
        data.append(contentsOf: Array("WAVE".utf8))
        data.append(contentsOf: Array("fmt ".utf8)); put32(16); put16(1); put16(1)
        put32(UInt32(rate)); put32(UInt32(rate * 2)); put16(2); put16(16)
        data.append(contentsOf: Array("data".utf8)); put32(bytes)
        samples.withUnsafeBytes { data.append(contentsOf: $0) }
        return data
    }
}
