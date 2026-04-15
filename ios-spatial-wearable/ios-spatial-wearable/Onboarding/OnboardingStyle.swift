import SwiftUI
import UIKit

/// Shared styling primitives for onboarding screens.
enum OnboardingStyle {
    static let fieldWidth: CGFloat = 337
    static let cornerRadius: CGFloat = 5

    /// Tries `Creato Display`, falls back to system.
    static func font(_ size: CGFloat, weight: Font.Weight = .regular) -> Font {
        .custom("Creato Display", size: size).weight(weight)
    }

    /// Pure blue from Figma (`#0000FF`).
    static let figmaPrimaryBlue = Color(red: 0, green: 0, blue: 1)
}

// MARK: - Welcome (sign-up) screen — matches Figma "Welcome" frame

struct WelcomeHeroBackground: View {
    var body: some View {
        GeometryReader { geo in
            ZStack {
                if UIImage(named: "WelcomeBackground") != nil {
                    Image("WelcomeBackground")
                        .resizable()
                        .scaledToFill()
                        .frame(width: geo.size.width, height: geo.size.height)
                        .clipped()
                } else {
                    LinearGradient(
                        colors: [
                            Color(red: 0.52, green: 0.62, blue: 0.82),
                            Color(red: 0.28, green: 0.38, blue: 0.58),
                            Color(red: 0.12, green: 0.16, blue: 0.28)
                        ],
                        startPoint: .topLeading,
                        endPoint: .bottomTrailing
                    )
                }
                Rectangle()
                    .fill(Color.white.opacity(0.06))
                    .blendMode(.overlay)
            }
        }
        .ignoresSafeArea()
    }
}

struct WelcomeSignUpFieldStyle: ViewModifier {
    func body(content: Content) -> some View {
        content
            .font(OnboardingStyle.font(15))
            .foregroundStyle(Color.black.opacity(0.88))
            .tint(OnboardingStyle.figmaPrimaryBlue)
            .padding(.horizontal, 10)
            .frame(width: OnboardingStyle.fieldWidth, height: 40, alignment: .leading)
            .background(Color.white.opacity(0.5))
            .clipShape(RoundedRectangle(cornerRadius: OnboardingStyle.cornerRadius))
    }
}

extension View {
    func welcomeSignUpField() -> some View { modifier(WelcomeSignUpFieldStyle()) }
}

struct WelcomePrimaryButtonStyle: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(OnboardingStyle.font(16, weight: .semibold))
            .foregroundStyle(.white)
            .frame(width: OnboardingStyle.fieldWidth, height: 40)
            .background(OnboardingStyle.figmaPrimaryBlue)
            .clipShape(RoundedRectangle(cornerRadius: OnboardingStyle.cornerRadius))
            .opacity(configuration.isPressed ? 0.85 : 1.0)
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
