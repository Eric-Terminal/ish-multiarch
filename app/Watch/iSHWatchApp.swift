import SwiftUI

@main
struct iSHWatchApp: App {
    @StateObject private var runtime = WatchRuntime()

    var body: some Scene {
        WindowGroup {
            ContentView(runtime: runtime)
        }
    }
}
