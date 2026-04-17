import SwiftUI

struct DashboardView: View {
    @Environment(BLEManager.self) private var ble
    @Environment(AuthViewModel.self) private var auth
    @Environment(OnboardingCoordinator.self) private var coordinator
    @AppStorage("hasCompletedOnboarding") private var hasCompletedOnboarding = false

    @State private var device: Device?
    @State private var errorMessage: String?

    var body: some View {
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
        NavigationStack {
            ScrollView {
                VStack(spacing: 16) {
                    Text("Dashboard")
                        .font(.largeTitle.bold())
                        .frame(maxWidth: .infinity, alignment: .leading)
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
                            .frame(maxWidth: .infinity)
                            .padding(.vertical, 12)
                    }
                    .buttonStyle(.bordered)
                    .padding(.top, 12)
                }
                .padding(20)
            }
            .background(Color(.systemGroupedBackground))
            .toolbar(.hidden, for: .navigationBar)
        }
    }

    private var eventCard: some View {
        card(title: "Event") {
            if let e = coordinator.selectedEvent ?? coordinator.createdGroup?.event {
                VStack(alignment: .leading, spacing: 4) {
                    Text(e.name).font(.headline)
                    if let v = e.venue { Text(v).foregroundStyle(.secondary).font(.subheadline) }
                    if let c = e.category { Text(c).foregroundStyle(.secondary).font(.caption) }
                }
            } else {
                Text("No event selected").foregroundStyle(.secondary)
            }
        }
    }

    private var groupCard: some View {
        card(title: "Group") {
            if let g = coordinator.createdGroup {
                HStack(spacing: 12) {
                    ZStack {
                        ForEach(Array(g.members.prefix(4).enumerated()), id: \.offset) { idx, _ in
                            Circle()
                                .fill(Color.blue.opacity(0.3))
                                .frame(width: 30, height: 30)
                                .overlay(Circle().stroke(Color(.systemBackground), lineWidth: 2))
                                .offset(x: CGFloat(idx) * 16)
                        }
                    }
                    .frame(height: 30)
                    VStack(alignment: .leading) {
                        Text(g.name).font(.headline)
                        Text("\(g.members.count) members").foregroundStyle(.secondary).font(.caption)
                    }
                    Spacer()
                }
            } else {
                Text("No group").foregroundStyle(.secondary)
            }
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
    DashboardView()
        .environment(BLEManager())
        .environment(AuthViewModel())
        .environment(OnboardingCoordinator())
}
