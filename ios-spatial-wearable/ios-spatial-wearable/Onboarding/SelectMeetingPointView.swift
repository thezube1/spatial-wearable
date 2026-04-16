import SwiftUI

struct SelectMeetingPointView: View {
    @Environment(OnboardingCoordinator.self) private var coordinator

    /// Inner map (Figma `Group 25` area inside 10 pt padding).
    private let mapInnerWidth: CGFloat = 317
    private let mapInnerHeight: CGFloat = 405.894

    var body: some View {
        @Bindable var coordinator = coordinator
        ZStack {
            SelectMeetingPointBackgroundView()
            WelcomeFullScreenFilmGrain()
                .ignoresSafeArea()

            GeometryReader { geo in
                let topPad = max(0, 135 - geo.safeAreaInsets.top)
                VStack(spacing: 0) {
                    ScrollView(showsIndicators: false) {
                        VStack(spacing: 0) {
                            Color.clear.frame(height: topPad)

                            VStack(spacing: 5) {
                                Text("Where should everyone meet?")
                                    .font(OnboardingStyle.font(24, weight: .bold))
                                    .foregroundStyle(.white)
                                    .multilineTextAlignment(.center)
                                    .frame(maxWidth: 400)
                                    .shadow(color: .black.opacity(0.2), radius: 4, x: 0, y: 1)

                                Text("Tap a location on the map.")
                                    .font(OnboardingStyle.font(18))
                                    .foregroundStyle(.white)
                                    .multilineTextAlignment(.center)
                                    .frame(maxWidth: OnboardingStyle.fieldWidth)
                            }
                            .padding(.bottom, 11)

                            meetingPointMapCard(coordinator: coordinator)
                                .padding(.bottom, 20)

                            TextField(
                                "",
                                text: $coordinator.meetingPointName,
                                prompt: Text("Name your meeting point")
                                    .foregroundColor(.black.opacity(0.45))
                            )
                            .font(OnboardingStyle.font(16))
                            .foregroundStyle(.black)
                            .padding(.horizontal, 10)
                            .frame(width: OnboardingStyle.fieldWidth, height: 40)
                            .background(Color.white.opacity(0.5))
                            .clipShape(RoundedRectangle(cornerRadius: OnboardingStyle.cornerRadius))

                            Color.clear.frame(height: 24)
                        }
                        .padding(.horizontal, 28)
                        .frame(minWidth: 0, maxWidth: .infinity)
                    }

                    Button("Continue") {
                        coordinator.step = .confirmation
                    }
                    .buttonStyle(WelcomePrimaryButtonStyle())
                    .disabled(
                        coordinator.meetingPointName.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
                    )
                    .opacity(
                        coordinator.meetingPointName.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
                            ? 0.45 : 1
                    )
                    .padding(.horizontal, 28)
                    .padding(.top, 8)
                    .padding(.bottom, max(16, geo.safeAreaInsets.bottom + 8))
                }

                VStack {
                    HStack {
                        Button {
                            coordinator.step = .groupSetup
                        } label: {
                            Image(systemName: "chevron.left")
                                .font(.system(size: 20, weight: .semibold))
                                .foregroundStyle(.white)
                                .frame(width: 28, height: 28)
                        }
                        .buttonStyle(.plain)
                        Spacer()
                    }
                    .padding(.horizontal, 28)
                    .padding(.top, geo.safeAreaInsets.top + 8)
                    Spacer()
                }
            }
        }
    }

    private func meetingPointMapCard(coordinator: OnboardingCoordinator) -> some View {
        let outerH = 20 + mapInnerHeight
        return ZStack {
            RoundedRectangle(cornerRadius: OnboardingStyle.cornerRadius)
                .fill(Color.white.opacity(0.5))
                .frame(width: OnboardingStyle.fieldWidth, height: outerH)

            GeometryReader { g in
                let w = g.size.width
                let h = g.size.height
                ZStack {
                    Group {
                        if UIImage(named: "MeetingPointMap") != nil {
                            Image("MeetingPointMap")
                                .resizable()
                                .scaledToFill()
                                .frame(width: w, height: h)
                                .clipped()
                        } else {
                            Color(white: 0.85)
                        }
                    }

                    Circle()
                        .fill(OnboardingStyle.figmaPrimaryBlue)
                        .frame(width: 17, height: 17)
                        .overlay(
                            Circle()
                                .stroke(Color.white.opacity(0.5), lineWidth: 2)
                        )
                        .position(
                            x: coordinator.meetingPointX * w,
                            y: coordinator.meetingPointY * h
                        )
                }
                .frame(width: w, height: h)
                .contentShape(Rectangle())
                .gesture(
                    DragGesture(minimumDistance: 0)
                        .onEnded { value in
                            let nx = min(1, max(0, value.location.x / w))
                            let ny = min(1, max(0, value.location.y / h))
                            coordinator.meetingPointX = nx
                            coordinator.meetingPointY = ny
                        }
                )
            }
            .frame(width: mapInnerWidth, height: mapInnerHeight)
        }
        .frame(width: OnboardingStyle.fieldWidth, height: outerH)
    }
}

#Preview {
    SelectMeetingPointView()
        .environment(OnboardingCoordinator())
}
