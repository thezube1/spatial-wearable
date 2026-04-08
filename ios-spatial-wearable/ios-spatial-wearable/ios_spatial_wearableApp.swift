import SwiftUI

@main
struct ios_spatial_wearableApp: App {
    @State private var bleManager = BLEManager()

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environment(bleManager)
        }
    }
}
