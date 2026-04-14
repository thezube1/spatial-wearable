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
        if hasCompletedOnboarding { return }

        let device = try? await APIClient.shared.getMyDevice()
        if device == nil {
            coordinator.step = .linkWristband
        }
        // Otherwise: leave coordinator.step at whatever was persisted.
    }
}

#Preview {
    ContentView()
        .environment(BLEManager())
        .environment(AuthViewModel())
        .environment(OnboardingCoordinator())
}
