import SwiftUI

struct CreateOrJoinGroupView: View {
    @Environment(OnboardingCoordinator.self) private var coordinator

    @State private var query: String = ""
    @State private var results: [UserProfile] = []
    @State private var errorMessage: String?
    @State private var searchTask: Task<Void, Never>?

    var body: some View {
        @Bindable var coordinator = coordinator
        ZStack {
            CreateGroupBackgroundView()
            WelcomeFullScreenFilmGrain()
                .ignoresSafeArea()

            GeometryReader { geo in
                let topPad = max(0, 135 - geo.safeAreaInsets.top)
                VStack(spacing: 0) {
                    ScrollView(showsIndicators: false) {
                        VStack(spacing: 0) {
                            Color.clear.frame(height: topPad)

                            Text("Who are you going with?")
                                .font(OnboardingStyle.font(24, weight: .bold))
                                .foregroundStyle(.white)
                                .multilineTextAlignment(.center)
                                .frame(maxWidth: 400)
                                .shadow(color: .black.opacity(0.2), radius: 4, x: 0, y: 1)
                                .padding(.bottom, 20)

                            modePickerRow
                                .padding(.bottom, 39)

                            if coordinator.groupMode == .create {
                                createGroupSearchField
                                    .padding(.bottom, 24)

                                VStack(spacing: 15) {
                                    ForEach(results) { user in
                                        createGroupUserRow(user)
                                    }
                                }
                            } else {
                                joinCodeField(code: $coordinator.joinCode)
                                    .padding(.bottom, 24)
                            }

                            if let errorMessage {
                                Text(errorMessage)
                                    .font(OnboardingStyle.font(13))
                                    .foregroundStyle(.red.opacity(0.95))
                                    .multilineTextAlignment(.center)
                                    .padding(.vertical, 8)
                            }

                            Color.clear.frame(height: 24)
                        }
                        .padding(.horizontal, 28)
                        .frame(minWidth: 0, maxWidth: .infinity)
                    }

                    if coordinator.groupMode == .create {
                        Button("Continue") { coordinator.step = .groupSetup }
                            .buttonStyle(WelcomePrimaryButtonStyle())
                            .padding(.horizontal, 28)
                            .padding(.top, 8)
                            .padding(.bottom, max(16, geo.safeAreaInsets.bottom + 8))
                    } else {
                        Button("Join") {
                            Task { await join() }
                        }
                        .buttonStyle(WelcomePrimaryButtonStyle())
                        .disabled(coordinator.joinCode.isEmpty)
                        .opacity(coordinator.joinCode.isEmpty ? 0.45 : 1)
                        .padding(.horizontal, 28)
                        .padding(.top, 8)
                        .padding(.bottom, max(16, geo.safeAreaInsets.bottom + 8))
                    }
                }

                VStack {
                    HStack {
                        Button {
                            coordinator.step = .selectEvent
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
        .task { if coordinator.groupMode == .create { await runSearch("") } }
        .onChange(of: coordinator.groupMode) { _, new in
            if new == .create { Task { await runSearch(query) } }
        }
    }

    private var modePickerRow: some View {
        HStack(spacing: 20) {
            modeCard(
                mode: .create,
                title: "Create group",
                isSelected: coordinator.groupMode == .create,
                icon: { createModeIcon() }
            )
            modeCard(
                mode: .join,
                title: "Join group",
                isSelected: coordinator.groupMode == .join,
                icon: { joinModeIcon() }
            )
        }
        .frame(width: OnboardingStyle.fieldWidth)
    }

    private func modeCard(
        mode: OnboardingCoordinator.GroupMode,
        title: String,
        isSelected: Bool,
        @ViewBuilder icon: () -> some View
    ) -> some View {
        Button {
            coordinator.groupMode = mode
        } label: {
            VStack(spacing: 10) {
                icon()
                Text(title)
                    .font(OnboardingStyle.font(18))
                    .foregroundStyle(isSelected ? Color.white : Color.black)
            }
            .frame(maxWidth: .infinity)
            .frame(height: 103)
            .background(isSelected ? OnboardingStyle.figmaPrimaryBlue : Color.white.opacity(0.5))
            .clipShape(RoundedRectangle(cornerRadius: OnboardingStyle.cornerRadius))
        }
        .buttonStyle(.plain)
    }

    private func createModeIcon() -> some View {
        ZStack {
            Circle()
                .fill(Color.white)
                .frame(width: 55, height: 55)
            Image(systemName: "plus")
                .font(.system(size: 25, weight: .regular))
                .foregroundStyle(OnboardingStyle.figmaPrimaryBlue)
        }
    }

    private func joinModeIcon() -> some View {
        ZStack {
            Circle()
                .fill(Color.white)
                .frame(width: 55, height: 55)
            Image(systemName: "arrow.triangle.merge")
                .font(.system(size: 22, weight: .regular))
                .foregroundStyle(.black)
        }
    }

    private var createGroupSearchField: some View {
        HStack(spacing: 10) {
            Image(systemName: "magnifyingglass")
                .font(.system(size: 17, weight: .regular))
                .foregroundStyle(Color.black.opacity(0.65))
            TextField(
                "",
                text: $query,
                prompt: Text("Search names, usernames...")
                    .foregroundColor(.black.opacity(0.45))
            )
            .font(OnboardingStyle.font(16))
            .foregroundStyle(.black)
            .textInputAutocapitalization(.never)
            .autocorrectionDisabled()
            .onChange(of: query) { _, new in debounceSearch(new) }
        }
        .padding(.horizontal, 10)
        .frame(width: OnboardingStyle.fieldWidth, height: 40)
        .background(Color.white.opacity(0.5))
        .clipShape(RoundedRectangle(cornerRadius: OnboardingStyle.cornerRadius))
    }

    private func joinCodeField(code: Binding<String>) -> some View {
        TextField(
            "",
            text: code,
            prompt: Text("Enter join code")
                .foregroundColor(.black.opacity(0.45))
        )
        .font(OnboardingStyle.font(16))
        .foregroundStyle(.black)
        .textInputAutocapitalization(.never)
        .autocorrectionDisabled()
        .padding(.horizontal, 10)
        .frame(width: OnboardingStyle.fieldWidth, height: 40, alignment: .leading)
        .background(Color.white.opacity(0.5))
        .clipShape(RoundedRectangle(cornerRadius: OnboardingStyle.cornerRadius))
    }

    private func createGroupUserRow(_ user: UserProfile) -> some View {
        let isSelected = coordinator.selectedMembers.contains(where: { $0.id == user.id })
        let sym = OnboardingStyle.avatarSymbol(forUserId: user.id)
        let primary: Color = isSelected ? .white : .black
        let secondary: Color = isSelected ? .white.opacity(0.92) : .black.opacity(0.88)

        return HStack(alignment: .center, spacing: 10) {
            ZStack {
                Circle()
                    .fill(Color(white: 0.85))
                    .frame(width: 50, height: 50)
                Image(systemName: sym)
                    .font(.system(size: 22))
                    .foregroundStyle(isSelected ? OnboardingStyle.figmaPrimaryBlue.opacity(0.95) : .black.opacity(0.45))
            }

            VStack(alignment: .leading, spacing: 5) {
                Text(user.display_name ?? user.username ?? "Unknown")
                    .font(OnboardingStyle.font(16, weight: .semibold))
                    .foregroundStyle(primary)
                if let u = user.username {
                    Text("@\(u)")
                        .font(OnboardingStyle.font(16))
                        .foregroundStyle(secondary)
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)

            ZStack {
                RoundedRectangle(cornerRadius: 2, style: .continuous)
                    .fill(Color.white)
                    .frame(width: 20, height: 20)
                if isSelected {
                    Image(systemName: "checkmark")
                        .font(.system(size: 12, weight: .bold))
                        .foregroundStyle(OnboardingStyle.figmaPrimaryBlue)
                }
            }
        }
        .padding(10)
        .frame(minHeight: 81)
        .background(isSelected ? OnboardingStyle.figmaPrimaryBlue : Color.white.opacity(0.5))
        .clipShape(RoundedRectangle(cornerRadius: OnboardingStyle.cornerRadius))
        .contentShape(Rectangle())
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

#Preview {
    CreateOrJoinGroupView()
        .environment(OnboardingCoordinator())
}
