import SwiftUI

struct DashboardView: View {
    @Environment(BLEManager.self) private var bleManager
    @State private var showDisconnectConfirm = false

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(spacing: 20) {
                    connectionCard
                    if bleManager.connectionState == .connected {
                        signalCard
                        dataCard
                    }
                    deviceActions
                }
                .padding(20)
            }
            .background(Color(.systemGroupedBackground))
            .navigationTitle("Dashboard")
            .refreshable {
                if bleManager.connectionState != .connected {
                    bleManager.startScanning()
                    try? await Task.sleep(for: .seconds(3))
                    bleManager.stopScanning()
                }
            }
        }
    }

    // MARK: - Connection Status Card

    private var connectionCard: some View {
        VStack(spacing: 16) {
            HStack(spacing: 14) {
                ZStack {
                    Circle()
                        .fill(bleManager.connectionState.color.opacity(0.15))
                        .frame(width: 52, height: 52)
                    Image(systemName: connectionIcon)
                        .font(.title2)
                        .foregroundStyle(bleManager.connectionState.color)
                        .symbolEffect(
                            .pulse,
                            isActive: bleManager.connectionState == .scanning
                                || bleManager.connectionState == .connecting
                        )
                }

                VStack(alignment: .leading, spacing: 4) {
                    Text(bleManager.connectionState.label)
                        .font(.headline)
                    if let name = bleManager.connectedPeripheral?.name {
                        Text(name)
                            .font(.subheadline)
                            .foregroundStyle(.secondary)
                    } else if bleManager.connectionState == .disconnected {
                        Text("No device connected")
                            .font(.subheadline)
                            .foregroundStyle(.secondary)
                    }
                }

                Spacer()

                // Live indicator
                if bleManager.connectionState == .connected {
                    HStack(spacing: 4) {
                        Circle()
                            .fill(.green)
                            .frame(width: 8, height: 8)
                        Text("LIVE")
                            .font(.caption2.weight(.bold))
                            .foregroundStyle(.green)
                    }
                    .padding(.horizontal, 10)
                    .padding(.vertical, 5)
                    .background(.green.opacity(0.1), in: Capsule())
                }
            }

            // Connection timeline
            HStack(spacing: 0) {
                timelineStep("Bluetooth", isActive: bleManager.isBluetoothOn, isDone: bleManager.isBluetoothOn)
                timelineLine(isActive: bleManager.connectionState != .disconnected)
                timelineStep("Found", isActive: bleManager.connectionState == .connecting, isDone: bleManager.connectionState == .connected)
                timelineLine(isActive: bleManager.connectionState == .connected)
                timelineStep("Connected", isActive: false, isDone: bleManager.connectionState == .connected)
            }
        }
        .padding(20)
        .background(.background, in: RoundedRectangle(cornerRadius: 20))
        .shadow(color: .black.opacity(0.04), radius: 8, y: 4)
    }

    // MARK: - Signal Strength Card

    private var signalCard: some View {
        VStack(spacing: 12) {
            HStack {
                Label("Signal Strength", systemImage: "antenna.radiowaves.left.and.right")
                    .font(.subheadline.weight(.medium))
                    .foregroundStyle(.secondary)
                Spacer()
            }

            HStack(alignment: .bottom, spacing: 4) {
                Text("\(bleManager.rssi)")
                    .font(.system(size: 42, weight: .bold, design: .rounded))
                    .foregroundStyle(rssiColor)
                Text("dBm")
                    .font(.title3.weight(.medium))
                    .foregroundStyle(.secondary)
                    .padding(.bottom, 6)

                Spacer()

                // Signal bars
                HStack(spacing: 3) {
                    ForEach(0..<4) { i in
                        RoundedRectangle(cornerRadius: 2)
                            .fill(i < signalBars ? rssiColor : Color.secondary.opacity(0.2))
                            .frame(width: 8, height: CGFloat(10 + i * 8))
                    }
                }
                .padding(.bottom, 6)
            }

            // Quality label
            HStack {
                Text(signalQuality)
                    .font(.caption.weight(.medium))
                    .foregroundStyle(rssiColor)
                    .padding(.horizontal, 10)
                    .padding(.vertical, 4)
                    .background(rssiColor.opacity(0.1), in: Capsule())
                Spacer()
            }
        }
        .padding(20)
        .background(.background, in: RoundedRectangle(cornerRadius: 20))
        .shadow(color: .black.opacity(0.04), radius: 8, y: 4)
    }

    // MARK: - Data Card

    private var dataCard: some View {
        VStack(spacing: 12) {
            HStack {
                Label("Incoming Data", systemImage: "arrow.down.circle")
                    .font(.subheadline.weight(.medium))
                    .foregroundStyle(.secondary)
                Spacer()
            }

            if let data = bleManager.lastReceivedString {
                Text(data)
                    .font(.system(.body, design: .monospaced))
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(14)
                    .background(Color(.secondarySystemBackground), in: RoundedRectangle(cornerRadius: 12))
            } else {
                VStack(spacing: 8) {
                    Image(systemName: "ellipsis")
                        .font(.title2)
                        .foregroundStyle(.tertiary)
                    Text("Waiting for data from device...")
                        .font(.caption)
                        .foregroundStyle(.tertiary)
                }
                .frame(maxWidth: .infinity)
                .padding(.vertical, 20)
            }
        }
        .padding(20)
        .background(.background, in: RoundedRectangle(cornerRadius: 20))
        .shadow(color: .black.opacity(0.04), radius: 8, y: 4)
    }

    // MARK: - Actions

    private var deviceActions: some View {
        VStack(spacing: 12) {
            if bleManager.connectionState == .connected {
                Button(role: .destructive) {
                    showDisconnectConfirm = true
                } label: {
                    Label("Disconnect", systemImage: "xmark.circle")
                        .frame(maxWidth: .infinity)
                        .padding(.vertical, 14)
                }
                .buttonStyle(.bordered)
                .confirmationDialog("Disconnect from device?", isPresented: $showDisconnectConfirm) {
                    Button("Disconnect", role: .destructive) {
                        bleManager.disconnect()
                    }
                }
            } else {
                Button {
                    bleManager.startScanning()
                } label: {
                    Label(
                        bleManager.isScanning ? "Scanning..." : "Scan for Devices",
                        systemImage: "magnifyingglass"
                    )
                    .frame(maxWidth: .infinity)
                    .padding(.vertical, 14)
                }
                .buttonStyle(.borderedProminent)
                .disabled(bleManager.isScanning)
            }
        }
    }

    // MARK: - Helpers

    private var connectionIcon: String {
        switch bleManager.connectionState {
        case .disconnected: "bolt.slash.fill"
        case .scanning: "magnifyingglass"
        case .connecting: "arrow.triangle.2.circlepath"
        case .connected: "bolt.fill"
        case .failed: "exclamationmark.triangle.fill"
        }
    }

    private var signalBars: Int {
        if bleManager.rssi > -50 { return 4 }
        if bleManager.rssi > -65 { return 3 }
        if bleManager.rssi > -80 { return 2 }
        return 1
    }

    private var rssiColor: Color {
        if bleManager.rssi > -50 { return .green }
        if bleManager.rssi > -65 { return .blue }
        if bleManager.rssi > -80 { return .orange }
        return .red
    }

    private var signalQuality: String {
        if bleManager.rssi > -50 { return "Excellent" }
        if bleManager.rssi > -65 { return "Good" }
        if bleManager.rssi > -80 { return "Fair" }
        return "Weak"
    }

    // MARK: - Timeline Components

    private func timelineStep(_ label: String, isActive: Bool, isDone: Bool) -> some View {
        VStack(spacing: 6) {
            ZStack {
                Circle()
                    .fill(isDone ? Color.green : (isActive ? Color.orange : Color.secondary.opacity(0.2)))
                    .frame(width: 28, height: 28)
                if isDone {
                    Image(systemName: "checkmark")
                        .font(.caption.weight(.bold))
                        .foregroundStyle(.white)
                } else {
                    Circle()
                        .fill(.background)
                        .frame(width: 10, height: 10)
                }
            }
            Text(label)
                .font(.caption2)
                .foregroundStyle(isDone ? .primary : .secondary)
        }
    }

    private func timelineLine(isActive: Bool) -> some View {
        Rectangle()
            .fill(isActive ? Color.green : Color.secondary.opacity(0.2))
            .frame(height: 2)
            .frame(maxWidth: .infinity)
            .padding(.bottom, 20)
    }
}
