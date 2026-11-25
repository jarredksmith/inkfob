import SwiftUI
import UniformTypeIdentifiers

/// Installs new pager firmware over Bluetooth: the latest published build, or a .bin file from Files.
struct UpdateView: View {
    @EnvironmentObject var link: PagerLink
    @State private var latest = ""
    @State private var busy = false
    @State private var message = ""
    @State private var importing = false

    // The published firmware lives in the repository itself, under firmware/release.
    private static let releaseInfo = URL(string: "https://raw.githubusercontent.com/jarredksmith/inkfob/main/firmware/release/version.txt")!
    private static let releaseFile = URL(string: "https://raw.githubusercontent.com/jarredksmith/inkfob/main/firmware/release/pager-firmware.bin")!

    var body: some View {
        NavigationStack {
            List {
                Section("Pager firmware") {
                    LabeledContent("Installed", value: link.settings["version"] ?? "Not connected")
                    LabeledContent("Latest", value: latest.isEmpty ? "Checking" : latest)
                }
                if link.transferActive {
                    Section("Installing") {
                        ProgressView(value: link.transferProgress)
                        Text("\(Int(link.transferProgress * 100))%. Keep this screen open and the pager close.")
                            .font(.footnote).foregroundStyle(.secondary)
                        Button("Cancel", role: .destructive) { link.cancelTransfer() }
                    }
                } else {
                    Section {
                        Button(busy ? "Downloading" : "Install the latest") { installLatest() }
                            .disabled(!link.isReady || busy || latest.isEmpty || latest == "None published yet")
                        Button("Install from a file") { importing = true }
                            .disabled(!link.isReady || busy)
                    } footer: {
                        Text("Takes a few minutes. The pager restarts by itself when it finishes and keeps its pairing and settings.")
                    }
                }
                if !message.isEmpty {
                    Section { Text(message).font(.callout) }
                }
            }
            .navigationTitle("Update")
            .task { await checkLatest() }
            .refreshable { await checkLatest() }
            .fileImporter(isPresented: $importing, allowedContentTypes: [.data]) { result in
                guard case .success(let url) = result else { return }
                let scoped = url.startAccessingSecurityScopedResource()
                defer { if scoped { url.stopAccessingSecurityScopedResource() } }
                guard let image = try? Data(contentsOf: url) else { message = "Could not read that file."; return }
                install(image)
            }
        }
    }

    private func checkLatest() async {
        var request = URLRequest(url: Self.releaseInfo)
        request.cachePolicy = .reloadIgnoringLocalCacheData
        guard let (data, response) = try? await URLSession.shared.data(for: request),
              (response as? HTTPURLResponse)?.statusCode == 200 else {
            latest = "None published yet"
            return
        }
        let tag = String(decoding: data, as: UTF8.self).trimmingCharacters(in: .whitespacesAndNewlines)
        latest = tag.isEmpty || tag.count > 20 ? "None published yet" : tag
    }

    private func installLatest() {
        busy = true
        message = ""
        Task {
            var request = URLRequest(url: Self.releaseFile)
            request.cachePolicy = .reloadIgnoringLocalCacheData
            let result = try? await URLSession.shared.data(for: request)
            await MainActor.run {
                busy = false
                if let image = result?.0, image.count > 200_000 {
                    install(image)
                } else {
                    message = "Could not download the firmware."
                }
            }
        }
    }

    private func install(_ image: Data) {
        message = ""
        link.sendFirmware(image) { ok, result in
            message = ok ? "Installed. The pager is restarting; it reconnects in about half a minute." : result
        }
    }
}
