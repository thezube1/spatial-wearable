import SwiftUI

struct CreateOrJoinGroupView: View {
    @Environment(OnboardingCoordinator.self) private var coordinator

    @State private var query: String = ""
    @State private var results: [UserProfile] = []
    @State private var errorMessage: String?
    @State private var searchTask: Task<Void, Never>?

    var body: some View {
        @Bindable var coordinator = coordinator
        return ZStack {
            OnboardingBackground()

            VStack(spacing: 18) {
                Text("Who are you going with?")
                    .font(OnboardingStyle.font(24, weight: .bold))
                    .foregroundStyle(.white)
                    .padding(.top, 32)

                HStack(spacing: 10) {
                    modeButton(.create, label: "Create group")
                    modeButton(.join, label: "Join group")
                }
                .frame(width: OnboardingStyle.fieldWidth)

                if coordinator.groupMode == .create {
                    TextField("", text: $query,
                              prompt: Text("Search for people").foregroundColor(.white.opacity(0.6)))
                        .onboardingField()
                        .onChange(of: query) { _, new in debounceSearch(new) }

                    ScrollView {
                        VStack(spacing: 8) {
                            ForEach(results) { user in
                                userRow(user)
                            }
                        }
                    }
                    .frame(width: OnboardingStyle.fieldWidth)

                    Button("Continue") { coordinator.step = .groupSetup }
                        .buttonStyle(PrimaryBlueButtonStyle())
                        .disabled(coordinator.selectedMembers.isEmpty)
                        .opacity(coordinator.selectedMembers.isEmpty ? 0.5 : 1)
                } else {
                    TextField("", text: $coordinator.joinCode,
                              prompt: Text("Enter join code").foregroundColor(.white.opacity(0.6)))
                        .autocapitalization(.none)
                        .onboardingField()

                    Button("Join") {
                        Task { await join() }
                    }
                    .buttonStyle(PrimaryBlueButtonStyle())
                    .disabled(coordinator.joinCode.isEmpty)
                    .opacity(coordinator.joinCode.isEmpty ? 0.5 : 1)
                }

                if let errorMessage {
                    Text(errorMessage)
                        .font(OnboardingStyle.font(12))
                        .foregroundStyle(.red.opacity(0.9))
                }

                Spacer()
            }
            .padding(.bottom, 24)
        }
        .task { if coordinator.groupMode == .create { await runSearch("") } }
    }

    private func modeButton(_ mode: OnboardingCoordinator.GroupMode, label: String) -> some View {
        let selected = coordinator.groupMode == mode
        return Button {
            coordinator.groupMode = mode
        } label: {
            Text(label)
                .font(OnboardingStyle.font(14, weight: .semibold))
                .frame(maxWidth: .infinity)
                .padding(.vertical, 12)
                .background(selected ? Color.blue : Color.white.opacity(0.12))
                .foregroundStyle(.white)
                .clipShape(RoundedRectangle(cornerRadius: OnboardingStyle.cornerRadius))
        }
    }

    private func userRow(_ user: UserProfile) -> some View {
        let isSelected = coordinator.selectedMembers.contains(where: { $0.id == user.id })
        return HStack(spacing: 12) {
            Circle().fill(Color.white.opacity(0.2)).frame(width: 32, height: 32)
                .overlay(Image(systemName: "person.fill").foregroundStyle(.white.opacity(0.7)))
            VStack(alignment: .leading, spacing: 1) {
                Text(user.display_name ?? user.username ?? "Unknown")
                    .font(OnboardingStyle.font(14, weight: .semibold))
                if let u = user.username {
                    Text("@\(u)").font(OnboardingStyle.font(12)).opacity(0.6)
                }
            }
            Spacer()
            ZStack {
                RoundedRectangle(cornerRadius: 3)
                    .fill(isSelected ? Color.white : Color.clear)
                    .frame(width: 20, height: 20)
                RoundedRectangle(cornerRadius: 3)
                    .stroke(Color.white, lineWidth: 1)
                    .frame(width: 20, height: 20)
                if isSelected {
                    Image(systemName: "checkmark")
                        .font(.system(size: 12, weight: .bold))
                        .foregroundStyle(.blue)
                }
            }
        }
        .padding(10)
        .foregroundStyle(.white)
        .background(isSelected ? Color.blue : Color.white.opacity(0.1))
        .clipShape(RoundedRectangle(cornerRadius: OnboardingStyle.cornerRadius))
        .onTapGesture {
            if isSelected {
                coordinator.selectedMembers.removeAll { $0.id == user.id }
            } else {
                coordinator.selectedMembers.append(user)
            }
        }
    }

    private func debounceSearch(_ q: String) {
        searchTask?.cancel()
        searchTask = Task {
            try? await Task.sleep(for: .milliseconds(250))
            if Task.isCancelled { return }
            await runSearch(q)
        }
    }

    private func runSearch(_ q: String) async {
        do { results = try await APIClient.shared.searchUsers(query: q) }
        catch { errorMessage = error.localizedDescription }
    }

    private func join() async {
        do {
            let g = try await APIClient.shared.joinGroup(joinCode: coordinator.joinCode)
            coordinator.createdGroup = g
            coordinator.step = .confirmation
        } catch {
            errorMessage = error.localizedDescription
        }
    }
}
