import CoreGraphics
import Foundation
import ImageIO

guard CommandLine.arguments.count == 3 else {
    fputs("usage: create-icns.swift source.png output.icns\n", stderr)
    exit(2)
}

let sourceURL = URL(fileURLWithPath: CommandLine.arguments[1]) as CFURL
let outputURL = URL(fileURLWithPath: CommandLine.arguments[2])

guard let source = CGImageSourceCreateWithURL(sourceURL, nil) else {
    fputs("could not read source image\n", stderr)
    exit(1)
}

let representations = [
    (type: "icp4", size: 16),
    (type: "icp5", size: 32),
    (type: "icp6", size: 64),
    (type: "ic07", size: 128),
    (type: "ic08", size: 256),
    (type: "ic09", size: 512),
    (type: "ic10", size: 1024),
]

func appendBigEndian(_ value: UInt32, to data: inout Data) {
    var bigEndian = value.bigEndian
    withUnsafeBytes(of: &bigEndian) { data.append(contentsOf: $0) }
}

func appendFourCC(_ value: String, to data: inout Data) {
    data.append(value.data(using: .ascii)!)
}

var chunks = Data()

for representation in representations {
    let size = representation.size
    let thumbnailOptions: [CFString: Any] = [
        kCGImageSourceCreateThumbnailFromImageAlways: true,
        kCGImageSourceCreateThumbnailWithTransform: true,
        kCGImageSourceThumbnailMaxPixelSize: size,
    ]

    guard let image = CGImageSourceCreateThumbnailAtIndex(
        source,
        0,
        thumbnailOptions as CFDictionary
    ) else {
        fputs("could not render \(size)x\(size) icon\n", stderr)
        exit(1)
    }

    let png = NSMutableData()
    guard let pngDestination = CGImageDestinationCreateWithData(
        png,
        "public.png" as CFString,
        1,
        nil
    ) else {
        fputs("could not create \(size)x\(size) PNG\n", stderr)
        exit(1)
    }

    CGImageDestinationAddImage(pngDestination, image, nil)
    guard CGImageDestinationFinalize(pngDestination) else {
        fputs("could not encode \(size)x\(size) PNG\n", stderr)
        exit(1)
    }

    appendFourCC(representation.type, to: &chunks)
    appendBigEndian(UInt32(png.length + 8), to: &chunks)
    chunks.append(png as Data)
}

var icon = Data("icns".utf8)
appendBigEndian(UInt32(chunks.count + 8), to: &icon)
icon.append(chunks)

do {
    try icon.write(to: outputURL, options: .atomic)
} catch {
    fputs("could not write ICNS file: \(error)\n", stderr)
    exit(1)
}
