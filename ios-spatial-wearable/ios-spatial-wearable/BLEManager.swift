import CoreBluetooth
import SwiftUI

// UUIDs must match the Arduino firmware (arduino/10_wearable_persistent_pairing).
let spatialServiceUUID = CBUUID(string: "12345678-1234-5678-1234-56781234abcd")
let spatialCharacteristicUUID = CBUUID(string: "beb5483e-36e1-4688-b7f5-ea07361b26a8")
let macReadCharacteristicUUID = CBUUID(string: "12345678-1234-5678-1234-56781234abce")
let ownerWriteCharacteristicUUID = CBUUID(string: "12345678-1234-5678-1234-56781234abcf")
let ownerAuthCharacteristicUUID  = CBUUID(string: "12345678-1234-5678-1234-56781234abd0")

struct DiscoveredDevice: Identifiable {
    let id: UUID
    let peripheral: CBPeripheral
    let name: String
    var rssi: Int
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
    private var rssiTimer: Timer?

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
        if let peripheral = connectedPeripheral {
            centralManager.cancelPeripheralConnection(peripheral)
        }
        connectedPeripheral = nil
        spatialCharacteristic = nil
        macCharacteristic = nil
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
    /// has stored. Firmware disconnects us if the value doesn't match.
    func authenticate(userId: String) async throws {
        try await writeString(userId, toCharacteristic: ownerAuthCharacteristicUUID)
    }

    private func writeString(_ value: String, toCharacteristic uuid: CBUUID) async throws {
        guard let peripheral = connectedPeripheral else {
            throw NSError(domain: "BLEManager", code: 10,
                          userInfo: [NSLocalizedDescriptionKey: "Not connected"])
        }
        let characteristic = try await waitForCharacteristic(uuid, timeout: 5.0)
        guard let data = value.data(using: .utf8) else {
            throw NSError(domain: "BLEManager", code: 12,
                          userInfo: [NSLocalizedDescriptionKey: "Non-UTF8 value"])
        }
        try await withCheckedThrowingContinuation { (cont: CheckedContinuation<Void, Error>) in
            self.pendingWriteContinuation = cont
            self.pendingWriteCharUUID = uuid
            peripheral.writeValue(data, for: characteristic, type: .withResponse)
        }
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
            self.lastReceivedData = data
            self.lastReceivedString = String(data: data, encoding: .utf8)
        }
    }

    nonisolated func peripheral(_ peripheral: CBPeripheral, didReadRSSI RSSI: NSNumber, error: Error?) {
        DispatchQueue.main.async { self.rssi = RSSI.intValue }
    }
}
