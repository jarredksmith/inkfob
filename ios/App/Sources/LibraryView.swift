import SwiftUI

/// Free, public-domain books from Project Gutenberg.
/// Search tries the Gutendex catalogue first and falls back to Project Gutenberg's own search feed;
/// the popular list works even when both searches are down.
struct LibraryView: View {
    @EnvironmentObject var link: PagerLink
    @State private var query = ""
    @State private var books: [Book] = []
    @State private var searching = false
    @State private var message = "Search by title or author. Everything here is in the public domain."
    @State private var working: Int?

    struct Book: Identifiable {
        let id: Int
        let title: String
        let byline: String
        /// Where the plain-text file can be found, most reliable first. Project Gutenberg's own site
        /// can be down for days at a time, so mirrors that carry the same files come first.
        /// Mirrors file a book under its digits: book 1661 lives in 1/6/6/1661/.
        var textAddresses: [String] {
            let digits = Array(String(id))
            let folder = (digits.count > 1 ? digits.dropLast().map(String.init).joined(separator: "/") : "0") + "/\(id)/"
            let mirrors = ["https://www.mirrorservice.org/sites/ftp.ibiblio.org/pub/docs/books/gutenberg/",
                           "https://mirror.csclub.uwaterloo.ca/gutenberg/"]
            var list: [String] = []
            for mirror in mirrors {
                for name in ["\(id)-0.txt", "\(id).txt", "\(id)-8.txt"] { list.append(mirror + folder + name) }
            }
            list.append("https://www.gutenberg.org/cache/epub/\(id)/pg\(id).txt")
            return list
        }
    }

    static let popular: [Book] = [
        Book(id: 1661, title: "The Adventures of Sherlock Holmes", byline: "Arthur Conan Doyle"),
        Book(id: 2852, title: "The Hound of the Baskervilles", byline: "Arthur Conan Doyle"),
        Book(id: 244, title: "A Study in Scarlet", byline: "Arthur Conan Doyle"),
        Book(id: 2097, title: "The Sign of the Four", byline: "Arthur Conan Doyle"),
        Book(id: 345, title: "Dracula", byline: "Bram Stoker"),
        Book(id: 84, title: "Frankenstein", byline: "Mary Shelley"),
        Book(id: 43, title: "Dr Jekyll and Mr Hyde", byline: "Robert Louis Stevenson"),
        Book(id: 120, title: "Treasure Island", byline: "Robert Louis Stevenson"),
        Book(id: 35, title: "The Time Machine", byline: "H. G. Wells"),
        Book(id: 36, title: "The War of the Worlds", byline: "H. G. Wells"),
        Book(id: 215, title: "The Call of the Wild", byline: "Jack London"),
        Book(id: 76, title: "Adventures of Huckleberry Finn", byline: "Mark Twain"),
        Book(id: 74, title: "The Adventures of Tom Sawyer", byline: "Mark Twain"),
        Book(id: 11, title: "Alice's Adventures in Wonderland", byline: "Lewis Carroll"),
        Book(id: 1342, title: "Pride and Prejudice", byline: "Jane Austen"),
        Book(id: 2701, title: "Moby Dick", byline: "Herman Melville"),
        Book(id: 205, title: "Walden", byline: "Henry David Thoreau"),
        Book(id: 132, title: "The Art of War", byline: "Sun Tzu"),
        Book(id: 2680, title: "Meditations", byline: "Marcus Aurelius"),
    ]

    private struct GutendexResults: Decodable {
        struct Item: Decodable {
            struct Author: Decodable { let name: String }
            let id: Int
            let title: String
            let authors: [Author]
        }
        let results: [Item]
    }

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
            if !books.isEmpty {
                Section("Search results") { ForEach(books) { row($0) } }
            }
            Section {
                ForEach(Self.popular) { row($0) }
            } header: {
                Text("Popular")
            } footer: {
                Text("Tap a book to send it. A novel takes a few minutes over Bluetooth; keep the app open. The pager has \(ByteCountFormatter.string(fromByteCount: Int64(link.freeBytes), countStyle: .file)) free.")
            }
        }
        .navigationTitle("Free books")
        .navigationBarTitleDisplayMode(.inline)
        .onAppear { link.requestFiles() }
    }

    private func row(_ book: Book) -> some View {
        Button { fetch(book) } label: {
            VStack(alignment: .leading, spacing: 2) {
                Text(book.title).foregroundStyle(.primary)
                Text(book.byline).font(.footnote).foregroundStyle(.secondary)
                if working == book.id { Text("Getting the book...").font(.footnote) }
            }
        }
        .disabled(!link.isReady || link.transferActive || working != nil)
    }

    /// A web request that gives up after a few seconds instead of waiting for ever.
    private static func load(_ url: URL, timeout: TimeInterval) async -> Data? {
        var request = URLRequest(url: url, timeoutInterval: timeout)
        request.setValue("Inkfob/0.5 (github.com/jarredksmith/inkfob)", forHTTPHeaderField: "User-Agent")
        guard let (data, response) = try? await URLSession.shared.data(for: request),
              (response as? HTTPURLResponse)?.statusCode == 200 else { return nil }
        return data
    }

    private func search() {
        let words = query.trimmingCharacters(in: .whitespaces)
        guard !words.isEmpty else { return }
        searching = true
        message = "Searching..."
        Task {
            var found = await Self.searchGutendex(words)
            if found.isEmpty { found = await Self.searchGutenberg(words) }
            let list = found
            await MainActor.run {
                books = list
                searching = false
                message = list.isEmpty
                    ? "No results. Project Gutenberg's search may be down; the popular list below still works."
                    : "\(list.count) books found"
            }
        }
    }

    private static func searchGutendex(_ words: String) async -> [Book] {
        var parts = URLComponents(string: "https://gutendex.com/books")!
        parts.queryItems = [URLQueryItem(name: "search", value: words), URLQueryItem(name: "languages", value: "en")]
        guard let url = parts.url, let data = await load(url, timeout: 8),
              let decoded = try? JSONDecoder().decode(GutendexResults.self, from: data) else { return [] }
        return decoded.results.map { Book(id: $0.id, title: $0.title, byline: $0.authors.map(\.name).joined(separator: "; ")) }
    }

    /// Project Gutenberg's own search, which answers as an Atom feed: one <entry> per book.
    private static func searchGutenberg(_ words: String) async -> [Book] {
        var parts = URLComponents(string: "https://www.gutenberg.org/ebooks/search.opds/")!
        parts.queryItems = [URLQueryItem(name: "query", value: words)]
        guard let url = parts.url, let data = await load(url, timeout: 12) else { return [] }
        let feed = String(decoding: data, as: UTF8.self)
        var found: [Book] = []
        for chunk in feed.components(separatedBy: "<entry>").dropFirst() {
            guard let idText = between(chunk, "/ebooks/", ".opds"), let id = Int(idText),
                  let title = between(chunk, "<title>", "</title>") else { continue }
            let author = between(chunk, "<content type=\"text\">", "</content>") ?? ""
            if !found.contains(where: { $0.id == id }) {
                found.append(Book(id: id, title: plainText(title), byline: plainText(author)))
            }
        }
        return found
    }

    private static func between(_ text: String, _ start: String, _ end: String) -> String? {
        guard let a = text.range(of: start), let b = text.range(of: end, range: a.upperBound..<text.endIndex) else { return nil }
        return String(text[a.upperBound..<b.lowerBound])
    }

    private static func plainText(_ text: String) -> String {
        text.replacingOccurrences(of: "&amp;", with: "&").replacingOccurrences(of: "&quot;", with: "\"")
            .replacingOccurrences(of: "&#39;", with: "'").replacingOccurrences(of: "&apos;", with: "'")
            .replacingOccurrences(of: "&lt;", with: "<").replacingOccurrences(of: "&gt;", with: ">")
            .trimmingCharacters(in: .whitespacesAndNewlines)
    }

    private func fetch(_ book: Book) {
        working = book.id
        message = "Getting \(book.title)..."
        Task {
            var raw: String?
            for address in book.textAddresses {
                guard let url = URL(string: address), let data = await Self.load(url, timeout: 20), data.count > 2000 else { continue }
                // files ending -8.txt are in the older Latin-1 encoding
                raw = address.hasSuffix("-8.txt") ? String(data: data, encoding: .isoLatin1)
                                                  : (String(data: data, encoding: .utf8) ?? String(data: data, encoding: .isoLatin1))
                if raw != nil { break }
            }
            guard let text = raw else {
                await MainActor.run {
                    message = "Could not download that book from Project Gutenberg or its mirrors. Try again in a minute."
                    working = nil
                }
                return
            }
            let cleaned = PagerLink.plain(Self.tidy(text), limit: nil)
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
