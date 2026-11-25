import PhotosUI
import SwiftUI
import UIKit

/// Chooses what the pager's home screen looks like, and turns a photo into an e-ink picture for it.
struct FacesView: View {
    @EnvironmentObject var link: PagerLink
    @State private var picked: PhotosPickerItem?
    @State private var source: UIImage?
    @State private var brightness: Double = 0
    @State private var contrast: Double = 1.2
    @State private var inverted = false
    @State private var message = ""

    static let faces: [(name: String, detail: String)] = [
        ("Classic", "Clock, date, then weather, events and notifications"),
        ("Big clock", "The time as large as it goes, date underneath"),
        ("Weather", "Temperature and sky first, clock below"),
        ("Agenda", "Your next events, with the clock in the top bar"),
        ("Minimal", "Only the time"),
        ("Photo", "Your picture, with the time in the corner"),
    ]

    private var current: Int { Int(link.settings["home"] ?? "0") ?? 0 }
    private var needsUpdate: Bool { link.isReady && link.settings["version"] != nil && link.settings["home"] == nil }

    var body: some View {
        NavigationStack {
            List {
                if needsUpdate {
                    Section { Text("Faces need pager firmware v1.6 or newer. Install it from the Update tab.").font(.callout) }
                }
                Section("Home screen face") {
                    ForEach(Array(Self.faces.enumerated()), id: \.offset) { index, face in
                        Button {
                            link.set("home", String(index))
                        } label: {
                            HStack {
                                VStack(alignment: .leading, spacing: 2) {
                                    Text(face.name).foregroundStyle(.primary)
                                    Text(face.detail).font(.footnote).foregroundStyle(.secondary)
                                }
                                Spacer()
                                if index == current { Image(systemName: "checkmark").foregroundStyle(.tint) }
                            }
                        }
                    }
                }
                .disabled(!link.isReady || needsUpdate)

                Section {
                    PhotosPicker(selection: $picked, matching: .images) {
                        Label(source == nil ? "Choose a photo" : "Choose a different photo", systemImage: "photo")
                    }
                    if let preview {
                        Image(uiImage: preview)
                            .interpolation(.none)
                            .resizable()
                            .aspectRatio(CGFloat(InkPicture.width) / CGFloat(InkPicture.height), contentMode: .fit)
                            .border(Color.secondary)
                            .listRowInsets(EdgeInsets(top: 8, leading: 8, bottom: 8, trailing: 8))
                        LabeledContent("Brightness") { Slider(value: $brightness, in: -0.4...0.4) }
                        LabeledContent("Contrast") { Slider(value: $contrast, in: 0.6...2.2) }
                        Toggle("Invert", isOn: $inverted)
                        Button("Send to pager") { send() }
                            .disabled(!link.isReady || link.transferActive || needsUpdate)
                    }
                    if link.transferActive { ProgressView(value: link.transferProgress) }
                    if !message.isEmpty { Text(message).font(.callout) }
                } header: {
                    Text("Photo face")
                } footer: {
                    Text("The screen is black and white only, so the picture is turned into dots. Simple, high-contrast photos look best.")
                }
            }
            .navigationTitle("Faces")
            .onAppear { link.requestSettings() }
            .onChange(of: picked) { _, item in
                guard let item else { return }
                Task {
                    if let data = try? await item.loadTransferable(type: Data.self), let image = UIImage(data: data) {
                        await MainActor.run { source = image; message = "" }
                    } else {
                        await MainActor.run { message = "Could not open that photo." }
                    }
                }
            }
        }
    }

    private var bits: Data? {
        guard let source else { return nil }
        return InkPicture.dither(source, brightness: brightness, contrast: contrast, inverted: inverted)
    }

    private var preview: UIImage? {
        guard let bits else { return nil }
        return InkPicture.image(from: bits)
    }

    private func send() {
        guard let bits else { return }
        message = "Sending..."
        link.sendData(name: "face.img", data: bits) { ok, text in
            if ok {
                link.set("home", "5")
                message = "On the pager now."
            } else {
                message = text
            }
        }
    }
}

/// Converts pictures to the pager's format: 250 x 122, one bit per dot, rows padded to whole bytes, 1 = black.
enum InkPicture {
    static let width = 250
    static let height = 122
    static let rowBytes = (width + 7) / 8

    static func dither(_ image: UIImage, brightness: Double, contrast: Double, inverted: Bool) -> Data? {
        var gray = [UInt8](repeating: 255, count: width * height)
        guard let context = CGContext(data: &gray, width: width, height: height, bitsPerComponent: 8, bytesPerRow: width,
                                      space: CGColorSpaceCreateDeviceGray(), bitmapInfo: CGImageAlphaInfo.none.rawValue)
        else { return nil }
        // Fill the screen, cropping whatever does not fit. UIKit handles the photo's rotation.
        UIGraphicsPushContext(context)
        context.translateBy(x: 0, y: CGFloat(height))
        context.scaleBy(x: 1, y: -1)
        context.setFillColor(gray: 1, alpha: 1)
        context.fill(CGRect(x: 0, y: 0, width: width, height: height))
        let scale = max(CGFloat(width) / image.size.width, CGFloat(height) / image.size.height)
        let size = CGSize(width: image.size.width * scale, height: image.size.height * scale)
        image.draw(in: CGRect(x: (CGFloat(width) - size.width) / 2, y: (CGFloat(height) - size.height) / 2,
                              width: size.width, height: size.height))
        UIGraphicsPopContext()

        var level = gray.map { value -> Double in
            var v = (Double(value) / 255 - 0.5) * contrast + 0.5 + brightness
            if inverted { v = 1 - v }
            return min(1, max(0, v))
        }
        var out = Data(count: rowBytes * height)
        for y in 0..<height {
            for x in 0..<width {
                let i = y * width + x
                let old = level[i]
                let white = old >= 0.5
                let error = old - (white ? 1 : 0)
                if !white { out[y * rowBytes + x / 8] |= UInt8(0x80 >> (x % 8)) }
                if x + 1 < width { level[i + 1] += error * 7 / 16 }
                if y + 1 < height {
                    if x > 0 { level[i + width - 1] += error * 3 / 16 }
                    level[i + width] += error * 5 / 16
                    if x + 1 < width { level[i + width + 1] += error * 1 / 16 }
                }
            }
        }
        return out
    }

    static func image(from bits: Data) -> UIImage? {
        var gray = [UInt8](repeating: 255, count: width * height)
        for y in 0..<height {
            for x in 0..<width where bits[y * rowBytes + x / 8] & UInt8(0x80 >> (x % 8)) != 0 {
                gray[y * width + x] = 0
            }
        }
        guard let provider = CGDataProvider(data: Data(gray) as CFData),
              let cg = CGImage(width: width, height: height, bitsPerComponent: 8, bitsPerPixel: 8, bytesPerRow: width,
                               space: CGColorSpaceCreateDeviceGray(), bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.none.rawValue),
                               provider: provider, decode: nil, shouldInterpolate: false, intent: .defaultIntent)
        else { return nil }
        return UIImage(cgImage: cg)
    }
}
