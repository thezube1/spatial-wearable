import SwiftUI
import UIKit

private enum SelectEventLocalThumbnail {
    static func assetName(for eventName: String) -> String? {
        let n = eventName.lowercased()
        if n.contains("rolling loud") { return "EventRollingLoud" }
        if n.contains("summer smash") { return "EventSummerSmash" }
        if n.contains("hard summer") { return "EventHardSummer" }
        if n.contains("camp flog") || n.contains("flog gnaw") { return "EventCampFlogGnaw" }
        return nil
    }
}

private let selectEventApiDate: DateFormatter = {
    let d = DateFormatter()
    d.locale = Locale(identifier: "en_US_POSIX")
    d.dateFormat = "yyyy-MM-dd"
    return d
}()

private let selectEventShortDate: DateFormatter = {
    let d = DateFormatter()
    d.dateFormat = "M/d"
    return d
}()

private extension Event {
    var selectEventVenueLine: String {
        switch (venue.flatMap { $0.isEmpty ? nil : $0 }, city.flatMap { $0.isEmpty ? nil : $0 }) {
        case let (v?, c?): return "\(v), \(c)"
        case let (v?, nil): return v
        case let (nil, c?): return c
        default: return ""
        }
    }

    func selectEventDateRangeLabel() -> String {
        guard let s = starts_on.flatMap({ selectEventApiDate.date(from: $0) }),
              let e = ends_on.flatMap({ selectEventApiDate.date(from: $0) }) else { return "" }
        return "\(selectEventShortDate.string(from: s)) - \(selectEventShortDate.string(from: e))"
    }
}

struct SelectEventView: View {
    @Environment(OnboardingCoordinator.self) private var coordinator

    @State private var events: [Event] = []
    @State private var search = ""
    @State private var isLoading = false
    @State private var errorMessage: String?

    private var filtered: [Event] {
        guard !search.isEmpty else { return events }
        let q = search.lowercased()
        return events.filter {
            $0.name.lowercased().contains(q)
                || ($0.venue?.lowercased().contains(q) ?? false)
                || ($0.city?.lowercased().contains(q) ?? false)
                || ($0.category?.lowercased().contains(q) ?? false)
        }
    }

    var body: some View {
        ZStack {
            SelectEventBackgroundView()
            WelcomeFullScreenFilmGrain()
                .ignoresSafeArea()

            GeometryReader { geo in
                let topPad = max(0, 135 - geo.safeAreaInsets.top)
                VStack(spacing: 0) {
                    ScrollView(showsIndicators: false) {
                        VStack(spacing: 0) {
                            Color.clear.frame(height: topPad)

                            Text("Where are you headed?")
                                .font(OnboardingStyle.font(24, weight: .bold))
                                .foregroundStyle(.white)
                                .multilineTextAlignment(.center)
                                .frame(maxWidth: 400)
                                .shadow(color: .black.opacity(0.2), radius: 4, x: 0, y: 1)
                                .padding(.bottom, 32)

                            selectEventSearchField
                                .padding(.bottom, 10)

                            HStack {
                                Text("Upcoming Events")
                                    .font(OnboardingStyle.font(18, weight: .semibold))
                                    .foregroundStyle(.white)
                                Spacer()
                            }
                            .padding(.bottom, 10)

                            if isLoading {
                                ProgressView()
                                    .tint(.white)
                                    .padding(.vertical, 24)
                            } else if let errorMessage {
                                Text(errorMessage)
                                    .font(OnboardingStyle.font(13))
                                    .foregroundStyle(.red.opacity(0.95))
                                    .multilineTextAlignment(.center)
                                    .padding(.vertical, 16)
                            } else {
                                VStack(spacing: 15) {
                                    ForEach(filtered) { event in
                                        EventCard(
                                            event: event,
                                            isSelected: coordinator.selectedEvent?.id == event.id
                                        )
                                        .onTapGesture { coordinator.selectedEvent = event }
                                    }
                                }
                            }

                            Color.clear.frame(height: 24)
                        }
                        .padding(.horizontal, 28)
                        .frame(minWidth: 0, maxWidth: .infinity)
                        .frame(minHeight: geo.size.height - geo.safeAreaInsets.bottom - 100)
                    }

                    Button(coordinator.returnToGroupSetupAfterEvent ? "Save event" : "Continue") {
                        if coordinator.returnToGroupSetupAfterEvent {
                            coordinator.returnToGroupSetupAfterEvent = false
                            coordinator.step = .groupSetup
                        } else {
                            coordinator.step = .createOrJoin
                        }
                    }
                    .buttonStyle(WelcomePrimaryButtonStyle())
                    .disabled(coordinator.selectedEvent == nil)
                    .opacity(coordinator.selectedEvent == nil ? 0.45 : 1)
                    .padding(.horizontal, 28)
                    .padding(.top, 8)
                    .padding(.bottom, max(16, geo.safeAreaInsets.bottom + 8))
                }

                VStack {
                    HStack {
                        Button {
                            if coordinator.returnToGroupSetupAfterEvent {
                                coordinator.returnToGroupSetupAfterEvent = false
                                coordinator.step = .groupSetup
                            } else {
                                coordinator.step = .linkWristband
                            }
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
        .task { await load() }
    }

    private var selectEventSearchField: some View {
        HStack(spacing: 10) {
            Image(systemName: "magnifyingglass")
                .font(.system(size: 17, weight: .regular))
                .foregroundStyle(Color.black.opacity(0.65))
            TextField(
                "",
                text: $search,
                prompt: Text("Search festivals, raves, concerts, venues...")
                    .foregroundColor(.black.opacity(0.45))
            )
            .font(OnboardingStyle.font(16))
            .foregroundStyle(.black)
            .textInputAutocapitalization(.never)
            .autocorrectionDisabled()
        }
        .padding(.horizontal, 10)
        .frame(width: OnboardingStyle.fieldWidth, height: 40)
        .background(Color.white.opacity(0.5))
        .clipShape(RoundedRectangle(cornerRadius: 5))
    }

    private func load() async {
        isLoading = true
        defer { isLoading = false }
        do { events = try await APIClient.shared.listEvents() }
        catch { errorMessage = error.localizedDescription }
    }
}

struct EventCard: View {
    let event: Event
    let isSelected: Bool

    private var primaryText: Color { isSelected ? .white : .black }
    private var secondaryText: Color { isSelected ? .white.opacity(0.92) : .black.opacity(0.88) }

    var body: some View {
        HStack(alignment: .top, spacing: 10) {
            eventThumbnail
                .frame(width: 100, height: 100)
                .clipShape(RoundedRectangle(cornerRadius: 5))

            VStack(alignment: .leading, spacing: 5) {
                Text(event.name)
                    .font(OnboardingStyle.font(16, weight: .semibold))
                    .foregroundStyle(primaryText)
                    .fixedSize(horizontal: false, vertical: true)
                Text(event.selectEventVenueLine)
                    .font(OnboardingStyle.font(12))
                    .foregroundStyle(secondaryText)
                    .fixedSize(horizontal: false, vertical: true)
                if let cat = event.category, !cat.isEmpty {
                    Text(cat)
                        .font(OnboardingStyle.font(12))
                        .foregroundStyle(secondaryText)
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)

            Text(event.selectEventDateRangeLabel())
                .font(OnboardingStyle.font(12, weight: .medium))
                .foregroundStyle(primaryText)
                .multilineTextAlignment(.trailing)
                .frame(alignment: .top)
        }
        .padding(10)
        .frame(minHeight: 122)
        .background(isSelected ? OnboardingStyle.figmaPrimaryBlue : Color.white.opacity(0.5))
        .clipShape(RoundedRectangle(cornerRadius: 5))
    }

    @ViewBuilder
    private var eventThumbnail: some View {
        if let asset = SelectEventLocalThumbnail.assetName(for: event.name),
           UIImage(named: asset) != nil {
            Image(asset)
                .resizable()
                .interpolation(.high)
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
}

#Preview {
    SelectEventView()
        .environment(OnboardingCoordinator())
}
