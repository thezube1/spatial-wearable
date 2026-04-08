import SwiftUI

struct OnboardingView: View {
    @Environment(BLEManager.self) private var bleManager
    @Binding var hasCompletedOnboarding: Bool
    @State private var currentPage = 0

    var body: some View {
        ZStack {
            // Background gradient
            LinearGradient(
                colors: [Color(.systemBackground), Color.blue.opacity(0.08)],
                startPoint: .top,
                endPoint: .bottom
            )
            .ignoresSafeArea()

            VStack {
                // Page indicator
                HStack(spacing: 8) {
                    ForEach(0..<3) { index in
                        Capsule()
                            .fill(index == currentPage ? Color.blue : Color.secondary.opacity(0.3))
                            .frame(width: index == currentPage ? 24 : 8, height: 8)
                            .animation(.spring(duration: 0.3), value: currentPage)
                    }
                }
                .padding(.top, 60)

                TabView(selection: $currentPage) {
                    welcomePage.tag(0)
                    bluetoothPage.tag(1)
                    connectPage.tag(2)
                }
                .tabViewStyle(.page(indexDisplayMode: .never))
                .animation(.easeInOut(duration: 0.3), value: currentPage)
            }
        }
    }

    // MARK: - Page 1: Welcome

    private var welcomePage: some View {
        VStack(spacing: 32) {
            Spacer()

            ZStack {
                Circle()
                    .fill(Color.blue.opacity(0.1))
                    .frame(width: 160, height: 160)
                Circle()
                    .fill(Color.blue.opacity(0.05))
                    .frame(width: 220, height: 220)
                Image(systemName: "wave.3.right.circle.fill")
                    .font(.system(size: 80))
                    .foregroundStyle(.blue)
            }

            VStack(spacing: 12) {
                Text("Spatial Wearable")
                    .font(.largeTitle.weight(.bold))
                Text("Connect to your ESP32 device and explore spatial data in real-time.")
                    .font(.body)
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.center)
                    .padding(.horizontal, 40)
            }

            Spacer()

            Button {
                withAnimation { currentPage = 1 }
            } label: {
                Text("Get Started")
                    .font(.headline)
                    .frame(maxWidth: .infinity)
                    .padding(.vertical, 16)
            }
            .buttonStyle(.borderedProminent)
            .padding(.horizontal, 32)
            .padding(.bottom, 48)
        }
    }

    // MARK: - Page 2: Bluetooth Permission

    private var bluetoothPage: some View {
        VStack(spacing: 32) {
            Spacer()

            ZStack {
                Circle()
                    .fill(bluetoothStatusColor.opacity(0.1))
                    .frame(width: 160, height: 160)
                Image(systemName: bluetoothIconName)
                    .font(.system(size: 72))
                    .foregroundStyle(bluetoothStatusColor)
                    .symbolEffect(.pulse, isActive: bleManager.bluetoothState == .poweredOn)
            }

            VStack(spacing: 12) {
                Text("Bluetooth")
                    .font(.largeTitle.weight(.bold))
                Text(bluetoothStatusMessage)
                    .font(.body)
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.center)
                    .padding(.horizontal, 40)
            }

            Spacer()

            VStack(spacing: 12) {
                Button {
                    withAnimation { currentPage = 2 }
                } label: {
                    Text("Continue")
                        .font(.headline)
                        .frame(maxWidth: .infinity)
                        .padding(.vertical, 16)
                }
                .buttonStyle(.borderedProminent)
                .disabled(!bleManager.isBluetoothOn)

                if !bleManager.isBluetoothOn && bleManager.bluetoothState != .unknown {
                    Text("Enable Bluetooth in Settings to continue")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }
            .padding(.horizontal, 32)
            .padding(.bottom, 48)
        }
    }

    // MARK: - Page 3: Scan & Connect

    private var connectPage: some View {
        VStack(spacing: 24) {
            VStack(spacing: 8) {
                Text("Find Your Device")
                    .font(.title2.weight(.bold))
                    .padding(.top, 32)
                Text("Scan for nearby ESP32 devices")
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
            }

            // Scan button
            Button {
                bleManager.startScanning()
            } label: {
                HStack(spacing: 8) {
                    if bleManager.isScanning {
                        ProgressView()
                            .tint(.white)
                    }
                    Text(bleManager.isScanning ? "Scanning..." : "Scan for Devices")
                }
                .font(.headline)
                .frame(maxWidth: .infinity)
                .padding(.vertical, 14)
            }
            .buttonStyle(.borderedProminent)
            .disabled(bleManager.isScanning)
            .padding(.horizontal, 32)

            // Device list
            if bleManager.discoveredDevices.isEmpty && !bleManager.isScanning {
                VStack(spacing: 16) {
                    Spacer()
                    Image(systemName: "antenna.radiowaves.left.and.right")
                        .font(.system(size: 48))
                        .foregroundStyle(.tertiary)
                    Text("No devices found yet")
                        .font(.subheadline)
                        .foregroundStyle(.tertiary)
                    Spacer()
                }
            } else {
                ScrollView {
                    LazyVStack(spacing: 12) {
                        ForEach(bleManager.discoveredDevices) { device in
                            DeviceRow(device: device) {
                                bleManager.connect(to: device)
                            }
                        }
                    }
                    .padding(.horizontal, 20)
                }
            }

            // Connected state — finish onboarding
            if bleManager.connectionState == .connected {
                VStack(spacing: 16) {
                    HStack(spacing: 8) {
                        Image(systemName: "checkmark.circle.fill")
                            .foregroundStyle(.green)
                        Text("Connected to \(bleManager.connectedPeripheral?.name ?? "device")")
                            .font(.subheadline.weight(.medium))
                    }

                    Button {
                        hasCompletedOnboarding = true
                    } label: {
                        Text("Go to Dashboard")
                            .font(.headline)
                            .frame(maxWidth: .infinity)
                            .padding(.vertical, 16)
                    }
                    .buttonStyle(.borderedProminent)
                    .tint(.green)
                }
                .padding(.horizontal, 32)
                .padding(.bottom, 24)
                .transition(.move(edge: .bottom).combined(with: .opacity))
            }

            // Skip option
            if bleManager.connectionState != .connected {
                Button {
                    hasCompletedOnboarding = true
                } label: {
                    Text("Skip for now")
                        .font(.subheadline)
                        .foregroundStyle(.secondary)
                }
                .padding(.bottom, 24)
            }
        }
    }

    // MARK: - Helpers

    private var bluetoothIconName: String {
        switch bleManager.bluetoothState {
        case .poweredOn: "bluetooth.circle.fill"
        case .poweredOff: "bluetooth.slash"
        case .unauthorized: "lock.circle"
        default: "bluetooth"
        }
    }

    private var bluetoothStatusColor: Color {
        switch bleManager.bluetoothState {
        case .poweredOn: .blue
        case .poweredOff, .unauthorized: .red
        default: .secondary
        }
    }

    private var bluetoothStatusMessage: String {
        switch bleManager.bluetoothState {
        case .poweredOn:
            "Bluetooth is ready. Let's find your device."
        case .poweredOff:
            "Bluetooth is turned off. Please enable it in Settings."
        case .unauthorized:
            "Bluetooth access was denied. Please allow it in Settings."
        case .unsupported:
            "This device does not support Bluetooth Low Energy."
        default:
            "Checking Bluetooth status..."
        }
    }
}

// MARK: - Device Row

struct DeviceRow: View {
    let device: DiscoveredDevice
    let onConnect: () -> Void

    var body: some View {
        HStack(spacing: 14) {
            // Signal icon
            Image(systemName: signalIcon)
                .font(.title3)
                .foregroundStyle(signalColor)
                .frame(width: 32)

            VStack(alignment: .leading, spacing: 2) {
                Text(device.name)
                    .font(.body.weight(.medium))
                Text("\(device.rssi) dBm")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Spacer()

            Button("Connect", action: onConnect)
                .buttonStyle(.bordered)
                .controlSize(.small)
        }
        .padding(14)
        .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 14))
    }

    private var signalIcon: String {
        if device.rssi > -50 { return "wifi" }
        if device.rssi > -70 { return "wifi" }
        return "wifi.exclamationmark"
    }

    private var signalColor: Color {
        if device.rssi > -50 { return .green }
        if device.rssi > -70 { return .orange }
        return .red
    }
}
