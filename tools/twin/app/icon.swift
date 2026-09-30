// The app icon: "TWIN" in lit LED dots on a dark matrix, as the panel draws text.
//   swift icon.swift OUT.png      (1024 x 1024; build.sh makes the .icns from it)
import AppKit

let size = 1024.0
let out = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "icon.png"
let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: Int(size), pixelsHigh: Int(size), bitsPerSample: 8,
                           samplesPerPixel: 4, hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
let ctx = NSGraphicsContext.current!.cgContext

// The macOS icon grid: an 824-point rounded square centred on 1024 (Apple's app icon template).
let body = CGRect(x: 100, y: 100, width: 824, height: 824)
ctx.addPath(CGPath(roundedRect: body, cornerWidth: 185, cornerHeight: 185, transform: nil))
ctx.setFillColor(CGColor(red: 0.07, green: 0.08, blue: 0.10, alpha: 1)); ctx.fillPath()

// 5x7 glyphs, rows top to bottom.
let glyphs: [Character: [String]] = [
    "T": ["11111", "00100", "00100", "00100", "00100", "00100", "00100"],
    "W": ["10001", "10001", "10001", "10101", "10101", "11011", "10001"],
    "I": ["11111", "00100", "00100", "00100", "00100", "00100", "11111"],
    "N": ["10001", "11001", "10101", "10011", "10001", "10001", "10001"],
]
let cols = 23, rows = 13, pitch = 30.0
let ox = (size - Double(cols) * pitch) / 2 + pitch / 2, oy = (size - Double(rows) * pitch) / 2 + pitch / 2
var lit = Set<Int>()
for (k, ch) in "TWIN".enumerated() {
    for (r, line) in glyphs[ch]!.enumerated() {
        for (c, bit) in line.enumerated() where bit == "1" { lit.insert((r + 3) * cols + 1 + k * 6 + c - (k > 0 ? 0 : 0)) }
    }
}
for r in 0..<rows {
    for c in 0..<cols {
        let x = ox + Double(c) * pitch, y = size - (oy + Double(r) * pitch)
        let on = lit.contains(r * cols + c)
        if on {   // a soft glow under a lit dot, as on the GOB-coated panel
            ctx.setFillColor(CGColor(red: 1.0, green: 0.62, blue: 0.10, alpha: 0.22))
            ctx.fillEllipse(in: CGRect(x: x - 19, y: y - 19, width: 38, height: 38))
        }
        ctx.setFillColor(on ? CGColor(red: 1.0, green: 0.72, blue: 0.25, alpha: 1) : CGColor(red: 0.16, green: 0.18, blue: 0.22, alpha: 1))
        ctx.fillEllipse(in: CGRect(x: x - 10, y: y - 10, width: 20, height: 20))
    }
}
// A cyan scan line under the text: the twin's own mark.
ctx.setFillColor(CGColor(red: 0.20, green: 0.85, blue: 0.95, alpha: 0.9))
ctx.fill(CGRect(x: ox - 10, y: size - (oy + 11.0 * pitch) - 4, width: Double(cols - 1) * pitch + 20, height: 8))
NSGraphicsContext.current = nil
try! rep.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: out))
