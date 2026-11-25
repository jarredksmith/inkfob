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
    @State private var zoom: Double = 1
    @State private var zoomStart: Double = 1
    @State private var panX: Double = 0      // -1 ... 1, how far the photo is slid inside the frame
    @State private var panY: Double = 0
    @State private var panStartX: Double = 0
    @State private var panStartY: Double = 0
    @State private var dragging = false
    @State private var pinching = false
    @State private var message = ""

    static let faces: [(name: String, detail: String)] = [
        ("Classic", "Clock, date, then weather, events and notifications"),
        ("Big clock", "The time as large as it goes, date underneath"),
        ("Weather", "Temperature and sky first, clock below"),
        ("Agenda", "Your next events, with the clock in the top bar"),
        ("Minimal", "Only the time"),
        ("Photo", "Your picture, with the time in the corner"),
        ("Custom", "Clock in the middle, your pick in each corner"),
    ]
    static let cornerChoices = ["Nothing", "Date", "Weather", "Next event", "Steps", "Phone battery",
                                "Unread count", "Countdown", "Stock or score", "Reminder"]
    static let cornerNames = ["Top left", "Top right", "Bottom left", "Bottom right"]

    private var current: Int { Int(link.settings["home"] ?? "0") ?? 0 }
    private var needsUpdate: Bool { link.isReady && link.settings["version"] != nil && link.settings["home"] == nil }
    private var hasCustom: Bool { link.settings["corners"] != nil }

    var body: some View {
        NavigationStack {
            List {
                if needsUpdate {
                    Section { Text("Faces need newer pager firmware. Install it from the Update tab.").font(.callout) }
                }
                Section("Home screen face") {
                    ForEach(Array(Self.faces.enumerated()), id: \.offset) { index, face in
                        if index < 6 || hasCustom {
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
                }
                .disabled(!link.isReady || needsUpdate)

                if hasCustom {
                    Section {
                        ForEach(0..<4, id: \.self) { corner in
                            Picker(Self.cornerNames[corner], selection: cornerBinding(corner)) {
                                ForEach(Array(Self.cornerChoices.enumerated()), id: \.offset) { index, name in
                                    Text(name).tag(index)
                                }
                            }
                        }
                    } header: {
                        Text("Custom face corners")
                    } footer: {
                        Text("Countdown, stock or score and reminder come from Pager > More for the pager.")
                    }
                    .disabled(!link.isReady)
                }

                Section {
                    PhotosPicker(selection: $picked, matching: .images) {
                        Label(source == nil ? "Choose a photo" : "Choose a different photo", systemImage: "photo")
                    }
                    if let source, let bits = InkPicture.dither(source, zoom: zoom, panX: panX, panY: panY,
                                                                brightness: brightness, contrast: contrast, inverted: inverted),
                       let preview = InkPicture.image(from: bits) {
                        framingPreview(preview, source: source)
                        Text("Drag the picture to move it. Pinch, or use the sliders, to zoom and nudge.")
                            .font(.footnote).foregroundStyle(.secondary)
                        LabeledContent("Zoom") { Slider(value: $zoom, in: 1...4) }
                        LabeledContent("Up / down") { Slider(value: $panY, in: -1...1) }
                        LabeledContent("Left / right") { Slider(value: $panX, in: -1...1) }
                        LabeledContent("Brightness") { Slider(value: $brightness, in: -0.4...0.4) }
                        LabeledContent("Contrast") { Slider(value: $contrast, in: 0.6...2.2) }
                        Toggle("Invert", isOn: $inverted)
                        Button("Send to pager") { send(bits) }
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
                        let small = InkPicture.shrink(image)
                        await MainActor.run {
                            source = small
                            zoom = 1; zoomStart = 1; panX = 0; panY = 0; panStartX = 0; panStartY = 0
                            message = ""
                        }
                    } else {
                        await MainActor.run { message = "Could not open that photo." }
                    }
                }
            }
        }
    }

    /// The preview is the frame: dragging slides the photo underneath it.
    private func framingPreview(_ preview: UIImage, source: UIImage) -> some View {
        Image(uiImage: preview)
            .interpolation(.none)
            .resizable()
            .aspectRatio(CGFloat(InkPicture.width) / CGFloat(InkPicture.height), contentMode: .fit)
            .border(Color.secondary)
            .overlay {
                GeometryReader { geo in
                    Color.clear
                        .contentShape(Rectangle())
                        .highPriorityGesture(
                            DragGesture(minimumDistance: 2)
                                .onChanged { drag in
                                    if !dragging {
                                        dragging = true
                                        panStartX = panX
                                        panStartY = panY
                                    }
                                    let dotsPerPoint = Double(InkPicture.width) / Double(max(geo.size.width, 1))
                                    let room = InkPicture.room(for: source, zoom: zoom)
                                    if room.width > 0.5 {
                                        panX = clamp(panStartX + Double(drag.translation.width) * dotsPerPoint / Double(room.width))
                                    }
                                    if room.height > 0.5 {
                                        panY = clamp(panStartY + Double(drag.translation.height) * dotsPerPoint / Double(room.height))
                                    }
                                }
                                .onEnded { _ in dragging = false }
                        )
                        .simultaneousGesture(
                            MagnificationGesture()
                                .onChanged { scale in
                                    if !pinching {
                                        pinching = true
                                        zoomStart = zoom
                                    }
                                    zoom = min(4, max(1, zoomStart * Double(scale)))
                                }
                                .onEnded { _ in pinching = false }
                        )
                }
            }
            .listRowInsets(EdgeInsets(top: 8, leading: 8, bottom: 8, trailing: 8))
    }

    private func clamp(_ value: Double) -> Double { min(1, max(-1, value)) }

    private func cornerBinding(_ corner: Int) -> Binding<Int> {
        Binding(
            get: {
                let digits = Array(link.settings["corners"] ?? "1236")
                return corner < digits.count ? (digits[corner].wholeNumberValue ?? 0) : 0
            },
            set: { choice in
                var digits = Array(link.settings["corners"] ?? "1236")
                while digits.count < 4 { digits.append("0") }
                digits[corner] = Character(String(choice))
                link.set("corners", String(digits.prefix(4)))
            })
    }

    private func send(_ bits: Data) {
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

    /// Photos are far larger than the screen; a smaller copy keeps dragging smooth and fixes rotation.
    static func shrink(_ image: UIImage) -> UIImage {
        let longest = max(image.size.width, image.size.height)
        let scale = min(1, 1200 / max(longest, 1))
        let size = CGSize(width: max(1, image.size.width * scale), height: max(1, image.size.height * scale))
        let format = UIGraphicsImageRendererFormat()
        format.scale = 1
        return UIGraphicsImageRenderer(size: size, format: format).image { _ in
            image.draw(in: CGRect(origin: .zero, size: size))
        }
    }

    /// The photo's size, in screen dots, once it fills the frame at this zoom.
    static func filledSize(for image: UIImage, zoom: Double) -> CGSize {
        let fill = max(CGFloat(width) / max(image.size.width, 1), CGFloat(height) / max(image.size.height, 1)) * CGFloat(zoom)
        return CGSize(width: image.size.width * fill, height: image.size.height * fill)
    }

    /// How far, in dots, the photo can slide each way from centre before an edge shows.
    static func room(for image: UIImage, zoom: Double) -> CGSize {
        let size = filledSize(for: image, zoom: zoom)
        return CGSize(width: max(0, (size.width - CGFloat(width)) / 2), height: max(0, (size.height - CGFloat(height)) / 2))
    }

    static func dither(_ image: UIImage, zoom: Double, panX: Double, panY: Double,
                       brightness: Double, contrast: Double, inverted: Bool) -> Data? {
        var gray = [UInt8](repeating: 255, count: width * height)
        guard let context = CGContext(data: &gray, width: width, height: height, bitsPerComponent: 8, bytesPerRow: width,
                                      space: CGColorSpaceCreateDeviceGray(), bitmapInfo: CGImageAlphaInfo.none.rawValue)
        else { return nil }
        UIGraphicsPushContext(context)
        context.translateBy(x: 0, y: CGFloat(height))
        context.scaleBy(x: 1, y: -1)
        context.setFillColor(gray: 1, alpha: 1)
        context.fill(CGRect(x: 0, y: 0, width: width, height: height))
        let size = filledSize(for: image, zoom: zoom)
        let slide = room(for: image, zoom: zoom)
        let x = (CGFloat(width) - size.width) / 2 + CGFloat(panX) * slide.width
        let y = (CGFloat(height) - size.height) / 2 + CGFloat(panY) * slide.height
        image.draw(in: CGRect(x: x, y: y, width: size.width, height: size.height))
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
