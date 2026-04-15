import SwiftUI

/// Thin wrapper that delegates to `OnboardingCoordinator` and swaps screens
/// based on the current step.
struct OnboardingView: View {
    @Environment(OnboardingCoordinator.self) private var coordinator

    var body: some View {
        switch coordinator.step {
        case .welcome:        WelcomeView()
        case .linkWristband:  LinkWristbandView()
        case .selectEvent:    SelectEventView()
        case .createOrJoin:   CreateOrJoinGroupView()
        case .groupSetup:     GroupSetupView()
        case .confirmation:   ConfirmationView()
        }
    }
}

#Preview {
    OnboardingView()
        .environment(OnboardingCoordinator())
        .environment(AuthViewModel())
        .environment(BLEManager())
}
