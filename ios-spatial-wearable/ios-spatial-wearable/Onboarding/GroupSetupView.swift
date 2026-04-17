import SwiftUI

struct GroupSetupView: View {
    @Environment(OnboardingCoordinator.self) private var coordinator

    @State private var me: UserProfile?
    @State private var errorMessage: String?
    @State private var isSubmitting = false
    @State private var showDiscardAlert = false

    var everyone: [UserProfile] {
        var list: [UserProfile] = []
        if let me { list.append(me) }
        list.append(contentsOf: coordinator.selectedMembers)
        return list
    }

    var body: some View {
        @Bindable var coordinator = coordinator
        return ZStack {
            OnboardingBackground()

            VStack(spacing: 18) {
                HStack {
                    Button {
                        showDiscardAlert = true
                    } label: {
                        HStack(spacing: 4) {
                            Image(systemName: "chevron.left")
                            Text("Back")
                        }
                        .font(OnboardingStyle.font(14, weight: .semibold))
                        .foregroundStyle(.white)
                    }
                    Spacer()
                }
                .frame(width: OnboardingStyle.fieldWidth)
                .padding(.top, 16)

                Text("Let's set your group up.")
                    .font(OnboardingStyle.font(24, weight: .bold))
                    .foregroundStyle(.white)
                    .padding(.top, 4)

                // Group name card
                VStack(alignment: .leading, spacing: 6) {
                    HStack(spacing: 2) {
                        Text("Group name").font(OnboardingStyle.font(12, weight: .semibold))
                            .foregroundStyle(.white.opacity(0.85))
                        Text("*").foregroundStyle(.red)
                    }
                    HStack(spacing: 12) {
                        ZStack {
                            Circle().fill(Color.white.opacity(0.18)).frame(width: 44, height: 44)
                            Image(systemName: "pencil").foregroundStyle(.white)
                        }
                        TextField("", text: $coordinator.groupName,
                                  prompt: Text("Required — e.g. \"Rave crew\"")
                                    .foregroundColor(.white.opacity(0.6)))
                            .font(OnboardingStyle.font(15, weight: .bold))
                            .foregroundStyle(.white)
                    }
                    .padding(12)
                    .background(Color.white.opacity(0.1))
                    .clipShape(RoundedRectangle(cornerRadius: OnboardingStyle.cornerRadius))
                }
                .frame(width: OnboardingStyle.fieldWidth, alignment: .leading)

                // Event card (configurable — routes back to event picker).
                Button {
                    coordinator.returnToGroupSetupAfterEvent = true
                    coordinator.step = .selectEvent
                } label: {
                    HStack(spacing: 12) {
                        Image(systemName: "calendar")
                            .foregroundStyle(.white)
                            .frame(width: 28)
                        VStack(alignment: .leading, spacing: 2) {
                            Text("Event")
                                .font(OnboardingStyle.font(11))
                                .foregroundStyle(.white.opacity(0.7))
                            Text(coordinator.selectedEvent?.name ?? "Choose event")
                                .font(OnboardingStyle.font(14, weight: .semibold))
                                .foregroundStyle(.white)
                        }
                        Spacer()
                        Text("Change")
                            .font(OnboardingStyle.font(12, weight: .semibold))
                            .foregroundStyle(.blue)
                    }
                    .padding(12)
                    .frame(width: OnboardingStyle.fieldWidth)
                    .background(Color.white.opacity(0.1))
                    .clipShape(RoundedRectangle(cornerRadius: OnboardingStyle.cornerRadius))
                }
                .buttonStyle(.plain)

                VStack(spacing: 4) {
                    Text("Tap an avatar to choose a group leader.")
                        .font(OnboardingStyle.font(13, weight: .semibold))
                        .foregroundStyle(.white)
                    Text("Defaults to you (the group creator). The leader keeps an eye out for everyone.")
                        .font(OnboardingStyle.font(12))
                        .foregroundStyle(.white.opacity(0.75))
                }
                .multilineTextAlignment(.center)
                .frame(width: OnboardingStyle.fieldWidth)

                // Avatar grid
                let columns = [GridItem(.adaptive(minimum: 80), spacing: 10)]
                LazyVGrid(columns: columns, spacing: 16) {
                    ForEach(everyone) { person in
                        avatarTile(person)
                    }
                }
                .frame(width: OnboardingStyle.fieldWidth)

                if let errorMessage {
                    Text(errorMessage)
                        .font(OnboardingStyle.font(12))
                        .foregroundStyle(.red.opacity(0.9))
                }

                Spacer()

                Button {
                    Task { await submit() }
                } label: {
                    if isSubmitting { ProgressView().tint(.white) }
                    else { Text("Continue") }
                }
                .buttonStyle(PrimaryBlueButtonStyle())
                .disabled(coordinator.groupName.isEmpty || isSubmitting)
                .opacity(coordinator.groupName.isEmpty ? 0.5 : 1)
                .padding(.bottom, 24)
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

    private func discardAndGoBack() {
        coordinator.groupName = ""
        coordinator.selectedMembers = []
        coordinator.leaderId = nil
        coordinator.createdGroup = nil
        coordinator.step = .createOrJoin
    }

    private func avatarTile(_ person: UserProfile) -> some View {
        let isLeader = coordinator.leaderId == person.id
        let isMe = me?.id == person.id
        return VStack(spacing: 6) {
            ZStack(alignment: .topTrailing) {
                Circle()
                    .fill(Color.white.opacity(0.2))
                    .frame(width: 60, height: 60)
                    .overlay(Image(systemName: "person.fill").foregroundStyle(.white.opacity(0.7)))
                    .overlay(
                        Circle().stroke(isLeader ? Color.blue : Color.clear, lineWidth: 3)
                    )
                if isLeader {
                    Image(systemName: "star.fill")
                        .font(.system(size: 12))
                        .foregroundStyle(.white)
                        .padding(5)
                        .background(Circle().fill(Color.blue))
                        .offset(x: 4, y: -4)
                }
            }
            Text(isMe ? "You" : (person.display_name ?? person.username ?? "—"))
                .font(OnboardingStyle.font(11, weight: isLeader ? .semibold : .regular))
                .foregroundStyle(.white)
                .lineLimit(1)
            Text(isLeader ? "Leader" : " ")
                .font(OnboardingStyle.font(10, weight: .semibold))
                .foregroundStyle(isLeader ? .blue : .clear)
        }
        .onTapGesture { coordinator.leaderId = person.id }
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
            coordinator.step = .confirmation
        } catch {
            errorMessage = error.localizedDescription
        }
    }
}

#Preview {
    GroupSetupView()
        .environment(OnboardingCoordinator())
}
