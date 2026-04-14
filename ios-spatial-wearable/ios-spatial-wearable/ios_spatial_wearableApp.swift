import SwiftUI

@main
struct ios_spatial_wearableApp: App {
    @State private var bleManager = BLEManager()
    @State private var auth = AuthViewModel()
    @State private var coordinator = OnboardingCoordinator()

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environment(bleManager)
                .environment(auth)
                .environment(coordinator)
        }
    }
}
