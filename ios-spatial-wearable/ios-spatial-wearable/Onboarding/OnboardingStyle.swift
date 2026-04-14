import SwiftUI

/// Shared styling primitives for onboarding screens.
enum OnboardingStyle {
    static let fieldWidth: CGFloat = 337
    static let cornerRadius: CGFloat = 5

    /// Tries `Creato Display`, falls back to system.
    static func font(_ size: CGFloat, weight: Font.Weight = .regular) -> Font {
        .custom("Creato Display", size: size).weight(weight)
    }
}

struct OnboardingBackground: View {
    var body: some View {
        ZStack {
            // Placeholder: darkened, blurred image stand-in.
            LinearGradient(
                colors: [Color.black.opacity(0.85), Color.blue.opacity(0.55), Color.black.opacity(0.9)],
                startPoint: .topLeading, endPoint: .bottomTrailing
            )
            Color.black.opacity(0.35)
        }
        .ignoresSafeArea()
    }
}

struct OnboardingFieldStyle: ViewModifier {
    func body(content: Content) -> some View {
        content
            .padding(.horizontal, 14)
            .padding(.vertical, 12)
            .frame(width: OnboardingStyle.fieldWidth)
            .background(Color.white.opacity(0.12))
            .overlay(
                RoundedRectangle(cornerRadius: OnboardingStyle.cornerRadius)
                    .stroke(Color.white.opacity(0.35), lineWidth: 1)
            )
            .clipShape(RoundedRectangle(cornerRadius: OnboardingStyle.cornerRadius))
            .foregroundStyle(.white)
            .tint(.white)
    }
}

extension View {
    func onboardingField() -> some View { modifier(OnboardingFieldStyle()) }
}

struct PrimaryBlueButtonStyle: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(OnboardingStyle.font(16, weight: .semibold))
            .foregroundStyle(.white)
            .frame(width: OnboardingStyle.fieldWidth)
            .padding(.vertical, 14)
            .background(Color.blue)
            .clipShape(RoundedRectangle(cornerRadius: OnboardingStyle.cornerRadius))
            .opacity(configuration.isPressed ? 0.85 : 1.0)
    }
}
