import SwiftUI

struct PagerSettingsView: View {
    @EnvironmentObject var link: PagerLink
    @State private var name = ""
    @State private var line1 = ""
    @State private var line2 = ""
    @State private var qr = ""

    private static let sleepChoices = ["Never", "1 minute", "5 minutes", "15 minutes", "30 minutes", "1 hour"]
    private static let popupChoices = ["Until I dismiss it", "10 seconds", "30 seconds", "1 minute", "5 minutes"]
    private static let checkChoices = ["Off", "Every 5 minutes", "Every 15 minutes", "Every 30 minutes", "Every hour"]

    var body: some View {
        NavigationStack {
            Form {
                if !link.isReady {
                    Section { Text("Connect the pager to change its settings.").foregroundStyle(.secondary) }
                }
                Section("Screen") {
                    Toggle("Small text", isOn: flag("small"))
                    Toggle("Flip screen", isOn: flag("flip"))
                    Toggle("Fast refresh", isOn: flag("fast"))
                }
                if link.settings["btn2"] != nil {
                    Section {
                        Toggle("Use the two buttons only", isOn: flag("btn2"))
                    } header: {
                        Text("Controls")
                    } footer: {
                        Text("For a wheel that is stuck or hard to reach. Tap EXIT to go up and MENU to go down. Double-tap either button, or press both together, to select. Hold EXIT to go back, hold MENU for the menu. The wheel is ignored while this is on.")
                    }
                } else if link.isReady {
                    Section("Controls") {
                        Text("Two-button controls need pager firmware v2.0. Install it from the Update tab, then come back here.").font(.callout)
                    }
                }
                if link.settings["popup"] != nil {
                    Section {
                        Picker("Stay on screen", selection: choice("popup")) {
                            ForEach(Array(Self.popupChoices.enumerated()), id: \.offset) { index, label in Text(label).tag(index) }
                        }
                    } header: {
                        Text("Notifications")
                    } footer: {
                        Text("When a notification pops up on the pager and you leave it alone, the screen goes back to what it was showing. The notification stays in the list as new. Pressing any button keeps it open.")
                    }
                }
                Section {
                    Picker("Sleep after", selection: choice("sleep")) {
                        ForEach(Array(Self.sleepChoices.enumerated()), id: \.offset) { index, label in Text(label).tag(index) }
                    }
                    Picker("While asleep, check phone", selection: choice("check")) {
                        ForEach(Array(Self.checkChoices.enumerated()), id: \.offset) { index, label in Text(label).tag(index) }
                    }
                    Picker("Sleep screen", selection: choice("face")) {
                        Text("Status").tag(0)
                        Text("My card").tag(1)
                    }
                } header: {
                    Text("Sleep")
                } footer: {
                    Text("Asleep, the pager is disconnected and only hears from the phone at each check.")
                }
                Section {
                    TextField("Name", text: $name)
                    TextField("Line 1", text: $line1)
                    TextField("Line 2", text: $line2)
                    TextField("QR code link or text", text: $qr)
                        .textInputAutocapitalization(.never).autocorrectionDisabled()
                    Button("Save card") {
                        link.set("name", name); link.set("line1", line1); link.set("line2", line2); link.set("qr", qr)
                    }
                } header: {
                    Text("My card")
                } footer: {
                    Text("Shown under Tools > My card, and on the sleep screen if you choose it.")
                }
            }
            .disabled(!link.isReady)
            .navigationTitle("Settings")
            .onAppear { load(); link.requestSettings() }
            .onChange(of: link.settings) { _, _ in load() }
        }
    }

    private func load() {
        name = link.settings["name"] ?? name
        line1 = link.settings["line1"] ?? line1
        line2 = link.settings["line2"] ?? line2
        qr = link.settings["qr"] ?? qr
    }

    private func flag(_ key: String) -> Binding<Bool> {
        Binding(get: { link.settings[key] == "1" }, set: { link.set(key, $0 ? "1" : "0") })
    }

    private func choice(_ key: String) -> Binding<Int> {
        Binding(get: { Int(link.settings[key] ?? "0") ?? 0 }, set: { link.set(key, String($0)) })
    }
}
