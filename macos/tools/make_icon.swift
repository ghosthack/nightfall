// Renders the crescent-moon app icon into an .iconset directory (then: iconutil -c icns).
import AppKit
let out = CommandLine.arguments[1]
try? FileManager.default.createDirectory(atPath: out, withIntermediateDirectories: true)
for (size, scale) in [(16,1),(16,2),(32,1),(32,2),(128,1),(128,2),(256,1),(256,2),(512,1),(512,2)] {
    let px = size * scale
    let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: px, pixelsHigh: px, bitsPerSample: 8,
                               samplesPerPixel: 4, hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB,
                               bytesPerRow: 0, bitsPerPixel: 0)!
    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
    let s = CGFloat(px)
    let inset = s * 0.1
    NSColor(srgbRed: 0.08, green: 0.086, blue: 0.11, alpha: 1).setFill()
    NSBezierPath(roundedRect: NSRect(x: inset, y: inset, width: s - 2*inset, height: s - 2*inset),
                 xRadius: s * 0.18, yRadius: s * 0.18).fill()
    let moon = NSBezierPath(ovalIn: NSRect(x: s*0.18, y: s*0.18, width: s*0.64, height: s*0.64))
    let cut = NSBezierPath(ovalIn: NSRect(x: s*0.36, y: s*0.30, width: s*0.56, height: s*0.56))
    NSGraphicsContext.current!.cgContext.saveGState()
    moon.addClip()
    NSColor(srgbRed: 0.95, green: 0.90, blue: 0.78, alpha: 1).setFill()
    moon.fill()
    NSGraphicsContext.current!.compositingOperation = .clear
    cut.fill()
    NSGraphicsContext.current!.cgContext.restoreGState()
    NSGraphicsContext.restoreGraphicsState()
    let name = scale == 1 ? "icon_\(size)x\(size).png" : "icon_\(size)x\(size)@2x.png"
    try! rep.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: out + "/" + name))
}
