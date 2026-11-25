import SwiftUI
import UIKit

@main
struct InkfobApp: App {
    @StateObject private var hub = Hub()
    @Environment(\.scenePhase) private var phase

    var body: some Scene {
        WindowGroup {
            RootView()
                .environmentObject(hub)
                .environmentObject(hub.link)
                .environmentObject(hub.feeds)
                .onChange(of: phase) { _, now in
                    if now == .active { hub.cameToFront() }
                }
        }
    }
}

/// Ties the Bluetooth link to the weather and calendar feeds.
final class Hub: ObservableObject {
    let link = PagerLink()
    let feeds = Feeds()

    init() {
        link.onRefreshRequest = { [weak self] in self?.pushHomeLines() }
    }

    func cameToFront() {
        feeds.refreshPermissions()
        feeds.updateLocation()
        link.look()
        if link.isReady { pushHomeLines() }
    }

    /// Fetches weather and events and sends them. Safe to call from the background:
    /// the pager's own request wakes the app for a few seconds, which is enough.
    func pushHomeLines() {
        let task = UIApplication.shared.beginBackgroundTask(withName: "inkfob.push")
        Task {
            let lines = await feeds.gather()
            await MainActor.run {
                if link.isReady { link.sendHomeLines(lines) }
                UIApplication.shared.endBackgroundTask(task)
            }
        }
    }
}

struct RootView: View {
    var body: some View {
        TabView {
            HomeView().tabItem { Label("Pager", systemImage: "rectangle.inset.filled") }
            FacesView().tabItem { Label("Faces", systemImage: "square.on.square") }
            FilesView().tabItem { Label("Files", systemImage: "doc.text") }
            PagerSettingsView().tabItem { Label("Settings", systemImage: "slider.horizontal.3") }
            UpdateView().tabItem { Label("Update", systemImage: "arrow.down.circle") }
        }
    }
}
