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

    /// Deterministic index for stable UI (avatars, status dots) from a string id.
    static func stableIndex(_ string: String, modulo: Int) -> Int {
        guard modulo > 0 else { return 0 }
        var hash: UInt32 = 5381
        for b in string.utf8 {
            hash = ((hash << 5) &+ hash) &+ UInt32(b)
        }
        return Int(hash % UInt32(modulo))
    }

    /// SF Symbols pool for member avatars when no photo is available.
    static let avatarSymbolPool: [String] = [
        "person.fill", "face.smiling", "hare.fill", "leaf.fill", "flame.fill",
        "bolt.fill", "star.fill", "moon.fill", "sun.max.fill", "cloud.fill",
        "drop.fill", "snowflake", "pawprint.fill", "bird.fill", "fish.fill",
        "figure.walk", "figure.run", "heart.fill", "music.note"
    ]

    static func avatarSymbol(forUserId id: String) -> String {
        avatarSymbolPool[stableIndex(id, modulo: avatarSymbolPool.count)]
    }

    /// Group Setup status-dot colors (Figma `Ellipse 45` swatches).
    static let memberStatusPalette: [Color] = [
        Color(red: 0.79, green: 0.596, blue: 1),
        Color(red: 0.596, green: 0.798, blue: 1),
        Color(red: 1, green: 0.596, blue: 0.602),
        Color(red: 0, green: 0, blue: 1),
        Color(red: 0.596, green: 0.872, blue: 1),
        Color(red: 0.629, green: 1, blue: 0.596),
        Color(red: 0.98, green: 0.75, blue: 0.4),
        Color(red: 0.85, green: 0.5, blue: 0.95)
    ]

    static func memberStatusColor(forUserId id: String) -> Color {
        memberStatusPalette[stableIndex(id, modulo: memberStatusPalette.count)]
    }
}

// MARK: - Welcome (sign-up) screen

/// Full-screen film grain (UI ref): fine specks over **entire** hierarchy, including fields and chrome.
struct WelcomeFullScreenFilmGrain: View {
    var body: some View {
        GeometryReader { geo in
            Canvas { context, size in
                let w = max(1, size.width)
                let h = max(1, size.height)
                for i in 0..<11_000 {
                    let x = CGFloat((i * 7919 + 331) % Int(w))
                    let y = CGFloat((i * 6151 + 127) % Int(h))
                    let o = Double((i * 17) % 90) / 550.0 + 0.02
                    let light = (i % 5) == 0
                    let c = light
                        ? Color.white.opacity(o * 1.15)
                        : Color.black.opacity(o * 1.35)
                    context.fill(
                        Path(CGRect(x: x, y: y, width: 1.0, height: 1.0)),
                        with: .color(c)
                    )
                }
            }
            .frame(width: geo.size.width, height: geo.size.height)
            .blendMode(.overlay)
            .opacity(0.5)
            .drawingGroup()
        }
        .allowsHitTesting(false)
    }
}

struct WelcomeHeroBackground: View {
    var body: some View {
        GeometryReader { geo in
            let w = geo.size.width
            let h = geo.size.height
            ZStack {
                Color.white
                Group {
                    if UIImage(named: "WelcomeBackground") != nil {
                        Image("WelcomeBackground")
                            .resizable()
                            .scaledToFill()
                            .frame(width: w, height: h)
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
                        .frame(width: w, height: h)
                    }
                }
            }
        }
        .ignoresSafeArea()
    }
}

/// Blurred full-bleed photo for Link Wristband (`IMG_0449 2`); asset is pre-blurred like Figma layer blur.
struct LinkWristbandBackgroundView: View {
    var body: some View {
        GeometryReader { geo in
            let w = geo.size.width
            let h = geo.size.height
            ZStack {
                Color.white
                if UIImage(named: "LinkWristbandBackground") != nil {
                    Image("LinkWristbandBackground")
                        .resizable()
                        .scaledToFill()
                        .frame(width: w, height: h)
                        .clipped()
                } else {
                    LinearGradient(
                        colors: [
                            Color(white: 0.35),
                            Color(white: 0.55),
                            Color(white: 0.4)
                        ],
                        startPoint: .top,
                        endPoint: .bottom
                    )
                    .frame(width: w, height: h)
                    .blur(radius: 40)
                }
            }
        }
        .ignoresSafeArea()
    }
}

/// Select Event bg (`image 2.png`): grayscale, darker exposure, layer blur 10 (Figma).
struct SelectEventBackgroundView: View {
    var body: some View {
        GeometryReader { geo in
            let w = geo.size.width
            let h = geo.size.height
            ZStack {
                Color.white
                if UIImage(named: "SelectEventBackground") != nil {
                    Image("SelectEventBackground")
                        .resizable()
                        .scaledToFill()
                        .frame(width: w, height: h)
                        .clipped()
                        .saturation(0)
                        .brightness(-0.2)
                        .blur(radius: 10)
                } else {
                    Color(white: 0.28)
                }
            }
        }
        .ignoresSafeArea()
    }
}

/// Create Group bg (`image 4.png`): grayscale (Figma saturation −1), layer blur 15.
struct CreateGroupBackgroundView: View {
    var body: some View {
        GeometryReader { geo in
            let w = geo.size.width
            let h = geo.size.height
            ZStack {
                Color.white
                if UIImage(named: "CreateGroupBackground") != nil {
                    Image("CreateGroupBackground")
                        .resizable()
                        .scaledToFill()
                        .frame(width: w, height: h)
                        .clipped()
                        .saturation(0)
                        .blur(radius: 15)
                } else {
                    Color(white: 0.32)
                }
            }
        }
        .ignoresSafeArea()
    }
}

/// Group Setup bg (`bg.png`): grayscale, layer blur 10; bottom gray slab blur 75 (Figma `Rectangle 32`).
struct GroupSetupBackgroundView: View {
    var body: some View {
        GeometryReader { geo in
            let w = geo.size.width
            let h = geo.size.height
            ZStack {
                Color.white
                if UIImage(named: "GroupSetupBackground") != nil {
                    Image("GroupSetupBackground")
                        .resizable()
                        .scaledToFill()
                        .frame(width: w, height: h)
                        .clipped()
                        .saturation(0)
                        .blur(radius: 10)
                } else {
                    Color(white: 0.3)
                }
                VStack {
                    Spacer(minLength: 0)
                    Rectangle()
                        .fill(Color(red: 0.473, green: 0.473, blue: 0.473))
                        .frame(height: h * 0.58)
                        .frame(maxWidth: .infinity)
                        .blur(radius: 75)
                }
                .allowsHitTesting(false)
            }
        }
        .ignoresSafeArea()
    }
}

/// Select Meeting Point bg (`image 7.png`): layer blur 10 (Figma); optional grayscale to match ref.
struct SelectMeetingPointBackgroundView: View {
    var body: some View {
        GeometryReader { geo in
            let w = geo.size.width
            let h = geo.size.height
            ZStack {
                Color.white
                if UIImage(named: "SelectMeetingPointBackground") != nil {
                    Image("SelectMeetingPointBackground")
                        .resizable()
                        .scaledToFill()
                        .frame(width: w, height: h)
                        .clipped()
                        .saturation(0)
                        .blur(radius: 10)
                } else {
                    Color(white: 0.28)
                }
            }
        }
        .ignoresSafeArea()
    }
}

/// Confirmation bg (`image 6.png`): grayscale, layer blur 15.
struct ConfirmationBackgroundView: View {
    var body: some View {
        GeometryReader { geo in
            let w = geo.size.width
            let h = geo.size.height
            ZStack {
                Color.white
                if UIImage(named: "ConfirmationBackground") != nil {
                    Image("ConfirmationBackground")
                        .resizable()
                        .scaledToFill()
                        .frame(width: w, height: h)
                        .clipped()
                        .saturation(0)
                        .blur(radius: 15)
                } else {
                    Color(white: 0.3)
                }
            }
        }
        .ignoresSafeArea()
    }
}

/// Dashboard bg (matches Event/Group/Device refs): grayscale, stronger blur.
struct DashboardBackgroundView: View {
    var body: some View {
        GeometryReader { geo in
            let w = geo.size.width
            let h = geo.size.height
            ZStack {
                Color.white
                if UIImage(named: "ConfirmationBackground") != nil {
                    Image("ConfirmationBackground")
                        .resizable()
                        .scaledToFill()
                        .frame(width: w, height: h)
                        .clipped()
                        .saturation(0)
                        .blur(radius: 20)
                } else {
                    Color(white: 0.35)
                }
            }
        }
        .ignoresSafeArea()
    }
}

struct WelcomeSignUpFieldStyle: ViewModifier {
    func body(content: Content) -> some View {
        content
            .font(OnboardingStyle.font(18))
            .foregroundStyle(Color.black)
            .tint(OnboardingStyle.figmaPrimaryBlue)
            .padding(.horizontal, 10)
            .frame(width: OnboardingStyle.fieldWidth, height: 40, alignment: .leading)
            .background(Color(white: 0.94).opacity(0.52))
            .clipShape(RoundedRectangle(cornerRadius: OnboardingStyle.cornerRadius))
    }
}

extension View {
    func welcomeSignUpField() -> some View { modifier(WelcomeSignUpFieldStyle()) }
}

struct WelcomePrimaryButtonStyle: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(OnboardingStyle.font(18, weight: .semibold))
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
