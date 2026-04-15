import SwiftUI

struct DeviceTabView: View {
    @Environment(BLEManager.self) private var ble
    @Environment(OnboardingCoordinator.self) private var coordinator

    @Binding var device: Device?
    @Binding var errorMessage: String?

    @State private var isUnlinking = false
    @State private var showPairSheet = false
    @State private var showUnlinkConfirm = false

    private var mac: String? {
        device?.mac_address ?? coordinator.linkedDeviceMAC
    }

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(spacing: 16) {
                    Text("Device")
                        .font(.largeTitle.bold())
                        .frame(maxWidth: .infinity, alignment: .leading)
                    if mac != nil {
                        infoCard
                        actionCard
                    } else {
                        emptyCard
                    }

                    if let errorMessage {
                        Text(errorMessage)
                            .font(.caption)
                            .foregroundStyle(.red)
                            .frame(maxWidth: .infinity, alignment: .leading)
                    }
                }
                .padding(20)
            }
            .background(Color(.systemGroupedBackground))
            .toolbar(.hidden, for: .navigationBar)
        }
        .sheet(isPresented: $showPairSheet) {
            NavigationStack {
                LinkWristbandView(onLinked: { newMAC in
                    Task {
                        device = try? await APIClient.shared.getMyDevice()
                        coordinator.linkedDeviceMAC = newMAC
                        showPairSheet = false
                    }
                })
                .toolbar {
                    ToolbarItem(placement: .cancellationAction) {
                        Button("Cancel") { showPairSheet = false }
                            .foregroundStyle(.white)
                    }
                }
            }
        }
        .confirmationDialog("Unlink this wristband?",
                            isPresented: $showUnlinkConfirm,
                            titleVisibility: .visible) {
            Button("Unlink", role: .destructive) { Task { await unlink() } }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("The wristband will remain paired to this account on the device until you hold the BOOT button for 5 seconds to forget the owner.")
        }
    }

    private var infoCard: some View {
        card(title: "Linked Wristband") {
            VStack(alignment: .leading, spacing: 10) {
                HStack(spacing: 8) {
                    Circle().fill(ble.connectionState.color).frame(width: 8, height: 8)
                    Text(ble.connectionState.label).font(.subheadline)
                }
                if let mac {
                    LabeledRow(label: "MAC", value: mac, mono: true)
                }
                if let linkedAt = device?.linked_at {
                    LabeledRow(label: "Linked", value: linkedAt, mono: false)
                }
                if ble.connectionState == .connected, ble.rssi != 0 {
                    LabeledRow(label: "Signal", value: "\(ble.rssi) dBm", mono: false)
                }
            }
        }
    }

    private var actionCard: some View {
        card(title: "Actions") {
            VStack(spacing: 10) {
                Button {
                    showPairSheet = true
                } label: {
                    Label("Pair a different wristband", systemImage: "plus.circle")
                        .frame(maxWidth: .infinity)
                        .padding(.vertical, 10)
                }
                .buttonStyle(.bordered)

                Button(role: .destructive) {
                    showUnlinkConfirm = true
                } label: {
                    Label(isUnlinking ? "Unlinking…" : "Unlink this wristband",
                          systemImage: "xmark.circle")
                        .frame(maxWidth: .infinity)
                        .padding(.vertical, 10)
                }
                .buttonStyle(.bordered)
                .disabled(isUnlinking)
            }
        }
    }

    private var emptyCard: some View {
        card(title: "Wristband") {
            VStack(alignment: .leading, spacing: 12) {
                Text("No device linked").foregroundStyle(.secondary)
                Button {
                    showPairSheet = true
                } label: {
                    Label("Pair a wristband", systemImage: "plus.circle")
                        .frame(maxWidth: .infinity)
                        .padding(.vertical, 10)
                }
                .buttonStyle(.borderedProminent)
            }
        }
    }

    private func unlink() async {
        isUnlinking = true
        errorMessage = nil
        defer { isUnlinking = false }
        do {
            try await APIClient.shared.unlinkDevice()
            ble.disconnect()
            coordinator.linkedDeviceMAC = nil
            device = nil
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    private func card<Content: View>(title: String, @ViewBuilder content: () -> Content) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(title).font(.caption.weight(.semibold)).foregroundStyle(.secondary)
            content()
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(16)
        .background(.background, in: RoundedRectangle(cornerRadius: 16))
        .shadow(color: .black.opacity(0.04), radius: 6, y: 3)
    }
}

#Preview {
    struct PreviewWrapper: View {
        @State var device: Device?
        @State var error: String?
        var body: some View {
            DeviceTabView(device: $device, errorMessage: $error)
                .environment(BLEManager())
                .environment(OnboardingCoordinator())
        }
    }
    return PreviewWrapper()
}

private struct LabeledRow: View {
    let label: String
    let value: String
    let mono: Bool

    var body: some View {
        HStack(alignment: .firstTextBaseline) {
            Text(label).font(.caption).foregroundStyle(.secondary).frame(width: 60, alignment: .leading)
            Text(value)
                .font(mono ? .footnote.monospaced() : .footnote)
                .textSelection(.enabled)
            Spacer()
        }
    }
}
