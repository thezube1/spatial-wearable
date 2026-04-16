import SwiftUI

private enum EventTabThumbnail {
    static func assetName(for eventName: String) -> String? {
        let n = eventName.lowercased()
        if n.contains("rolling loud") { return "EventRollingLoud" }
        if n.contains("summer smash") { return "EventSummerSmash" }
        if n.contains("hard summer") { return "EventHardSummer" }
        if n.contains("camp flog") || n.contains("flog gnaw") { return "EventCampFlogGnaw" }
        return nil
    }
}

private let eventTabApiDate: DateFormatter = {
    let d = DateFormatter()
    d.locale = Locale(identifier: "en_US_POSIX")
    d.dateFormat = "yyyy-MM-dd"
    return d
}()

private let eventTabShortDate: DateFormatter = {
    let d = DateFormatter()
    d.dateFormat = "M/d"
    return d
}()

struct EventTabView: View {
    let group: GroupDetail?
    let meetingPointName: String

    var body: some View {
        ScrollView(showsIndicators: false) {
            VStack(spacing: 20) {
                eventCard
                groupCard
                mapCard
                meetingStrip
            }
            .padding(.horizontal, 28)
            .padding(.top, 10)
            .padding(.bottom, 20)
        }
    }

    private var selectedEvent: Event? {
        group?.event
    }

    private var eventCard: some View {
        HStack(alignment: .top, spacing: 10) {
            Group {
                if let event = selectedEvent,
                   let assetName = EventTabThumbnail.assetName(for: event.name),
                   UIImage(named: assetName) != nil {
                    Image(assetName)
                        .resizable()
                        .scaledToFill()
                } else {
                    Color.gray.opacity(0.35)
                }
            }
            .frame(width: 100, height: 100)
            .clipShape(RoundedRectangle(cornerRadius: 5))

            VStack(alignment: .leading, spacing: 3) {
                Text(selectedEvent?.name ?? "Event")
                    .font(OnboardingStyle.font(34 / 2, weight: .semibold))
                    .foregroundStyle(.black)
                Text(venueLine(for: selectedEvent))
                    .font(OnboardingStyle.font(16))
                    .foregroundStyle(.black.opacity(0.6))
                if let category = selectedEvent?.category {
                    Text(category)
                        .font(OnboardingStyle.font(16))
                        .foregroundStyle(.black.opacity(0.6))
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)

            Text(dateRangeLabel(for: selectedEvent))
                .font(OnboardingStyle.font(14))
                .foregroundStyle(.black.opacity(0.7))
        }
        .padding(10)
        .frame(width: OnboardingStyle.fieldWidth, alignment: .topLeading)
        .frame(minHeight: 122, alignment: .topLeading)
        .background(Color.white.opacity(0.5))
        .clipShape(RoundedRectangle(cornerRadius: 5))
    }

    private var groupCard: some View {
        HStack(spacing: 10) {
            avatarCircle(id: group?.leader?.user_id ?? "lead")
            VStack(alignment: .leading, spacing: 4) {
                Text(group?.name ?? "Your group")
                    .font(OnboardingStyle.font(18, weight: .semibold))
                    .foregroundStyle(.black.opacity(0.8))
                Text("We're headed to \(selectedEvent?.name ?? "the event")!")
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

    private var mapCard: some View {
        ZStack {
            RoundedRectangle(cornerRadius: 5)
                .fill(Color.white.opacity(0.5))
                .frame(width: OnboardingStyle.fieldWidth, height: 425.894)

            GeometryReader { geo in
                let inner = CGSize(width: geo.size.width - 20, height: geo.size.height - 20)
                ZStack {
                    Group {
                        if UIImage(named: "MeetingPointMap") != nil {
                            Image("MeetingPointMap")
                                .resizable()
                                .scaledToFill()
                        } else {
                            Color.gray.opacity(0.25)
                        }
                    }
                    .frame(width: inner.width, height: inner.height)
                    .clipped()

                    ForEach(Array((group?.members ?? []).prefix(5).enumerated()), id: \.offset) { idx, member in
                        avatarCircle(id: member.user_id, size: 25)
                            .offset(x: memberOffsetX(idx, width: inner.width),
                                    y: memberOffsetY(idx, height: inner.height))
                    }

                    Circle()
                        .fill(OnboardingStyle.figmaPrimaryBlue)
                        .frame(width: 17, height: 17)
                        .overlay(Circle().stroke(Color.white.opacity(0.5), lineWidth: 2))
                        .offset(y: 12)
                }
                .frame(width: geo.size.width, height: geo.size.height)
            }
            .frame(width: OnboardingStyle.fieldWidth - 20, height: 405.894)
        }
    }

    private var meetingStrip: some View {
        HStack(spacing: 10) {
            Circle()
                .fill(OnboardingStyle.figmaPrimaryBlue)
                .frame(width: 12, height: 12)
            Text("Meeting at:")
                .font(OnboardingStyle.font(18, weight: .semibold))
                .foregroundStyle(OnboardingStyle.figmaPrimaryBlue)
            Text(meetingPointName.isEmpty ? "Not set" : meetingPointName)
                .font(OnboardingStyle.font(18))
                .foregroundStyle(OnboardingStyle.figmaPrimaryBlue)
            Spacer()
        }
        .padding(.horizontal, 72)
        .frame(width: OnboardingStyle.fieldWidth, height: 40, alignment: .leading)
        .background(Color.white.opacity(0.5))
        .clipShape(RoundedRectangle(cornerRadius: 5))
    }

    private func memberOffsetX(_ index: Int, width: CGFloat) -> CGFloat {
        let values: [CGFloat] = [-width * 0.25, -width * 0.15, 0.05, width * 0.2, width * 0.12]
        return values[index % values.count]
    }

    private func memberOffsetY(_ index: Int, height: CGFloat) -> CGFloat {
        let values: [CGFloat] = [height * 0.12, -height * 0.08, -height * 0.22, -height * 0.04, height * 0.03]
        return values[index % values.count]
    }

    private func avatarCircle(id: String, size: CGFloat = 50) -> some View {
        ZStack {
            Circle().fill(Color(white: 0.85))
            Image(systemName: OnboardingStyle.avatarSymbol(forUserId: id))
                .font(.system(size: size * 0.42))
                .foregroundStyle(.black.opacity(0.4))
        }
        .frame(width: size, height: size)
    }

    private func venueLine(for event: Event?) -> String {
        guard let event else { return "" }
        switch (event.venue.flatMap { $0.isEmpty ? nil : $0 }, event.city.flatMap { $0.isEmpty ? nil : $0 }) {
        case let (v?, c?): return "\(v), \(c)"
        case let (v?, nil): return v
        case let (nil, c?): return c
        default: return ""
        }
    }

    private func dateRangeLabel(for event: Event?) -> String {
        guard let event,
              let s = event.starts_on.flatMap({ eventTabApiDate.date(from: $0) }),
              let e = event.ends_on.flatMap({ eventTabApiDate.date(from: $0) }) else { return "" }
        return "\(eventTabShortDate.string(from: s)) - \(eventTabShortDate.string(from: e))"
    }
}

#Preview {
    EventTabView(group: nil, meetingPointName: "Ferris Wheel")
}
