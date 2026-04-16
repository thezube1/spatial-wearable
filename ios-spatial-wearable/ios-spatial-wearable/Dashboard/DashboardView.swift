import SwiftUI

struct DashboardView: View {
    @Environment(BLEManager.self) private var ble
    @Environment(OnboardingCoordinator.self) private var coordinator

    @State private var device: Device?
    @State private var errorMessage: String?
    @State private var selectedTab: DashboardTab = .event

    private enum DashboardTab: String, CaseIterable {
        case event = "Event"
        case group = "Group"
        case device = "Device"
    }

    var body: some View {
        ZStack {
            DashboardBackgroundView()
            WelcomeFullScreenFilmGrain()
                .ignoresSafeArea()

            VStack(spacing: 0) {
                topLogo
                    .padding(.top, 18)
                    .padding(.bottom, 8)

                Group {
                    switch selectedTab {
                    case .event:
                        EventTabView(group: coordinator.createdGroup, meetingPointName: coordinator.meetingPointName)
                    case .group:
                        GroupTabView(group: coordinator.createdGroup)
                    case .device:
                        DeviceTabView(device: $device, errorMessage: $errorMessage)
                    }
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)

                tabStrip
            }
        }
        .ignoresSafeArea(edges: .bottom)
        .task {
            device = try? await APIClient.shared.getMyDevice()
            if let g = try? await APIClient.shared.getMyGroup() {
                coordinator.createdGroup = g
            }
            await reconnectLoop()
        }
    }

    /// Runs for the lifetime of the dashboard. Whenever we're not connected
    /// and the backend says this user owns a wristband, scan for it and
    /// re-auth with the Supabase user_id. Cancellation (view disappears,
    /// sign-out) naturally ends the loop via Task cancellation.
    private func reconnectLoop() async {
        while !Task.isCancelled {
            if device == nil {
                device = try? await APIClient.shared.getMyDevice()
            }
            let idle: Bool = {
                switch ble.connectionState {
                case .disconnected, .failed: return true
                default: return false
                }
            }()
            if idle,
               let mac = device?.mac_address ?? coordinator.linkedDeviceMAC,
               let uid = await SupabaseService.shared.currentUserId() {
                do {
                    try await ble.reconnect(toMAC: mac)
                    try await ble.authenticate(userId: uid)
                } catch {
                    // Swallow and retry — transient BLE failures are expected.
                }
            }
            try? await Task.sleep(for: .seconds(5))
        }
    }

    private var topLogo: some View {
        ZStack {
            Circle()
                .fill(Color.white.opacity(0.5))
                .frame(width: 34, height: 34)
            Image("WelcomeLogo")
                .resizable()
                .scaledToFit()
                .frame(width: 18, height: 18)
                .foregroundStyle(OnboardingStyle.figmaPrimaryBlue)
        }
    }

    private var tabStrip: some View {
        HStack {
            ForEach(DashboardTab.allCases, id: \.rawValue) { tab in
                Button {
                    selectedTab = tab
                } label: {
                    Text(tab.rawValue)
                        .font(OnboardingStyle.font(18))
                        .foregroundStyle(selectedTab == tab ? OnboardingStyle.figmaPrimaryBlue : Color.black.opacity(0.35))
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(.plain)
            }
        }
        .frame(height: 48)
        .padding(.bottom, 4)
        .background(Color.white.opacity(0.75))
    }
}

#Preview {
    DashboardView()
        .environment(BLEManager())
        .environment(AuthViewModel())
        .environment(OnboardingCoordinator())
}
