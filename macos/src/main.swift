// Nightfall for macOS - a tiny menu-bar screen blanker.
//
//   - Menu bar moon: click -> menu (Blank Screen Now, Clock / Digital Rain / Gray Mosaic,
//     Start at Login, Quit)
//   - Global hotkey Ctrl+Option+B (Carbon hotkey: no Accessibility permission needed)
//   - Opening the app again (Finder, Spotlight, Launchpad, `open -a Nightfall`) blanks now
//   - `Nightfall --blank` blanks immediately at launch; `--rain` / `--mosaic` / `--clock` override
//     the saved style for this run
//
// While blanked: a black window above everything on every display, cursor hidden, and either a
// dim clock that jumps to a random display/spot every 20 s (no burn-in), a dim green digital
// rain (15 frames/s, redrawing only the cells that change), or a mosaic of dark gray rectangles
// that slowly crossfade to new shades (15 frames/s, changed cells only). Any key or mouse click wakes it;
// moving the mouse alone does not.
//
// NIGHTFALL_DEBUG=<dir> logs state and saves PNGs of what is drawn.
// NIGHTFALL_AUTOWAKE=<secs> sends a keyDown that many seconds after each blank (test hook).
// NIGHTFALL_SELFTEST=1 (with NIGHTFALL_DEBUG) runs a scripted input sequence through the real
// event path (NSApp.sendEvent) and exits - used by test/e2e.sh because posting OS-level events
// needs Accessibility permission.

import AppKit
import Carbon.HIToolbox
import ServiceManagement

let graceSeconds = 0.8
let moveSeconds = 20

let debugDir = ProcessInfo.processInfo.environment["NIGHTFALL_DEBUG"]
let selfTest = ProcessInfo.processInfo.environment["NIGHTFALL_SELFTEST"] == "1"

func dbg(_ msg: String) {
    guard let dir = debugDir else { return }
    let line = String(format: "%.3f %@\n", ProcessInfo.processInfo.systemUptime, msg)
    let url = URL(fileURLWithPath: dir).appendingPathComponent("nightfall.log")
    if let h = try? FileHandle(forWritingTo: url) {
        h.seekToEndOfFile(); h.write(line.data(using: .utf8)!); try? h.close()
    } else {
        try? line.write(to: url, atomically: false, encoding: .utf8)
    }
}

// MARK: - style

enum Style: String { case clock, rain, mosaic }

var styleOverride: Style? = CommandLine.arguments.contains("--rain") ? .rain
    : CommandLine.arguments.contains("--mosaic") ? .mosaic
    : CommandLine.arguments.contains("--clock") ? .clock : nil
var savedStyle: Style {
    get { Style(rawValue: UserDefaults.standard.string(forKey: "style") ?? "") ?? .clock }
    set { UserDefaults.standard.set(newValue.rawValue, forKey: "style") }
}

// MARK: - digital rain (same model as linux/src/rain.h and windows/src/rain.h)

let rainLevels = 12          // 0 = empty, 1 = head, 2..11 = trail from bright to dim
let rainGlyphs = 66          // 56 half-width katakana (U+FF66..U+FF9D) + 10 digits
let rainFPS = 15.0

struct Rain {
    struct Col { var head = 0, len = 6, speed = 1 }
    let cols: Int, rows: Int
    var col: [Col]
    var glyph: [UInt8]
    var shownLevel: [UInt8]
    var shownGlyph: [UInt8]
    var frame = 0

    init(cols: Int, rows: Int) {
        self.cols = max(1, cols); self.rows = max(2, rows)
        let n = self.cols * self.rows
        col = Array(repeating: Col(), count: self.cols)
        glyph = (0..<n).map { _ in UInt8.random(in: 0..<UInt8(rainGlyphs)) }
        shownLevel = Array(repeating: 0, count: n)
        shownGlyph = Array(repeating: 0, count: n)
        for c in 0..<self.cols { spawn(c, first: true) }
    }

    mutating func spawn(_ c: Int, first: Bool) {
        let maxlen = max(6, rows * 2 / 3)
        col[c].len = 6 + Int.random(in: 0..<(maxlen - 5))
        col[c].speed = 1 + Int.random(in: 0..<3)
        col[c].head = first ? Int.random(in: 0..<(rows * 2)) - rows : -Int.random(in: 0...rows)
    }

    func level(_ c: Col, _ row: Int) -> Int {
        let d = c.head - row
        if d < 0 || d >= c.len { return 0 }
        if d == 0 { return 1 }
        return 2 + (d - 1) * (rainLevels - 2) / (c.len - 1)
    }

    mutating func step() {
        frame += 1
        for c in 0..<cols where frame % col[c].speed == 0 {
            col[c].head += 1
            let h = col[c].head
            if h >= 0 && h < rows { glyph[h * cols + c] = UInt8.random(in: 0..<UInt8(rainGlyphs)) }
            if h - col[c].len >= rows { spawn(c, first: false) }
        }
        for _ in 0...(cols / 6) {      // a few glyphs inside trails flicker to a new character
            let c = Int.random(in: 0..<cols)
            let row = col[c].head - 1 - Int.random(in: 0..<col[c].len)
            if row >= 0 && row < rows { glyph[row * cols + c] = UInt8.random(in: 0..<UInt8(rainGlyphs)) }
        }
    }

    /// Updates what is shown and reports every cell whose look changed: (col, row, level, glyph).
    mutating func update(_ changed: (Int, Int, Int, Int) -> Void) {
        for c in 0..<cols {
            let cl = col[c]
            let from = max(0, cl.head - cl.len - 3), to = min(rows - 1, cl.head)
            if from > to { continue }
            for row in from...to {
                let i = row * cols + c
                let lv = UInt8(level(cl, row)), g = glyph[i]
                if lv == shownLevel[i] && (lv == 0 || g == shownGlyph[i]) { continue }
                shownLevel[i] = lv; shownGlyph[i] = g
                changed(c, row, Int(lv), Int(g))
            }
        }
    }
}

func rainColor(_ level: Int) -> NSColor {
    if level == 1 { return NSColor(srgbRed: 170/255, green: 1, blue: 180/255, alpha: 1) }
    let t = 1.0 - 0.88 * Double(level - 2) / Double(rainLevels - 3)
    return NSColor(srgbRed: 10 * t / 255, green: 220 * t / 255, blue: 70 * t / 255, alpha: 1)
}

/// Layer-hosting view: one small layer per grid cell, all sharing pre-drawn glyph images, so a
/// frame only swaps the image (or hides) the few hundred cells that changed and the GPU composites.
final class RainView: NSView {
    var rain: Rain
    let cell: CGFloat                       // points
    private var tiles: [[CGImage]] = []     // [level][glyph], drawn once at the display's scale
    private var cells: [CALayer?]

    init(frame: NSRect, scale: CGFloat) {
        cell = max(14, (frame.height / 36).rounded(.down))
        rain = Rain(cols: Int(frame.width / cell) + 1, rows: Int(frame.height / cell) + 1)
        cells = Array(repeating: nil, count: rain.cols * rain.rows)
        super.init(frame: frame)
        let root = CALayer()
        root.backgroundColor = CGColor.black
        root.isGeometryFlipped = true
        layer = root
        wantsLayer = true
        makeTiles(scale: scale)
    }
    required init?(coder: NSCoder) { fatalError() }
    override var isOpaque: Bool { true }

    private func makeTiles(scale: CGFloat) {
        let px = Int(cell * scale)
        let font = NSFont.monospacedSystemFont(ofSize: cell * 0.8, weight: .regular)
        let space = CGColorSpace(name: CGColorSpace.sRGB)!
        tiles = [[]]
        for lv in 1..<rainLevels {
            var row: [CGImage] = []
            for g in 0..<rainGlyphs {
                let cp = g < 56 ? 0xFF66 + g : 0x30 + (g - 56)
                let str = NSAttributedString(string: String(UnicodeScalar(cp)!),
                                             attributes: [.font: font, .foregroundColor: rainColor(lv)])
                let ctx = CGContext(data: nil, width: px, height: px, bitsPerComponent: 8, bytesPerRow: 0,
                                    space: space, bitmapInfo: CGImageAlphaInfo.noneSkipFirst.rawValue)!
                ctx.setFillColor(CGColor.black); ctx.fill(CGRect(x: 0, y: 0, width: px, height: px))
                ctx.scaleBy(x: scale, y: scale)
                NSGraphicsContext.saveGraphicsState()
                NSGraphicsContext.current = NSGraphicsContext(cgContext: ctx, flipped: false)
                let sz = str.size()
                str.draw(at: NSPoint(x: (cell - sz.width) / 2, y: (cell - sz.height) / 2))
                NSGraphicsContext.restoreGraphicsState()
                row.append(ctx.makeImage()!)
            }
            tiles.append(row)
        }
    }

    func tick() {
        rain.step()
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        let cols = rain.cols
        rain.update { c, row, lv, g in
            let i = row * cols + c
            if lv == 0 { cells[i]?.isHidden = true; return }
            let l: CALayer
            if let have = cells[i] { l = have } else {
                l = CALayer()
                l.frame = CGRect(x: CGFloat(c) * cell, y: CGFloat(row) * cell, width: cell, height: cell)
                l.isOpaque = true
                layer?.addSublayer(l)
                cells[i] = l
            }
            l.contents = tiles[lv][g]
            l.isHidden = false
        }
        CATransaction.commit()
    }

    /// Debug snapshot of what the layers show.
    func snapshot(to url: URL, scale: CGFloat) {
        guard let root = layer else { return }
        let w = Int(bounds.width * scale), h = Int(bounds.height * scale)
        guard let ctx = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8, bytesPerRow: 0,
                                  space: CGColorSpace(name: CGColorSpace.sRGB)!,
                                  bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue) else { return }
        ctx.scaleBy(x: scale, y: scale)
        root.render(in: ctx)
        if let img = ctx.makeImage() {
            try? NSBitmapImageRep(cgImage: img).representation(using: .png, properties: [:])?.write(to: url)
        }
    }
}

// MARK: - gray mosaic (same model as linux/src/mosaic.h and windows/src/mosaic.h)

let mosaicMaxCell = 50, mosaicMinCell = 24   // pixels
let mosaicMaxGray = 64                       // of 255: black to about 25% gray
let mosaicChangeSeconds = 12.0               // each cell starts a new fade about this often
let mosaicFPS = 15.0
let mosaicFadeFrames = 15...45               // 1..3 s

/// Largest cell size in [min, max] pixels dividing a (and b, if nonzero), or 0.
func mosaicDivisor(_ a: Int, _ b: Int) -> Int {
    for d in stride(from: mosaicMaxCell, through: mosaicMinCell, by: -1) where a % d == 0 && (b == 0 || b % d == 0) {
        return d
    }
    return 0
}

/// Cell edges in pixels along an axis of px pixels.
func mosaicEdges(_ px: Int, square: Int) -> [Int] {
    var n = square > 0 ? px / square : 0
    if n == 0 { let d = mosaicDivisor(px, 0); n = d > 0 ? px / d : (px + mosaicMaxCell - 1) / mosaicMaxCell }
    n = max(1, n)
    return (0...n).map { $0 * px / n }
}

/// One layer whose contents is a tiny image with one pixel per cell, scaled up to the display
/// with nearest-neighbour filtering, so every pixel becomes a crisp rectangle. The fades are
/// stepped here, 15 times a second, and the window server composites a single quad per frame.
/// (A layer per cell cost the window server ~3x more; letting Core Animation run per-cell
/// fades made it recomposite at the display's full refresh rate, ~6x more.)
final class MosaicView: NSView {
    private struct Cell { var gray: UInt8, from: UInt8, to: UInt8, t: Int, dur: Int }
    private var cells: [Cell] = []
    private var pixels: [UInt8]                 // sRGB, 4 bytes per cell (R, G, B, unused)
    private let image: CALayer
    private var owed = 0.0
    let cols: Int, rows: Int

    init(frame: NSRect, scale: CGFloat) {
        let pw = Int((frame.width * scale).rounded()), ph = Int((frame.height * scale).rounded())
        let sq = mosaicDivisor(pw, ph)
        cols = mosaicEdges(pw, square: sq).count - 1; rows = mosaicEdges(ph, square: sq).count - 1
        pixels = Array(repeating: 0, count: cols * rows * 4)
        image = CALayer()
        super.init(frame: frame)
        let root = CALayer()
        root.backgroundColor = CGColor.black
        layer = root
        wantsLayer = true
        image.frame = CGRect(origin: .zero, size: frame.size)
        image.magnificationFilter = .nearest
        image.contentsGravity = .resize
        image.isOpaque = true
        image.actions = ["contents": NSNull()]
        root.addSublayer(image)
        for _ in 0..<(cols * rows) {
            let g = UInt8.random(in: 0...UInt8(mosaicMaxGray))
            cells.append(Cell(gray: g, from: g, to: g, t: 0, dur: 0))
        }
        render(force: true)
        dbg("mosaic: \(cols)x\(rows) cells for \(pw)x\(ph) px")
    }
    required init?(coder: NSCoder) { fatalError() }
    override var isOpaque: Bool { true }

    private func render(force: Bool) {
        var changed = force
        for i in cells.indices where pixels[i * 4] != cells[i].gray {
            let g = cells[i].gray
            pixels[i * 4] = g; pixels[i * 4 + 1] = g; pixels[i * 4 + 2] = g
            changed = true
        }
        guard changed else { return }
        let data = Data(pixels) as CFData
        guard let provider = CGDataProvider(data: data),
              let img = CGImage(width: cols, height: rows, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: cols * 4,
                                space: CGColorSpace(name: CGColorSpace.sRGB)!,
                                bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.noneSkipLast.rawValue),
                                provider: provider, decode: nil, shouldInterpolate: false, intent: .defaultIntent)
        else { return }
        image.contents = img
    }

    func tick() {
        for i in cells.indices where cells[i].dur > 0 {
            cells[i].t += 1
            var p = Double(cells[i].t) / Double(cells[i].dur)
            p = p * p * (3 - 2 * p)                          // ease in and out
            let from = Double(cells[i].from), to = Double(cells[i].to)
            cells[i].gray = UInt8(from + (to - from) * p + 0.5)
            if cells[i].t >= cells[i].dur { cells[i].gray = cells[i].to; cells[i].dur = 0 }
        }
        owed += Double(cells.count) / (mosaicChangeSeconds * mosaicFPS)
        while owed >= 1 {
            owed -= 1
            let i = Int.random(in: 0..<cells.count)
            if cells[i].dur > 0 { continue }
            cells[i].from = cells[i].gray
            cells[i].to = UInt8.random(in: 0...UInt8(mosaicMaxGray))
            cells[i].t = 0
            cells[i].dur = Int.random(in: mosaicFadeFrames)
        }
        render(force: false)
    }

    /// Debug snapshot of what the layers show.
    func snapshot(to url: URL, scale: CGFloat) {
        guard let root = layer else { return }
        let w = Int(bounds.width * scale), h = Int(bounds.height * scale)
        guard let ctx = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8, bytesPerRow: 0,
                                  space: CGColorSpace(name: CGColorSpace.sRGB)!,
                                  bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue) else { return }
        ctx.scaleBy(x: scale, y: scale)
        root.render(in: ctx)
        if let img = ctx.makeImage() {
            try? NSBitmapImageRep(cgImage: img).representation(using: .png, properties: [:])?.write(to: url)
        }
    }
}

// MARK: - blank windows

final class BlankWindow: NSWindow {
    weak var controller: Blanker?
    override var canBecomeKey: Bool { true }
    override var canBecomeMain: Bool { true }
    override func keyDown(with event: NSEvent) { controller?.input("key \(event.keyCode)") }
    override func flagsChanged(with event: NSEvent) {
        // a modifier going down counts as a key; releases (e.g. of the hotkey) do not
        if !event.modifierFlags.intersection(.deviceIndependentFlagsMask).isEmpty {
            controller?.input("modifier")
        }
    }
    override func mouseDown(with event: NSEvent) { controller?.input("click left") }
    override func rightMouseDown(with event: NSEvent) { controller?.input("click right") }
    override func otherMouseDown(with event: NSEvent) { controller?.input("click other") }
    override func mouseMoved(with event: NSEvent) { NSCursor.setHiddenUntilMouseMoves(true) }
    override func scrollWheel(with event: NSEvent) {}
}

final class ClockView: NSView {
    var showClock = false
    var fx: CGFloat = 0.5, fy: CGFloat = 0.5
    override var isOpaque: Bool { true }

    override func draw(_ dirtyRect: NSRect) {
        NSColor.black.setFill()
        bounds.fill()
        guard showClock else { return }
        let now = Date()
        let tf = DateFormatter(); tf.timeStyle = .short; tf.dateStyle = .none
        let df = DateFormatter(); df.dateStyle = .full; df.timeStyle = .none
        let timePx = max(24, bounds.height / 7)
        let tAttr: [NSAttributedString.Key: Any] = [
            .font: NSFont.monospacedDigitSystemFont(ofSize: timePx, weight: .ultraLight),
            .foregroundColor: NSColor(srgbRed: 78/255, green: 78/255, blue: 84/255, alpha: 1)]
        let dAttr: [NSAttributedString.Key: Any] = [
            .font: NSFont.systemFont(ofSize: timePx / 4, weight: .regular),
            .foregroundColor: NSColor(srgbRed: 52/255, green: 52/255, blue: 58/255, alpha: 1)]
        let t = NSAttributedString(string: tf.string(from: now), attributes: tAttr)
        let d = NSAttributedString(string: df.string(from: now), attributes: dAttr)
        let ts = t.size(), ds = d.size()
        let bw = max(ts.width, ds.width) + timePx       // slack so 9:59 -> 10:00 never clips
        let bh = ts.height + ds.height
        let x = bounds.width > bw ? (bounds.width - bw) * fx : 0
        let y = bounds.height > bh ? (bounds.height - bh) * fy : 0
        d.draw(at: NSPoint(x: x + timePx * 0.04, y: y))
        t.draw(at: NSPoint(x: x, y: y + ds.height))
    }
}

final class Blanker {
    private var windows: [BlankWindow] = []
    private var views: [ClockView] = []
    private var rainViews: [RainView] = []
    private var mosaicViews: [MosaicView] = []
    private var timer: Timer?
    private var rainTimer: Timer?
    private(set) var style: Style = .clock
    private var started = Date()
    private var lastMove = Date()
    private var ticks = 0
    private var lastMinute = -1
    private var prevApp: NSRunningApplication?
    var isActive: Bool { !windows.isEmpty }
    var onWake: (() -> Void)?

    func start() {
        if isActive { NSApp.activate(ignoringOtherApps: true); windows.first?.makeKeyAndOrderFront(nil); return }
        prevApp = NSWorkspace.shared.frontmostApplication
        style = styleOverride ?? savedStyle
        for screen in NSScreen.screens {
            let w = BlankWindow(contentRect: screen.frame, styleMask: .borderless, backing: .buffered, defer: false)
            w.controller = self
            w.level = .screenSaver
            w.backgroundColor = .black
            w.isOpaque = true
            w.hasShadow = false
            w.isReleasedWhenClosed = false
            w.acceptsMouseMovedEvents = true
            w.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .stationary, .ignoresCycle]
            let v = ClockView(frame: NSRect(origin: .zero, size: screen.frame.size))
            if style == .rain {
                let rv = RainView(frame: NSRect(origin: .zero, size: screen.frame.size), scale: screen.backingScaleFactor)
                w.contentView = rv
                rainViews.append(rv)
            } else if style == .mosaic {
                let mv = MosaicView(frame: NSRect(origin: .zero, size: screen.frame.size), scale: screen.backingScaleFactor)
                w.contentView = mv
                mosaicViews.append(mv)
            } else {
                w.contentView = v
            }
            w.setFrame(screen.frame, display: false)
            windows.append(w); views.append(v)
        }
        NSApp.presentationOptions = [.hideDock, .hideMenuBar]
        NSApp.activate(ignoringOtherApps: true)
        for w in windows.reversed() { w.makeKeyAndOrderFront(nil) }
        windows.first?.makeKey()
        NSCursor.hide()
        started = Date()
        ticks = 0
        moveClock()
        let t = Timer(timeInterval: 1, repeats: true) { [weak self] _ in self?.tick() }
        t.tolerance = 0.5
        RunLoop.main.add(t, forMode: .common)
        timer = t
        if style == .rain {
            let rt = Timer(timeInterval: 1 / rainFPS, repeats: true) { [weak self] _ in
                self?.rainViews.forEach { $0.tick() }
            }
            rt.tolerance = 0.01
            RunLoop.main.add(rt, forMode: .common)
            rainTimer = rt
        } else if style == .mosaic {
            let mt = Timer(timeInterval: 1 / mosaicFPS, repeats: true) { [weak self] _ in
                self?.mosaicViews.forEach { $0.tick() }
            }
            mt.tolerance = 0.01
            RunLoop.main.add(mt, forMode: .common)
            rainTimer = mt
        }
        dbg("blank: \(windows.count) display(s) style=\(style.rawValue)")
        if let secs = Double(ProcessInfo.processInfo.environment["NIGHTFALL_AUTOWAKE"] ?? "") {
            // test hook: deliver a real keyDown through the app's event path after `secs`
            DispatchQueue.main.asyncAfter(deadline: .now() + secs) { [weak self] in
                guard let self, let w = self.keyWindow else { return }
                self.dump("autowake")
                if let e = NSEvent.keyEvent(with: .keyDown, location: .zero, modifierFlags: [], timestamp: 0,
                                            windowNumber: w.windowNumber, context: nil, characters: " ",
                                            charactersIgnoringModifiers: " ", isARepeat: false, keyCode: 49) {
                    NSApp.sendEvent(e)
                }
            }
        }
    }

    private func moveClock() {
        let pick = Int.random(in: 0..<max(1, views.count))
        for (i, v) in views.enumerated() {
            v.showClock = i == pick
            v.fx = .random(in: 0...1); v.fy = .random(in: 0...1)
            v.needsDisplay = true
        }
        lastMinute = Calendar.current.component(.minute, from: Date())
        lastMove = Date()
    }

    private func tick() {
        ticks += 1
        if debugDir != nil && ticks == 3 { dump("start") }
        if style != .clock { return }
        if Date().timeIntervalSince(lastMove) >= Double(moveSeconds) {   // wall clock: timers drift
            if debugDir != nil { dump("before-move") }
            moveClock()
            dbg("clock moved")
        } else if Calendar.current.component(.minute, from: Date()) != lastMinute {
            lastMinute = Calendar.current.component(.minute, from: Date())
            views.forEach { $0.needsDisplay = true }
        }
    }

    func input(_ what: String) {
        dbg("input \(what)")
        if Date().timeIntervalSince(started) < graceSeconds { dbg("ignored (grace)"); return }
        stop()
    }

    func stop() {
        guard isActive else { return }
        timer?.invalidate(); timer = nil
        rainTimer?.invalidate(); rainTimer = nil
        for w in windows { w.orderOut(nil); w.close() }
        windows.removeAll(); views.removeAll(); rainViews.removeAll(); mosaicViews.removeAll()
        NSCursor.unhide()
        NSApp.presentationOptions = []
        prevApp?.activate()
        prevApp = nil
        dbg("wake")
        onWake?()
    }

    func dump(_ tag: String) {
        guard let dir = debugDir else { return }
        for (i, w) in windows.enumerated() {
            let v: NSView = w.contentView ?? views[i]
            dbg("\(tag) display=\(i) size=\(Int(w.frame.width))x\(Int(w.frame.height)) " +
                "covers=\(w.frame == w.screen?.frame) key=\(w.isKeyWindow) visible=\(w.isVisible) " +
                "level=\(w.level.rawValue) clock_here=\(views[i].showClock) app_active=\(NSApp.isActive)")
            if let rv = v as? RainView {
                rv.snapshot(to: URL(fileURLWithPath: dir).appendingPathComponent("\(tag)-display\(i).png"),
                            scale: w.backingScaleFactor)
            } else if let mv = v as? MosaicView {
                mv.snapshot(to: URL(fileURLWithPath: dir).appendingPathComponent("\(tag)-display\(i).png"),
                            scale: w.backingScaleFactor)
            } else if let rep = v.bitmapImageRepForCachingDisplay(in: v.bounds) {
                v.cacheDisplay(in: v.bounds, to: rep)
                let url = URL(fileURLWithPath: dir).appendingPathComponent("\(tag)-display\(i).png")
                try? rep.representation(using: .png, properties: [:])?.write(to: url)
            }
        }
    }

    // test hook: the windows currently shown
    var keyWindow: BlankWindow? { windows.first(where: { $0.isKeyWindow }) ?? windows.first }
}

// MARK: - app

final class AppDelegate: NSObject, NSApplicationDelegate {
    let blanker = Blanker()
    var statusItem: NSStatusItem!
    var hotKeyRef: EventHotKeyRef?
    var hotKeyOK = false
    var loginItem: NSMenuItem!
    var clockItem: NSMenuItem!
    var rainItem: NSMenuItem!
    var mosaicItem: NSMenuItem!

    func applicationDidFinishLaunching(_ n: Notification) {
        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
        let img = NSImage(systemSymbolName: "moon.fill", accessibilityDescription: "Nightfall")
        img?.isTemplate = true
        statusItem.button?.image = img
        statusItem.button?.toolTip = "Nightfall - Ctrl+Option+B blanks the screen"

        let menu = NSMenu()
        let blankItem = NSMenuItem(title: "Blank Screen Now", action: #selector(blankNow), keyEquivalent: "b")
        blankItem.keyEquivalentModifierMask = [.control, .option]
        menu.addItem(blankItem)
        menu.addItem(.separator())
        clockItem = NSMenuItem(title: "Clock", action: #selector(pickClock), keyEquivalent: "")
        rainItem = NSMenuItem(title: "Digital Rain", action: #selector(pickRain), keyEquivalent: "")
        mosaicItem = NSMenuItem(title: "Gray Mosaic", action: #selector(pickMosaic), keyEquivalent: "")
        menu.addItem(clockItem); menu.addItem(rainItem); menu.addItem(mosaicItem)
        menu.addItem(.separator())
        loginItem = NSMenuItem(title: "Start at Login", action: #selector(toggleLogin), keyEquivalent: "")
        menu.addItem(loginItem)
        menu.addItem(.separator())
        menu.addItem(NSMenuItem(title: "Quit Nightfall", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q"))
        menu.delegate = self
        statusItem.menu = menu

        registerHotKey()
        dbg("launched hotkey_ok=\(hotKeyOK)")

        if CommandLine.arguments.contains("--blank") || selfTest {
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.2) { self.blanker.start() }
        }
        if selfTest { runSelfTest() }
    }

    // Opening the app again while it's running (Finder, Spotlight, Launchpad, Dock, `open`) blanks now.
    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool {
        dbg("reopen")
        blanker.start()
        return false
    }

    @objc func blankNow() { blanker.start() }
    @objc func pickClock() { savedStyle = .clock; styleOverride = nil }
    @objc func pickRain() { savedStyle = .rain; styleOverride = nil }
    @objc func pickMosaic() { savedStyle = .mosaic; styleOverride = nil }

    @objc func toggleLogin() {
        let svc = SMAppService.mainApp
        do {
            if svc.status == .enabled { try svc.unregister() } else { try svc.register() }
        } catch {
            let a = NSAlert(); a.messageText = "Couldn't change login item"; a.informativeText = error.localizedDescription
            a.runModal()
        }
    }

    func registerHotKey() {
        var spec = EventTypeSpec(eventClass: OSType(kEventClassKeyboard), eventKind: UInt32(kEventHotKeyPressed))
        InstallEventHandler(GetApplicationEventTarget(), { _, _, ctx in
            let me = Unmanaged<AppDelegate>.fromOpaque(ctx!).takeUnretainedValue()
            dbg("hotkey")
            me.blanker.start()
            return noErr
        }, 1, &spec, Unmanaged.passUnretained(self).toOpaque(), nil)
        let id = EventHotKeyID(signature: OSType(0x4E46_4C4C), id: 1)   // 'NFLL'
        let st = RegisterEventHotKey(UInt32(kVK_ANSI_B), UInt32(controlKey | optionKey), id,
                                     GetApplicationEventTarget(), 0, &hotKeyRef)
        hotKeyOK = st == noErr
    }

    // MARK: self test (drives real NSEvents through NSApp.sendEvent)

    func runSelfTest() {
        func ev(_ type: NSEvent.EventType, _ w: NSWindow?) -> NSEvent? {
            switch type {
            case .keyDown:
                return NSEvent.keyEvent(with: .keyDown, location: .zero, modifierFlags: [], timestamp: 0,
                                        windowNumber: w?.windowNumber ?? 0, context: nil, characters: " ",
                                        charactersIgnoringModifiers: " ", isARepeat: false, keyCode: 49)
            default:
                return NSEvent.mouseEvent(with: type, location: NSPoint(x: 200, y: 200), modifierFlags: [],
                                          timestamp: 0, windowNumber: w?.windowNumber ?? 0, context: nil,
                                          eventNumber: 0, clickCount: 1, pressure: 1)
            }
        }
        func send(_ type: NSEvent.EventType) {
            if let e = ev(type, blanker.keyWindow) { NSApp.sendEvent(e) }
        }
        // each delay is relative to the previous step, so a late step never squeezes the next one
        // into the grace period
        var steps: [(Double, () -> Void)] = []
        steps.append((0.7, { send(.keyDown) }))                 // inside grace -> ignored
        steps.append((2.5, { for _ in 0..<10 { send(.mouseMoved) }; dbg("st: moved, active=\(self.blanker.isActive)") }))
        steps.append((20.5, { dbg("st: before key, active=\(self.blanker.isActive)"); send(.keyDown) }))
        steps.append((1.0, { dbg("st: after key, active=\(self.blanker.isActive)"); self.blanker.start() }))
        steps.append((2.5, { send(.leftMouseDown) }))
        steps.append((1.0, { dbg("st: after click, active=\(self.blanker.isActive)"); self.blanker.start() }))
        steps.append((2.5, { send(.rightMouseDown) }))
        steps.append((1.0, { dbg("st: after right click, active=\(self.blanker.isActive)"); dbg("st: done"); NSApp.terminate(nil) }))
        func run(_ i: Int) {
            guard i < steps.count else { return }
            DispatchQueue.main.asyncAfter(deadline: .now() + steps[i].0) { steps[i].1(); run(i + 1) }
        }
        run(0)
    }
}

extension AppDelegate: NSMenuDelegate {
    func menuWillOpen(_ menu: NSMenu) {
        loginItem.state = SMAppService.mainApp.status == .enabled ? .on : .off
        let st = styleOverride ?? savedStyle
        clockItem.state = st == .clock ? .on : .off
        rainItem.state = st == .rain ? .on : .off
        mosaicItem.state = st == .mosaic ? .on : .off
    }
}

let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate
app.setActivationPolicy(.accessory)
app.run()
