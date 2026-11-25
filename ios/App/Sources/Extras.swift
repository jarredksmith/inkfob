import EventKit
import Foundation
import SwiftUI

struct Countdown: Codable, Identifiable, Equatable {
    var id = UUID()
    var name: String
    var date: Date
}

/// The slower, optional lines: a countdown, a reminder, a stock or score, and a line for the day.
final class Extras: ObservableObject {
    static let leagues: [(name: String, path: String)] = [
        ("NFL", "football/nfl"), ("NBA", "basketball/nba"), ("MLB", "baseball/mlb"), ("NHL", "hockey/nhl"),
        ("College football", "football/college-football"), ("College basketball", "basketball/mens-college-basketball"),
    ]

    @Published var countdowns: [Countdown] = [] { didSet { save() } }
    @Published var showReminders: Bool = UserDefaults.standard.bool(forKey: "showReminders") {
        didSet { UserDefaults.standard.set(showReminders, forKey: "showReminders") }
    }
    /// 0 off, 1 stocks, 2 sports
    @Published var tickerMode: Int = UserDefaults.standard.integer(forKey: "tickerMode") {
        didSet { UserDefaults.standard.set(tickerMode, forKey: "tickerMode") }
    }
    @Published var stockSymbols: String = UserDefaults.standard.string(forKey: "stockSymbols") ?? "AAPL" {
        didSet { UserDefaults.standard.set(stockSymbols, forKey: "stockSymbols") }
    }
    @Published var league: Int = UserDefaults.standard.integer(forKey: "league") {
        didSet { UserDefaults.standard.set(league, forKey: "league") }
    }
    @Published var team: String = UserDefaults.standard.string(forKey: "team") ?? "" {
        didSet { UserDefaults.standard.set(team, forKey: "team") }
    }
    /// 0 off, 1 daily nudge, 2 on this day
    @Published var dayLineMode: Int = UserDefaults.standard.object(forKey: "dayLineMode") as? Int ?? 1 {
        didSet { UserDefaults.standard.set(dayLineMode, forKey: "dayLineMode") }
    }
    @Published var remindersAllowed = false
    @Published var lastLines: [String] = []

    private let store = EKEventStore()

    init() {
        if let data = UserDefaults.standard.data(forKey: "countdowns"),
           let saved = try? JSONDecoder().decode([Countdown].self, from: data) {
            countdowns = saved
        }
        refreshPermissions()
    }

    private func save() {
        if let data = try? JSONEncoder().encode(countdowns) { UserDefaults.standard.set(data, forKey: "countdowns") }
    }

    func refreshPermissions() {
        remindersAllowed = EKEventStore.authorizationStatus(for: .reminder) == .fullAccess
    }

    func askForReminders() {
        store.requestFullAccessToReminders { [weak self] _, _ in
            DispatchQueue.main.async { self?.refreshPermissions() }
        }
    }

    /// Returns the three extra slots (countdown, reminder, ticker) and the line for the day.
    func gather() async -> (more: [String], dayLine: String) {
        let countdown = countdownLine()
        let reminder = showReminders ? await reminderLine() : ""
        var ticker = ""
        if tickerMode == 1 { ticker = await stockLine() }
        if tickerMode == 2 { ticker = await scoreLine() }
        var dayLine = ""
        if dayLineMode == 1 { dayLine = Self.nudge(for: Date()) }
        if dayLineMode == 2 { dayLine = await onThisDay() }
        let more = [countdown, reminder, ticker]
        let shown = more.filter { !$0.isEmpty } + (dayLine.isEmpty ? [] : [dayLine])
        await MainActor.run { lastLines = shown }
        return (more, dayLine)
    }

    // MARK: Countdown

    func countdownLine() -> String {
        let today = Calendar.current.startOfDay(for: Date())
        let upcoming = countdowns
            .map { ($0.name, Calendar.current.dateComponents([.day], from: today, to: Calendar.current.startOfDay(for: $0.date)).day ?? 0) }
            .filter { $0.1 >= 0 }
            .sorted { $0.1 < $1.1 }
        guard let next = upcoming.first else { return "" }
        if next.1 == 0 { return "\(next.0) is today" }
        return "\(next.0) in \(next.1) day\(next.1 == 1 ? "" : "s")"
    }

    // MARK: Reminders

    private func reminderLine() async -> String {
        guard EKEventStore.authorizationStatus(for: .reminder) == .fullAccess else { return "" }
        let end = Calendar.current.date(byAdding: .day, value: 1, to: Calendar.current.startOfDay(for: Date()))
        let predicate = store.predicateForIncompleteReminders(withDueDateStarting: nil, ending: end, calendars: nil)
        let titles: [String] = await withCheckedContinuation { done in
            store.fetchReminders(matching: predicate) { found in
                done.resume(returning: (found ?? []).compactMap { $0.title })
            }
        }
        guard let first = titles.first else { return "" }
        return titles.count > 1 ? "To do: \(first) (+\(titles.count - 1))" : "To do: \(first)"
    }

    // MARK: Stocks and scores

    private func fetch(_ address: String) async -> Data? {
        guard let url = URL(string: address) else { return nil }
        var request = URLRequest(url: url, timeoutInterval: 8)
        request.setValue("Inkfob/0.3 (github.com/jarredksmith/inkfob)", forHTTPHeaderField: "User-Agent")
        guard let (data, response) = try? await URLSession.shared.data(for: request),
              (response as? HTTPURLResponse)?.statusCode == 200 else { return nil }
        return data
    }

    /// Prices from Stooq's free quote file. The change shown is since today's open.
    private func stockLine() async -> String {
        let symbols = stockSymbols.uppercased().split(whereSeparator: { !$0.isLetter && !$0.isNumber && $0 != "." })
            .map(String.init).prefix(2)
        var parts: [String] = []
        for symbol in symbols {
            let code = symbol.contains(".") ? symbol.lowercased() : symbol.lowercased() + ".us"
            guard let data = await fetch("https://stooq.com/q/l/?s=\(code)&f=sd2t2ohlc&h&e=csv") else { continue }
            let rows = String(decoding: data, as: UTF8.self).split(whereSeparator: \.isNewline)
            guard rows.count >= 2 else { continue }
            let fields = rows[1].split(separator: ",", omittingEmptySubsequences: false)
            guard fields.count >= 7, let open = Double(fields[3]), let close = Double(fields[6]), open > 0 else { continue }
            let change = (close - open) / open * 100
            parts.append(String(format: "%@ %.2f %+.1f%%", symbol, close, change))
        }
        return parts.joined(separator: "  ")
    }

    /// Today's game for one team, from ESPN's public scoreboard.
    private func scoreLine() async -> String {
        let wanted = team.trimmingCharacters(in: .whitespaces).uppercased()
        guard !wanted.isEmpty, Self.leagues.indices.contains(league) else { return "" }
        guard let data = await fetch("https://site.api.espn.com/apis/site/v2/sports/\(Self.leagues[league].path)/scoreboard"),
              let root = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let events = root["events"] as? [[String: Any]] else { return "" }
        for event in events {
            guard let competition = (event["competitions"] as? [[String: Any]])?.first,
                  let sides = competition["competitors"] as? [[String: Any]], sides.count == 2 else { continue }
            let names = sides.map { (($0["team"] as? [String: Any])?["abbreviation"] as? String ?? "?").uppercased() }
            guard names.contains(wanted) else { continue }
            let scores = sides.map { $0["score"] as? String ?? "" }
            let status = (event["status"] as? [String: Any])?["type"] as? [String: Any]
            let state = status?["state"] as? String ?? ""
            let detail = status?["shortDetail"] as? String ?? ""
            let mine = names.firstIndex(of: wanted) ?? 0
            let other = 1 - mine
            if state == "pre" { return "\(names[mine]) vs \(names[other])  \(detail)" }
            return "\(names[mine]) \(scores[mine])  \(names[other]) \(scores[other])  \(detail)"
        }
        return "\(wanted): no game today"
    }

    // MARK: The line for the day

    private func onThisDay() async -> String {
        let parts = Calendar.current.dateComponents([.month, .day, .year], from: Date())
        let address = String(format: "https://api.wikimedia.org/feed/v1/wikipedia/en/onthisday/selected/%02d/%02d",
                             parts.month ?? 1, parts.day ?? 1)
        guard let data = await fetch(address),
              let root = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let items = root["selected"] as? [[String: Any]], !items.isEmpty else { return "" }
        let item = items[(parts.year ?? 0) % items.count]
        guard let text = item["text"] as? String else { return "" }
        let year = (item["year"] as? Int).map { "On this day in \($0): " } ?? "On this day: "
        return year + text
    }

    static let nudges = [
        "Do the smallest useful thing first.", "Drink some water before the next coffee.",
        "Send the message you have been putting off.", "Ten minutes outside counts.",
        "Finish one thing before starting two.", "Say thank you to someone who is not expecting it.",
        "Stand up and stretch. Right now is fine.", "What would make today feel done? Do that.",
        "Leave it better than you found it.", "Ask the question. Guessing is slower.",
        "Slow is smooth, and smooth gets there.", "Write it down so you can stop holding it.",
        "The first draft only has to exist.", "Call instead of texting, just once today.",
        "Protect one hour for the hard thing.", "Eat lunch away from the screen.",
        "Notice something good and say it out loud.", "Done today beats perfect next week.",
        "Tidy one small corner.", "Take the stairs if there are stairs.",
        "Give the plan a deadline.", "Learn one new thing and tell somebody.",
        "Put the phone down for the next conversation.", "Fix the tiny annoyance you keep stepping over.",
        "Start before you feel ready.", "Go to bed a little earlier tonight.",
        "Make something, however small.", "Check in on a friend.",
        "Say no to one thing so you can say yes to another.", "Look up. The sky is doing something.",
        "Back up the thing you would hate to lose.",
    ]

    static func nudge(for date: Date) -> String {
        let day = Calendar.current.ordinality(of: .day, in: .year, for: date) ?? 1
        return nudges[day % nudges.count]
    }
}

/// Settings for the extra lines, reached from the Pager tab.
struct ExtrasView: View {
    @EnvironmentObject var hub: Hub
    @EnvironmentObject var extras: Extras
    @EnvironmentObject var ringer: Ringer
    @EnvironmentObject var link: PagerLink
    @State private var newName = ""
    @State private var newDate = Date().addingTimeInterval(7 * 86400)

    var body: some View {
        Form {
            Section {
                ForEach(extras.countdowns) { item in
                    HStack {
                        Text(item.name)
                        Spacer()
                        Text(item.date.formatted(date: .abbreviated, time: .omitted)).foregroundStyle(.secondary)
                    }
                }
                .onDelete { extras.countdowns.remove(atOffsets: $0) }
                TextField("Name (for example Vacation)", text: $newName)
                DatePicker("Date", selection: $newDate, displayedComponents: .date)
                Button("Add countdown") {
                    extras.countdowns.append(Countdown(name: newName.trimmingCharacters(in: .whitespaces), date: newDate))
                    newName = ""
                }
                .disabled(newName.trimmingCharacters(in: .whitespaces).isEmpty)
            } header: {
                Text("Countdowns")
            } footer: {
                Text("The pager shows the nearest one, like \"Vacation in 12 days\".")
            }

            Section("Reminders") {
                Toggle("Show what is due today", isOn: $extras.showReminders)
                if extras.showReminders && !extras.remindersAllowed {
                    Button("Allow access to Reminders") { extras.askForReminders() }
                }
            }

            Section {
                Picker("Show", selection: $extras.tickerMode) {
                    Text("Nothing").tag(0)
                    Text("Stock prices").tag(1)
                    Text("A team's game").tag(2)
                }
                if extras.tickerMode == 1 {
                    TextField("Symbols, up to two (AAPL, MSFT)", text: $extras.stockSymbols)
                        .textInputAutocapitalization(.characters).autocorrectionDisabled()
                }
                if extras.tickerMode == 2 {
                    Picker("League", selection: $extras.league) {
                        ForEach(Array(Extras.leagues.enumerated()), id: \.offset) { index, item in Text(item.name).tag(index) }
                    }
                    TextField("Team letters on the scoreboard (OKC)", text: $extras.team)
                        .textInputAutocapitalization(.characters).autocorrectionDisabled()
                }
            } header: {
                Text("Stock or score")
            } footer: {
                Text("Refreshed when the pager asks, about every 30 minutes, so it is a glance and not a live feed.")
            }

            Section("Line for the day") {
                Picker("Show", selection: $extras.dayLineMode) {
                    Text("Nothing").tag(0)
                    Text("Daily nudge").tag(1)
                    Text("On this day in history").tag(2)
                }
            }

            Section {
                if ringer.notificationsAllowed {
                    Label("Ready. On the pager: Tools > Find my phone.", systemImage: "checkmark.circle")
                } else {
                    Button("Allow notifications for Find my phone") { ringer.askPermission() }
                }
                Button(ringer.ringing ? "Stop" : "Test the ring") { ringer.ringing ? ringer.stop() : ringer.ring() }
            } header: {
                Text("Find my phone")
            } footer: {
                Text("The pager asks this app to ring. It works best when Inkfob has been opened recently; iOS may hold back the sound if the app has been closed for a long time.")
            }

            Section {
                Button("Send to pager now") { hub.pushHomeLines() }.disabled(!link.isReady)
                ForEach(extras.lastLines, id: \.self) { Text($0).font(.footnote).foregroundStyle(.secondary) }
            } footer: {
                Text("These show on the pager under Menu > Today, and can go in the corners of the Custom face.")
            }
        }
        .navigationTitle("More for the pager")
        .navigationBarTitleDisplayMode(.inline)
        .onAppear { extras.refreshPermissions(); ringer.refresh() }
    }
}
