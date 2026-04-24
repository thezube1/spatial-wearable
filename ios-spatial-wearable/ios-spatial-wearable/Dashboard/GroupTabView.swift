import SwiftUI
import UIKit

struct GroupTabView: View {
    @Environment(OnboardingCoordinator.self) private var coordinator
    @Environment(BLEManager.self) private var ble

    @State private var group: GroupDetail?
    @State private var me: UserProfile?
    @State private var isLoading = true
    @State private var errorMessage: String?
    @State private var trackingStatus: String?

    // Persisted per-group selection. Solo (no other members) => ignored.
    // Exactly one other member => auto-target that member; this override only
    // matters when the group has 2+ other members.
    @AppStorage("trackingTarget") private var trackingTargetRaw: String = ""

    @State private var showRename = false
    @State private var showShare = false
    @State private var showAddMember = false
    @State private var showDeleteConfirm = false
    @State private var showLeaveConfirm = false
    @State private var pendingRemove: GroupMember?

    @State private var showCreateFlow = false
    @State private var joinCode = ""

    var body: some View {
        NavigationStack {
            VStack(spacing: 0) {
                HStack {
                    Text("Group")
                        .font(OnboardingStyle.font(28, weight: .bold))
                        .foregroundStyle(.white)
                        .shadow(color: .black.opacity(0.25), radius: 4, x: 0, y: 1)
                    Spacer()
                    if group != nil, canManage {
                        Menu {
                            Button { showRename = true } label: { Label("Rename", systemImage: "pencil") }
                            Button { showAddMember = true } label: { Label("Add member", systemImage: "person.badge.plus") }
                            if isCreator {
                                Button(role: .destructive) { showDeleteConfirm = true } label: {
                                    Label("Delete group", systemImage: "trash")
                                }
                            }
                        } label: {
                            Image(systemName: "ellipsis.circle")
                                .font(.title2)
                                .foregroundStyle(.white)
                        }
                    }
                }
                .padding(.horizontal, 20)
                .padding(.vertical, 8)

                Group {
                    if isLoading {
                        ProgressView().tint(.white).frame(maxWidth: .infinity, maxHeight: .infinity)
                    } else if let group {
                        groupDetail(group)
                    } else {
                        noGroupView
                    }
                }
            }
            .background(Color.clear)
            .toolbar(.hidden, for: .navigationBar)
        }
        .task { await load() }
        .sheet(isPresented: $showRename) {
            if let g = group {
                RenameGroupSheet(currentName: g.name) { newName in
                    await rename(to: newName)
                }
            }
        }
        .sheet(isPresented: $showAddMember) {
            if let g = group {
                AddMemberSheet(group: g) { updated in
                    group = updated
                    Task { await pushTargetToWearable() }
                }
            }
        }
        .alert("Delete group?", isPresented: $showDeleteConfirm) {
            Button("Cancel", role: .cancel) {}
            Button("Delete", role: .destructive) { Task { await deleteGroup() } }
        } message: {
            Text("Everyone will be removed from this group. This cannot be undone.")
        }
        .alert("Leave group?", isPresented: $showLeaveConfirm) {
            Button("Cancel", role: .cancel) {}
            Button("Leave", role: .destructive) { Task { await leaveGroup() } }
        }
        .alert("Remove \(pendingRemove?.display_name ?? pendingRemove?.username ?? "member")?",
               isPresented: Binding(get: { pendingRemove != nil }, set: { if !$0 { pendingRemove = nil } })) {
            Button("Cancel", role: .cancel) { pendingRemove = nil }
            Button("Remove", role: .destructive) {
                if let m = pendingRemove { Task { await remove(member: m) } }
            }
        }
    }

    // Other members = everyone except me. The "effective target" is:
    //   0 others → nil (wearable shows "Not tracking")
    //   1 other  → auto-lock to that person
    //   2+       → stored selection if still a member, else first other
    private func otherMembers(_ g: GroupDetail) -> [GroupMember] {
        g.members.filter { $0.user_id != me?.id }
    }

    private func effectiveTarget(in g: GroupDetail) -> GroupMember? {
        let others = otherMembers(g)
        if others.isEmpty { return nil }
        if others.count == 1 { return others[0] }
        if let stored = others.first(where: { $0.user_id == trackingTargetRaw }) {
            return stored
        }
        return others.first
    }

    private func setTarget(_ member: GroupMember) {
        trackingTargetRaw = member.user_id
        Task { await pushTargetToWearable() }
    }

    private func pushTargetToWearable() async {
        guard let g = group else { return }
        guard ble.connectionState == .connected else {
            trackingStatus = "Wearable not connected"
            return
        }
        let target = effectiveTarget(in: g)
        do {
            if let t = target {
                guard let mac = t.linked_device_mac else {
                    trackingStatus = "\(t.display_name ?? t.username ?? "Member") has no linked wearable"
                    return
                }
                let name = t.display_name ?? t.username ?? "Friend"
                try await ble.setTrackingTarget(mac: mac, name: name)
                trackingStatus = "Tracking \(name)"
            } else {
                try await ble.clearTrackingTarget()
                trackingStatus = "Not tracking (solo group)"
            }
        } catch {
            trackingStatus = "Sync failed: \(error.localizedDescription)"
        }
    }

    private var isCreator: Bool {
        guard let g = group, let me else { return false }
        return g.created_by == me.id
    }

    private var canManage: Bool {
        guard let g = group, let me else { return false }
        return g.created_by == me.id || g.leader?.user_id == me.id
    }

    // MARK: - No Group

    private var noGroupView: some View {
        ScrollView {
            VStack(spacing: 20) {
                VStack(spacing: 8) {
                    Image(systemName: "person.3.fill")
                        .font(.system(size: 44))
                        .foregroundStyle(.white)
                        .padding(.top, 32)
                    Text("You're not in a group yet")
                        .font(OnboardingStyle.font(18, weight: .semibold))
                        .foregroundStyle(.white)
                    Text("Create a new group or join one with an invite code.")
                        .font(OnboardingStyle.font(14))
                        .foregroundStyle(.white.opacity(0.85))
                        .multilineTextAlignment(.center)
                        .padding(.horizontal, 32)
                }

                VStack(alignment: .leading, spacing: 12) {
                    Text("Join with code")
                        .font(OnboardingStyle.font(12, weight: .semibold))
                        .foregroundStyle(.black.opacity(0.55))
                    HStack {
                        TextField("Enter invite code", text: $joinCode)
                            .font(OnboardingStyle.font(16))
                            .foregroundStyle(.black)
                            .textInputAutocapitalization(.characters)
                            .autocorrectionDisabled()
                            .padding(10)
                            .background(Color.white.opacity(0.6),
                                        in: RoundedRectangle(cornerRadius: OnboardingStyle.cornerRadius))
                        Button("Join") { Task { await join() } }
                            .font(OnboardingStyle.font(15, weight: .semibold))
                            .foregroundStyle(.white)
                            .padding(.horizontal, 16)
                            .padding(.vertical, 10)
                            .background(OnboardingStyle.figmaPrimaryBlue)
                            .clipShape(RoundedRectangle(cornerRadius: OnboardingStyle.cornerRadius))
                            .disabled(joinCode.isEmpty)
                            .opacity(joinCode.isEmpty ? 0.5 : 1)
                    }
                }
                .padding(16)
                .background(Color.white.opacity(0.5),
                            in: RoundedRectangle(cornerRadius: OnboardingStyle.cornerRadius))

                Button {
                    showCreateFlow = true
                } label: {
                    HStack {
                        Image(systemName: "plus.circle.fill")
                        Text("Create a new group")
                    }
                    .font(OnboardingStyle.font(16, weight: .semibold))
                    .foregroundStyle(.white)
                    .frame(maxWidth: .infinity)
                    .padding(.vertical, 14)
                }
                .background(OnboardingStyle.figmaPrimaryBlue)
                .clipShape(RoundedRectangle(cornerRadius: OnboardingStyle.cornerRadius))

                if let errorMessage {
                    Text(errorMessage)
                        .foregroundStyle(.red.opacity(0.95))
                        .font(OnboardingStyle.font(12))
                }
            }
            .padding(20)
        }
        .sheet(isPresented: $showCreateFlow) {
            CreateGroupSheet { created in
                group = created
            }
        }
    }

    // MARK: - Group detail

    private func groupDetail(_ g: GroupDetail) -> some View {
        ScrollView {
            VStack(spacing: 16) {
                headerCard(g)
                inviteCard(g)
                membersCard(g)
                footerActions(g)
            }
            .padding(20)
        }
    }

    private func headerCard(_ g: GroupDetail) -> some View {
        VStack(spacing: 12) {
            ZStack {
                Circle()
                    .fill(Color(white: 0.85))
                    .frame(width: 80, height: 80)
                Image(systemName: OnboardingStyle.avatarSymbol(forUserId: "group:\(g.id)"))
                    .font(.system(size: 32))
                    .foregroundStyle(.black.opacity(0.45))
            }
            Text(g.name)
                .font(OnboardingStyle.font(20, weight: .bold))
                .foregroundStyle(.black)
            if let ev = g.event {
                Label(ev.name, systemImage: "calendar")
                    .font(OnboardingStyle.font(14))
                    .foregroundStyle(.black.opacity(0.7))
            }
            Text("\(g.members.count) member\(g.members.count == 1 ? "" : "s")")
                .font(OnboardingStyle.font(12))
                .foregroundStyle(.black.opacity(0.6))
        }
        .frame(maxWidth: .infinity)
        .padding(20)
        .background(Color.white.opacity(0.5),
                    in: RoundedRectangle(cornerRadius: OnboardingStyle.cornerRadius))
    }

    private func inviteCard(_ g: GroupDetail) -> some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("Invite code")
                .font(OnboardingStyle.font(12, weight: .semibold))
                .foregroundStyle(.black.opacity(0.55))
                .textCase(.uppercase)
            HStack {
                Text(g.join_code ?? "—")
                    .font(OnboardingStyle.font(20, weight: .bold).monospaced())
                    .foregroundStyle(.black)
                    .textSelection(.enabled)
                Spacer()
                if let code = g.join_code {
                    Button {
                        UIPasteboard.general.string = code
                    } label: {
                        Image(systemName: "doc.on.doc")
                            .foregroundStyle(OnboardingStyle.figmaPrimaryBlue)
                    }
                    ShareLink(item: "Join my group \"\(g.name)\" with code: \(code)") {
                        Image(systemName: "square.and.arrow.up")
                            .foregroundStyle(OnboardingStyle.figmaPrimaryBlue)
                    }
                }
            }
        }
        .padding(16)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(Color.white.opacity(0.5),
                    in: RoundedRectangle(cornerRadius: OnboardingStyle.cornerRadius))
    }

    private func membersCard(_ g: GroupDetail) -> some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                Text("Members")
                    .font(OnboardingStyle.font(12, weight: .semibold))
                    .foregroundStyle(.black.opacity(0.55))
                    .textCase(.uppercase)
                Spacer()
                if canManage {
                    Button {
                        showAddMember = true
                    } label: {
                        Label("Add", systemImage: "plus")
                            .font(OnboardingStyle.font(12, weight: .semibold))
                            .foregroundStyle(OnboardingStyle.figmaPrimaryBlue)
                    }
                }
            }
            ForEach(g.members) { member in
                memberRow(member, group: g)
                if member.id != g.members.last?.id {
                    Divider().background(Color.black.opacity(0.1))
                }
            }
            if let trackingStatus {
                Text(trackingStatus)
                    .font(OnboardingStyle.font(12))
                    .foregroundStyle(.black.opacity(0.6))
                    .padding(.top, 4)
            }
        }
        .padding(16)
        .background(Color.white.opacity(0.5),
                    in: RoundedRectangle(cornerRadius: OnboardingStyle.cornerRadius))
    }

    private func memberRow(_ m: GroupMember, group g: GroupDetail) -> some View {
        let isLeader = g.leader?.user_id == m.user_id
        let isCreatorOfGroup = g.created_by == m.user_id
        let isSelf = me?.id == m.user_id
        let isTarget = effectiveTarget(in: g)?.user_id == m.user_id
        let canPickTarget = !isSelf && otherMembers(g).count >= 2
        let avatarSymbol = OnboardingStyle.avatarSymbol(forUserId: m.user_id)
        let dotColor = OnboardingStyle.memberStatusColor(forUserId: m.user_id)
        return HStack(spacing: 12) {
            ZStack {
                Circle()
                    .fill(Color(white: 0.85))
                    .frame(width: 40, height: 40)
                Image(systemName: avatarSymbol)
                    .font(.system(size: 18))
                    .foregroundStyle(.black.opacity(0.45))
                if isLeader {
                    Image(systemName: "star.fill")
                        .font(.system(size: 10))
                        .foregroundStyle(.white)
                        .padding(4)
                        .background(Circle().fill(OnboardingStyle.figmaPrimaryBlue))
                        .offset(x: 14, y: -14)
                }
            }
            VStack(alignment: .leading, spacing: 2) {
                HStack(spacing: 6) {
                    Circle()
                        .fill(dotColor)
                        .frame(width: 8, height: 8)
                    Text(isSelf ? "You" : (m.display_name ?? m.username ?? "Unknown"))
                        .font(OnboardingStyle.font(15, weight: .semibold))
                        .foregroundStyle(.black)
                }
                HStack(spacing: 6) {
                    if let u = m.username {
                        Text("@\(u)")
                            .font(OnboardingStyle.font(12))
                            .foregroundStyle(.black.opacity(0.6))
                    }
                    if isLeader {
                        Text("Leader")
                            .font(OnboardingStyle.font(10, weight: .bold))
                            .padding(.horizontal, 6).padding(.vertical, 2)
                            .background(OnboardingStyle.figmaPrimaryBlue.opacity(0.18), in: Capsule())
                            .foregroundStyle(OnboardingStyle.figmaPrimaryBlue)
                    }
                }
            }
            Spacer()
            if !isSelf && m.linked_device_mac != nil {
                if isTarget {
                    Label("Tracking", systemImage: "dot.radiowaves.left.and.right")
                        .labelStyle(.titleAndIcon)
                        .font(OnboardingStyle.font(10, weight: .bold))
                        .padding(.horizontal, 8).padding(.vertical, 4)
                        .background(Color.green.opacity(0.22), in: Capsule())
                        .foregroundStyle(Color(red: 0.15, green: 0.55, blue: 0.25))
                } else if canPickTarget {
                    Button("Track") { setTarget(m) }
                        .font(OnboardingStyle.font(12, weight: .semibold))
                        .foregroundStyle(OnboardingStyle.figmaPrimaryBlue)
                        .padding(.horizontal, 10).padding(.vertical, 6)
                        .background(Color.white.opacity(0.6),
                                    in: Capsule())
                }
            }
            if canManage && !isSelf && !isCreatorOfGroup {
                Button {
                    pendingRemove = m
                } label: {
                    Image(systemName: "minus.circle.fill").foregroundStyle(.red.opacity(0.85))
                }
            }
        }
    }

    private func footerActions(_ g: GroupDetail) -> some View {
        VStack(spacing: 10) {
            if !isCreator {
                Button(role: .destructive) {
                    showLeaveConfirm = true
                } label: {
                    Text("Leave group")
                        .font(OnboardingStyle.font(15, weight: .semibold))
                        .foregroundStyle(.white)
                        .frame(maxWidth: .infinity)
                        .padding(.vertical, 12)
                }
                .background(Color.red.opacity(0.75))
                .clipShape(RoundedRectangle(cornerRadius: OnboardingStyle.cornerRadius))
            }
            if let errorMessage {
                Text(errorMessage)
                    .foregroundStyle(.red.opacity(0.95))
                    .font(OnboardingStyle.font(12))
            }
        }
    }

    // MARK: - Actions

    private func load() async {
        isLoading = true
        defer { isLoading = false }
        do {
            async let meTask = APIClient.shared.getMe()
            async let groupTask = APIClient.shared.getMyGroup()
            me = try await meTask
            group = try await groupTask
            if let g = group { coordinator.createdGroup = g }
            await pushTargetToWearable()
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    private func rename(to newName: String) async {
        guard let g = group else { return }
        do {
            let updated = try await APIClient.shared.renameGroup(id: g.id, name: newName)
            group = updated
            coordinator.createdGroup = updated
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    private func deleteGroup() async {
        guard let g = group else { return }
        do {
            try await APIClient.shared.deleteGroup(id: g.id)
            group = nil
            coordinator.createdGroup = nil
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    private func leaveGroup() async {
        guard let g = group else { return }
        do {
            try await APIClient.shared.leaveGroup(id: g.id)
            group = nil
            coordinator.createdGroup = nil
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    private func remove(member m: GroupMember) async {
        guard let g = group else { return }
        pendingRemove = nil
        do {
            try await APIClient.shared.removeMember(groupId: g.id, userId: m.user_id)
            group = try await APIClient.shared.getGroup(id: g.id)
            await pushTargetToWearable()
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    private func join() async {
        do {
            let g = try await APIClient.shared.joinGroup(joinCode: joinCode.trimmingCharacters(in: .whitespaces))
            group = g
            coordinator.createdGroup = g
            joinCode = ""
        } catch {
            errorMessage = error.localizedDescription
        }
    }
}

// MARK: - Rename sheet

private struct RenameGroupSheet: View {
    let currentName: String
    let onSave: (String) async -> Void
    @Environment(\.dismiss) private var dismiss
    @State private var name: String

    init(currentName: String, onSave: @escaping (String) async -> Void) {
        self.currentName = currentName
        self.onSave = onSave
        _name = State(initialValue: currentName)
    }

    var body: some View {
        NavigationStack {
            Form {
                Section("Group name") {
                    TextField("Name", text: $name)
                }
            }
            .navigationTitle("Rename group")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") { dismiss() }
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Save") {
                        Task {
                            await onSave(name.trimmingCharacters(in: .whitespaces))
                            dismiss()
                        }
                    }
                    .disabled(name.trimmingCharacters(in: .whitespaces).isEmpty || name == currentName)
                }
            }
        }
        .presentationDetents([.medium])
    }
}

// MARK: - Add member sheet

private struct AddMemberSheet: View {
    let group: GroupDetail
    let onUpdate: (GroupDetail) -> Void
    @Environment(\.dismiss) private var dismiss

    @State private var query = ""
    @State private var results: [UserProfile] = []
    @State private var errorMessage: String?
    @State private var searchTask: Task<Void, Never>?

    var body: some View {
        NavigationStack {
            VStack(spacing: 0) {
                TextField("Search people", text: $query)
                    .textInputAutocapitalization(.never)
                    .padding(12)
                    .background(Color(.secondarySystemBackground), in: RoundedRectangle(cornerRadius: 10))
                    .padding()
                    .onChange(of: query) { _, new in debounce(new) }

                List(results) { user in
                    let alreadyMember = group.members.contains(where: { $0.user_id == user.id })
                    HStack {
                        VStack(alignment: .leading) {
                            Text(user.display_name ?? user.username ?? "Unknown").font(.subheadline.weight(.semibold))
                            if let u = user.username { Text("@\(u)").font(.caption).foregroundStyle(.secondary) }
                        }
                        Spacer()
                        if alreadyMember {
                            Text("In group").font(.caption).foregroundStyle(.secondary)
                        } else {
                            Button("Add") { Task { await add(user) } }
                                .buttonStyle(.borderedProminent)
                                .controlSize(.small)
                        }
                    }
                }
                .listStyle(.plain)

                if let errorMessage {
                    Text(errorMessage).foregroundStyle(.red).font(.footnote).padding()
                }
            }
            .navigationTitle("Add member")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Done") { dismiss() }
                }
            }
        }
        .task { await runSearch("") }
    }

    private func debounce(_ q: String) {
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

    private func add(_ user: UserProfile) async {
        do {
            try await APIClient.shared.addMember(groupId: group.id, userId: user.id)
            let refreshed = try await APIClient.shared.getGroup(id: group.id)
            onUpdate(refreshed)
        } catch {
            errorMessage = error.localizedDescription
        }
    }
}

// MARK: - Create group sheet

private struct CreateGroupSheet: View {
    let onCreated: (GroupDetail) -> Void
    @Environment(\.dismiss) private var dismiss

    @State private var name = ""
    @State private var events: [Event] = []
    @State private var selectedEventId: String?
    @State private var isSubmitting = false
    @State private var errorMessage: String?

    var body: some View {
        NavigationStack {
            Form {
                Section("Group name") {
                    TextField("e.g. Rave crew", text: $name)
                }
                Section("Event") {
                    if events.isEmpty {
                        Text("Loading…").foregroundStyle(.secondary)
                    } else {
                        Picker("Event", selection: $selectedEventId) {
                            Text("None").tag(String?.none)
                            ForEach(events) { ev in
                                Text(ev.name).tag(String?.some(ev.id))
                            }
                        }
                    }
                }
                if let errorMessage {
                    Section { Text(errorMessage).foregroundStyle(.red) }
                }
            }
            .navigationTitle("New group")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Create") { Task { await submit() } }
                        .disabled(name.trimmingCharacters(in: .whitespaces).isEmpty
                                  || selectedEventId == nil || isSubmitting)
                }
            }
        }
        .task {
            do { events = try await APIClient.shared.listEvents() }
            catch { errorMessage = error.localizedDescription }
        }
    }

    private func submit() async {
        guard let eventId = selectedEventId else { return }
        isSubmitting = true
        defer { isSubmitting = false }
        do {
            let g = try await APIClient.shared.createGroup(
                name: name.trimmingCharacters(in: .whitespaces),
                eventId: eventId, memberIds: [], leaderId: nil)
            onCreated(g)
            dismiss()
        } catch {
            errorMessage = error.localizedDescription
        }
    }
}

#Preview {
    GroupTabView()
        .environment(BLEManager())
        .environment(OnboardingCoordinator())
}
