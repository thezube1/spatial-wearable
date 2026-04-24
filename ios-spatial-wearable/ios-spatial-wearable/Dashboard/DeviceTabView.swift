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
        ScrollView(showsIndicators: false) {
            VStack(spacing: 16) {
                titleCard

                if mac != nil {
                    connectionRow
                    if let mac {
                        statusRow(label: "MAC",
                                  valueText: Text(mac).font(OnboardingStyle.font(15, weight: .semibold).monospaced()))
                    }
                    if let linkedAt = device?.linked_at {
                        statusRow(label: "Linked",
                                  valueText: Text(linkedAt).font(OnboardingStyle.font(16, weight: .semibold)))
                    }
                    if ble.connectionState == .connected, ble.rssi != 0 {
                        statusRow(label: "Signal",
                                  valueText: Text("\(ble.rssi) dBm").font(OnboardingStyle.font(16, weight: .semibold)))
                    }

                    actionButtons
                } else {
                    emptyCard
                }

                if let errorMessage {
                    Text(errorMessage)
                        .font(OnboardingStyle.font(12))
                        .foregroundStyle(.red.opacity(0.95))
                        .frame(width: OnboardingStyle.fieldWidth, alignment: .leading)
                }
            }
            .padding(.horizontal, 28)
            .padding(.top, 10)
            .padding(.bottom, 20)
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

    private var titleCard: some View {
        HStack {
            Text("Device Status")
                .font(OnboardingStyle.font(17, weight: .semibold))
                .foregroundStyle(.black.opacity(0.6))
            Spacer()
        }
        .frame(width: OnboardingStyle.fieldWidth)
    }

    private var connectionRow: some View {
        let c = ble.connectionState
        return HStack {
            Text("Connection Status")
                .font(OnboardingStyle.font(14))
                .foregroundStyle(.black.opacity(0.55))
            Spacer()
            Circle()
                .fill(c.color)
                .frame(width: 9, height: 9)
            Text(c.label)
                .font(OnboardingStyle.font(16, weight: .semibold))
                .foregroundStyle(.black)
        }
        .padding(.horizontal, 10)
        .frame(width: OnboardingStyle.fieldWidth, height: 54)
        .background(Color.white.opacity(0.5))
        .clipShape(RoundedRectangle(cornerRadius: OnboardingStyle.cornerRadius))
    }

    private func statusRow(label: String, valueText: Text) -> some View {
        HStack {
            Text(label)
                .font(OnboardingStyle.font(14))
                .foregroundStyle(.black.opacity(0.55))
            Spacer()
            valueText
                .foregroundStyle(.black)
                .textSelection(.enabled)
                .lineLimit(1)
        }
        .padding(.horizontal, 10)
        .frame(width: OnboardingStyle.fieldWidth, height: 54)
        .background(Color.white.opacity(0.5))
        .clipShape(RoundedRectangle(cornerRadius: OnboardingStyle.cornerRadius))
    }

    private var actionButtons: some View {
        VStack(spacing: 10) {
            Button {
                showPairSheet = true
            } label: {
                Label("Pair a different wristband", systemImage: "plus.circle")
                    .font(OnboardingStyle.font(15, weight: .semibold))
                    .foregroundStyle(.white)
                    .frame(maxWidth: .infinity)
                    .padding(.vertical, 12)
            }
            .background(OnboardingStyle.figmaPrimaryBlue)
            .clipShape(RoundedRectangle(cornerRadius: OnboardingStyle.cornerRadius))

            Button(role: .destructive) {
                showUnlinkConfirm = true
            } label: {
                Label(isUnlinking ? "Unlinking…" : "Unlink this wristband",
                      systemImage: "xmark.circle")
                    .font(OnboardingStyle.font(15, weight: .semibold))
                    .foregroundStyle(.white)
                    .frame(maxWidth: .infinity)
                    .padding(.vertical, 12)
            }
            .background(Color.red.opacity(0.75))
            .clipShape(RoundedRectangle(cornerRadius: OnboardingStyle.cornerRadius))
            .disabled(isUnlinking)
        }
        .frame(width: OnboardingStyle.fieldWidth)
        .padding(.top, 6)
    }

    private var emptyCard: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("No device linked")
                .font(OnboardingStyle.font(14))
                .foregroundStyle(.black.opacity(0.6))
            Button {
                showPairSheet = true
            } label: {
                Label("Pair a wristband", systemImage: "plus.circle")
                    .font(OnboardingStyle.font(15, weight: .semibold))
                    .foregroundStyle(.white)
                    .frame(maxWidth: .infinity)
                    .padding(.vertical, 12)
            }
            .background(OnboardingStyle.figmaPrimaryBlue)
            .clipShape(RoundedRectangle(cornerRadius: OnboardingStyle.cornerRadius))
        }
        .padding(16)
        .frame(width: OnboardingStyle.fieldWidth)
        .background(Color.white.opacity(0.5))
        .clipShape(RoundedRectangle(cornerRadius: OnboardingStyle.cornerRadius))
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
