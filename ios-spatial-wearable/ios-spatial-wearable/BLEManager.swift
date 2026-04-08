import CoreBluetooth
import SwiftUI

// UUIDs for the ESP32S3 BLE service — match these on the Arduino side
let spatialServiceUUID = CBUUID(string: "4fafc201-1fb5-459e-8fcc-c5c9c331914b")
let spatialCharacteristicUUID = CBUUID(string: "beb5483e-36e1-4688-b7f5-ea07361b26a8")

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
        case disconnected
        case scanning
        case connecting
        case connected
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
    private var rssiTimer: Timer?

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

        // Auto-stop after 15 seconds
        DispatchQueue.main.asyncAfter(deadline: .now() + 15) { [weak self] in
            guard let self, self.isScanning else { return }
            self.stopScanning()
        }
    }

    func stopScanning() {
        centralManager.stopScan()
        isScanning = false
        if connectionState == .scanning {
            connectionState = .disconnected
        }
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
        connectionState = .disconnected
        rssi = 0
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

        // Filter out unnamed devices for cleaner list
        guard name != "Unknown" else { return }

        DispatchQueue.main.async {
            if let idx = self.discoveredDevices.firstIndex(where: { $0.id == peripheral.identifier }) {
                self.discoveredDevices[idx].rssi = RSSI.intValue
            } else {
                self.discoveredDevices.append(
                    DiscoveredDevice(
                        id: peripheral.identifier,
                        peripheral: peripheral,
                        name: name,
                        rssi: RSSI.intValue
                    )
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

    nonisolated func centralManager(
        _ central: CBCentralManager,
        didFailToConnect peripheral: CBPeripheral,
        error: Error?
    ) {
        DispatchQueue.main.async {
            self.connectionState = .failed(error?.localizedDescription ?? "Connection failed")
        }
    }

    nonisolated func centralManager(
        _ central: CBCentralManager,
        didDisconnectPeripheral peripheral: CBPeripheral,
        error: Error?
    ) {
        DispatchQueue.main.async {
            self.connectedPeripheral = nil
            self.spatialCharacteristic = nil
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
        for service in services {
            peripheral.discoverCharacteristics([spatialCharacteristicUUID], for: service)
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
                DispatchQueue.main.async {
                    self.spatialCharacteristic = characteristic
                }
                // Subscribe to notifications if supported
                if characteristic.properties.contains(.notify) {
                    peripheral.setNotifyValue(true, for: characteristic)
                }
                // Read initial value
                if characteristic.properties.contains(.read) {
                    peripheral.readValue(for: characteristic)
                }
            }
        }
    }

    nonisolated func peripheral(
        _ peripheral: CBPeripheral,
        didUpdateValueFor characteristic: CBCharacteristic,
        error: Error?
    ) {
        guard let data = characteristic.value else { return }
        DispatchQueue.main.async {
            self.lastReceivedData = data
            self.lastReceivedString = String(data: data, encoding: .utf8)
        }
    }

    nonisolated func peripheral(_ peripheral: CBPeripheral, didReadRSSI RSSI: NSNumber, error: Error?) {
        DispatchQueue.main.async {
            self.rssi = RSSI.intValue
        }
    }
}
