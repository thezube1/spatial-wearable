import SwiftUI
import UIKit

struct DashboardView: View {
    @Environment(BLEManager.self) private var ble
    @Environment(AuthViewModel.self) private var auth
    @Environment(OnboardingCoordinator.self) private var coordinator
    @AppStorage("hasCompletedOnboarding") private var hasCompletedOnboarding = false

    @State private var device: Device?
    @State private var errorMessage: String?

    init() {
        // Translucent tab bar so the aesthetic background shows through.
        let appearance = UITabBarAppearance()
        appearance.configureWithTransparentBackground()
        appearance.backgroundColor = UIColor.white.withAlphaComponent(0.45)
        UITabBar.appearance().standardAppearance = appearance
        UITabBar.appearance().scrollEdgeAppearance = appearance
    }

    var body: some View {
        ZStack {
            DashboardBackgroundView()

            TabView {
                homeTab
                    .tabItem { Label("Home", systemImage: "house.fill") }

                GroupTabView()
                    .tabItem { Label("Group", systemImage: "person.3.fill") }

                LocationTabView()
                    .tabItem { Label("Map", systemImage: "map.fill") }

                DeviceTabView(device: $device, errorMessage: $errorMessage)
                    .tabItem { Label("Device", systemImage: "applewatch") }
            }
            .tint(OnboardingStyle.figmaPrimaryBlue)

            WelcomeFullScreenFilmGrain()
                .ignoresSafeArea()
                .allowsHitTesting(false)
        }
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

    private var homeTab: some View {
        ZStack {
            Color.clear

            ScrollView {
                VStack(spacing: 16) {
                    Text("Dashboard")
                        .font(OnboardingStyle.font(28, weight: .bold))
                        .foregroundStyle(.white)
                        .shadow(color: .black.opacity(0.25), radius: 4, x: 0, y: 1)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .padding(.top, 8)

                    eventCard
                    groupCard

                    Button(role: .destructive) {
                        Task {
                            await auth.signOut()
                            hasCompletedOnboarding = false
                            coordinator.reset()
                        }
                    } label: {
                        Text("Sign Out")
                            .font(OnboardingStyle.font(16, weight: .semibold))
                            .foregroundStyle(.white)
                            .frame(maxWidth: .infinity)
                            .padding(.vertical, 12)
                    }
                    .background(Color.red.opacity(0.75))
                    .clipShape(RoundedRectangle(cornerRadius: OnboardingStyle.cornerRadius))
                    .padding(.top, 12)
                }
                .padding(20)
            }
        }
    }

    private var eventCard: some View {
        card(title: "Event") {
            if let e = coordinator.selectedEvent ?? coordinator.createdGroup?.event {
                VStack(alignment: .leading, spacing: 4) {
                    Text(e.name)
                        .font(OnboardingStyle.font(16, weight: .semibold))
                        .foregroundStyle(.black)
                    if let v = e.venue {
                        Text(v)
                            .font(OnboardingStyle.font(14))
                            .foregroundStyle(.black.opacity(0.7))
                    }
                    if let c = e.category {
                        Text(c)
                            .font(OnboardingStyle.font(12))
                            .foregroundStyle(.black.opacity(0.6))
                    }
                }
            } else {
                Text("No event selected")
                    .font(OnboardingStyle.font(14))
                    .foregroundStyle(.black.opacity(0.6))
            }
        }
    }

    private var groupCard: some View {
        card(title: "Group") {
            if let g = coordinator.createdGroup {
                HStack(spacing: 12) {
                    HStack(spacing: -12) {
                        ForEach(Array(g.members.prefix(4).enumerated()), id: \.offset) { _, m in
                            ZStack {
                                Circle()
                                    .fill(Color(white: 0.85))
                                    .frame(width: 34, height: 34)
                                Image(systemName: OnboardingStyle.avatarSymbol(forUserId: m.user_id))
                                    .font(.system(size: 14))
                                    .foregroundStyle(.black.opacity(0.45))
                                Circle()
                                    .stroke(Color.white.opacity(0.8), lineWidth: 2)
                                    .frame(width: 34, height: 34)
                            }
                        }
                    }
                    VStack(alignment: .leading) {
                        Text(g.name)
                            .font(OnboardingStyle.font(16, weight: .semibold))
                            .foregroundStyle(.black)
                        Text("\(g.members.count) members")
                            .font(OnboardingStyle.font(12))
                            .foregroundStyle(.black.opacity(0.6))
                    }
                    Spacer()
                }
            } else {
                Text("No group")
                    .font(OnboardingStyle.font(14))
                    .foregroundStyle(.black.opacity(0.6))
            }
        }
    }

    private func card<Content: View>(title: String, @ViewBuilder content: () -> Content) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(title)
                .font(OnboardingStyle.font(12, weight: .semibold))
                .foregroundStyle(.black.opacity(0.55))
                .textCase(.uppercase)
            content()
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(16)
        .background(Color.white.opacity(0.5))
        .clipShape(RoundedRectangle(cornerRadius: OnboardingStyle.cornerRadius))
    }
}

#Preview {
    DashboardView()
        .environment(BLEManager())
        .environment(AuthViewModel())
        .environment(OnboardingCoordinator())
}
