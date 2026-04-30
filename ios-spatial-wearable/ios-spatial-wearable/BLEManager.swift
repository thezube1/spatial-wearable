import CoreBluetooth
import SwiftUI

// UUIDs must match the Arduino firmware (arduino/10_wearable_persistent_pairing).
let spatialServiceUUID = CBUUID(string: "12345678-1234-5678-1234-56781234abcd")
let spatialCharacteristicUUID = CBUUID(string: "beb5483e-36e1-4688-b7f5-ea07361b26a8")
let macReadCharacteristicUUID = CBUUID(string: "12345678-1234-5678-1234-56781234abce")
let ownerWriteCharacteristicUUID = CBUUID(string: "12345678-1234-5678-1234-56781234abcf")
let ownerAuthCharacteristicUUID  = CBUUID(string: "12345678-1234-5678-1234-56781234abd0")
let locationCharacteristicUUID   = CBUUID(string: "12345678-1234-5678-1234-56781234abd1")
let targetCharacteristicUUID     = CBUUID(string: "12345678-1234-5678-1234-56781234abd2")
let peerLocationCharacteristicUUID = CBUUID(string: "12345678-1234-5678-1234-56781234abd3")
let candidatesCharacteristicUUID = CBUUID(string: "12345678-1234-5678-1234-56781234abd4")
let selectedTargetCharacteristicUUID = CBUUID(string: "12345678-1234-5678-1234-56781234abd5")
let gpsSyncCharacteristicUUID = CBUUID(string: "12345678-1234-5678-1234-56781234abd6")

struct WearableLocation: Equatable {
    let latitude: Double
    let longitude: Double
    let receivedAt: Date
}

struct PeerLocation: Equatable {
    let latitude: Double
    let longitude: Double
    let name: String
    let receivedAt: Date
}

struct DiscoveredDevice: Identifiable {
    let id: UUID
    let peripheral: CBPeripheral
    let name: String
    var rssi: Int
}

/// One entry in the wearable's candidate list (a group member who has a
/// linked device the wearer can choose to track).
struct CandidateEntry: Equatable {
    let mac: String   // "AA:BB:CC:DD:EE:FF"
    let name: String  // display name, will be truncated to 24 UTF-8 bytes
}

/// State of a user-initiated GPS sync (firmware 18, abd6 characteristic).
/// Drives the "Sync GPS" UI on the Map tab.
enum GpsSyncState: Equatable {
    /// No sync in progress.
    case idle
    /// Running on the wearable; radios off; `secondsRemaining` is a local
    /// estimate used to drive the countdown UI even while the BLE link is
    /// down. It can be 0 with state still .running for the brief window
    /// between the wearable's poll completing and iOS reconnecting.
    case running(secondsRemaining: Int)
    /// Wearable acquired a fresh fix during the sync window.
    case success
    /// Wearable's 30 s window elapsed without acquiring a fix, OR the iOS
    /// side hit its outer timeout waiting for the wearable to come back.
    case failed
}

@Observable
final class BLEManager: NSObject, @unchecked Sendable {
    var isBluetoothOn = false
    var bluetoothState: CBManagerState = .unknown
    var isScanning = false
    var discoveredDevices: [DiscoveredDevice] = []
    var connectedPeripheral: CBPeripheral?
    var connectionState: ConnectionState = .disconnected
    var lastReceivedData: Data?
    var lastReceivedString: String?
    var rssi: Int = 0
    var errorMessage: String?
    var wearableLocation: WearableLocation?
    var peerLocation: PeerLocation?
    /// Display name of the currently-selected tracking target. Set when iOS
    /// writes the target characteristic; used to label the peer's map pin.
    var trackingTargetName: String?
    /// MAC of the wearable's currently-tracked target, mirrored from the
    /// firmware via the …abd5 notify channel. Source of truth for the iOS
    /// Group screen "Tracking <name>" badge — updates whether the wearer
    /// confirmed via long-press or the phone wrote …abd2.
    var currentTrackedMAC: String?
    /// Last candidate list pushed to the wearable. Re-sent on reconnect so
    /// the firmware doesn't lose the list across BLE drops.
    var lastSentCandidates: [CandidateEntry] = []
    /// State of an in-flight GPS sync (firmware 18). The Map tab observes
    /// this to render the countdown / success / failure overlay.
    var gpsSyncState: GpsSyncState = .idle

    enum ConnectionState: Equatable {
        case disconnected, scanning, connecting, connected
        case failed(String)

        var label: String {
            switch self {
            case .disconnected: "Disconnected"
            case .scanning: "Scanning..."
            case .connecting: "Connecting..."
            case .connected: "Connected"
            case .failed(let msg): "Failed: \(msg)"
            }
        }

        var color: Color {
            switch self {
            case .disconnected: .secondary
            case .scanning: .orange
            case .connecting: .orange
            case .connected: .green
            case .failed: .red
            }
        }
    }

    private var centralManager: CBCentralManager!
    private var spatialCharacteristic: CBCharacteristic?
    private var macCharacteristic: CBCharacteristic?
    private var ownerWriteCharacteristic: CBCharacteristic?
    private var ownerAuthCharacteristic: CBCharacteristic?
    private var locationCharacteristic: CBCharacteristic?
    private var targetCharacteristic: CBCharacteristic?
    private var peerLocationCharacteristic: CBCharacteristic?
    private var candidatesCharacteristic: CBCharacteristic?
    private var selectedTargetCharacteristic: CBCharacteristic?
    private var gpsSyncCharacteristic: CBCharacteristic?
    private var rssiTimer: Timer?
    private var gpsSyncCountdownTimer: Timer?
    private var gpsSyncTimeoutTask: Task<Void, Never>?
    private var gpsSyncResolveTask: Task<Void, Never>?

    // Async support.
    private var pendingMACContinuation: CheckedContinuation<String, Error>?
    private var pendingWriteContinuation: CheckedContinuation<Void, Error>?
    private var pendingWriteCharUUID: CBUUID?
    private var pendingServicesContinuation: CheckedContinuation<Void, Error>?
    private var reconnectTargetMAC: String?
    private var reconnectContinuation: CheckedContinuation<Void, Error>?

    override init() {
        super.init()
        centralManager = CBCentralManager(delegate: self, queue: nil)
    }

    func startScanning() {
        guard centralManager.state == .poweredOn else { return }
        discoveredDevices.removeAll()
        connectionState = .scanning
        isScanning = true
        centralManager.scanForPeripherals(
            withServices: nil,
            options: [CBCentralManagerScanOptionAllowDuplicatesKey: false]
        )
        DispatchQueue.main.asyncAfter(deadline: .now() + 15) { [weak self] in
            guard let self, self.isScanning else { return }
            self.stopScanning()
        }
    }

    func stopScanning() {
        centralManager.stopScan()
        isScanning = false
        if connectionState == .scanning { connectionState = .disconnected }
    }

    func connect(to device: DiscoveredDevice) {
        stopScanning()
        connectionState = .connecting
        centralManager.connect(device.peripheral, options: nil)
    }

    func disconnect() {
        rssiTimer?.invalidate()
        rssiTimer = nil
        // User-initiated disconnect — also abandon any in-flight GPS sync.
        gpsSyncCountdownTimer?.invalidate()
        gpsSyncTimeoutTask?.cancel()
        gpsSyncResolveTask?.cancel()
        gpsSyncState = .idle
        if let peripheral = connectedPeripheral {
            centralManager.cancelPeripheralConnection(peripheral)
        }
        connectedPeripheral = nil
        spatialCharacteristic = nil
        macCharacteristic = nil
        locationCharacteristic = nil
        targetCharacteristic = nil
        peerLocationCharacteristic = nil
        candidatesCharacteristic = nil
        selectedTargetCharacteristic = nil
        gpsSyncCharacteristic = nil
        wearableLocation = nil
        peerLocation = nil
        trackingTargetName = nil
        currentTrackedMAC = nil
        connectionState = .disconnected
        rssi = 0
    }

    /// Reads the MAC characteristic from the currently connected peripheral.
    /// Rediscovers services/characteristics if needed.
    func readMAC() async throws -> String {
        guard let peripheral = connectedPeripheral else {
            throw NSError(domain: "BLEManager", code: 1,
                          userInfo: [NSLocalizedDescriptionKey: "Not connected"])
        }
        return try await withMACReadTimeout(seconds: 8) {
            let char = try await self.waitForCharacteristic(macReadCharacteristicUUID, timeout: 5.0)
            return try await withCheckedThrowingContinuation { cont in
                self.pendingMACContinuation = cont
                peripheral.readValue(for: char)
            }
        }
    }

    private func withMACReadTimeout(seconds: Double,
                                    _ op: @escaping () async throws -> String) async throws -> String {
        try await withThrowingTaskGroup(of: String.self) { group in
            group.addTask { try await op() }
            group.addTask {
                try await Task.sleep(nanoseconds: UInt64(seconds * 1_000_000_000))
                await MainActor.run {
                    if let cont = self.pendingMACContinuation {
                        self.pendingMACContinuation = nil
                        cont.resume(throwing: NSError(domain: "BLEManager", code: 3,
                            userInfo: [NSLocalizedDescriptionKey: "MAC read timed out"]))
                    }
                }
                throw NSError(domain: "BLEManager", code: 3,
                              userInfo: [NSLocalizedDescriptionKey: "MAC read timed out"])
            }
            defer { group.cancelAll() }
            guard let first = try await group.next() else {
                throw NSError(domain: "BLEManager", code: 4,
                              userInfo: [NSLocalizedDescriptionKey: "MAC read failed"])
            }
            return first
        }
    }

    /// Poll until the given characteristic has been discovered (service discovery
    /// completes asynchronously after `didConnect`, so callers that write
    /// immediately will otherwise race). Throws on timeout.
    private func characteristic(for uuid: CBUUID) -> CBCharacteristic? {
        if uuid == ownerWriteCharacteristicUUID { return ownerWriteCharacteristic }
        if uuid == ownerAuthCharacteristicUUID { return ownerAuthCharacteristic }
        if uuid == macReadCharacteristicUUID   { return macCharacteristic }
        if uuid == targetCharacteristicUUID    { return targetCharacteristic }
        if uuid == candidatesCharacteristicUUID { return candidatesCharacteristic }
        if uuid == gpsSyncCharacteristicUUID   { return gpsSyncCharacteristic }
        return nil
    }

    private func waitForCharacteristic(_ uuid: CBUUID, timeout: TimeInterval) async throws -> CBCharacteristic {
        let start = Date()
        while Date().timeIntervalSince(start) < timeout {
            if let c = characteristic(for: uuid) { return c }
            try await Task.sleep(for: .milliseconds(100))
        }
        throw NSError(domain: "BLEManager", code: 11,
                      userInfo: [NSLocalizedDescriptionKey: "Characteristic \(uuid) not discovered"])
    }

    /// Write the Supabase user_id to the owner-write characteristic during pairing.
    /// Firmware stores it in NVS and exits pairing mode.
    func writeOwner(userId: String) async throws {
        try await writeString(userId, toCharacteristic: ownerWriteCharacteristicUUID)
    }

    /// Prove ownership on a reconnect by writing the same user_id the wearable
    /// has stored. Firmware disconnects us if the value doesn't match. After a
    /// successful auth, re-push the cached candidate list so the firmware's
    /// in-RAM copy stays in sync across reconnects.
    func authenticate(userId: String) async throws {
        try await writeString(userId, toCharacteristic: ownerAuthCharacteristicUUID)
        let cached = await MainActor.run { self.lastSentCandidates }
        if !cached.isEmpty {
            try? await sendCandidateList(cached)
        }
    }

    private func writeString(_ value: String, toCharacteristic uuid: CBUUID) async throws {
        guard let data = value.data(using: .utf8) else {
            throw NSError(domain: "BLEManager", code: 12,
                          userInfo: [NSLocalizedDescriptionKey: "Non-UTF8 value"])
        }
        try await writeData(data, toCharacteristic: uuid)
    }

    private func writeData(_ data: Data, toCharacteristic uuid: CBUUID) async throws {
        guard let peripheral = connectedPeripheral else {
            throw NSError(domain: "BLEManager", code: 10,
                          userInfo: [NSLocalizedDescriptionKey: "Not connected"])
        }
        let characteristic = try await waitForCharacteristic(uuid, timeout: 5.0)
        try await withCheckedThrowingContinuation { (cont: CheckedContinuation<Void, Error>) in
            self.pendingWriteContinuation = cont
            self.pendingWriteCharUUID = uuid
            peripheral.writeValue(data, for: characteristic, type: .withResponse)
        }
    }

    /// Push the selected group member's wearable MAC + display name to the
    /// firmware. Payload: [6-byte MAC][1-byte name_len][name UTF-8 (<=24 bytes)].
    /// Firmware persists in NVS and uses it to filter ESP-NOW + BLE scan results.
    func setTrackingTarget(mac: String, name: String) async throws {
        guard let macBytes = Self.parseMAC(mac) else {
            throw NSError(domain: "BLEManager", code: 30,
                          userInfo: [NSLocalizedDescriptionKey: "Invalid MAC: \(mac)"])
        }
        let nameData = Array(name.utf8.prefix(24))
        var payload = Data()
        payload.append(contentsOf: macBytes)
        payload.append(UInt8(nameData.count))
        payload.append(contentsOf: nameData)
        try await writeData(payload, toCharacteristic: targetCharacteristicUUID)
        await MainActor.run {
            self.trackingTargetName = name
            // Drop any stale peer pin until the firmware pushes fresh GPS.
            self.peerLocation = nil
        }
    }

    /// Tell the wearable to stop tracking anyone (solo group or unlinked target).
    func clearTrackingTarget() async throws {
        try await writeData(Data([0x00]), toCharacteristic: targetCharacteristicUUID)
        await MainActor.run {
            self.trackingTargetName = nil
            self.peerLocation = nil
        }
    }

    /// Push the group's eligible-tracking-candidates list to the wearable.
    /// Wire format on …abd4: `[u8 count]` then for each candidate
    /// `[6-byte MAC][u8 nameLen][nameLen UTF-8]`. Capped at 8 entries.
    func setCandidateList(_ entries: [CandidateEntry]) async throws {
        try await sendCandidateList(entries)
    }

    /// Clear the wearable's candidate list (count=0). The firmware then
    /// auto-clears any active target that was on the list.
    func clearCandidateList() async throws {
        try await writeData(Data([0x00]), toCharacteristic: candidatesCharacteristicUUID)
        await MainActor.run { self.lastSentCandidates = [] }
    }

    /// Ask the wearable to enter GPS-sync mode (firmware 18). Writes `0x01`
    /// to abd6, then immediately flips local state to `.running` and starts
    /// a 30 s countdown. The BLE link will drop almost instantly (the
    /// wearable kills its radios) and the dashboard's reconnect loop will
    /// bring it back up; the wearable then notifies us with success/failed
    /// over abd6 once we've re-authed. The outer 90 s timeout protects
    /// against the wearable never coming back at all.
    func requestGpsSync() async throws {
        // Flip the UI to "running" before issuing the write so the user
        // sees instant feedback even if the BLE write blocks for a beat.
        DispatchQueue.main.async { self.startLocalGpsSyncCountdown() }
        do {
            try await writeData(Data([0x01]), toCharacteristic: gpsSyncCharacteristicUUID)
        } catch {
            // The wearable disconnects ~300 ms after accepting this write
            // (firmware 18 reboots into focused-fix mode), so it's normal
            // for CoreBluetooth to surface a write error here even though
            // the trigger landed cleanly. We swallow it; the abd6 notify
            // on reconnect resolves the state. If the write actually failed
            // *before* delivery, our 90 s outer timeout marks it as failed.
        }
    }

    /// MUST be called on the main thread. Resets timers, sets the .running
    /// state, kicks off the per-second countdown, and arms the 90 s outer
    /// timeout that resolves to .failed if the wearable never reports back.
    private func startLocalGpsSyncCountdown() {
        gpsSyncCountdownTimer?.invalidate()
        gpsSyncTimeoutTask?.cancel()
        gpsSyncResolveTask?.cancel()
        gpsSyncResolveTask = nil
        let total = 30
        gpsSyncState = .running(secondsRemaining: total)

        let started = Date()
        gpsSyncCountdownTimer = Timer.scheduledTimer(withTimeInterval: 1.0, repeats: true) { [weak self] timer in
            guard let self else { timer.invalidate(); return }
            let elapsed = Int(Date().timeIntervalSince(started))
            let remaining = max(0, total - elapsed)
            // Stop animating once we reach 0; .running stays until the
            // wearable's abd6 notify (or our outer timeout) resolves it.
            // The view swaps to "Finishing up…" copy when remaining == 0.
            switch self.gpsSyncState {
            case .running:
                self.gpsSyncState = .running(secondsRemaining: remaining)
            default:
                timer.invalidate()
            }
            if remaining == 0 {
                timer.invalidate()
            }
        }

        // Outer timeout: 90 s from request. The wearable's sync flow does a
        // double reboot (boot → focused fix → reboot → normal init → BLE
        // re-advertise), so the round-trip can run ~50 s in worst-case
        // boot/scan timing. 90 s gives that comfortable headroom while
        // still capping the UI on a wearable that genuinely never comes
        // back (battery dead, out of range).
        gpsSyncTimeoutTask = Task { [weak self] in
            try? await Task.sleep(for: .seconds(90))
            DispatchQueue.main.async {
                guard let self else { return }
                if case .running = self.gpsSyncState {
                    self.gpsSyncCountdownTimer?.invalidate()
                    self.gpsSyncState = .failed
                    self.scheduleGpsSyncIdleReset(after: 5)
                }
            }
        }
    }

    /// MUST be called on the main thread.
    private func resolveGpsSync(_ state: GpsSyncState) {
        gpsSyncCountdownTimer?.invalidate()
        gpsSyncTimeoutTask?.cancel()
        gpsSyncState = state
        scheduleGpsSyncIdleReset(after: state == .success ? 3 : 5)
    }

    /// MUST be called on the main thread.
    private func scheduleGpsSyncIdleReset(after seconds: Int) {
        gpsSyncResolveTask?.cancel()
        gpsSyncResolveTask = Task { [weak self] in
            try? await Task.sleep(for: .seconds(seconds))
            DispatchQueue.main.async {
                guard let self else { return }
                self.gpsSyncState = .idle
            }
        }
    }

    /// Parse the 2-byte abd6 payload `[status, secondsRemaining]`. Status:
    /// 0=idle, 1=running, 2=success, 3=failed. We only act on the terminal
    /// states (2/3); a stray "running" notify after the wearable is back
    /// online is benign because the local countdown owns the running UI.
    /// MUST be called on the main thread (delegate marshals via
    /// DispatchQueue.main.async before invoking).
    private func handleGpsSyncPayload(_ data: Data) {
        guard data.count >= 1 else { return }
        let status = data[0]
        switch status {
        case 0x02: resolveGpsSync(.success)
        case 0x03: resolveGpsSync(.failed)
        default:   break
        }
    }

    private func sendCandidateList(_ entries: [CandidateEntry]) async throws {
        let capped = Array(entries.prefix(8))
        var payload = Data()
        payload.append(UInt8(capped.count))
        for entry in capped {
            guard let macBytes = Self.parseMAC(entry.mac) else { continue }
            let nameBytes = Array(entry.name.utf8.prefix(24))
            payload.append(contentsOf: macBytes)
            payload.append(UInt8(nameBytes.count))
            payload.append(contentsOf: nameBytes)
        }
        try await writeData(payload, toCharacteristic: candidatesCharacteristicUUID)
        await MainActor.run { self.lastSentCandidates = capped }
    }

    /// Parse the 6-byte payload from the …abd5 selected-target notify channel.
    /// All-0xFF means "no target". Updates `currentTrackedMAC` (the source of
    /// truth for the iOS Group screen badge).
    private func handleSelectedTargetPayload(_ data: Data) {
        guard data.count == 6 else { return }
        let bytes = Array(data)
        if bytes == [0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF] {
            currentTrackedMAC = nil
            trackingTargetName = nil
            peerLocation = nil
            return
        }
        let mac = bytes.map { String(format: "%02X", $0) }.joined(separator: ":")
        currentTrackedMAC = mac
        if let match = lastSentCandidates.first(where: { $0.mac.uppercased() == mac.uppercased() }) {
            trackingTargetName = match.name
        }
    }

    private static func parseMAC(_ s: String) -> [UInt8]? {
        let parts = s.split(separator: ":")
        guard parts.count == 6 else { return nil }
        var out: [UInt8] = []
        out.reserveCapacity(6)
        for p in parts {
            guard p.count == 2, let b = UInt8(p, radix: 16) else { return nil }
            out.append(b)
        }
        return out
    }

    /// Rediscover and reconnect to the wristband whose MAC matches `mac`.
    /// Match is performed on the advertised local name `SW-XXXX` where XXXX is
    /// the last 4 MAC hex digits — the firmware derives the name that way.
    func reconnect(toMAC mac: String) async throws {
        let suffix = Self.nameSuffix(fromMAC: mac)
        let expectedName = "SW-\(suffix)"
        reconnectTargetMAC = expectedName

        guard centralManager.state == .poweredOn else {
            throw NSError(domain: "BLEManager", code: 20,
                          userInfo: [NSLocalizedDescriptionKey: "Bluetooth not powered on"])
        }

        discoveredDevices.removeAll()
        connectionState = .scanning
        isScanning = true
        centralManager.scanForPeripherals(
            withServices: [spatialServiceUUID],
            options: [CBCentralManagerScanOptionAllowDuplicatesKey: false]
        )

        try await withCheckedThrowingContinuation { (cont: CheckedContinuation<Void, Error>) in
            self.reconnectContinuation = cont
            DispatchQueue.main.asyncAfter(deadline: .now() + 20) { [weak self] in
                guard let self, let c = self.reconnectContinuation else { return }
                self.reconnectContinuation = nil
                self.reconnectTargetMAC = nil
                self.stopScanning()
                c.resume(throwing: NSError(domain: "BLEManager", code: 21,
                    userInfo: [NSLocalizedDescriptionKey: "Reconnect timed out"]))
            }
        }
    }

    private static func nameSuffix(fromMAC mac: String) -> String {
        // mac = "AA:BB:CC:DD:EE:FF" → last two bytes concatenated "EEFF".
        let parts = mac.split(separator: ":")
        guard parts.count == 6 else { return "" }
        return (parts[4] + parts[5]).uppercased()
    }

    /// Parse the 9-byte location payload pushed by the wearable:
    /// [valid(u8), lat(float32 LE), lon(float32 LE)].
    private func handleLocationPayload(_ data: Data) {
        guard data.count == 9 else { return }
        let valid = data[0] != 0
        guard valid else {
            wearableLocation = nil
            return
        }
        let lat: Float = data.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: 1, as: Float.self) }
        let lon: Float = data.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: 5, as: Float.self) }
        wearableLocation = WearableLocation(
            latitude: Double(lat),
            longitude: Double(lon),
            receivedAt: Date()
        )
    }

    /// Parse the 9-byte peer-location payload pushed by the wearable (abd3):
    /// [valid(u8), lat(float32 LE), lon(float32 LE)]. valid=0 clears the pin.
    private func handlePeerLocationPayload(_ data: Data) {
        guard data.count == 9 else { return }
        let valid = data[0] != 0
        guard valid else {
            peerLocation = nil
            return
        }
        let lat: Float = data.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: 1, as: Float.self) }
        let lon: Float = data.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: 5, as: Float.self) }
        peerLocation = PeerLocation(
            latitude: Double(lat),
            longitude: Double(lon),
            name: trackingTargetName ?? "Peer",
            receivedAt: Date()
        )
    }

    private func startRSSIUpdates() {
        rssiTimer?.invalidate()
        rssiTimer = Timer.scheduledTimer(withTimeInterval: 2.0, repeats: true) { [weak self] _ in
            self?.connectedPeripheral?.readRSSI()
        }
    }
}

// MARK: - CBCentralManagerDelegate

extension BLEManager: CBCentralManagerDelegate {
    nonisolated func centralManagerDidUpdateState(_ central: CBCentralManager) {
        DispatchQueue.main.async {
            self.bluetoothState = central.state
            self.isBluetoothOn = central.state == .poweredOn
        }
    }

    nonisolated func centralManager(
        _ central: CBCentralManager,
        didDiscover peripheral: CBPeripheral,
        advertisementData: [String: Any],
        rssi RSSI: NSNumber
    ) {
        let name = peripheral.name
            ?? (advertisementData[CBAdvertisementDataLocalNameKey] as? String)
            ?? "Unknown"
        guard name != "Unknown" else { return }

        DispatchQueue.main.async {
            if let idx = self.discoveredDevices.firstIndex(where: { $0.id == peripheral.identifier }) {
                self.discoveredDevices[idx].rssi = RSSI.intValue
            } else {
                self.discoveredDevices.append(
                    DiscoveredDevice(id: peripheral.identifier, peripheral: peripheral,
                                     name: name, rssi: RSSI.intValue)
                )
            }

            // Reconnect path: match by advertised local name and auto-connect.
            if let target = self.reconnectTargetMAC, name == target {
                self.reconnectTargetMAC = nil
                self.stopScanning()
                self.connectionState = .connecting
                self.centralManager.connect(peripheral, options: nil)
            }
        }
    }

    nonisolated func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        DispatchQueue.main.async {
            self.connectedPeripheral = peripheral
            self.connectionState = .connected
            peripheral.delegate = self
            peripheral.discoverServices([spatialServiceUUID])
            self.startRSSIUpdates()

            if let cont = self.reconnectContinuation {
                self.reconnectContinuation = nil
                cont.resume()
            }
        }
    }

    nonisolated func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        DispatchQueue.main.async {
            self.connectionState = .failed(error?.localizedDescription ?? "Connection failed")
            if let cont = self.pendingMACContinuation {
                self.pendingMACContinuation = nil
                cont.resume(throwing: error ?? NSError(domain: "BLE", code: -1))
            }
        }
    }

    nonisolated func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        DispatchQueue.main.async {
            self.connectedPeripheral = nil
            self.spatialCharacteristic = nil
            self.macCharacteristic = nil
            self.ownerWriteCharacteristic = nil
            self.ownerAuthCharacteristic = nil
            self.locationCharacteristic = nil
            self.targetCharacteristic = nil
            self.peerLocationCharacteristic = nil
            self.candidatesCharacteristic = nil
            self.selectedTargetCharacteristic = nil
            // Note: gpsSyncCharacteristic is cleared but gpsSyncState is
            // intentionally preserved — a sync-in-progress disconnect is
            // expected (the wearable kills its radios) and the dashboard's
            // reconnect loop will bring the link back up so the abd6 notify
            // can resolve the state.
            self.gpsSyncCharacteristic = nil
            self.wearableLocation = nil
            self.peerLocation = nil
            self.currentTrackedMAC = nil
            self.connectionState = .disconnected
            self.rssi = 0
            self.rssiTimer?.invalidate()
            self.rssiTimer = nil

            if let cont = self.pendingWriteContinuation {
                self.pendingWriteContinuation = nil
                self.pendingWriteCharUUID = nil
                cont.resume(throwing: error ?? NSError(domain: "BLE", code: 13,
                    userInfo: [NSLocalizedDescriptionKey: "Disconnected before write completed"]))
            }
        }
    }
}

// MARK: - CBPeripheralDelegate

extension BLEManager: CBPeripheralDelegate {
    nonisolated func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        guard let services = peripheral.services else { return }
        for service in services where service.uuid == spatialServiceUUID {
            peripheral.discoverCharacteristics(
                [
                    spatialCharacteristicUUID,
                    macReadCharacteristicUUID,
                    ownerWriteCharacteristicUUID,
                    ownerAuthCharacteristicUUID,
                    locationCharacteristicUUID,
                    targetCharacteristicUUID,
                    peerLocationCharacteristicUUID,
                    candidatesCharacteristicUUID,
                    selectedTargetCharacteristicUUID,
                    gpsSyncCharacteristicUUID,
                ], for: service
            )
        }
    }

    nonisolated func peripheral(
        _ peripheral: CBPeripheral,
        didDiscoverCharacteristicsFor service: CBService,
        error: Error?
    ) {
        guard let characteristics = service.characteristics else { return }
        for characteristic in characteristics {
            if characteristic.uuid == spatialCharacteristicUUID {
                DispatchQueue.main.async { self.spatialCharacteristic = characteristic }
                if characteristic.properties.contains(.notify) {
                    peripheral.setNotifyValue(true, for: characteristic)
                }
                if characteristic.properties.contains(.read) {
                    peripheral.readValue(for: characteristic)
                }
            } else if characteristic.uuid == macReadCharacteristicUUID {
                DispatchQueue.main.async { self.macCharacteristic = characteristic }
                // If someone awaited readMAC() before discovery completed, kick off the read now.
                DispatchQueue.main.async {
                    if self.pendingMACContinuation != nil {
                        peripheral.readValue(for: characteristic)
                    }
                }
            } else if characteristic.uuid == ownerWriteCharacteristicUUID {
                DispatchQueue.main.async { self.ownerWriteCharacteristic = characteristic }
            } else if characteristic.uuid == ownerAuthCharacteristicUUID {
                DispatchQueue.main.async { self.ownerAuthCharacteristic = characteristic }
            } else if characteristic.uuid == targetCharacteristicUUID {
                DispatchQueue.main.async { self.targetCharacteristic = characteristic }
            } else if characteristic.uuid == locationCharacteristicUUID {
                DispatchQueue.main.async { self.locationCharacteristic = characteristic }
                if characteristic.properties.contains(.notify) {
                    peripheral.setNotifyValue(true, for: characteristic)
                }
                if characteristic.properties.contains(.read) {
                    peripheral.readValue(for: characteristic)
                }
            } else if characteristic.uuid == peerLocationCharacteristicUUID {
                DispatchQueue.main.async { self.peerLocationCharacteristic = characteristic }
                if characteristic.properties.contains(.notify) {
                    peripheral.setNotifyValue(true, for: characteristic)
                }
                if characteristic.properties.contains(.read) {
                    peripheral.readValue(for: characteristic)
                }
            } else if characteristic.uuid == candidatesCharacteristicUUID {
                DispatchQueue.main.async { self.candidatesCharacteristic = characteristic }
            } else if characteristic.uuid == selectedTargetCharacteristicUUID {
                DispatchQueue.main.async { self.selectedTargetCharacteristic = characteristic }
                if characteristic.properties.contains(.notify) {
                    peripheral.setNotifyValue(true, for: characteristic)
                }
                if characteristic.properties.contains(.read) {
                    peripheral.readValue(for: characteristic)
                }
            } else if characteristic.uuid == gpsSyncCharacteristicUUID {
                DispatchQueue.main.async { self.gpsSyncCharacteristic = characteristic }
                if characteristic.properties.contains(.notify) {
                    peripheral.setNotifyValue(true, for: characteristic)
                }
            }
        }
    }

    nonisolated func peripheral(
        _ peripheral: CBPeripheral,
        didWriteValueFor characteristic: CBCharacteristic,
        error: Error?
    ) {
        DispatchQueue.main.async {
            guard let cont = self.pendingWriteContinuation,
                  let pendingUUID = self.pendingWriteCharUUID,
                  characteristic.uuid == pendingUUID else { return }
            self.pendingWriteContinuation = nil
            self.pendingWriteCharUUID = nil
            if let err = error {
                cont.resume(throwing: err)
            } else {
                cont.resume()
            }
        }
    }

    nonisolated func peripheral(
        _ peripheral: CBPeripheral,
        didUpdateValueFor characteristic: CBCharacteristic,
        error: Error?
    ) {
        DispatchQueue.main.async {
            if characteristic.uuid == macReadCharacteristicUUID {
                if let cont = self.pendingMACContinuation {
                    self.pendingMACContinuation = nil
                    if let err = error {
                        cont.resume(throwing: err)
                    } else if let data = characteristic.value,
                              let mac = String(data: data, encoding: .utf8) {
                        cont.resume(returning: mac)
                    } else {
                        cont.resume(throwing: NSError(domain: "BLE", code: 2,
                            userInfo: [NSLocalizedDescriptionKey: "Empty MAC value"]))
                    }
                }
                return
            }
            guard let data = characteristic.value else { return }
            if characteristic.uuid == locationCharacteristicUUID {
                self.handleLocationPayload(data)
                return
            }
            if characteristic.uuid == peerLocationCharacteristicUUID {
                self.handlePeerLocationPayload(data)
                return
            }
            if characteristic.uuid == selectedTargetCharacteristicUUID {
                self.handleSelectedTargetPayload(data)
                return
            }
            if characteristic.uuid == gpsSyncCharacteristicUUID {
                self.handleGpsSyncPayload(data)
                return
            }
            self.lastReceivedData = data
            self.lastReceivedString = String(data: data, encoding: .utf8)
        }
    }

    nonisolated func peripheral(_ peripheral: CBPeripheral, didReadRSSI RSSI: NSNumber, error: Error?) {
        DispatchQueue.main.async { self.rssi = RSSI.intValue }
    }
}
