import MapKit
import SwiftUI

struct LocationTabView: View {
    @Environment(BLEManager.self) private var ble

    @State private var cameraPosition: MapCameraPosition = .automatic
    @State private var hasCenteredOnce = false

    var body: some View {
        NavigationStack {
            Group {
                if let location = ble.wearableLocation {
                    mapView(for: location)
                } else {
                    placeholder
                }
            }
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
            "Your wearable is looking for satellites. This can take up to a minute outdoors."
        case .connecting, .scanning:
            "Hold on while we reconnect over Bluetooth."
        default:
            "Once your wearable reconnects, its location will appear here."
        }
    }
}

#Preview {
    LocationTabView()
        .environment(BLEManager())
}
