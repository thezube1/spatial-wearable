import SwiftUI

struct ConfirmationView: View {
    @Environment(OnboardingCoordinator.self) private var coordinator
    @AppStorage("hasCompletedOnboarding") private var hasCompletedOnboarding = false

    var body: some View {
        ZStack {
            OnboardingBackground()

            VStack(spacing: 18) {
                Spacer(minLength: 20)

                Text("Great! You're all set up.")
                    .font(OnboardingStyle.font(24, weight: .bold))
                    .foregroundStyle(.white)

                if let event = coordinator.selectedEvent {
                    EventCard(event: event, isSelected: true)
                        .frame(width: OnboardingStyle.fieldWidth)
                }

                if let group = coordinator.createdGroup {
                    groupCard(group)
                }

                HStack(spacing: 8) {
                    Image(systemName: "mappin.circle.fill").foregroundStyle(.white)
                    Text("Meeting at: Ferris Wheel")
                        .font(OnboardingStyle.font(13, weight: .semibold))
                        .foregroundStyle(.white)
                }
                .padding(.horizontal, 14)
                .padding(.vertical, 8)
                .background(Color.white.opacity(0.15))
                .clipShape(Capsule())

                Text("Make sure your wristband is charged… and have fun!")
                    .font(OnboardingStyle.font(12))
                    .foregroundStyle(.white.opacity(0.75))
                    .multilineTextAlignment(.center)
                    .frame(width: OnboardingStyle.fieldWidth)

                Spacer()

                Button("Finish setup") {
                    hasCompletedOnboarding = true
                }
                .buttonStyle(PrimaryBlueButtonStyle())
                .padding(.bottom, 24)
            }
        }
    }

    private func groupCard(_ group: GroupDetail) -> some View {
        HStack(spacing: 12) {
            ZStack {
                ForEach(Array(group.members.prefix(4).enumerated()), id: \.offset) { idx, _ in
                    Circle()
                        .fill(Color.white.opacity(0.25))
                        .frame(width: 34, height: 34)
                        .overlay(Circle().stroke(Color.black.opacity(0.3), lineWidth: 2))
                        .offset(x: CGFloat(idx) * 18)
                }
            }
            .frame(width: 34 + CGFloat(max(0, min(3, group.members.count - 1))) * 18, height: 34)

            VStack(alignment: .leading, spacing: 2) {
                Text(group.name)
                    .font(OnboardingStyle.font(15, weight: .bold))
                    .foregroundStyle(.white)
                Text("\(group.members.count) members")
                    .font(OnboardingStyle.font(12))
                    .foregroundStyle(.white.opacity(0.6))
            }
            Spacer()
        }
        .padding(12)
        .frame(width: OnboardingStyle.fieldWidth)
        .background(Color.white.opacity(0.1))
        .clipShape(RoundedRectangle(cornerRadius: OnboardingStyle.cornerRadius))
    }
}
