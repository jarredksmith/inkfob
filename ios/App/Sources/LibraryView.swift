import SwiftUI

/// Free, public-domain books from Project Gutenberg, found through the Gutendex catalogue.
struct LibraryView: View {
    @EnvironmentObject var link: PagerLink
    @State private var query = ""
    @State private var books: [Book] = []
    @State private var searching = false
    @State private var message = "Search by title or author. Everything here is in the public domain."
    @State private var working: Int?

    struct Book: Decodable, Identifiable {
        struct Author: Decodable { let name: String }
        let id: Int
        let title: String
        let authors: [Author]
        let formats: [String: String]

        var textAddress: String? {
            let plain = formats.filter { $0.key.hasPrefix("text/plain") }
            return (plain.first { $0.key.contains("utf-8") } ?? plain.first)?.value
        }
        var byline: String { authors.map(\.name).joined(separator: "; ") }
    }

    private struct Results: Decodable { let results: [Book] }

    var body: some View {
        List {
            Section {
                TextField("Title or author", text: $query)
                    .autocorrectionDisabled()
                    .onSubmit { search() }
                Button(searching ? "Searching..." : "Search") { search() }
                    .disabled(searching || query.trimmingCharacters(in: .whitespaces).isEmpty)
            } footer: {
                Text(message)
            }
            if link.transferActive {
                Section("Sending \(link.transferName)") {
                    ProgressView(value: link.transferProgress)
                    Button("Cancel", role: .destructive) { link.cancelTransfer() }
                }
            }
            Section {
                ForEach(books) { book in
                    Button { fetch(book) } label: {
                        VStack(alignment: .leading, spacing: 2) {
                            Text(book.title).foregroundStyle(.primary)
                            Text(book.byline).font(.footnote).foregroundStyle(.secondary)
                            if working == book.id { Text("Getting the book...").font(.footnote) }
                        }
                    }
                    .disabled(!link.isReady || link.transferActive || working != nil || book.textAddress == nil)
                }
            } footer: {
                if !books.isEmpty {
                    Text("Tap a book to send it. A novel takes a few minutes over Bluetooth; keep the app open. The pager has \(ByteCountFormatter.string(fromByteCount: Int64(link.freeBytes), countStyle: .file)) free.")
                }
            }
        }
        .navigationTitle("Free books")
        .navigationBarTitleDisplayMode(.inline)
        .onAppear { link.requestFiles() }
    }

    private func search() {
        var parts = URLComponents(string: "https://gutendex.com/books")!
        parts.queryItems = [URLQueryItem(name: "search", value: query), URLQueryItem(name: "languages", value: "en")]
        guard let url = parts.url else { return }
        searching = true
        Task {
            var found: [Book] = []
            var note = "Nothing found. Try fewer words."
            if let (data, _) = try? await URLSession.shared.data(from: url),
               let decoded = try? JSONDecoder().decode(Results.self, from: data) {
                found = decoded.results.filter { $0.textAddress != nil }
                if !found.isEmpty { note = "\(found.count) books" }
            } else {
                note = "Could not reach the catalogue. Check the connection and try again."
            }
            let list = found, text = note
            await MainActor.run { books = list; message = text; searching = false }
        }
    }

    private func fetch(_ book: Book) {
        guard let address = book.textAddress, let url = URL(string: address.replacingOccurrences(of: "http://", with: "https://")) else { return }
        working = book.id
        Task {
            guard let (data, _) = try? await URLSession.shared.data(from: url) else {
                await MainActor.run { message = "Could not download that book."; working = nil }
                return
            }
            let cleaned = PagerLink.plain(Self.tidy(String(decoding: data, as: UTF8.self)), limit: nil)
            await MainActor.run {
                working = nil
                var text = cleaned
                var note = ""
                let room = max(0, link.freeBytes - 20_000)
                if room < 10_000 {
                    message = "The pager is full. Delete a file first."
                    return
                }
                if text.utf8.count > room {
                    let share = Int(Double(room) / Double(text.utf8.count) * 100)
                    text = String(text.prefix(room))
                    note = " Only the first \(share)% fit."
                }
                message = "Sending \(book.title)..."
                let tail = note
                link.sendFile(name: String(book.title.prefix(27)) + ".txt", text: text) { ok, result in
                    message = ok ? "Sent. Find it on the pager under Menu > My files.\(tail)" : result
                }
            }
        }
    }

    /// Drops Project Gutenberg's front and back matter and rejoins the hard-wrapped lines of each paragraph.
    static func tidy(_ raw: String) -> String {
        var text = raw.replacingOccurrences(of: "\r\n", with: "\n")
        if let start = text.range(of: "*** START OF"), let lineEnd = text[start.upperBound...].firstIndex(of: "\n") {
            text = String(text[text.index(after: lineEnd)...])
        }
        if let end = text.range(of: "*** END OF") {
            text = String(text[..<end.lowerBound])
        }
        let paragraphs = text.components(separatedBy: "\n\n").map { paragraph in
            paragraph.split(separator: "\n").map { $0.trimmingCharacters(in: .whitespaces) }.joined(separator: " ")
        }.filter { !$0.isEmpty }
        return paragraphs.joined(separator: "\n\n")
    }
}

/// Writes a flashcard deck: one card per line, question and answer separated by a bar.
struct DeckEditor: View {
    var onSend: (String, String) -> Void
    @Environment(\.dismiss) private var dismiss
    @State private var name = ""
    @State private var text = ""

    private var cards: Int { text.split(whereSeparator: \.isNewline).filter { $0.contains("|") }.count }

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    TextField("Deck name (for example Spanish)", text: $name)
                }
                Section {
                    TextEditor(text: $text).frame(minHeight: 260)
                        .autocorrectionDisabled()
                } header: {
                    Text("Cards")
                } footer: {
                    Text("One card per line: question | answer. For example:  hola | hello.  \(cards) cards so far. On the pager, press the wheel to flip and turn it for the next card.")
                }
            }
            .navigationTitle("New deck")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Send") { onSend(name, text); dismiss() }
                        .disabled(name.trimmingCharacters(in: .whitespaces).isEmpty || cards == 0)
                }
            }
        }
    }
}
