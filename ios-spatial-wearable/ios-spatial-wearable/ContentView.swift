import SwiftUI
import Auth

struct ContentView: View {
    @Environment(BLEManager.self) private var bleManager
    @Environment(AuthViewModel.self) private var auth
    @Environment(OnboardingCoordinator.self) private var coordinator
    @AppStorage("hasCompletedOnboarding") private var hasCompletedOnboarding = false

    @State private var hasLinkedDevice: Bool? = nil

    var body: some View {
        Group {
            if auth.currentSession == nil {
                WelcomeView()
            } else if hasCompletedOnboarding {
                DashboardView()
            } else {
                OnboardingView()
            }
        }
        .task(id: auth.currentSession?.accessToken) {
            await resolveEntryStep()
        }
    }

    /// After login, figure out whether we should jump to .linkWristband (no device)
    /// or resume the persisted onboarding step.
    private func resolveEntryStep() async {
        guard auth.currentSession != nil else { return }

        let device = try? await APIClient.shared.getMyDevice()
        if device != nil {
            // Server says this user already has a linked device — skip onboarding,
            // even if local AppStorage was cleared (fresh install, account switch, etc.).
            hasCompletedOnboarding = true
            return
        }

        // No linked device on the server — force onboarding regardless of the
        // locally persisted flag (handles account switches where the previous
        // user had completed onboarding).
        hasCompletedOnboarding = false
        coordinator.step = .linkWristband
    }
}

#Preview {
    ContentView()
        .environment(BLEManager())
        .environment(AuthViewModel())
        .environment(OnboardingCoordinator())
}
