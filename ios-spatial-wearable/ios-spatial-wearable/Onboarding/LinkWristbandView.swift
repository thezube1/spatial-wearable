import SwiftUI

struct LinkWristbandView: View {
    @Environment(OnboardingCoordinator.self) private var coordinator
    @Environment(BLEManager.self) private var ble

    @State private var statusText: String = "Searching…"
    @State private var isLinking = false
    @State private var errorMessage: String?
    @State private var dotPosition: CGFloat = 0

    var body: some View {
        ZStack {
            OnboardingBackground()

            VStack(spacing: 28) {
                Spacer()

                Text("Let's link your wristband.")
                    .font(OnboardingStyle.font(24, weight: .bold))
                    .foregroundStyle(.white)
                    .multilineTextAlignment(.center)

                // Moving-dot progress pill
                GeometryReader { geo in
                    ZStack(alignment: .leading) {
                        Capsule()
                            .fill(Color.white.opacity(0.15))
                            .frame(height: 10)
                        Circle()
                            .fill(Color.blue)
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

                VStack(alignment: .leading, spacing: 14) {
                    Text("1. Hold the BOOT button on the wristband for 5 seconds.")
                    Text("2. Release when the screen shows PAIRING MODE.")
                }
                .font(OnboardingStyle.font(15))
                .foregroundStyle(.white)
                .frame(width: OnboardingStyle.fieldWidth, alignment: .leading)

                HStack(spacing: 10) {
                    Circle().fill(Color.blue).frame(width: 8, height: 8)
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

                Spacer()

                Button("Skip for now") {
                    coordinator.step = .selectEvent
                }
                .font(OnboardingStyle.font(13))
                .foregroundStyle(.white.opacity(0.7))
                .padding(.bottom, 24)
            }
            .padding()
        }
        .task {
            await beginPairing()
        }
    }

    private func beginPairing() async {
        guard !isLinking else { return }
        isLinking = true
        defer { isLinking = false }

        statusText = "Searching…"
        ble.startScanning()

        // Wait until a device is discovered + auto-connect to strongest.
        for _ in 0..<30 {
            if let best = ble.discoveredDevices.max(by: { $0.rssi < $1.rssi }) {
                ble.connect(to: best)
                break
            }
            try? await Task.sleep(for: .milliseconds(500))
        }

        // Wait for connection.
        for _ in 0..<30 {
            if ble.connectionState == .connected { break }
            try? await Task.sleep(for: .milliseconds(500))
        }

        guard ble.connectionState == .connected else {
            errorMessage = "Couldn't find a wristband nearby."
            statusText = "Not found"
            return
        }

        statusText = "Reading device…"
        do {
            let mac = try await ble.readMAC()
            _ = try await APIClient.shared.linkDevice(mac: mac)

            // Commit the owner token on the ESP so it only talks to this user.
            guard let uid = await SupabaseService.shared.currentUserId() else {
                throw NSError(domain: "Pairing", code: 1,
                    userInfo: [NSLocalizedDescriptionKey: "No Supabase session"])
            }
            statusText = "Saving owner…"
            try await ble.writeOwner(userId: uid)

            coordinator.linkedDeviceMAC = mac
            statusText = "Linked"
            try? await Task.sleep(for: .milliseconds(400))
            coordinator.step = .selectEvent
        } catch {
            errorMessage = error.localizedDescription
            statusText = "Failed to link"
        }
    }
}
