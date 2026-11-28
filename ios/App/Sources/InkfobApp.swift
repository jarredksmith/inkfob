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
                .environmentObject(hub.extras)
                .environmentObject(hub.ringer)
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
    let extras = Extras()
    let ringer = Ringer()

    init() {
        link.onRefreshRequest = { [weak self] in self?.pushHomeLines() }
        link.onFindPhone = { [weak self] in self?.ringer.ring() }
    }

    func cameToFront() {
        feeds.refreshPermissions()
        extras.refreshPermissions()
        ringer.refresh()
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
            let extra = await extras.gather()
            await MainActor.run {
                if link.isReady {
                    link.sendHomeLines(lines)
                    link.sendMore(extra.more)
                    link.sendDayLine(extra.dayLine)
                    if let fix = feeds.lastFix {
                        link.sendFix(lat: fix.lat, lon: fix.lon, accuracy: fix.accuracy, age: fix.age)
                    }
                }
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
