import SwiftUI
import UniformTypeIdentifiers

struct FilesView: View {
    @EnvironmentObject var link: PagerLink
    @State private var writing = false
    @State private var importing = false
    @State private var message = ""

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
                } footer: {
                    Text("Plain text (.txt) works best. Files appear on the pager under Menu > My files.")
                }
                .disabled(!link.isReady || link.transferActive)
                Section("On the pager") {
                    if link.files.isEmpty {
                        Text(link.isReady ? "Nothing yet" : "Connect the pager to see its files").foregroundStyle(.secondary)
                    }
                    ForEach(link.files) { file in
                        HStack {
                            Text(file.name)
                            Spacer()
                            Text(ByteCountFormatter.string(fromByteCount: Int64(file.size), countStyle: .file))
                                .foregroundStyle(.secondary)
                        }
                    }
                    .onDelete { offsets in
                        for index in offsets { link.deleteFile(link.files[index].name) }
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
