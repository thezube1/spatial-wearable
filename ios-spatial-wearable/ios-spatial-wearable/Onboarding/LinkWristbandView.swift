import SwiftUI

struct LinkWristbandView: View {
    @Environment(OnboardingCoordinator.self) private var coordinator
    @Environment(BLEManager.self) private var ble

    /// If provided, called after a successful link instead of advancing onboarding.
    /// Used when the view is presented as a "pair new device" sheet from the Device tab.
    var onLinked: ((String) -> Void)? = nil

    @State private var statusText: String = "Searching…"
    @State private var isLinking = false
    @State private var errorMessage: String?
    @State private var dotPosition: CGFloat = 0
    @State private var selectedDeviceID: UUID?

    private static let devicePrefix = "SW-"

    private var sortedDevices: [DiscoveredDevice] {
        ble.discoveredDevices.sorted { lhs, rhs in
            let lIsWearable = lhs.name.hasPrefix(Self.devicePrefix)
            let rIsWearable = rhs.name.hasPrefix(Self.devicePrefix)
            if lIsWearable != rIsWearable { return lIsWearable }
            return lhs.rssi > rhs.rssi
        }
    }

    var body: some View {
        ZStack {
            LinkWristbandBackgroundView()

            ScrollView(showsIndicators: false) {
                VStack(spacing: 20) {
                    Spacer(minLength: 8)

                    Text("Let's link your wristband.")
                        .font(OnboardingStyle.font(24, weight: .bold))
                        .foregroundStyle(.white)
                        .multilineTextAlignment(.center)
                        .frame(maxWidth: 400)
                        .shadow(color: .black.opacity(0.25), radius: 4, x: 0, y: 1)

                    linkWristbandGraphic

                    GeometryReader { geo in
                        ZStack(alignment: .leading) {
                            Capsule()
                                .fill(Color.white.opacity(0.15))
                                .frame(height: 10)
                            Circle()
                                .fill(OnboardingStyle.figmaPrimaryBlue)
                                .frame(width: 14, height: 14)
                                .offset(x: dotPosition * (geo.size.width - 14))
                        }
                    }
                    .frame(width: OnboardingStyle.fieldWidth, height: 14)
                    .onAppear {
                        withAnimation(.easeInOut(duration: 1.6).repeatForever(autoreverses: true)) {
                            dotPosition = 1
                        }
                    }

                    VStack(alignment: .leading, spacing: 10) {
                        Text("1. Hold the BOOT button on the wristband for 5 seconds.")
                        Text("2. Release when the screen shows PAIRING MODE.")
                        Text("3. Pick your wristband from the list below.")
                    }
                    .font(OnboardingStyle.font(14))
                    .foregroundStyle(.white)
                    .frame(width: OnboardingStyle.fieldWidth, alignment: .leading)

                    deviceList

                    HStack(spacing: 10) {
                        Circle()
                            .fill(OnboardingStyle.figmaPrimaryBlue)
                            .frame(width: 8, height: 8)
                        Text(statusText)
                            .font(OnboardingStyle.font(14))
                            .foregroundStyle(.white.opacity(0.9))
                    }

                    if let errorMessage {
                        Text(errorMessage)
                            .font(OnboardingStyle.font(12))
                            .foregroundStyle(.red.opacity(0.9))
                            .frame(width: OnboardingStyle.fieldWidth, alignment: .leading)
                    }

                    Button {
                        Task { await confirmSelection() }
                    } label: {
                        if isLinking {
                            ProgressView().tint(.white)
                        } else {
                            Text("Confirm device")
                        }
                    }
                    .buttonStyle(WelcomePrimaryButtonStyle())
                    .disabled(!canConfirm)

                    Button("Skip for now") {
                        coordinator.step = .selectEvent
                    }
                    .font(OnboardingStyle.font(13))
                    .foregroundStyle(.white.opacity(0.85))
                    .underline()
                    .padding(.bottom, 24)
                }
                .padding()
            }

            WelcomeFullScreenFilmGrain()
                .ignoresSafeArea()
        }
        .task {
            ble.startScanning()
            statusText = "Searching…"
        }
    }

    private var canConfirm: Bool {
        selectedDeviceID != nil && !isLinking
    }

    /// Raster mockup (`WristbandMockup`) — full-bleed to match the hero illustration in Figma.
    private var linkWristbandGraphic: some View {
        Image("WristbandMockup")
            .resizable()
            .interpolation(.high)
            .scaledToFit()
            .frame(maxWidth: .infinity)
            .padding(.horizontal, -16)
    }

    @ViewBuilder
    private var deviceList: some View {
        let devices = sortedDevices
        ScrollView {
            VStack(spacing: 8) {
                if devices.isEmpty {
                    Text("Scanning for wristbands…")
                        .font(OnboardingStyle.font(13))
                        .foregroundStyle(.white.opacity(0.6))
                        .padding(.vertical, 16)
                } else {
                    ForEach(devices) { device in
                        deviceRow(device)
                    }
                }
            }
            .padding(.vertical, 4)
        }
        .frame(width: OnboardingStyle.fieldWidth)
        .frame(maxHeight: 220)
    }

    @ViewBuilder
    private func deviceRow(_ device: DiscoveredDevice) -> some View {
        let isWearable = device.name.hasPrefix(Self.devicePrefix)
        let isSelected = selectedDeviceID == device.id
        Button {
            selectedDeviceID = device.id
        } label: {
            HStack(spacing: 12) {
                Image(systemName: isWearable ? "applewatch.radiowaves.left.and.right" : "dot.radiowaves.left.and.right")
                    .foregroundStyle(isWearable ? OnboardingStyle.figmaPrimaryBlue : .white.opacity(0.5))
                VStack(alignment: .leading, spacing: 2) {
                    Text(device.name)
                        .font(OnboardingStyle.font(14, weight: isWearable ? .semibold : .regular))
                        .foregroundStyle(.white)
                    Text("\(device.rssi) dBm\(isWearable ? " · Wristband" : "")")
                        .font(OnboardingStyle.font(11))
                        .foregroundStyle(.white.opacity(0.6))
                }
                Spacer()
                if isSelected {
                    Image(systemName: "checkmark.circle.fill")
                        .foregroundStyle(OnboardingStyle.figmaPrimaryBlue)
                }
            }
            .padding(.horizontal, 14)
            .padding(.vertical, 10)
            .background(
                RoundedRectangle(cornerRadius: 12)
                    .fill(isSelected ? OnboardingStyle.figmaPrimaryBlue.opacity(0.18) : Color.white.opacity(0.08))
            )
            .overlay(
                RoundedRectangle(cornerRadius: 12)
                    .stroke(isWearable ? OnboardingStyle.figmaPrimaryBlue.opacity(isSelected ? 0.9 : 0.45) : Color.clear,
                            lineWidth: 1.5)
            )
        }
        .buttonStyle(.plain)
    }

    private func confirmSelection() async {
        guard !isLinking,
              let id = selectedDeviceID,
              let device = ble.discoveredDevices.first(where: { $0.id == id }) else { return }
        isLinking = true
        errorMessage = nil
        defer { isLinking = false }

        statusText = "Connecting…"
        ble.connect(to: device)

        for _ in 0..<30 {
            if ble.connectionState == .connected { break }
            try? await Task.sleep(for: .milliseconds(500))
        }
        guard ble.connectionState == .connected else {
            errorMessage = "Couldn't connect to that device."
            statusText = "Not connected"
            return
        }

        statusText = "Reading device…"
        do {
            let mac = try await ble.readMAC()
            _ = try await APIClient.shared.linkDevice(mac: mac)

            guard let uid = await SupabaseService.shared.currentUserId() else {
                throw NSError(domain: "Pairing", code: 1,
                    userInfo: [NSLocalizedDescriptionKey: "No Supabase session"])
            }
            statusText = "Saving owner…"
            try await ble.writeOwner(userId: uid)

            coordinator.linkedDeviceMAC = mac
            statusText = "Linked"
            try? await Task.sleep(for: .milliseconds(400))
            if let onLinked {
                onLinked(mac)
            } else {
                coordinator.step = .selectEvent
            }
        } catch {
            errorMessage = error.localizedDescription
            statusText = "Failed to link"
        }
    }
}

#Preview {
    LinkWristbandView()
        .environment(OnboardingCoordinator())
        .environment(BLEManager())
}
