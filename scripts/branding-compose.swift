// Composes the supplied logo onto the macOS app-icon grid: transparent 1024 canvas,
// 824 px superellipse body, soft drop shadow. Run by scripts/branding.py generate.
// Usage: swift branding-compose.swift SOURCE.png OUTPUT.png X Y WIDTH HEIGHT R G B
import CoreGraphics
import Foundation
import ImageIO
import UniformTypeIdentifiers

let args = CommandLine.arguments
guard args.count == 10, let crop = Optional(CGRect(x: Double(args[3])!, y: Double(args[4])!,
                                                   width: Double(args[5])!, height: Double(args[6])!)) else {
    FileHandle.standardError.write("usage: SOURCE OUTPUT X Y WIDTH HEIGHT R G B\n".data(using: .utf8)!); exit(2)
}
let fill = CGColor(srgbRed: Double(args[7])! / 255, green: Double(args[8])! / 255, blue: Double(args[9])! / 255, alpha: 1)
guard let input = CGImageSourceCreateWithURL(URL(fileURLWithPath: args[1]) as CFURL, nil),
      let source = CGImageSourceCreateImageAtIndex(input, 0, nil),
      let artwork = source.cropping(to: crop) else { FileHandle.standardError.write("cannot read source\n".data(using: .utf8)!); exit(1) }

let canvas = 1024.0, side = 824.0, origin = (canvas - side) / 2
// Superellipse |x|^n + |y|^n = 1 approximates Apple's continuous-corner icon shape.
func squircle(_ rect: CGRect, exponent n: Double = 5) -> CGPath {
    let path = CGMutablePath(), steps = 720
    for i in 0..<steps {
        let t = Double(i) / Double(steps) * 2 * Double.pi
        let c = cos(t), s = sin(t)
        let x = rect.midX + rect.width / 2 * (c < 0 ? -1 : 1) * pow(abs(c), 2 / n)
        let y = rect.midY + rect.height / 2 * (s < 0 ? -1 : 1) * pow(abs(s), 2 / n)
        if i == 0 { path.move(to: CGPoint(x: x, y: y)) } else { path.addLine(to: CGPoint(x: x, y: y)) }
    }
    path.closeSubpath(); return path
}

let space = CGColorSpace(name: CGColorSpace.sRGB)!
guard let context = CGContext(data: nil, width: Int(canvas), height: Int(canvas), bitsPerComponent: 8, bytesPerRow: 0,
                              space: space, bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else { exit(1) }
context.interpolationQuality = .high
let body = CGRect(x: origin, y: origin, width: side, height: side)
let shape = squircle(body)
context.saveGState()
context.setShadow(offset: CGSize(width: 0, height: -10), blur: 24, color: CGColor(gray: 0, alpha: 0.35))
context.addPath(shape); context.setFillColor(fill); context.fillPath()
context.restoreGState()
context.saveGState()
context.addPath(shape); context.clip()
// Scale the artwork to the body width, centered; its own rounded corners fall outside the clip.
let scale = side / crop.width
let drawn = CGSize(width: crop.width * scale, height: crop.height * scale)
context.draw(artwork, in: CGRect(x: origin + (side - drawn.width) / 2, y: origin + (side - drawn.height) / 2,
                                 width: drawn.width, height: drawn.height))
context.restoreGState()

guard let image = context.makeImage(),
      let output = CGImageDestinationCreateWithURL(URL(fileURLWithPath: args[2]) as CFURL, UTType.png.identifier as CFString, 1, nil)
else { exit(1) }
CGImageDestinationAddImage(output, image, nil)
exit(CGImageDestinationFinalize(output) ? 0 : 1)
