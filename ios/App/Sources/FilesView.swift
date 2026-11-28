import SwiftUI
import UniformTypeIdentifiers

struct FilesView: View {
    @EnvironmentObject var link: PagerLink
    @State private var writing = false
    @State private var importing = false
    @State private var makingDeck = false
    @State private var makingTodo = false
    @State private var message = ""

    /// The photo face is stored as a file too, but it is not something to read.
    private var shownFiles: [PagerLink.PagerFile] { link.files.filter { !$0.name.hasSuffix(".img") && !$0.name.hasSuffix(".list") } }

    var body: some View {
        NavigationStack {
            List {
                if link.transferActive {
                    Section("Sending \(link.transferName)") {
                        ProgressView(value: link.transferProgress)
                        Button("Cancel", role: .destructive) { link.cancelTransfer() }
                    }
                }
                Section {
                    Button { writing = true } label: { Label("Write a note", systemImage: "square.and.pencil") }
                    Button { importing = true } label: { Label("Send a text file or book", systemImage: "tray.and.arrow.down") }
                    Button { makingDeck = true } label: { Label("Make a flashcard deck", systemImage: "rectangle.on.rectangle") }
                    Button { makingTodo = true } label: { Label("To-do list", systemImage: "checklist") }
                    NavigationLink { LibraryView() } label: { Label("Get a free book", systemImage: "books.vertical") }
                } footer: {
                    Text("Plain text (.txt) works best. Files appear on the pager under Menu > Reading > My files. Flashcard decks open as cards.")
                }
                .disabled(!link.isReady || link.transferActive)
                Section("On the pager") {
                    if link.files.isEmpty {
                        Text(link.isReady ? "Nothing yet" : "Connect the pager to see its files").foregroundStyle(.secondary)
                    }
                    ForEach(shownFiles) { file in
                        HStack {
                            Text(file.name)
                            Spacer()
                            Text(ByteCountFormatter.string(fromByteCount: Int64(file.size), countStyle: .file))
                                .foregroundStyle(.secondary)
                        }
                    }
                    .onDelete { offsets in
                        for name in offsets.map({ shownFiles[$0].name }) { link.deleteFile(name) }
                    }
                    if link.isReady {
                        Text("\(ByteCountFormatter.string(fromByteCount: Int64(link.freeBytes), countStyle: .file)) free")
                            .font(.footnote).foregroundStyle(.secondary)
                    }
                }
                if !message.isEmpty {
                    Section { Text(message).font(.callout) }
                }
            }
            .navigationTitle("Files")
            .refreshable { link.requestFiles() }
            .sheet(isPresented: $writing) {
                NoteEditor { name, text in send(name: name, text: text) }
            }
            .sheet(isPresented: $makingTodo) {
                TodoEditor { text in send(name: "todo.list", text: text) }
            }
            .sheet(isPresented: $makingDeck) {
                DeckEditor { name, text in send(name: String(name.prefix(24)) + ".cards", text: text) }
            }
            .fileImporter(isPresented: $importing, allowedContentTypes: [.plainText, .text]) { result in
                guard case .success(let url) = result else { return }
                let scoped = url.startAccessingSecurityScopedResource()
                defer { if scoped { url.stopAccessingSecurityScopedResource() } }
                guard let data = try? Data(contentsOf: url) else { message = "Could not read that file."; return }
                let text = String(data: data, encoding: .utf8) ?? String(decoding: data, as: UTF8.self)
                send(name: url.deletingPathExtension().lastPathComponent, text: text)
            }
        }
    }

    private func send(name: String, text: String) {
        message = ""
        link.sendFile(name: name, text: text) { _, result in message = result }
    }
}

struct NoteEditor: View {
    var onSend: (String, String) -> Void
    @Environment(\.dismiss) private var dismiss
    @State private var name = ""
    @State private var text = ""

    var body: some View {
        NavigationStack {
            Form {
                TextField("Name (for example Packing list)", text: $name)
                TextEditor(text: $text).frame(minHeight: 260)
            }
            .navigationTitle("New note")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Send") { onSend(name, text); dismiss() }
                        .disabled(name.trimmingCharacters(in: .whitespaces).isEmpty || text.isEmpty)
                }
            }
        }
    }
}

/// One item per line. Sending replaces the list on the pager, so ticks made there start again.
struct TodoEditor: View {
    var onSend: (String) -> Void
    @Environment(\.dismiss) private var dismiss
    @AppStorage("todoText") private var text = ""

    private var items: [String] {
        text.split(whereSeparator: \.isNewline).map { $0.trimmingCharacters(in: .whitespaces) }.filter { !$0.isEmpty }
    }

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    TextEditor(text: $text).frame(minHeight: 280)
                } header: {
                    Text("Items")
                } footer: {
                    Text("One item per line, up to 40. \(items.count) so far. On the pager it is under Menu > To-do list, or pick the To-do face on the Faces tab to keep it on the home screen. Scroll to an item and press select to tick it; ticked items turn grey with a line through them. Sending again replaces the list and clears the ticks.")
                }
            }
            .navigationTitle("To-do list")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Send") { onSend(items.prefix(40).map { "- " + $0 }.joined(separator: "\n")); dismiss() }
                        .disabled(items.isEmpty)
                }
            }
        }
    }
}
