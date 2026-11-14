// Quiet Snow: the menu bar app. All the snow logic lives in QuietSnowCore; this file is windows, Metal and a menu.

#if canImport(AppKit) && canImport(MetalKit)
import AppKit
import MetalKit
import Carbon.HIToolbox
import ServiceManagement
import QuietSnowCore

let appVersion = "1.4.0"

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
    /// Desktop only: snow hides behind windows and rests on the floor alone. Off (the default): over all apps and screens.
    static var snowType: Int { get { d.integer(forKey: "snowType") } set { d.set(newValue, forKey: "snowType") } }
    static var desktopOnly: Bool { get { d.bool(forKey: "desktopOnly") } set { d.set(newValue, forKey: "desktopOnly") } }
    /// Draw at the display's real resolution on Retina screens. Off by default: it costs about four times the memory.
    static var sharp: Bool { get { d.bool(forKey: "sharp") } set { d.set(newValue, forKey: "sharp") } }
    /// Control+Option+Shift+S pauses and resumes. Off until the user turns it on.
    static var hotkey: Bool { get { d.bool(forKey: "hotkey") } set { d.set(newValue, forKey: "hotkey") } }
    /// Snowmen on window tops and a snow plough along the bottom of the screen. On by default.
    static var extras: Bool { get { d.object(forKey: "extras") as? Bool ?? true } set { d.set(newValue, forKey: "extras") } }
    static let keys = ["weather", "wind", "melt", "screenshots", "paused", "snowType", "desktopOnly", "sharp", "hotkey", "extras"]
}

// MARK: one overlay per display

/// The snow view. It only gets the mouse while the cursor is over snow or a snowball (see App.frame); then a click
/// scoops up a snowball and dragging rolls it.
final class SnowView: MTKView {
    var onMouse: ((NSEvent.EventType) -> Void)?
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }
    override func mouseDown(with e: NSEvent) { onMouse?(.leftMouseDown) }
    override func mouseDragged(with e: NSEvent) { onMouse?(.leftMouseDragged) }
    override func mouseUp(with e: NSEvent) { onMouse?(.leftMouseUp) }
}

final class Overlay {
    let screen: NSScreen
    let window: NSPanel
    let view: SnowView
    let world: Rect
    let scale: Float         // drawable pixels per point
    var covered = false      // desktop-only mode and an app is full screen on this display: nothing is drawn
    var clear = 0            // polls in a row without a full screen app

    init(screen: NSScreen, device: MTLDevice, delegate: MTKViewDelegate, primaryHeight: CGFloat) {
        self.screen = screen
        let f = screen.frame
        scale = Prefs.sharp ? Float(screen.backingScaleFactor) : 1
        world = Rect(x: Float(f.minX), y: Float(primaryHeight - f.maxY), w: Float(f.width), h: Float(f.height))
        let v = SnowView(frame: NSRect(origin: .zero, size: f.size), device: device)
        v.delegate = delegate
        v.isPaused = true
        v.enableSetNeedsDisplay = false
        v.framebufferOnly = true
        v.colorPixelFormat = .bgra8Unorm
        v.clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 0)
        // One pixel per point unless the user chose Sharp on Retina: a retina buffer costs about four times the memory.
        v.autoResizeDrawable = false
        v.drawableSize = CGSize(width: f.width * CGFloat(scale), height: f.height * CGFloat(scale))
        v.wantsLayer = true
        if let layer = v.layer as? CAMetalLayer {
            layer.isOpaque = false
            layer.maximumDrawableCount = 2
            layer.presentsWithTransaction = false
        }
        view = v
        // A non-activating panel: when it does take a click (on snow), your app stays the active one.
        window = NSPanel(contentRect: f, styleMask: [.borderless, .nonactivatingPanel], backing: .buffered, defer: false, screen: screen)
        window.hidesOnDeactivate = false
        window.becomesKeyOnlyIfNeeded = true
        window.isReleasedWhenClosed = false
        window.isOpaque = false
        window.backgroundColor = .clear
        window.hasShadow = false
        window.ignoresMouseEvents = true
        window.level = .screenSaver          // above the menu bar and the Dock, so the snow reaches the very edge of the screen
        window.collectionBehavior = [.canJoinAllSpaces, .stationary, .ignoresCycle, .fullScreenAuxiliary]
        window.sharingType = Prefs.inScreenshots ? .readOnly : .none
        window.contentView = v
        window.setFrame(f, display: false)   // global coordinates, so a display that is not at 0,0 is covered exactly
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
    var coverTimer: Timer?
    var link: AnyObject?
    var stepCount = 0
    var lastStep = CFAbsoluteTimeGetCurrent()
    var lastPoll = 0.0
    var glide: Float = 0
    var asleep = false
    var hiddenAway = false
    var interactive = false
    var lastDrag = (x: Float(0), y: Float(0), at: 0.0, vx: Float(0), vy: Float(0))
    var prevRects: [Int: Rect] = [:]
    var prevAt = 0.0
    var hotKeyRef: EventHotKeyRef?
    var hotKeyHandler: EventHandlerRef?

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
        sim.snowType = SnowType(rawValue: Prefs.snowType) ?? .balls
        sim.extras = Prefs.extras
        buildOverlays()
        buildMenu()
        applyHotkey()

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
        for o in overlays { o.view.onMouse = { [weak self] t in self?.mouse(t) } }
        interactive = false
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
        // While every display is covered by a full screen app nothing runs, so look again once a second.
        coverTimer?.invalidate()
        coverTimer = nil
        if Prefs.desktopOnly && !Prefs.paused && !asleep && !overlays.isEmpty && overlays.allSatisfy({ $0.covered }) {
            let t = Timer(timeInterval: 1, repeats: true) { [weak self] _ in self?.pollSurfaces() }
            RunLoop.main.add(t, forMode: .common)
            coverTimer = t
        }
        refreshMenu()
        guard running else { setInteractive(false); return }
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
            let px = Float(m.x), py = Float(primaryHeight() - m.y)
            sim.step(dt: dt, pointer: (x: px, y: py))
            setInteractive(sim.snowAt(x: px, y: py))
            flakeN = sim.fillFlakes(&flakes)
            flakeBuf = upload(flakes, into: flakeBuf)
            if stepCount % 2 == 0 || pileN == 0 {                                // piles change slowly: 15 Hz is plenty
                pileN = sim.fillPiles(&piles)
                pileBuf = upload(piles, into: pileBuf)
            }
        }
        glide = Float(min(max(now - lastStep, 0), 0.07))
        for o in overlays where !o.covered { o.view.draw() }
    }

    /// Click-through everywhere except over snow or a snowball, so ordinary clicks always reach your windows.
    func setInteractive(_ on: Bool) {
        guard on != interactive else { return }
        interactive = on
        for o in overlays { o.window.ignoresMouseEvents = !on }
    }

    /// Scoop, roll and drop a snowball. Positions come from the global mouse location, in the model's coordinates.
    func mouse(_ type: NSEvent.EventType) {
        let m = NSEvent.mouseLocation
        let x = Float(m.x), y = Float(primaryHeight() - m.y)
        let now = CFAbsoluteTimeGetCurrent()
        switch type {
        case .leftMouseDown:
            if sim.grab(x: x, y: y) { lastDrag = (x, y, now, 0, 0) }
        case .leftMouseDragged:
            let dt = Float(now - lastDrag.at)
            if dt > 0.001 { lastDrag = (x, y, now, (x - lastDrag.x) / dt, (y - lastDrag.y) / dt) }
            sim.drag(x: x, y: y)
        default:
            let still = now - lastDrag.at > 0.1
            sim.drop(vx: still ? 0 : lastDrag.vx, vy: still ? 0 : lastDrag.vy)
        }
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
        var all: [Rect] = []                              // every ordinary window, to hide snow behind in desktop-only mode
        var full = [Bool](repeating: false, count: overlays.count)
        for w in info {
            guard (w[kCGWindowLayer as String] as? Int) == 0,
                  (w[kCGWindowOwnerPID as String] as? Int) != Int(myPID),
                  (w[kCGWindowAlpha as String] as? Double ?? 1) > 0.1,
                  let id = w[kCGWindowNumber as String] as? Int,
                  let bounds = w[kCGWindowBounds as String] as? NSDictionary,
                  let b = CGRect(dictionaryRepresentation: bounds as CFDictionary) else { continue }
            let r = Rect(x: Float(b.minX), y: Float(b.minY), w: Float(b.width), h: Float(b.height))
            for (k, o) in overlays.enumerated() {          // a window that covers a whole display is a full screen app
                let m = o.world
                if r.x <= m.x + 2 && r.y <= m.y + 2 && r.maxX >= m.maxX - 2 && r.maxY >= m.maxY - 2 { full[k] = true }
            }
            if r.w < 100 || r.h < 50 { continue }
            all.append(r)
            // A window pinned under the menu bar is a maximised or full-screen one: let the snow pass over it.
            if sim.world.contains(where: { r.w >= $0.w * 0.9 && r.y < $0.y + 40 && r.x < $0.maxX && r.maxX > $0.x }) { continue }
            rects.append((id, r))
            if all.count >= 80 { break }
        }
        // A window that is being dragged stirs the snow around it, in the direction it moved.
        let at = CFAbsoluteTimeGetCurrent()
        let elapsed = Float(at - prevAt)
        if elapsed > 0.02 && elapsed < 0.5 {
            for w in rects {
                guard let p = prevRects[w.id] else { continue }
                let dx = w.rect.x - p.x, dy = w.rect.y - p.y
                if abs(dx) + abs(dy) >= 2 && abs(w.rect.w - p.w) < 1 && abs(w.rect.h - p.h) < 1 {
                    sim.stir(w.rect, vx: dx / elapsed, vy: dy / elapsed)
                }
            }
        }
        prevRects = Dictionary(rects.map { ($0.id, $0.rect) }, uniquingKeysWith: { a, _ in a })
        prevAt = at
        var inputs: [SurfaceInput] = []
        for (i, w) in rects.prefix(40).enumerated() {     // the list is front to back
            inputs.append(SurfaceInput(id: w.id, rect: Rect(x: w.rect.x, y: w.rect.y, w: w.rect.w, h: 0),
                                       occluders: rects[0..<i].map { $0.rect }, radius: windowRadius))
        }
        // The floor of each display. Where the Dock sits at the bottom, snow rests on the Dock itself (with rounded ends)
        // and on the bare bottom edge of the screen either side of it; otherwise on the bottom edge.
        let ph = primaryHeight()
        for (i, s) in NSScreen.screens.enumerated() {
            let f = s.frame
            let bottom = Float(ph - f.minY)
            // Another display directly underneath means this bottom edge is not a floor.
            let stacked = NSScreen.screens.contains { o in
                o !== s && abs(o.frame.maxY - f.minY) < 2 && o.frame.minX < f.maxX && o.frame.maxX > f.minX
            }
            if stacked { continue }
            let reserve = s.visibleFrame.minY - f.minY
            if let d = dockSpan(s) {
                let top = Float(ph - s.visibleFrame.minY)
                inputs.append(SurfaceInput(id: -(i + 1), rect: Rect(x: Float(d.x0), y: top, w: Float(d.x1 - d.x0), h: 0),
                                           isFloor: true, radius: Float(min(18, reserve * 0.28)), plough: false))
                inputs.append(SurfaceInput(id: -(100 + 2 * i), rect: Rect(x: Float(f.minX), y: bottom, w: Float(d.x0 - f.minX), h: 0), isFloor: true))
                inputs.append(SurfaceInput(id: -(101 + 2 * i), rect: Rect(x: Float(d.x1), y: bottom, w: Float(f.maxX - d.x1), h: 0), isFloor: true))
            } else {
                let y = Float(ph - s.visibleFrame.minY)
                inputs.append(SurfaceInput(id: -(i + 1), rect: Rect(x: Float(f.minX), y: y, w: Float(f.width), h: 0), isFloor: true))
            }
        }
        sim.setSurfaces(inputs)
        sim.setOccluders(Prefs.desktopOnly ? all : [])
        applyCover(full)
    }

    /// The corner radius of an ordinary window: larger from macOS 26.
    let windowRadius: Float = ProcessInfo.processInfo.operatingSystemVersion.majorVersion >= 26 ? 16 : 10

    /// Where the Dock is along the bottom of a screen, in Cocoa coordinates. macOS does not say: its window list reports
    /// the Dock as the whole screen, and the visible frame gives only its height. So this is an estimate from the Dock's
    /// own settings (icon size, the apps kept in it, running and recent apps, folders and the Bin), worked out at most
    /// every 2 seconds. nil when the Dock is hidden, on a side, or on another screen.
    var dockCache: (at: Double, screen: CGRect, span: (x0: CGFloat, x1: CGFloat, icons: Int)?)?
    func dockSpan(_ s: NSScreen) -> (x0: CGFloat, x1: CGFloat, icons: Int)? {
        let reserve = s.visibleFrame.minY - s.frame.minY
        guard reserve > 8 else { return nil }
        let now = CFAbsoluteTimeGetCurrent()
        if let c = dockCache, c.screen == s.frame, now - c.at < 2 { return c.span }
        let dock = UserDefaults(suiteName: "com.apple.dock")
        var span: (x0: CGFloat, x1: CGFloat, icons: Int)?
        if (dock?.string(forKey: "orientation") ?? "bottom") == "bottom" {
            var tile = CGFloat(dock?.double(forKey: "tilesize") ?? 0)
            if tile < 16 || tile > reserve { tile = max(16, reserve - 16) }
            let apps = (dock?.array(forKey: "persistent-apps") as? [[String: Any]]) ?? []
            let others = dock?.array(forKey: "persistent-others")?.count ?? 0
            let kept = Set(apps.compactMap { ($0["tile-data"] as? [String: Any])?["bundle-identifier"] as? String })
            let running = NSWorkspace.shared.runningApplications.filter {
                $0.activationPolicy == .regular && $0.bundleIdentifier != "com.apple.finder" && !kept.contains($0.bundleIdentifier ?? "")
            }.count
            let recents = dock?.object(forKey: "show-recents") as? Bool ?? true
            let extra = recents ? max(running, min(3, dock?.array(forKey: "recent-apps")?.count ?? 0)) : running
            let icons = 1 + apps.count + extra + others + 1        // Finder, kept apps, running or recent apps, folders, Bin
            let dividers: CGFloat = extra > 0 ? 2 : 1
            let width = min(CGFloat(icons) * tile * 1.08 + dividers * tile * 0.35 + tile * 0.5, s.frame.width - 8)
            span = (s.frame.midX - width / 2, s.frame.midX + width / 2, icons)
        }
        dockCache = (now, s.frame, span)
        return span
    }

    /// Desktop-only mode shows nothing on a display with a full screen app. Two clear polls in a row bring it back,
    /// so it cannot flicker.
    func applyCover(_ full: [Bool]) {
        var changed = false
        for (k, o) in overlays.enumerated() where k < full.count {
            let before = o.covered
            if Prefs.desktopOnly && full[k] { o.covered = true; o.clear = 0 }
            else if o.covered { o.clear += 1; if o.clear >= 2 { o.covered = false } }
            if o.covered != before {
                changed = true
                if o.covered { o.window.orderOut(nil) } else { o.window.orderFrontRegardless() }
            }
        }
        if changed { occlusionChanged() }
    }

    // MARK: MTKViewDelegate

    func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}

    func draw(in view: MTKView) {
        guard let o = overlays.first(where: { $0.view === view }),
              let pass = view.currentRenderPassDescriptor,
              let drawable = view.currentDrawable,
              let cb = renderer.queue.makeCommandBuffer(),
              let enc = cb.makeRenderCommandEncoder(descriptor: pass) else { return }
        renderer.encode(enc, origin: SIMD2(o.world.x, o.world.y), size: SIMD2(o.world.w, o.world.h), t: glide, time: Float(CACurrentMediaTime().truncatingRemainder(dividingBy: 1000)), scale: o.scale, pink: sim.pinkAmount,
                        piles: pileBuf, pileCount: pileN, flakes: flakeBuf, flakeCount: flakeN)
        enc.endEncoding()
        cb.present(drawable)
        cb.commit()
    }

    // MARK: menu

    var weatherItems: [Weather: NSMenuItem] = [:]
    var windItems: [(Float, NSMenuItem)] = []
    var pauseItem: NSMenuItem!, meltItem: NSMenuItem!, shotItem: NSMenuItem!, loginItem: NSMenuItem!
    var overItem: NSMenuItem!, deskItem: NSMenuItem!, sharpItem: NSMenuItem!, hotkeyItem: NSMenuItem!, extrasItem: NSMenuItem!
    var typeItems: [(SnowType, NSMenuItem)] = []

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
        let typeMenu = NSMenu()
        let typeItem = NSMenuItem(title: "Snow type", action: nil, keyEquivalent: "")
        typeItem.submenu = typeMenu
        m.addItem(typeItem)
        for (title, t) in [("Balls", SnowType.balls), ("Snowflakes", .flakes), ("Sleet", .sleet), ("Sparkles", .sparkles),
                           ("Hail", .hail), ("Mix of all", .mixed)] {
            let i = item(title, #selector(pickType(_:)), typeMenu)
            i.representedObject = t.rawValue
            typeItems.append((t, i))
        }
        m.addItem(.separator())
        overItem = item("Over all apps and screens", #selector(pickScope(_:)), m)
        overItem.representedObject = false
        deskItem = item("Desktop only", #selector(pickScope(_:)), m)
        deskItem.representedObject = true
        m.addItem(.separator())
        extrasItem = item("Snow toys: snowballs, snowmen, plough, sleigh", #selector(toggleExtras), m)
        meltItem = item("Melt slowly", #selector(toggleMelt), m)
        _ = item("Clear piles", #selector(clearPiles), m)
        pauseItem = item("Pause", #selector(togglePause), m)
        m.addItem(.separator())
        shotItem = item("Show in screenshots", #selector(toggleShots), m)
        loginItem = item("Open at login", #selector(toggleLogin), m)
        sharpItem = item("Sharp on Retina (more memory)", #selector(toggleSharp), m)
        hotkeyItem = item("Pause with Control+Option+Shift+S", #selector(toggleHotkey), m)
        m.addItem(.separator())
        _ = item("Copy diagnostics", #selector(copyDiagnostics), m)
        _ = item("Check for updates", #selector(checkUpdates), m)
        _ = item("Reset settings", #selector(resetSettings), m)
        let code = item("Enter a code…", #selector(enterCode), m)   // shown in place of Reset settings while Option is held
        code.keyEquivalentModifierMask = .option
        code.isAlternate = true
        m.addItem(.separator())
        _ = item("Quit", #selector(quit), m)
        status.menu = m
        refreshMenu()
    }

    func refreshMenu() {
        guard status != nil else { return }
        for (w, i) in weatherItems { i.state = w == sim.weather ? .on : .off }
        for (v, i) in windItems { i.state = v == sim.wind ? .on : .off }
        for (t, i) in typeItems { i.state = t == sim.snowType ? .on : .off }
        overItem.state = Prefs.desktopOnly ? .off : .on
        deskItem.state = Prefs.desktopOnly ? .on : .off
        meltItem.state = sim.melts ? .on : .off
        extrasItem.state = sim.extras ? .on : .off
        pauseItem.title = Prefs.paused ? "Resume" : "Pause"
        shotItem.state = Prefs.inScreenshots ? .on : .off
        loginItem.state = SMAppService.mainApp.status == .enabled ? .on : .off
        sharpItem.state = Prefs.sharp ? .on : .off
        hotkeyItem.state = Prefs.hotkey ? .on : .off
    }

    @objc func pickWeather(_ s: NSMenuItem) {
        guard let raw = s.representedObject as? String, let w = Weather(rawValue: raw) else { return }
        Prefs.weather = w; sim.weather = w; refreshMenu()
    }
    @objc func pickWind(_ s: NSMenuItem) {
        guard let v = s.representedObject as? Float else { return }
        Prefs.wind = v; sim.wind = v; refreshMenu()
    }
    @objc func pickType(_ s: NSMenuItem) {
        guard let raw = s.representedObject as? Int, let t = SnowType(rawValue: raw) else { return }
        Prefs.snowType = raw; sim.snowType = t; refreshMenu()
    }
    @objc func pickScope(_ s: NSMenuItem) {
        guard let desktop = s.representedObject as? Bool else { return }
        Prefs.desktopOnly = desktop
        for o in overlays where o.covered { o.covered = false; o.window.orderFrontRegardless() }
        occlusionChanged()
        pollSurfaces(); refreshMenu()
    }
    @objc func toggleExtras() { Prefs.extras.toggle(); sim.extras = Prefs.extras; refreshMenu() }
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
    @objc func toggleSharp() { Prefs.sharp.toggle(); buildOverlays(); updateTimer() }
    @objc func toggleHotkey() { Prefs.hotkey.toggle(); applyHotkey(); refreshMenu() }
    @objc func checkUpdates() {
        if let url = URL(string: "https://github.com/FinnTech3/quiet-snow/releases/latest") { NSWorkspace.shared.open(url) }
    }
    @objc func copyDiagnostics() {
        var t = "Quiet Snow \(appVersion)\n"
        t += "macOS \(ProcessInfo.processInfo.operatingSystemVersionString)\nGPU: \(device.name)\n"
        for (i, o) in overlays.enumerated() {
            let f = o.screen.frame
            t += "Display \(i + 1): \(Int(f.width)) x \(Int(f.height)) at \(Int(f.minX)),\(Int(f.minY)), scale \(o.screen.backingScaleFactor), drawing at \(o.scale)x, \(o.covered ? "hidden (full-screen app)" : "shown")\n"
        }
        t += "Weather \(sim.weather.rawValue), wind \(Int(sim.wind)), snow type \(sim.snowType), \(Prefs.desktopOnly ? "desktop only" : "over all apps")\n"
        t += "Flakes \(flakeN), pile vertices \(pileN), paused \(Prefs.paused ? "yes" : "no"), low power \(lowPower ? "yes" : "no")\n"
        t += "Snow toys \(sim.extras ? "on" : "off"): \(sim.snowmanCount) snowmen, \(sim.snowballCount) snowballs, plough \(sim.ploughX == nil ? "away" : "out"), sleigh \(sim.sleighX == nil ? "away" : "out")\n"
        for (i, o) in overlays.enumerated() {
            let reserve = Int(o.screen.visibleFrame.minY - o.screen.frame.minY)
            if let d = dockSpan(o.screen) {
                t += "Dock on display \(i + 1): estimated \(Int(d.x0)) to \(Int(d.x1)) (\(d.icons) icons), \(reserve) high\n"
            } else if reserve > 0 {
                t += "Dock on display \(i + 1): \(reserve) high, not at the bottom\n"
            }
        }
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(t, forType: .string)
    }
    /// A little box for a secret code. Only what is typed here is read: nothing listens to the keyboard anywhere else.
    @objc func enterCode() {
        let alert = NSAlert()
        alert.messageText = "Enter a code"
        alert.addButton(withTitle: "OK")
        alert.addButton(withTitle: "Cancel")
        let field = NSSecureTextField(frame: NSRect(x: 0, y: 0, width: 220, height: 24))
        alert.accessoryView = field
        alert.window.initialFirstResponder = field
        NSApp.activate(ignoringOtherApps: true)
        guard alert.runModal() == .alertFirstButtonReturn else { return }
        guard field.stringValue.trimmingCharacters(in: .whitespaces).lowercased() == "daddy" else { return }
        if Prefs.paused { Prefs.paused = false; updateTimer() }
        sim.startShow()
    }

    @objc func resetSettings() {
        for k in Prefs.keys { Prefs.d.removeObject(forKey: k) }
        sim.weather = Prefs.weather
        sim.wind = 0
        sim.melts = false
        sim.snowType = .balls
        sim.extras = true
        applyHotkey()
        buildOverlays()
        updateTimer()
    }
    func applyHotkey() {
        if let r = hotKeyRef { UnregisterEventHotKey(r); hotKeyRef = nil }
        guard Prefs.hotkey else { return }
        if hotKeyHandler == nil {
            var spec = EventTypeSpec(eventClass: OSType(kEventClassKeyboard), eventKind: UInt32(kEventHotKeyPressed))
            InstallEventHandler(GetApplicationEventTarget(), { _, _, data in
                guard let data = data else { return noErr }
                let app = Unmanaged<App>.fromOpaque(data).takeUnretainedValue()
                DispatchQueue.main.async { app.togglePause() }
                return noErr
            }, 1, &spec, Unmanaged.passUnretained(self).toOpaque(), &hotKeyHandler)
        }
        RegisterEventHotKey(UInt32(kVK_ANSI_S), UInt32(controlKey | optionKey | shiftKey), EventHotKeyID(signature: 0x5153_4E57, id: 1),
                            GetApplicationEventTarget(), 0, &hotKeyRef)
    }
    @objc func quit() { NSApp.terminate(nil) }
}

// MARK: entry

if CommandLine.arguments.contains("--version") {
    print("Quiet Snow \(appVersion)")
    exit(0)
}

guard let device = MTLCreateSystemDefaultDevice() else {
    fputs("Quiet Snow needs a Metal capable Mac.\n", stderr)
    exit(1)
}

if let i = CommandLine.arguments.firstIndex(of: "--snapshot") {
    guard i + 1 < CommandLine.arguments.count else { fputs("usage: QuietSnow --snapshot out.png\n", stderr); exit(2) }
    let a = CommandLine.arguments
    do { try writeSnapshot(device: device, to: a[i + 1], mix: a.contains("--mix"), show: a.contains("--show"), pink: a.contains("--pink")) } catch {
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
