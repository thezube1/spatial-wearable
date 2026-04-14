import CoreBluetooth
import SwiftUI

// UUIDs must match the Arduino firmware (arduino/09_wearable_pairing).
let spatialServiceUUID = CBUUID(string: "12345678-1234-5678-1234-56781234abcd")
let spatialCharacteristicUUID = CBUUID(string: "beb5483e-36e1-4688-b7f5-ea07361b26a8")
let macReadCharacteristicUUID = CBUUID(string: "12345678-1234-5678-1234-56781234abce")

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
    private var rssiTimer: Timer?

    // Async readMAC support.
    private var pendingMACContinuation: CheckedContinuation<String, Error>?

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
        if let char = macCharacteristic {
            return try await withCheckedThrowingContinuation { cont in
                self.pendingMACContinuation = cont
                peripheral.readValue(for: char)
            }
        }
        // Trigger (re)discovery, then wait for the value.
        peripheral.discoverServices([spatialServiceUUID])
        return try await withCheckedThrowingContinuation { cont in
            self.pendingMACContinuation = cont
            // Discovery callbacks below will read the MAC once found.
        }
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
        }
    }

    nonisolated func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        DispatchQueue.main.async {
            self.connectedPeripheral = peripheral
            self.connectionState = .connected
            peripheral.delegate = self
            peripheral.discoverServices([spatialServiceUUID])
            self.startRSSIUpdates()
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
            self.connectionState = .disconnected
            self.rssi = 0
            self.rssiTimer?.invalidate()
            self.rssiTimer = nil
        }
    }
}

// MARK: - CBPeripheralDelegate

extension BLEManager: CBPeripheralDelegate {
    nonisolated func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        guard let services = peripheral.services else { return }
        for service in services where service.uuid == spatialServiceUUID {
            peripheral.discoverCharacteristics(
                [spatialCharacteristicUUID, macReadCharacteristicUUID], for: service
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
