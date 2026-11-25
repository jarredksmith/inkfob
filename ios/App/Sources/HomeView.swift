import SwiftUI

struct HomeView: View {
    @EnvironmentObject var hub: Hub
    @EnvironmentObject var link: PagerLink
    @EnvironmentObject var feeds: Feeds

    var body: some View {
        NavigationStack {
            List {
                Section {
                    StatusRow()
                }
                if link.state == .needsRepair {
                    Section("One-time fix") {
                        Text("The pager is connected, but this iPhone is remembering it from before it could talk to the app. Open Settings > Bluetooth, tap the (i) beside Pager, choose Forget This Device, then pair it again.")
                            .font(.callout)
                    }
                }
                Section("On the pager's home screen") {
                    PagerPreview(lines: previewLines)
                        .listRowInsets(EdgeInsets())
                    Toggle("Weather", isOn: $feeds.showWeather)
                    Toggle("Next calendar events", isOn: $feeds.showCalendar)
                    Toggle("Celsius", isOn: $feeds.useCelsius)
                    Toggle("Steps today", isOn: $feeds.showSteps)
                    Button("Send now") { hub.pushHomeLines() }
                        .disabled(!link.isReady)
                    if let at = link.lastSentAt {
                        Text("Last sent \(at.formatted(date: .omitted, time: .shortened)). The pager also asks for an update every 30 minutes.")
                            .font(.footnote).foregroundStyle(.secondary)
                    }
                }
                if !feeds.locationAllowed || !feeds.calendarAllowed {
                    Section("Permissions") {
                        if !feeds.locationAllowed {
                            Text("Location is needed for the local forecast.").font(.callout)
                        }
                        if !feeds.calendarAllowed {
                            Text("Calendar access is needed to show your next events.").font(.callout)
                        }
                        Button("Allow access") { feeds.askForAccess() }
                    }
                }
            }
            .navigationTitle("Inkfob")
            .onChange(of: feeds.showWeather) { _, _ in hub.pushHomeLines() }
            .onChange(of: feeds.showCalendar) { _, _ in hub.pushHomeLines() }
            .onChange(of: feeds.useCelsius) { _, _ in hub.pushHomeLines() }
            .onChange(of: feeds.showSteps) { _, _ in hub.pushHomeLines() }
        }
    }

    private var previewLines: [String] {
        link.lastSent.isEmpty ? ["Nothing sent yet"] : link.lastSent.components(separatedBy: "\n")
    }
}

struct StatusRow: View {
    @EnvironmentObject var link: PagerLink

    var body: some View {
        HStack(spacing: 12) {
            Circle().fill(color).frame(width: 12, height: 12)
            VStack(alignment: .leading, spacing: 2) {
                Text(title).font(.headline)
                Text(detail).font(.footnote).foregroundStyle(.secondary)
            }
        }
    }

    private var color: Color {
        switch link.state {
        case .ready: return .green
        case .connecting: return .yellow
        case .needsRepair: return .orange
        default: return .gray
        }
    }

    private var title: String {
        switch link.state {
        case .ready: return "Pager connected"
        case .connecting: return "Connecting"
        case .needsRepair: return "Pager needs re-pairing"
        case .bluetoothOff: return "Bluetooth is off"
        case .looking: return "Looking for the pager"
        }
    }

    private var detail: String {
        switch link.state {
        case .ready: return "Firmware \(link.settings["version"] ?? "unknown")"
        case .looking: return "Pair it in Settings > Bluetooth first, and keep it awake and nearby."
        case .bluetoothOff: return "Turn Bluetooth on to reach the pager."
        default: return ""
        }
    }
}

/// A rough picture of the pager's e-ink home screen.
struct PagerPreview: View {
    let lines: [String]

    var body: some View {
        VStack(spacing: 4) {
            Text(Date().formatted(date: .omitted, time: .shortened))
                .font(.system(size: 34, weight: .heavy, design: .rounded))
            ForEach(Array(lines.enumerated()), id: \.offset) { _, line in
                Text(line).font(.system(size: 13, design: .monospaced)).lineLimit(1)
            }
        }
        .foregroundStyle(.black)
        .frame(maxWidth: .infinity)
        .padding(.vertical, 16)
        .background(Color(white: 0.88))
    }
}
