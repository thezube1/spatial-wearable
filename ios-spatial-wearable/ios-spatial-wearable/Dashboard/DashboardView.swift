import SwiftUI

struct DashboardView: View {
    @Environment(BLEManager.self) private var ble
    @Environment(AuthViewModel.self) private var auth
    @Environment(OnboardingCoordinator.self) private var coordinator
    @AppStorage("hasCompletedOnboarding") private var hasCompletedOnboarding = false

    @State private var device: Device?
    @State private var errorMessage: String?

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(spacing: 16) {
                    eventCard
                    groupCard
                    deviceCard

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
            .navigationTitle("Dashboard")
        }
        .task {
            device = try? await APIClient.shared.getMyDevice()
        }
    }

    private var eventCard: some View {
        card(title: "Event") {
            if let e = coordinator.selectedEvent {
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

    private var deviceCard: some View {
        card(title: "Wristband") {
            VStack(alignment: .leading, spacing: 6) {
                HStack {
                    Circle().fill(ble.connectionState.color).frame(width: 8, height: 8)
                    Text(ble.connectionState.label).font(.subheadline)
                }
                if let mac = device?.mac_address ?? coordinator.linkedDeviceMAC {
                    Text(mac).font(.caption.monospaced()).foregroundStyle(.secondary)
                } else {
                    Text("No device linked").foregroundStyle(.secondary).font(.caption)
                }
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
