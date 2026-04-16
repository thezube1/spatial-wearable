import SwiftUI

private let confirmationApiDate: DateFormatter = {
    let d = DateFormatter()
    d.locale = Locale(identifier: "en_US_POSIX")
    d.dateFormat = "yyyy-MM-dd"
    return d
}()

private let confirmationShortDate: DateFormatter = {
    let d = DateFormatter()
    d.dateFormat = "M/d"
    return d
}()

private enum ConfirmationLocalThumbnail {
    static func assetName(for eventName: String) -> String? {
        let n = eventName.lowercased()
        if n.contains("rolling loud") { return "EventRollingLoud" }
        if n.contains("summer smash") { return "EventSummerSmash" }
        if n.contains("hard summer") { return "EventHardSummer" }
        if n.contains("camp flog") || n.contains("flog gnaw") { return "EventCampFlogGnaw" }
        return nil
    }
}

private extension Event {
    var confirmationVenueLine: String {
        switch (venue.flatMap { $0.isEmpty ? nil : $0 }, city.flatMap { $0.isEmpty ? nil : $0 }) {
        case let (v?, c?): return "\(v), \(c)"
        case let (v?, nil): return v
        case let (nil, c?): return c
        default: return ""
        }
    }

    func confirmationDateRangeLabel() -> String {
        guard let s = starts_on.flatMap({ confirmationApiDate.date(from: $0) }),
              let e = ends_on.flatMap({ confirmationApiDate.date(from: $0) }) else { return "" }
        return "\(confirmationShortDate.string(from: s)) - \(confirmationShortDate.string(from: e))"
    }
}

struct ConfirmationView: View {
    @Environment(OnboardingCoordinator.self) private var coordinator
    @AppStorage("hasCompletedOnboarding") private var hasCompletedOnboarding = false

    var body: some View {
        ZStack {
            ConfirmationBackgroundView()
            WelcomeFullScreenFilmGrain()
                .ignoresSafeArea()

            GeometryReader { geo in
                let topPad = max(0, 135 - geo.safeAreaInsets.top)
                VStack(spacing: 0) {
                    ScrollView(showsIndicators: false) {
                        VStack(spacing: 0) {
                            Color.clear.frame(height: topPad)

                            Text("Great! You're all set up.")
                                .font(OnboardingStyle.font(24, weight: .bold))
                                .foregroundStyle(.white)
                                .multilineTextAlignment(.center)
                                .frame(maxWidth: 400)
                                .shadow(color: .black.opacity(0.2), radius: 4, x: 0, y: 1)
                                .padding(.bottom, 23)

                            if let event = coordinator.selectedEvent {
                                confirmationEventCard(event)
                                    .padding(.bottom, 20)
                            }

                            if let group = coordinator.createdGroup {
                                confirmationGroupCard(group)
                                    .padding(.bottom, 20)
                            }

                            meetingPointStrip
                                .padding(.bottom, 20)

                            Text("Make sure your wristband is charged... and\nhave fun!")
                                .font(OnboardingStyle.font(40 / 3))
                                .foregroundStyle(.white)
                                .multilineTextAlignment(.center)
                                .frame(width: OnboardingStyle.fieldWidth)

                            Color.clear.frame(height: 24)
                        }
                        .padding(.horizontal, 28)
                        .frame(minWidth: 0, maxWidth: .infinity)
                    }

                    Button("Finish setup") {
                        hasCompletedOnboarding = true
                    }
                    .buttonStyle(WelcomePrimaryButtonStyle())
                    .padding(.horizontal, 28)
                    .padding(.top, 8)
                    .padding(.bottom, max(16, geo.safeAreaInsets.bottom + 8))
                }

                VStack {
                    HStack {
                        Button {
                            coordinator.step = .selectMeetingPoint
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

    private func confirmationEventCard(_ event: Event) -> some View {
        HStack(alignment: .top, spacing: 10) {
            eventThumbnail(event)
                .frame(width: 100, height: 100)
                .clipShape(RoundedRectangle(cornerRadius: 5))

            VStack(alignment: .leading, spacing: 5) {
                Text(event.name)
                    .font(OnboardingStyle.font(16, weight: .semibold))
                    .foregroundStyle(.black)
                Text(event.confirmationVenueLine)
                    .font(OnboardingStyle.font(16))
                    .foregroundStyle(.black.opacity(0.68))
                    .fixedSize(horizontal: false, vertical: true)
                if let cat = event.category, !cat.isEmpty {
                    Text(cat)
                        .font(OnboardingStyle.font(16))
                        .foregroundStyle(.black.opacity(0.68))
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)

            Text(event.confirmationDateRangeLabel())
                .font(OnboardingStyle.font(14))
                .foregroundStyle(.black.opacity(0.7))
                .multilineTextAlignment(.trailing)
        }
        .padding(10)
        .frame(width: OnboardingStyle.fieldWidth, alignment: .topLeading)
        .frame(minHeight: 122, alignment: .topLeading)
        .background(Color.white.opacity(0.5))
        .clipShape(RoundedRectangle(cornerRadius: OnboardingStyle.cornerRadius))
    }

    @ViewBuilder
    private func eventThumbnail(_ event: Event) -> some View {
        if let asset = ConfirmationLocalThumbnail.assetName(for: event.name),
           UIImage(named: asset) != nil {
            Image(asset)
                .resizable()
                .scaledToFill()
        } else if let urlStr = event.image_url, let url = URL(string: urlStr) {
            AsyncImage(url: url) { phase in
                switch phase {
                case .success(let img):
                    img.resizable().scaledToFill()
                default:
                    Color(white: 0.85)
                }
            }
        } else {
            Color(white: 0.85)
        }
    }

    private func confirmationGroupCard(_ group: GroupDetail) -> some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(alignment: .center, spacing: 10) {
                avatarChip(for: group.leader ?? group.members.first)

                VStack(alignment: .leading, spacing: 5) {
                    Text(group.name)
                        .font(OnboardingStyle.font(16, weight: .semibold))
                        .foregroundStyle(.black)
                    Text(coordinator.selectedEvent.map { "We're headed to \($0.name)!" } ?? "Your crew is locked in.")
                        .font(OnboardingStyle.font(16))
                        .foregroundStyle(.black.opacity(0.5))
                }
                .frame(maxWidth: .infinity, alignment: .leading)
            }

            HStack(spacing: -15) {
                ForEach(Array(group.members.prefix(6).enumerated()), id: \.offset) { _, m in
                    avatarChip(for: m)
                }
            }
        }
        .padding(10)
        .frame(width: OnboardingStyle.fieldWidth, alignment: .topLeading)
        .frame(minHeight: 141, alignment: .topLeading)
        .background(Color.white.opacity(0.5))
        .clipShape(RoundedRectangle(cornerRadius: OnboardingStyle.cornerRadius))
    }

    private func avatarChip(for member: GroupMember?) -> some View {
        let id = member?.user_id ?? UUID().uuidString
        let symbol = OnboardingStyle.avatarSymbol(forUserId: id)
        return ZStack {
            Circle()
                .fill(Color(white: 0.85))
                .frame(width: 50, height: 50)
            Image(systemName: symbol)
                .font(.system(size: 20))
                .foregroundStyle(.black.opacity(0.4))
        }
    }

    private var meetingPointStrip: some View {
        let name = coordinator.meetingPointName.trimmingCharacters(in: .whitespacesAndNewlines)
        return HStack(spacing: 10) {
            Circle()
                .fill(OnboardingStyle.figmaPrimaryBlue)
                .frame(width: 12, height: 12)
            Text("Meeting at:")
                .font(OnboardingStyle.font(18, weight: .semibold))
                .foregroundStyle(OnboardingStyle.figmaPrimaryBlue)
            Text(name.isEmpty ? "Not set" : name)
                .font(OnboardingStyle.font(18))
                .foregroundStyle(OnboardingStyle.figmaPrimaryBlue)
            Spacer(minLength: 0)
        }
        .padding(.horizontal, 72)
        .frame(width: OnboardingStyle.fieldWidth, height: 40, alignment: .leading)
        .background(Color.white.opacity(0.5))
        .clipShape(RoundedRectangle(cornerRadius: OnboardingStyle.cornerRadius))
    }
}

#Preview {
    ConfirmationView()
        .environment(OnboardingCoordinator())
}
