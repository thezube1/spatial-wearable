import SwiftUI

struct GroupSetupView: View {
    @Environment(OnboardingCoordinator.self) private var coordinator

    @State private var me: UserProfile?
    @State private var errorMessage: String?
    @State private var isSubmitting = false
    @State private var showDiscardAlert = false

    private let gridColumns = [
        GridItem(.fixed(100), spacing: 18),
        GridItem(.fixed(100), spacing: 18),
        GridItem(.fixed(100), spacing: 18)
    ]

    var everyone: [UserProfile] {
        var list: [UserProfile] = []
        if let me { list.append(me) }
        list.append(contentsOf: coordinator.selectedMembers)
        return list
    }

    var body: some View {
        @Bindable var coordinator = coordinator
        ZStack {
            GroupSetupBackgroundView()
            WelcomeFullScreenFilmGrain()
                .ignoresSafeArea()

            GeometryReader { geo in
                let topPad = max(0, 135 - geo.safeAreaInsets.top)
                ZStack(alignment: .topLeading) {
                    VStack(spacing: 0) {
                        ScrollView(showsIndicators: false) {
                            VStack(spacing: 0) {
                                Color.clear.frame(height: topPad)

                                Text("Let's set your group up.")
                                    .font(OnboardingStyle.font(24, weight: .bold))
                                    .foregroundStyle(.white)
                                    .multilineTextAlignment(.center)
                                    .frame(maxWidth: 400)
                                    .shadow(color: .black.opacity(0.2), radius: 4, x: 0, y: 1)
                                    .padding(.bottom, 21)

                                groupNameCard(groupName: $coordinator.groupName, coordinator: coordinator)
                                    .padding(.bottom, 31)

                                Text("Select a group leader who will keep an eye out for everyone at all times.")
                                    .font(OnboardingStyle.font(16))
                                    .foregroundStyle(.white)
                                    .multilineTextAlignment(.center)
                                    .fixedSize(horizontal: false, vertical: true)
                                    .padding(.bottom, 20)

                                LazyVGrid(columns: gridColumns, spacing: 20) {
                                    ForEach(everyone) { person in
                                        leaderGridCell(person: person, coordinator: coordinator)
                                    }
                                }
                                .frame(maxWidth: OnboardingStyle.fieldWidth)

                                if let errorMessage {
                                    Text(errorMessage)
                                        .font(OnboardingStyle.font(13))
                                        .foregroundStyle(.red.opacity(0.95))
                                        .multilineTextAlignment(.center)
                                        .padding(.top, 16)
                                }

                                Color.clear.frame(height: 24)
                            }
                            .padding(.horizontal, 28)
                            .frame(minWidth: 0, maxWidth: .infinity)
                        }

                        Button {
                            Task { await submit() }
                        } label: {
                            if isSubmitting {
                                ProgressView().tint(.white)
                            } else {
                                Text("Continue")
                            }
                        }
                        .buttonStyle(WelcomePrimaryButtonStyle())
                        .disabled(coordinator.groupName.isEmpty || isSubmitting)
                        .opacity(coordinator.groupName.isEmpty ? 0.45 : 1)
                        .padding(.horizontal, 28)
                        .padding(.top, 8)
                        .padding(.bottom, max(16, geo.safeAreaInsets.bottom + 8))
                    }

                    Button {
                        showDiscardAlert = true
                    } label: {
                        Image(systemName: "chevron.left")
                            .font(.system(size: 20, weight: .semibold))
                            .foregroundStyle(.white)
                            .frame(width: 28, height: 28)
                    }
                    .buttonStyle(.plain)
                    .padding(.leading, 28)
                    .padding(.top, geo.safeAreaInsets.top + 12)
                }
            }
        }
        .task { await loadMe() }
        .alert("Delete group?", isPresented: $showDiscardAlert) {
            Button("Cancel", role: .cancel) { }
            Button("Delete", role: .destructive) { discardAndGoBack() }
        } message: {
            Text("This will discard the group you're setting up and take you back to the previous step.")
        }
    }

    private func groupNameCard(groupName: Binding<String>, coordinator: OnboardingCoordinator) -> some View {
        let groupGlyphId = "group:\(groupName.wrappedValue)"
        return HStack(alignment: .center, spacing: 10) {
            ZStack(alignment: .bottomTrailing) {
                ZStack {
                    Circle()
                        .fill(Color(white: 0.85))
                        .frame(width: 50, height: 50)
                    Image(systemName: OnboardingStyle.avatarSymbol(forUserId: groupGlyphId))
                        .font(.system(size: 22))
                        .foregroundStyle(.black.opacity(0.45))
                }
                Circle()
                    .fill(OnboardingStyle.figmaPrimaryBlue)
                    .frame(width: 22, height: 22)
                    .overlay(
                        Image(systemName: "pencil")
                            .font(.system(size: 11, weight: .semibold))
                            .foregroundStyle(.white)
                    )
                    .offset(x: 4, y: 4)
            }

            VStack(alignment: .leading, spacing: 5) {
                TextField(
                    "",
                    text: groupName,
                    prompt: Text("Group name")
                        .foregroundColor(.black.opacity(0.45))
                )
                .font(OnboardingStyle.font(18, weight: .semibold))
                .foregroundStyle(.black)
                .textInputAutocapitalization(.words)

                Button {
                    coordinator.returnToGroupSetupAfterEvent = true
                    coordinator.step = .selectEvent
                } label: {
                    Text(eventSubtitle(selectedEvent: coordinator.selectedEvent))
                        .font(OnboardingStyle.font(18))
                        .foregroundStyle(.black.opacity(0.92))
                        .multilineTextAlignment(.leading)
                        .lineLimit(2)
                        .fixedSize(horizontal: false, vertical: true)
                }
                .buttonStyle(.plain)
            }
            .frame(maxWidth: .infinity, alignment: .leading)
        }
        .padding(10)
        .frame(width: OnboardingStyle.fieldWidth)
        .frame(minHeight: 81, alignment: .center)
        .background(Color.white.opacity(0.5))
        .clipShape(RoundedRectangle(cornerRadius: OnboardingStyle.cornerRadius))
    }

    private func eventSubtitle(selectedEvent: Event?) -> String {
        if let name = selectedEvent?.name, !name.isEmpty {
            return "We're headed to \(name)!"
        }
        return "Choose an event…"
    }

    private func leaderGridCell(person: UserProfile, coordinator: OnboardingCoordinator) -> some View {
        let isLeader = coordinator.leaderId == person.id
        let isMe = me?.id == person.id
        let display = isMe ? "You" : (person.display_name ?? person.username ?? "—")
        let sym = OnboardingStyle.avatarSymbol(forUserId: person.id)
        let dot = OnboardingStyle.memberStatusColor(forUserId: person.id)

        return VStack(spacing: 2) {
            ZStack {
                Circle()
                    .fill(Color(white: 0.85))
                    .frame(width: 100, height: 100)
                Image(systemName: sym)
                    .font(.system(size: 36))
                    .foregroundStyle(.black.opacity(0.4))
                Circle()
                    .stroke(isLeader ? OnboardingStyle.figmaPrimaryBlue : Color.clear, lineWidth: 3)
                    .frame(width: 100, height: 100)
            }

            HStack(spacing: 2) {
                Circle()
                    .fill(dot)
                    .frame(width: 9, height: 9)
                Text(display)
                    .font(OnboardingStyle.font(14, weight: .medium))
                    .foregroundStyle(.white)
                    .lineLimit(1)
                    .minimumScaleFactor(0.75)
            }
            .frame(width: 100)
        }
        .contentShape(Rectangle())
        .onTapGesture { coordinator.leaderId = person.id }
    }

    private func discardAndGoBack() {
        coordinator.groupName = ""
        coordinator.selectedMembers = []
        coordinator.leaderId = nil
        coordinator.createdGroup = nil
        coordinator.step = .createOrJoin
    }

    private func loadMe() async {
        do {
            me = try await APIClient.shared.getMe()
            if coordinator.leaderId == nil { coordinator.leaderId = me?.id }
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    private func submit() async {
        guard let eventId = coordinator.selectedEvent?.id else {
            errorMessage = "Pick an event first."
            coordinator.returnToGroupSetupAfterEvent = true
            coordinator.step = .selectEvent
            return
        }
        isSubmitting = true
        defer { isSubmitting = false }
        do {
            let memberIds = coordinator.selectedMembers.map { $0.id }
            let group = try await APIClient.shared.createGroup(
                name: coordinator.groupName,
                eventId: eventId,
                memberIds: memberIds,
                leaderId: coordinator.leaderId
            )
            coordinator.createdGroup = group
            coordinator.step = .selectMeetingPoint
        } catch {
            errorMessage = error.localizedDescription
        }
    }
}

#Preview {
    GroupSetupView()
        .environment(OnboardingCoordinator())
}
