import SwiftUI

struct GroupTabView: View {
    let group: GroupDetail?

    var body: some View {
        ScrollView(showsIndicators: false) {
            VStack(spacing: 20) {
                headerCard

                ForEach(group?.members ?? []) { member in
                    memberRow(member)
                }
            }
            .padding(.horizontal, 28)
            .padding(.top, 10)
            .padding(.bottom, 20)
        }
    }

    private var headerCard: some View {
        HStack(spacing: 10) {
            avatarCircle(id: group?.leader?.user_id ?? "group-lead")
            VStack(alignment: .leading, spacing: 4) {
                Text(group?.name ?? "Your group")
                    .font(OnboardingStyle.font(18, weight: .semibold))
                    .foregroundStyle(.black.opacity(0.8))
                Text("We're headed to \(group?.event?.name ?? "your event")!")
                    .font(OnboardingStyle.font(16))
                    .foregroundStyle(.black.opacity(0.35))
            }
            .frame(maxWidth: .infinity, alignment: .leading)
        }
        .padding(10)
        .frame(width: OnboardingStyle.fieldWidth, alignment: .leading)
        .frame(minHeight: 81, alignment: .leading)
        .background(Color.white.opacity(0.5))
        .clipShape(RoundedRectangle(cornerRadius: 5))
    }

    private func memberRow(_ member: GroupMember) -> some View {
        HStack(spacing: 10) {
            avatarCircle(id: member.user_id)
            VStack(alignment: .leading, spacing: 4) {
                Text(member.display_name ?? member.username ?? "Member")
                    .font(OnboardingStyle.font(34 / 2, weight: .semibold))
                    .foregroundStyle(.black)
                if let username = member.username {
                    Text("@\(username)")
                        .font(OnboardingStyle.font(16))
                        .foregroundStyle(.black.opacity(0.45))
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)

            HStack(spacing: 4) {
                Circle()
                    .fill(OnboardingStyle.memberStatusColor(forUserId: member.user_id))
                    .frame(width: 9, height: 9)
                Text("Connected")
                    .font(OnboardingStyle.font(14))
                    .foregroundStyle(.black)
            }
        }
        .padding(10)
        .frame(width: OnboardingStyle.fieldWidth, alignment: .leading)
        .frame(minHeight: 81, alignment: .leading)
        .background(Color.white.opacity(0.5))
        .clipShape(RoundedRectangle(cornerRadius: 5))
    }

    private func avatarCircle(id: String) -> some View {
        ZStack {
            Circle()
                .fill(Color(white: 0.85))
            Image(systemName: OnboardingStyle.avatarSymbol(forUserId: id))
                .font(.system(size: 22))
                .foregroundStyle(.black.opacity(0.4))
        }
        .frame(width: 50, height: 50)
    }
}

#Preview {
    GroupTabView(group: nil)
}
