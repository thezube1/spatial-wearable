import MapKit
import SwiftUI

struct LocationTabView: View {
    @Environment(BLEManager.self) private var ble

    @State private var cameraPosition: MapCameraPosition = .automatic
    @State private var hasCenteredOnce = false
    @State private var syncErrorMessage: String?

    var body: some View {
        NavigationStack {
            ZStack {
                Group {
                    if let location = ble.wearableLocation {
                        mapView(for: location)
                    } else {
                        placeholder
                    }
                }

                // GPS-sync overlay sits above both map and placeholder so the
                // user gets the same UI regardless of whether a stale pin is
                // still on screen.
                if shouldShowSyncOverlay {
                    syncOverlay
                        .transition(.opacity.combined(with: .scale(scale: 0.96)))
                }
            }
            .animation(.easeInOut(duration: 0.2), value: ble.gpsSyncState)
            .toolbar(.hidden, for: .navigationBar)
            .safeAreaInset(edge: .top, spacing: 0) {
                Text("Location")
                    .font(OnboardingStyle.font(28, weight: .bold))
                    .foregroundStyle(.white)
                    .shadow(color: .black.opacity(0.25), radius: 4, x: 0, y: 1)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(.horizontal, 20)
                    .padding(.vertical, 8)
                    .background(Color.black.opacity(0.12))
            }
        }
    }

    private func mapView(for location: WearableLocation) -> some View {
        let coordinate = CLLocationCoordinate2D(
            latitude: location.latitude,
            longitude: location.longitude
        )
        return Map(position: $cameraPosition) {
            Annotation("Wearable", coordinate: coordinate) {
                ZStack {
                    Circle()
                        .fill(Color.blue.opacity(0.25))
                        .frame(width: 34, height: 34)
                    Circle()
                        .fill(Color.blue)
                        .frame(width: 16, height: 16)
                        .overlay(Circle().stroke(Color.white, lineWidth: 3))
                        .shadow(radius: 2)
                }
            }
            if let peer = ble.peerLocation {
                Annotation(peer.name, coordinate: CLLocationCoordinate2D(
                    latitude: peer.latitude, longitude: peer.longitude)
                ) {
                    ZStack {
                        Circle()
                            .fill(Color.orange.opacity(0.25))
                            .frame(width: 34, height: 34)
                        Circle()
                            .fill(Color.orange)
                            .frame(width: 16, height: 16)
                            .overlay(Circle().stroke(Color.white, lineWidth: 3))
                            .shadow(radius: 2)
                    }
                }
            }
        }
        .mapControls {
            MapCompass()
            MapScaleView()
        }
        .overlay(alignment: .bottom) {
            footerBadge(updatedAt: location.receivedAt)
                .padding(.bottom, 20)
        }
        .onAppear {
            recenter(on: coordinate, animated: false)
        }
        .onChange(of: location) { _, new in
            let c = CLLocationCoordinate2D(latitude: new.latitude, longitude: new.longitude)
            recenter(on: c, animated: true)
        }
    }

    private func recenter(on coordinate: CLLocationCoordinate2D, animated: Bool) {
        let region = MKCoordinateRegion(
            center: coordinate,
            span: MKCoordinateSpan(latitudeDelta: 0.005, longitudeDelta: 0.005)
        )
        if animated {
            withAnimation(.easeInOut(duration: 0.4)) {
                cameraPosition = .region(region)
            }
        } else {
            cameraPosition = .region(region)
            hasCenteredOnce = true
        }
    }

    private func footerBadge(updatedAt: Date) -> some View {
        HStack(spacing: 8) {
            Circle()
                .fill(OnboardingStyle.figmaPrimaryBlue)
                .frame(width: 8, height: 8)
            Text("Updated \(updatedAt, style: .relative) ago")
                .font(OnboardingStyle.font(12, weight: .semibold))
                .foregroundStyle(.black.opacity(0.8))
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 8)
        .background(Color.white.opacity(0.85), in: Capsule())
    }

    private var placeholder: some View {
        VStack(spacing: 16) {
            Image(systemName: "location.slash")
                .font(.system(size: 56, weight: .light))
                .foregroundStyle(.white.opacity(0.75))
            Text(titleForState)
                .font(OnboardingStyle.font(18, weight: .semibold))
                .foregroundStyle(.white)
            Text(subtitleForState)
                .font(OnboardingStyle.font(14))
                .foregroundStyle(.white.opacity(0.8))
                .multilineTextAlignment(.center)
                .padding(.horizontal, 32)

            if shouldShowSyncCTA {
                syncButton
                    .padding(.top, 12)
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(Color.clear)
    }

    private var titleForState: String {
        switch ble.connectionState {
        case .connected: "Waiting for GPS fix"
        case .connecting, .scanning: "Connecting to wearable"
        default: "Wearable not connected"
        }
    }

    private var subtitleForState: String {
        switch ble.connectionState {
        case .connected:
            "Your wearable is connected but doesn't have a GPS lock yet. Tap Sync GPS to briefly turn radios off and force a fresh fix."
        case .connecting, .scanning:
            "Hold on while we reconnect over Bluetooth."
        default:
            "Once your wearable reconnects, its location will appear here."
        }
    }

    // MARK: - GPS sync UI

    /// Show the CTA when we're connected and don't have a fresh fix.
    /// "Fresh" is anything within the last 90 s — older than that and the
    /// fix is stale enough that re-syncing is reasonable to offer.
    private var shouldShowSyncCTA: Bool {
        if ble.connectionState != .connected { return false }
        if case .running = ble.gpsSyncState { return false }
        if let loc = ble.wearableLocation, Date().timeIntervalSince(loc.receivedAt) < 90 {
            return false
        }
        return true
    }

    /// Show the modal-style overlay any time a sync is in flight or we're
    /// briefly displaying the success/failure result.
    private var shouldShowSyncOverlay: Bool {
        switch ble.gpsSyncState {
        case .running, .success, .failed: true
        case .idle: false
        }
    }

    private var syncButton: some View {
        Button {
            triggerSync()
        } label: {
            HStack(spacing: 8) {
                Image(systemName: "location.viewfinder")
                    .font(.system(size: 16, weight: .semibold))
                Text("Sync GPS")
                    .font(OnboardingStyle.font(16, weight: .semibold))
            }
            .foregroundStyle(.white)
            .padding(.horizontal, 22)
            .padding(.vertical, 12)
            .background(
                LinearGradient(
                    colors: [
                        OnboardingStyle.figmaPrimaryBlue,
                        OnboardingStyle.figmaPrimaryBlue.opacity(0.78)
                    ],
                    startPoint: .top, endPoint: .bottom
                )
            )
            .clipShape(Capsule())
            .shadow(color: .black.opacity(0.25), radius: 6, x: 0, y: 3)
        }
        .buttonStyle(.plain)
    }

    private func triggerSync() {
        syncErrorMessage = nil
        Task {
            do {
                try await ble.requestGpsSync()
            } catch {
                await MainActor.run {
                    syncErrorMessage = error.localizedDescription
                }
            }
        }
    }

    @ViewBuilder
    private var syncOverlay: some View {
        ZStack {
            Color.black.opacity(0.55).ignoresSafeArea()
            VStack(spacing: 16) {
                switch ble.gpsSyncState {
                case .running(let secondsRemaining):
                    ZStack {
                        Circle()
                            .stroke(Color.white.opacity(0.18), lineWidth: 8)
                            .frame(width: 132, height: 132)
                        Circle()
                            .trim(from: 0, to: max(0.001, CGFloat(secondsRemaining) / 30.0))
                            .stroke(
                                OnboardingStyle.figmaPrimaryBlue,
                                style: StrokeStyle(lineWidth: 8, lineCap: .round)
                            )
                            .rotationEffect(.degrees(-90))
                            .frame(width: 132, height: 132)
                            .animation(.linear(duration: 0.9), value: secondsRemaining)
                        VStack(spacing: 2) {
                            Text("\(secondsRemaining)")
                                .font(OnboardingStyle.font(36, weight: .bold))
                                .foregroundStyle(.white)
                                .monospacedDigit()
                            Text("seconds")
                                .font(OnboardingStyle.font(11))
                                .foregroundStyle(.white.opacity(0.7))
                        }
                    }
                    Text(secondsRemaining > 0 ? "Syncing GPS…" : "Finishing up…")
                        .font(OnboardingStyle.font(18, weight: .semibold))
                        .foregroundStyle(.white)
                    Text(secondsRemaining > 0
                         ? "Radios are off so the wearable can lock onto satellites. Hold the watch up with a clear sky view."
                         : "Reconnecting to the wearable to confirm the fix.")
                        .font(OnboardingStyle.font(13))
                        .foregroundStyle(.white.opacity(0.85))
                        .multilineTextAlignment(.center)
                        .padding(.horizontal, 32)

                case .success:
                    Image(systemName: "checkmark.circle.fill")
                        .font(.system(size: 56))
                        .foregroundStyle(.green)
                    Text("GPS Synced")
                        .font(OnboardingStyle.font(20, weight: .bold))
                        .foregroundStyle(.white)
                    Text("Your wearable's location is up to date.")
                        .font(OnboardingStyle.font(13))
                        .foregroundStyle(.white.opacity(0.85))
                        .multilineTextAlignment(.center)
                        .padding(.horizontal, 32)

                case .failed:
                    Image(systemName: "exclamationmark.triangle.fill")
                        .font(.system(size: 52))
                        .foregroundStyle(.orange)
                    Text("Sync Failed")
                        .font(OnboardingStyle.font(20, weight: .bold))
                        .foregroundStyle(.white)
                    Text("The wearable couldn't get a GPS lock. Please try again, ideally outdoors with a clear view of the sky.")
                        .font(OnboardingStyle.font(13))
                        .foregroundStyle(.white.opacity(0.85))
                        .multilineTextAlignment(.center)
                        .padding(.horizontal, 32)
                    Button {
                        triggerSync()
                    } label: {
                        Text("Try Again")
                            .font(OnboardingStyle.font(15, weight: .semibold))
                            .foregroundStyle(.white)
                            .padding(.horizontal, 22)
                            .padding(.vertical, 10)
                            .background(OnboardingStyle.figmaPrimaryBlue, in: Capsule())
                    }
                    .buttonStyle(.plain)
                    .padding(.top, 4)

                case .idle:
                    EmptyView()
                }
            }
            .padding(28)
            .background(
                RoundedRectangle(cornerRadius: 20)
                    .fill(Color(white: 0.12).opacity(0.92))
            )
            .padding(.horizontal, 28)
        }
    }
}

#Preview {
    LocationTabView()
        .environment(BLEManager())
}
