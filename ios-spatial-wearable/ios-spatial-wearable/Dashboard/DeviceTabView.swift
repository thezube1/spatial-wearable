import SwiftUI

struct DeviceTabView: View {
    @Binding var device: Device?
    @Binding var errorMessage: String?

    var body: some View {
        ScrollView(showsIndicators: false) {
            VStack(spacing: 20) {
                titleCard
                statusRow(label: "Connection Status", value: "Connected", dot: Color(red: 0.79, green: 0.596, blue: 1))
                statusRow(label: "Battery Level", value: batteryValue, dot: nil)
                statusRow(label: "Last Synced", value: lastSyncedText, dot: nil)

                if let errorMessage {
                    Text(errorMessage)
                        .font(OnboardingStyle.font(12))
                        .foregroundStyle(.red.opacity(0.9))
                        .frame(width: OnboardingStyle.fieldWidth, alignment: .leading)
                }
            }
            .padding(.horizontal, 28)
            .padding(.top, 10)
            .padding(.bottom, 20)
        }
    }

    private var titleCard: some View {
        HStack {
            Text("Device Status")
                .font(OnboardingStyle.font(34 / 2, weight: .semibold))
                .foregroundStyle(.black.opacity(0.6))
            Spacer()
        }
        .frame(width: OnboardingStyle.fieldWidth)
    }

    private var batteryValue: String {
        return "85%"
    }

    private var lastSyncedText: String {
        device?.linked_at.map { _ in "Just now" } ?? "Just now"
    }

    private func statusRow(label: String, value: String, dot: Color?) -> some View {
        HStack {
            Text(label)
                .font(OnboardingStyle.font(14))
                .foregroundStyle(.black.opacity(0.55))
            Spacer()
            if let dot {
                Circle()
                    .fill(dot)
                    .frame(width: 9, height: 9)
                Text(value)
                    .font(OnboardingStyle.font(18, weight: .semibold))
                    .foregroundStyle(.black)
            } else {
                Text(value)
                    .font(OnboardingStyle.font(18, weight: .semibold))
                    .foregroundStyle(.black)
            }
        }
        .padding(.horizontal, 10)
        .frame(width: OnboardingStyle.fieldWidth, height: 54)
        .background(Color.white.opacity(0.5))
        .clipShape(RoundedRectangle(cornerRadius: 5))
    }
}

#Preview {
    struct PreviewWrapper: View {
        @State var device: Device?
        @State var error: String?
        var body: some View {
            DeviceTabView(device: $device, errorMessage: $error)
                .environment(BLEManager())
        }
    }
    return PreviewWrapper()
}
