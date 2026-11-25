import CoreBluetooth
import CryptoKit
import Foundation
import UIKit

/// The Bluetooth link to the pager. The iPhone itself already holds the connection (that is how
/// notifications reach the pager); this joins that connection and talks to the pager's app service.
final class PagerLink: NSObject, ObservableObject, CBCentralManagerDelegate, CBPeripheralDelegate {
    static let service = CBUUID(string: "1F0B0001-8A4E-4C1B-9E2D-5B7A1C3D9F10")
    static let control = CBUUID(string: "1F0B0002-8A4E-4C1B-9E2D-5B7A1C3D9F10")
    static let event = CBUUID(string: "1F0B0003-8A4E-4C1B-9E2D-5B7A1C3D9F10")
    static let data = CBUUID(string: "1F0B0004-8A4E-4C1B-9E2D-5B7A1C3D9F10")
    private static let systemServices = [service, CBUUID(string: "1812"), CBUUID(string: "180F")]

    enum State: Equatable {
        case bluetoothOff, looking, connecting, needsRepair, ready
    }

    struct PagerFile: Identifiable, Equatable {
        let name: String
        let size: Int
        var id: String { name }
    }

    @Published var state: State = .looking
    @Published var settings: [String: String] = [:]
    @Published var files: [PagerFile] = []
    @Published var freeBytes: Int = 0
    @Published var transferName: String = ""
    @Published var transferProgress: Double = 0
    @Published var transferActive = false
    @Published var lastMessage: String = ""
    @Published var lastSent: String = UserDefaults.standard.string(forKey: "lastSent") ?? ""
    @Published var lastSentAt: Date? = UserDefaults.standard.object(forKey: "lastSentAt") as? Date

    /// Called when the pager asks for fresh weather and calendar (also wakes the app in the background).
    var onRefreshRequest: (() -> Void)?

    private var central: CBCentralManager!
    private var pager: CBPeripheral?
    private var controlChar: CBCharacteristic?
    private var eventChar: CBCharacteristic?
    private var dataChar: CBCharacteristic?
    private var lookTimer: Timer?
    private var listBuffer = ""
    private var settingsBuffer = ""
    private var payload = Data()
    private var payloadOffset = 0
    private var transferDone: ((Bool, String) -> Void)?

    override init() {
        super.init()
        central = CBCentralManager(delegate: self, queue: .main,
                                   options: [CBCentralManagerOptionRestoreIdentifierKey: "inkfob.central"])
        lookTimer = Timer.scheduledTimer(withTimeInterval: 5, repeats: true) { [weak self] _ in self?.look() }
    }

    var isReady: Bool { state == .ready }

    // MARK: Finding the pager

    func look() {
        guard central.state == .poweredOn else { return }
        if let pager, pager.state == .connected || pager.state == .connecting { return }
        let found = central.retrieveConnectedPeripherals(withServices: Self.systemServices)
        let match = found.first { ($0.name ?? "").localizedCaseInsensitiveContains("pager") }
            ?? found.first { ($0.name ?? "").localizedCaseInsensitiveContains("inkfob") }
        guard let match else {
            if state != .needsRepair { state = .looking }
            return
        }
        pager = match
        match.delegate = self
        state = .connecting
        central.connect(match, options: nil)
    }

    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        if central.state == .poweredOn {
            state = .looking
            look()
        } else {
            state = .bluetoothOff
        }
    }

    func centralManager(_ central: CBCentralManager, willRestoreState dict: [String: Any]) {
        if let restored = (dict[CBCentralManagerRestoredStatePeripheralsKey] as? [CBPeripheral])?.first {
            pager = restored
            restored.delegate = self
        }
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        peripheral.discoverServices([Self.service])
    }

    func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        state = .looking
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        controlChar = nil
        eventChar = nil
        dataChar = nil
        if transferActive { finishTransfer(false, "The pager disconnected.") }
        state = .looking
        // Left pending, this reconnects by itself (even in the background) when the pager is back.
        central.connect(peripheral, options: nil)
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        guard let svc = peripheral.services?.first(where: { $0.uuid == Self.service }) else {
            // The phone is showing an old picture of the pager from before the app service existed.
            state = .needsRepair
            return
        }
        peripheral.discoverCharacteristics([Self.control, Self.event, Self.data], for: svc)
    }

    func peripheral(_ peripheral: CBPeripheral, didModifyServices invalidatedServices: [CBService]) {
        peripheral.discoverServices([Self.service])
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        for c in service.characteristics ?? [] {
            if c.uuid == Self.control { controlChar = c }
            if c.uuid == Self.event { eventChar = c }
            if c.uuid == Self.data { dataChar = c }
        }
        guard controlChar != nil, dataChar != nil, let eventChar else {
            state = .needsRepair
            return
        }
        peripheral.setNotifyValue(true, for: eventChar)
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateNotificationStateFor characteristic: CBCharacteristic, error: Error?) {
        guard characteristic.uuid == Self.event, error == nil else { return }
        state = .ready
        requestSettings()
        requestFiles()
    }

    // MARK: Commands

    private func send(_ command: Character, _ body: Data = Data()) {
        guard let pager, let controlChar else { return }
        var packet = Data([command.asciiValue ?? 0])
        packet.append(body)
        pager.writeValue(packet, for: controlChar, type: .withResponse)
    }

    func sendHomeLines(_ lines: [String]) {
        let text = lines.prefix(3).map { Self.plain($0, limit: 40) }.joined(separator: "\n")
        send("I", Data(text.utf8))
        lastSent = text
        lastSentAt = Date()
        UserDefaults.standard.set(text, forKey: "lastSent")
        UserDefaults.standard.set(lastSentAt, forKey: "lastSentAt")
    }

    func set(_ key: String, _ value: String) {
        settings[key] = value
        send("S", Data("\(key)=\(Self.plain(value, limit: 100))".utf8))
    }

    func requestSettings() { settingsBuffer = ""; send("G") }
    func requestFiles() { listBuffer = ""; send("L") }

    func deleteFile(_ name: String) {
        send("D", Data(name.utf8))
        files.removeAll { $0.name == name }
    }

    /// Sends a text file to the pager's "My files".
    func sendFile(name: String, text: String, done: @escaping (Bool, String) -> Void) {
        let clean = Self.fileName(name)
        let body = Data(Self.plain(text, limit: nil).utf8)
        guard !clean.isEmpty, !body.isEmpty else { done(false, "Give it a name and some text."); return }
        var header = Data()
        var size = UInt32(body.count).littleEndian
        withUnsafeBytes(of: &size) { header.append(contentsOf: $0) }
        header.append(Data(clean.utf8))
        begin(name: clean, command: "N", header: header, body: body, done: done)
    }

    /// Installs new pager firmware.
    func sendFirmware(_ image: Data, done: @escaping (Bool, String) -> Void) {
        guard image.count > 200_000, image.first == 0xE9 else { done(false, "That is not a pager firmware file."); return }
        var header = Data()
        var size = UInt32(image.count).littleEndian
        withUnsafeBytes(of: &size) { header.append(contentsOf: $0) }
        let md5 = Insecure.MD5.hash(data: image).map { String(format: "%02x", $0) }.joined()
        header.append(Data(md5.utf8))
        begin(name: "Firmware", command: "F", header: header, body: image, done: done)
    }

    func cancelTransfer() {
        guard transferActive else { return }
        send("C")
        finishTransfer(false, "Cancelled.")
    }

    private func begin(name: String, command: Character, header: Data, body: Data, done: @escaping (Bool, String) -> Void) {
        guard isReady, !transferActive else { done(false, "The pager is not ready."); return }
        payload = body
        payloadOffset = 0
        transferName = name
        transferProgress = 0
        transferActive = true
        transferDone = done
        UIApplication.shared.isIdleTimerDisabled = true
        send(command, header)   // the pager answers "A" when it is ready for the bytes
    }

    private func pump() {
        guard transferActive, let pager, let dataChar else { return }
        let chunk = max(20, min(pager.maximumWriteValueLength(for: .withoutResponse), 244))
        while payloadOffset < payload.count, pager.canSendWriteWithoutResponse {
            let end = min(payloadOffset + chunk, payload.count)
            pager.writeValue(payload.subdata(in: payloadOffset..<end), for: dataChar, type: .withoutResponse)
            payloadOffset = end
        }
    }

    func peripheralIsReady(toSendWriteWithoutResponse peripheral: CBPeripheral) { pump() }

    private func finishTransfer(_ ok: Bool, _ message: String) {
        transferActive = false
        payload = Data()
        UIApplication.shared.isIdleTimerDisabled = false
        lastMessage = message
        let done = transferDone
        transferDone = nil
        done?(ok, message)
        if ok { requestFiles() }
    }

    // MARK: Replies from the pager

    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        guard characteristic.uuid == Self.event, let value = characteristic.value, let kind = value.first else { return }
        let body = value.dropFirst()
        let text = String(decoding: body, as: UTF8.self)
        switch Character(UnicodeScalar(kind)) {
        case "R":
            onRefreshRequest?()
        case "A":
            pump()
        case "P":
            if body.count >= 4, !payload.isEmpty {
                let got = body.withUnsafeBytes { $0.loadUnaligned(as: UInt32.self) }
                transferProgress = min(1, Double(UInt32(littleEndian: got)) / Double(payload.count))
            }
        case "K":
            if transferActive { transferProgress = 1; finishTransfer(true, text.isEmpty ? "Done." : "Done: \(text).") }
        case "X":
            if transferActive { finishTransfer(false, "The pager said: \(text)") } else { lastMessage = text }
        case "g":
            settingsBuffer += text
        case "h":
            var parsed: [String: String] = [:]
            for line in settingsBuffer.split(separator: "\n") {
                if let eq = line.firstIndex(of: "=") {
                    parsed[String(line[..<eq])] = String(line[line.index(after: eq)...])
                }
            }
            settings = parsed
        case "l":
            listBuffer += text
        case "m":
            files = listBuffer.split(separator: "\n").compactMap { line in
                let parts = line.split(separator: "\t")
                guard parts.count == 2, let size = Int(parts[1]) else { return nil }
                return PagerFile(name: String(parts[0]), size: size)
            }.sorted { $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending }
            freeBytes = Int(text) ?? 0
        default:
            break
        }
    }

    // MARK: Text clean-up (the pager's screen font only has plain English characters)

    static func plain(_ text: String, limit: Int?) -> String {
        var s = text.replacingOccurrences(of: "\u{2019}", with: "'").replacingOccurrences(of: "\u{2018}", with: "'")
            .replacingOccurrences(of: "\u{201C}", with: "\"").replacingOccurrences(of: "\u{201D}", with: "\"")
            .replacingOccurrences(of: "\u{2014}", with: "-").replacingOccurrences(of: "\u{2013}", with: "-")
            .replacingOccurrences(of: "\u{2026}", with: "...").replacingOccurrences(of: "\u{00B0}", with: "")
        s = s.applyingTransform(.toLatin, reverse: false) ?? s
        s = s.applyingTransform(.stripDiacritics, reverse: false) ?? s
        s = String(s.unicodeScalars.filter { $0 == "\n" || ($0.value >= 32 && $0.value < 127) }.map(Character.init))
        if let limit, s.count > limit { s = String(s.prefix(limit)) }
        return s
    }

    static func fileName(_ raw: String) -> String {
        let allowed = Set("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 -_.")
        return String(plain(raw, limit: nil).filter { allowed.contains($0) }.prefix(31)).trimmingCharacters(in: .whitespaces)
    }
}
