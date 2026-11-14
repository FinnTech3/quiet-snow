// Quiet Snow: the menu bar app. All the snow logic lives in QuietSnowCore; this file is windows, Metal and a menu.

#if canImport(AppKit) && canImport(MetalKit)
import AppKit
import MetalKit
import ServiceManagement
import QuietSnowCore

let version = "1.0.1"

// MARK: settings

enum Prefs {
    static let d = UserDefaults.standard
    static var weather: Weather {
        get { Weather(rawValue: d.string(forKey: "weather") ?? "") ?? (NSWorkspace.shared.accessibilityDisplayShouldReduceMotion ? .light : .steady) }
        set { d.set(newValue.rawValue, forKey: "weather") }
    }
    static var wind: Float { get { d.float(forKey: "wind") } set { d.set(newValue, forKey: "wind") } }
    static var melt: Bool { get { d.bool(forKey: "melt") } set { d.set(newValue, forKey: "melt") } }
    static var inScreenshots: Bool { get { d.bool(forKey: "screenshots") } set { d.set(newValue, forKey: "screenshots") } }
    static var paused: Bool { get { d.bool(forKey: "paused") } set { d.set(newValue, forKey: "paused") } }
}

// MARK: one overlay per display

final class Overlay {
    let screen: NSScreen
    let window: NSWindow
    let view: MTKView
    let world: Rect

    init(screen: NSScreen, device: MTLDevice, delegate: MTKViewDelegate, primaryHeight: CGFloat) {
        self.screen = screen
        let f = screen.frame
        world = Rect(x: Float(f.minX), y: Float(primaryHeight - f.maxY), w: Float(f.width), h: Float(f.height))
        let v = MTKView(frame: NSRect(origin: .zero, size: f.size), device: device)
        v.delegate = delegate
        v.isPaused = true
        v.enableSetNeedsDisplay = false
        v.framebufferOnly = true
        v.colorPixelFormat = .bgra8Unorm
        v.clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 0)
        // One pixel per point on purpose: a retina buffer would cost about 60 MB for snow that is soft anyway.
        v.autoResizeDrawable = false
        v.drawableSize = f.size
        v.wantsLayer = true
        if let layer = v.layer as? CAMetalLayer {
            layer.isOpaque = false
            layer.maximumDrawableCount = 2
            layer.presentsWithTransaction = false
        }
        view = v
        window = NSWindow(contentRect: f, styleMask: .borderless, backing: .buffered, defer: false, screen: screen)
        window.isReleasedWhenClosed = false
        window.isOpaque = false
        window.backgroundColor = .clear
        window.hasShadow = false
        window.ignoresMouseEvents = true
        window.level = .floating
        window.collectionBehavior = [.canJoinAllSpaces, .stationary, .ignoresCycle]
        window.sharingType = Prefs.inScreenshots ? .readOnly : .none
        window.contentView = v
        window.orderFrontRegardless()
    }
}

// MARK: the app

final class App: NSObject, NSApplicationDelegate, MTKViewDelegate {
    let device: MTLDevice
    let renderer: Renderer
    let sim = Sim()

    var overlays: [Overlay] = []
    var status: NSStatusItem!
    var timer: Timer?
    var link: AnyObject?
    var stepCount = 0
    var lastStep = CFAbsoluteTimeGetCurrent()
    var lastPoll = 0.0
    var glide: Float = 0
    var asleep = false
    var hiddenAway = false

    var flakes: [Float] = [], piles: [Float] = []
    var flakeBuf: MTLBuffer?, pileBuf: MTLBuffer?
    var flakeN = 0, pileN = 0

    init(device: MTLDevice) throws {
        self.device = device
        renderer = try Renderer(device: device)
        super.init()
    }

    func applicationDidFinishLaunching(_ note: Notification) {
        sim.weather = Prefs.weather
        sim.wind = Prefs.wind
        sim.melts = Prefs.melt
        buildOverlays()
        buildMenu()

        let nc = NotificationCenter.default
        nc.addObserver(self, selector: #selector(screensChanged), name: NSApplication.didChangeScreenParametersNotification, object: nil)
        nc.addObserver(self, selector: #selector(powerChanged), name: .NSProcessInfoPowerStateDidChange, object: nil)
        nc.addObserver(self, selector: #selector(occlusionChanged), name: NSWindow.didChangeOcclusionStateNotification, object: nil)
        let ws = NSWorkspace.shared.notificationCenter
        for n in [NSWorkspace.screensDidSleepNotification, NSWorkspace.willSleepNotification] {
            ws.addObserver(self, selector: #selector(goToSleep), name: n, object: nil)
        }
        for n in [NSWorkspace.screensDidWakeNotification, NSWorkspace.didWakeNotification] {
            ws.addObserver(self, selector: #selector(wake), name: n, object: nil)
        }
        let dc = DistributedNotificationCenter.default()
        dc.addObserver(self, selector: #selector(goToSleep), name: .init("com.apple.screenIsLocked"), object: nil)
        dc.addObserver(self, selector: #selector(wake), name: .init("com.apple.screenIsUnlocked"), object: nil)
        updateTimer()
    }

    // MARK: overlays

    func primaryHeight() -> CGFloat { NSScreen.screens.first?.frame.height ?? 0 }

    @objc func screensChanged() { buildOverlays(); updateTimer() }

    func buildOverlays() {
        overlays.forEach { $0.window.orderOut(nil) }
        overlays = NSScreen.screens.map { Overlay(screen: $0, device: device, delegate: self, primaryHeight: primaryHeight()) }
        sim.setWorld(overlays.map { $0.world })
        applyPower()
        pollSurfaces()
    }

    // MARK: pause rules

    @objc func goToSleep() { asleep = true; updateTimer() }
    @objc func wake() { asleep = false; updateTimer() }
    @objc func occlusionChanged() {
        hiddenAway = !overlays.contains { $0.window.occlusionState.contains(.visible) }
        updateTimer()
    }
    @objc func powerChanged() { applyPower(); updateTimer() }

    var lowPower: Bool { ProcessInfo.processInfo.isLowPowerModeEnabled }
    func applyPower() { sim.densityScale = lowPower ? 0.6 : 1 }

    var running: Bool { !Prefs.paused && !asleep && !hiddenAway }

    func updateTimer() {
        timer?.invalidate()
        timer = nil
        if #available(macOS 14.0, *) { (link as? CADisplayLink)?.invalidate() }
        link = nil
        refreshMenu()
        guard running else { return }
        lastStep = CFAbsoluteTimeGetCurrent()
        let fps: Float = 60                                          // never below 60, whatever the power state
        if #available(macOS 14.0, *), let view = overlays.first?.view {
            // Paced by the display itself, so motion is even and ProMotion screens can slow it to save power.
            let l = view.displayLink(target: self, selector: #selector(displayTick))
            l.preferredFrameRateRange = CAFrameRateRange(minimum: fps, maximum: fps, preferred: fps)
            l.add(to: .main, forMode: .common)
            link = l
        } else {
            let t = Timer(timeInterval: 1 / Double(fps), repeats: true) { [weak self] _ in self?.frame() }
            t.tolerance = 0.004
            RunLoop.main.add(t, forMode: .common)
            timer = t
        }
    }

    @objc func displayTick() { frame() }

    // MARK: per frame

    /// Called for every display refresh, 60 a second. The model steps at 30 Hz (20 in Low Power Mode); in between, flakes
    /// glide on by their last velocity, so every one of the 60 frames shows motion for half the model cost.
    func frame() {
        let now = CFAbsoluteTimeGetCurrent()
        let interval = lowPower ? 1.0 / 20 : 1.0 / 30
        if now - lastStep >= interval - 0.004 {
            let dt = Float(now - lastStep)
            lastStep = now
            stepCount += 1
            let pollEvery = NSEvent.pressedMouseButtons != 0 ? 0.05 : 0.5      // a drag needs quick eyes, idle does not
            if now - lastPoll >= pollEvery { lastPoll = now; pollSurfaces() }

            let m = NSEvent.mouseLocation
            sim.step(dt: dt, pointer: (x: Float(m.x), y: Float(primaryHeight() - m.y)))
            flakeN = sim.fillFlakes(&flakes)
            flakeBuf = upload(flakes, into: flakeBuf)
            if stepCount % 2 == 0 || pileN == 0 {                                // piles change slowly: 15 Hz is plenty
                pileN = sim.fillPiles(&piles)
                pileBuf = upload(piles, into: pileBuf)
            }
        }
        glide = Float(min(max(now - lastStep, 0), 0.07))
        for o in overlays { o.view.draw() }
    }

    func upload(_ data: [Float], into old: MTLBuffer?) -> MTLBuffer? {
        let bytes = data.count * MemoryLayout<Float>.stride
        if bytes == 0 { return old }
        var buf = old
        if buf == nil || buf!.length < bytes {
            buf = device.makeBuffer(length: max(bytes * 2, 4096), options: .storageModeShared)
        }
        data.withUnsafeBytes { buf!.contents().copyMemory(from: $0.baseAddress!, byteCount: bytes) }
        return buf
    }

    // MARK: what is on screen

    func pollSurfaces() {
        let myPID = ProcessInfo.processInfo.processIdentifier
        let opts: CGWindowListOption = [.optionOnScreenOnly, .excludeDesktopElements]
        guard let info = CGWindowListCopyWindowInfo(opts, kCGNullWindowID) as? [[String: Any]] else { return }
        var rects: [(id: Int, rect: Rect)] = []
        for w in info {
            guard (w[kCGWindowLayer as String] as? Int) == 0,
                  (w[kCGWindowOwnerPID as String] as? Int) != Int(myPID),
                  (w[kCGWindowAlpha as String] as? Double ?? 1) > 0.1,
                  let id = w[kCGWindowNumber as String] as? Int,
                  let bounds = w[kCGWindowBounds as String] as? NSDictionary,
                  let b = CGRect(dictionaryRepresentation: bounds as CFDictionary) else { continue }
            let r = Rect(x: Float(b.minX), y: Float(b.minY), w: Float(b.width), h: Float(b.height))
            if r.w < 100 || r.h < 50 { continue }
            // A window pinned under the menu bar is a maximised or full-screen one: let the snow pass over it.
            if sim.world.contains(where: { r.w >= $0.w * 0.9 && r.y < $0.y + 40 && r.x < $0.maxX && r.maxX > $0.x }) { continue }
            rects.append((id, r))
            if rects.count >= 40 { break }
        }
        var inputs: [SurfaceInput] = []
        for (i, w) in rects.enumerated() {                // the list is front to back
            inputs.append(SurfaceInput(id: w.id, rect: Rect(x: w.rect.x, y: w.rect.y, w: w.rect.w, h: 0),
                                       occluders: rects[0..<i].map { $0.rect }))
        }
        // The floor of each display: the top of the Dock when it sits at the bottom, else the screen edge.
        let ph = primaryHeight()
        for (i, s) in NSScreen.screens.enumerated() {
            let f = s.frame
            let y = Float(ph - s.visibleFrame.minY)
            // Another display directly underneath means this bottom edge is not a floor.
            let stacked = NSScreen.screens.contains { o in
                o !== s && abs(o.frame.maxY - f.minY) < 2 && o.frame.minX < f.maxX && o.frame.maxX > f.minX
            }
            if stacked { continue }
            inputs.append(SurfaceInput(id: -(i + 1), rect: Rect(x: Float(f.minX), y: y, w: Float(f.width), h: 0), isFloor: true))
        }
        sim.setSurfaces(inputs)
    }

    // MARK: MTKViewDelegate

    func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}

    func draw(in view: MTKView) {
        guard let o = overlays.first(where: { $0.view === view }),
              let pass = view.currentRenderPassDescriptor,
              let drawable = view.currentDrawable,
              let cb = renderer.queue.makeCommandBuffer(),
              let enc = cb.makeRenderCommandEncoder(descriptor: pass) else { return }
        renderer.encode(enc, origin: SIMD2(o.world.x, o.world.y), size: SIMD2(o.world.w, o.world.h), t: glide,
                        piles: pileBuf, pileCount: pileN, flakes: flakeBuf, flakeCount: flakeN)
        enc.endEncoding()
        cb.present(drawable)
        cb.commit()
    }

    // MARK: menu

    var weatherItems: [Weather: NSMenuItem] = [:]
    var windItems: [(Float, NSMenuItem)] = []
    var pauseItem: NSMenuItem!, meltItem: NSMenuItem!, shotItem: NSMenuItem!, loginItem: NSMenuItem!

    func item(_ title: String, _ sel: Selector, _ menu: NSMenu) -> NSMenuItem {
        let i = NSMenuItem(title: title, action: sel, keyEquivalent: "")
        i.target = self
        menu.addItem(i)
        return i
    }

    func buildMenu() {
        status = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
        if let b = status.button {
            let img = NSImage(systemSymbolName: "snowflake", accessibilityDescription: "Quiet Snow")
            img?.isTemplate = true
            b.image = img
        }
        let m = NSMenu()
        for w in Weather.allCases {
            let i = item(w.rawValue.capitalized, #selector(pickWeather(_:)), m)
            i.representedObject = w.rawValue
            weatherItems[w] = i
        }
        m.addItem(.separator())
        for (title, v) in [("Still air", Float(0)), ("Breeze to the right", 60), ("Breeze to the left", -60),
                           ("Gale to the right", 220), ("Gale to the left", -220)] {
            let i = item(title, #selector(pickWind(_:)), m)
            i.representedObject = v
            windItems.append((v, i))
        }
        m.addItem(.separator())
        meltItem = item("Melt slowly", #selector(toggleMelt), m)
        _ = item("Clear piles", #selector(clearPiles), m)
        pauseItem = item("Pause", #selector(togglePause), m)
        m.addItem(.separator())
        shotItem = item("Show in screenshots", #selector(toggleShots), m)
        loginItem = item("Open at login", #selector(toggleLogin), m)
        m.addItem(.separator())
        _ = item("Quit", #selector(quit), m)
        status.menu = m
        refreshMenu()
    }

    func refreshMenu() {
        guard status != nil else { return }
        for (w, i) in weatherItems { i.state = w == sim.weather ? .on : .off }
        for (v, i) in windItems { i.state = v == sim.wind ? .on : .off }
        meltItem.state = sim.melts ? .on : .off
        pauseItem.title = Prefs.paused ? "Resume" : "Pause"
        shotItem.state = Prefs.inScreenshots ? .on : .off
        loginItem.state = SMAppService.mainApp.status == .enabled ? .on : .off
    }

    @objc func pickWeather(_ s: NSMenuItem) {
        guard let raw = s.representedObject as? String, let w = Weather(rawValue: raw) else { return }
        Prefs.weather = w; sim.weather = w; refreshMenu()
    }
    @objc func pickWind(_ s: NSMenuItem) {
        guard let v = s.representedObject as? Float else { return }
        Prefs.wind = v; sim.wind = v; refreshMenu()
    }
    @objc func toggleMelt() { Prefs.melt.toggle(); sim.melts = Prefs.melt; refreshMenu() }
    @objc func clearPiles() { sim.clearPiles() }
    @objc func togglePause() { Prefs.paused.toggle(); updateTimer() }
    @objc func toggleShots() {
        Prefs.inScreenshots.toggle()
        for o in overlays { o.window.sharingType = Prefs.inScreenshots ? .readOnly : .none }
        refreshMenu()
    }
    @objc func toggleLogin() {
        let s = SMAppService.mainApp
        if s.status == .enabled { try? s.unregister() } else { try? s.register() }
        refreshMenu()
    }
    @objc func quit() { NSApp.terminate(nil) }
}

// MARK: entry

if CommandLine.arguments.contains("--version") {
    print("Quiet Snow \(version)")
    exit(0)
}

guard let device = MTLCreateSystemDefaultDevice() else {
    fputs("Quiet Snow needs a Metal capable Mac.\n", stderr)
    exit(1)
}

if let i = CommandLine.arguments.firstIndex(of: "--snapshot") {
    guard i + 1 < CommandLine.arguments.count else { fputs("usage: QuietSnow --snapshot out.png\n", stderr); exit(2) }
    do { try writeSnapshot(device: device, to: CommandLine.arguments[i + 1]) } catch {
        fputs("snapshot failed: \(error)\n", stderr); exit(1)
    }
    exit(0)
}

if let i = CommandLine.arguments.firstIndex(of: "--frames") {
    guard i + 1 < CommandLine.arguments.count else { fputs("usage: QuietSnow --frames dir\n", stderr); exit(2) }
    do { try writeFrames(device: device, to: CommandLine.arguments[i + 1]) } catch {
        fputs("frames failed: \(error)\n", stderr); exit(1)
    }
    exit(0)
}

do {
    let delegate = try App(device: device)
    let app = NSApplication.shared
    app.setActivationPolicy(.accessory)
    app.delegate = delegate
    app.run()
} catch {
    fputs("Quiet Snow could not start: \(error)\n", stderr)
    exit(1)
}

#else
print("Quiet Snow runs on macOS only. The snow model builds and tests anywhere: swift test")
#endif
