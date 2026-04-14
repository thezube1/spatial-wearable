import SwiftUI

struct SelectEventView: View {
    @Environment(OnboardingCoordinator.self) private var coordinator

    @State private var events: [Event] = []
    @State private var search = ""
    @State private var isLoading = false
    @State private var errorMessage: String?

    var filtered: [Event] {
        guard !search.isEmpty else { return events }
        let q = search.lowercased()
        return events.filter {
            $0.name.lowercased().contains(q) || ($0.venue?.lowercased().contains(q) ?? false)
        }
    }

    var body: some View {
        ZStack {
            OnboardingBackground()

            VStack(spacing: 18) {
                Text("Where are you headed?")
                    .font(OnboardingStyle.font(24, weight: .bold))
                    .foregroundStyle(.white)
                    .padding(.top, 32)

                TextField("", text: $search,
                          prompt: Text("Search events").foregroundColor(.white.opacity(0.6)))
                    .onboardingField()

                HStack {
                    Text("Upcoming Events")
                        .font(OnboardingStyle.font(14, weight: .semibold))
                        .foregroundStyle(.white.opacity(0.9))
                    Spacer()
                }
                .frame(width: OnboardingStyle.fieldWidth)

                ScrollView {
                    VStack(spacing: 10) {
                        ForEach(filtered) { event in
                            EventCard(event: event,
                                      isSelected: coordinator.selectedEvent?.id == event.id)
                                .onTapGesture { coordinator.selectedEvent = event }
                        }
                    }
                    .padding(.vertical, 4)
                }
                .frame(width: OnboardingStyle.fieldWidth)

                Button(coordinator.returnToGroupSetupAfterEvent ? "Save event" : "Continue") {
                    if coordinator.returnToGroupSetupAfterEvent {
                        coordinator.returnToGroupSetupAfterEvent = false
                        coordinator.step = .groupSetup
                    } else {
                        coordinator.step = .createOrJoin
                    }
                }
                .buttonStyle(PrimaryBlueButtonStyle())
                .disabled(coordinator.selectedEvent == nil)
                .opacity(coordinator.selectedEvent == nil ? 0.5 : 1)
                .padding(.bottom, 24)
            }
        }
        .task { await load() }
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

    var body: some View {
        HStack(spacing: 12) {
            AsyncImage(url: event.image_url.flatMap(URL.init)) { phase in
                switch phase {
                case .success(let img): img.resizable().scaledToFill()
                default: Color.white.opacity(0.15)
                }
            }
            .frame(width: 56, height: 56)
            .clipShape(RoundedRectangle(cornerRadius: 4))

            VStack(alignment: .leading, spacing: 2) {
                Text(event.name)
                    .font(OnboardingStyle.font(15, weight: .bold))
                Text(event.venue ?? "")
                    .font(OnboardingStyle.font(12))
                    .opacity(0.5)
                Text(event.category ?? "")
                    .font(OnboardingStyle.font(12))
                    .opacity(0.5)
            }

            Spacer()

            Text(dateRange)
                .font(OnboardingStyle.font(12))
                .opacity(0.8)
                .multilineTextAlignment(.trailing)
        }
        .padding(10)
        .foregroundStyle(isSelected ? Color.white : Color.white)
        .background(isSelected ? Color.blue : Color.white.opacity(0.1))
        .clipShape(RoundedRectangle(cornerRadius: OnboardingStyle.cornerRadius))
    }

    private var dateRange: String {
        let s = event.starts_on ?? ""
        let e = event.ends_on ?? ""
        if s.isEmpty && e.isEmpty { return "" }
        if s == e { return s }
        return "\(s)\n\(e)"
    }
}
