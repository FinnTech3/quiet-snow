// The whole snow model. Foundation only, no drawing, no AppKit: that is what lets it be tested anywhere.
// Coordinates are points, origin top-left of the primary display, y grows downwards.

import Foundation

public struct Rect: Equatable {
    public var x: Float, y: Float, w: Float, h: Float
    public init(x: Float, y: Float, w: Float, h: Float) {
        self.x = x; self.y = y; self.w = w; self.h = h
    }
    public var maxX: Float { x + w }
    public var maxY: Float { y + h }
    public func contains(_ px: Float, _ py: Float) -> Bool { px >= x && px < maxX && py >= y && py < maxY }
}

/// A top edge snow can rest on. `rect.y` is the edge. `occluders` are the windows in front of it. `radius` is the corner
/// radius of the thing underneath (a window or the Dock), so the snow can follow the corner down. `plough` lets the snow
/// plough drive along a floor; the Dock is a floor it must not drive over.
public struct SurfaceInput {
    public var id: Int
    public var rect: Rect
    public var occluders: [Rect]
    public var isFloor: Bool
    public var radius: Float
    public var plough: Bool
    public init(id: Int, rect: Rect, occluders: [Rect] = [], isFloor: Bool = false, radius: Float = 0, plough: Bool = true) {
        self.id = id; self.rect = rect; self.occluders = occluders; self.isFloor = isFloor
        self.radius = radius; self.plough = plough
    }
}

public enum Weather: String, CaseIterable {
    case light, steady, flurry
    /// Flakes per million square points of screen.
    public var density: Float {
        switch self {
        case .light: return 190
        case .steady: return 500
        case .flurry: return 1400
        }
    }
}

/// What a flake looks like. The model only records which kind each flake is; the shader draws it.
public enum SnowType: Int, CaseIterable {
    case balls = 0, flakes, sleet, sparkles, hail, mixed
}

final class Surface {
    var rect: Rect
    var isFloor: Bool
    var heights: [Float]
    var visible: [Bool]
    var spill: Float = 0
    var radius: Float = 0
    var ploughable = false
    var cap: Float { isFloor ? Sim.floorCap : Sim.windowCap }
    init(rect: Rect, isFloor: Bool, heights: [Float], visible: [Bool]) {
        self.rect = rect; self.isFloor = isFloor; self.heights = heights; self.visible = visible
    }
}

public final class Sim {
    public static let columnWidth: Float = 4
    static let windowCap: Float = 16
    static let floorCap: Float = 30
    static let debrisCapacity = 600
    static let grace = 2   // polls a surface may miss before its snow is let go

    // Flake pool: [0, normalCount) are ordinary flakes, the rest is a pool for debris (state 2) or free slots (state 0).
    var xs: [Float] = [], ys: [Float] = [], vys: [Float] = [], sizes: [Float] = []
    var phases: [Float] = [], exs: [Float] = [], eys: [Float] = [], state: [UInt8] = []
    var vxs: [Float] = []
    var cvx: [Float] = [], cvy: [Float] = []     // velocity at the last step, so drawing can glide between steps
    public private(set) var normalCount = 0
    var debrisCursor = 0

    var surfaces: [Int: Surface] = [:]
    var order: [Int] = []
    var list: [Surface] = []
    public private(set) var world: [Rect] = []
    var worldBox = Rect(x: 0, y: 0, w: 1, h: 1)

    public var wind: Float = 0            // points per second, negative blows left
    public var melts = false
    public var snowType: SnowType = .balls {
        didSet { if snowType != oldValue { for i in 0..<normalCount { assignKind(i) } } }
    }
    public var weather: Weather = .steady { didSet { if weather != oldValue { rebuildFlakes() } } }
    public var densityScale: Float = 1 { didSet { if densityScale != oldValue { rebuildFlakes() } } }

    var clock: Float = 0
    var rng: UInt64
    var lastPointer: (x: Float, y: Float)?
    var sweepAcc: [Int: Float] = [:]
    var smoothed: [Float] = []
    var occluders: [Rect] = []
    var kinds: [UInt8] = []
    var missing: [Int: Int] = [:]

    /// Snowmen on window tops, the snowballs they break into, and a snow plough on the floor. Off in the model by default.
    public var extras = false { didSet { if !extras { snowmen = []; balls = []; plough = nil } } }
    var snowmen: [Snowman] = []
    var balls: [Ball] = []
    var plough: Plough?
    var snowmanClock: Float = 0
    var ploughWait: Float = 30

    public init(seed: UInt64 = 0x9E37_79B9_7F4A_7C15) {
        rng = seed == 0 ? 1 : seed
    }

    // MARK: random

    @inline(__always) func rand() -> Float {
        rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17
        return Float(rng >> 40) / Float(1 << 24)
    }

    @inline(__always) func rand(_ a: Float, _ b: Float) -> Float { a + (b - a) * rand() }

    // MARK: setup

    /// Tell the sim which screens exist (rectangles in global top-left coordinates).
    public func setWorld(_ screens: [Rect]) {
        guard let first = screens.first else { return }
        world = screens
        var minX = first.x, minY = first.y, maxX = first.maxX, maxY = first.maxY
        for s in screens {
            minX = min(minX, s.x); minY = min(minY, s.y); maxX = max(maxX, s.maxX); maxY = max(maxY, s.maxY)
        }
        worldBox = Rect(x: minX, y: minY, w: maxX - minX, h: maxY - minY)
        rebuildFlakes()
    }

    /// Windows that hide the snow behind them (desktop-only mode). Empty means the snow shows everywhere.
    public func setOccluders(_ rects: [Rect]) { occluders = rects }

    /// A window being dragged stirs the snow near it: flakes within 60 points of its edges are pushed the way it moves
    /// (velocity in points per second).
    public func stir(_ r: Rect, vx: Float, vy: Float) {
        let m: Float = 60
        for i in 0..<xs.count where state[i] != 0 {
            if xs[i] < r.x - m || xs[i] > r.maxX + m || ys[i] < r.y - m || ys[i] > r.maxY + m { continue }
            exs[i] = clamp(exs[i] + vx * 0.3, 400)
            eys[i] = clamp(eys[i] + vy * 0.3, 400)
        }
    }

    func hidden(_ x: Float, _ y: Float) -> Bool {
        for o in occluders where o.contains(x, y) { return true }
        return false
    }

    func targetCount() -> Int {
        var area: Float = 0
        for s in world { area += s.w * s.h }
        let n = Int(weather.density * densityScale * area / 1_000_000)
        return max(60, min(n, 5000))
    }

    func rebuildFlakes() {
        guard !world.isEmpty else { return }
        normalCount = targetCount()
        let total = normalCount + Sim.debrisCapacity
        xs = [Float](repeating: 0, count: total)
        ys = xs; vys = xs; sizes = xs; phases = xs; exs = xs; eys = xs; vxs = xs; cvx = xs; cvy = xs
        state = [UInt8](repeating: 0, count: total)
        kinds = state
        var area: Float = 0
        for r in world { area += r.w * r.h }
        for i in 0..<normalCount {
            respawn(i)
            // Start spread over the screens (not the gaps between them) so it is already snowing.
            var pick = rand(0, area)
            var on = world[world.count - 1]
            for r in world {
                if pick < r.w * r.h { on = r; break }
                pick -= r.w * r.h
            }
            xs[i] = rand(on.x, on.maxX)
            ys[i] = rand(on.y, on.maxY)
            state[i] = 1
        }
        debrisCursor = normalCount
    }

    func respawn(_ i: Int) {
        // One in twenty is a big, soft, fast flake in the foreground. It never lands: it is there for depth.
        let size = rand() < 0.05 ? rand(5.5, 8.5) : rand(1.4, 4.2)
        sizes[i] = size
        vys[i] = 22 + 14 * size
        // Snow comes in over the top of each screen and, with wind, over the upwind edge, in proportion to what crosses
        // each, so every part of every screen gets the same density whatever the size, layout or wind.
        let vx = wind * (0.55 + 0.1 * size)
        func upwind(_ r: Rect) -> Bool { vx > 0 ? r.x == worldBox.x : r.maxX == worldBox.maxX }
        var topW: Float = 0, edgeH: Float = 0
        for r in world {
            topW += r.w
            if upwind(r) { edgeH += r.h }
        }
        if edgeH > 0 && vx != 0 && rand() * (topW * vys[i] + edgeH * abs(vx)) < edgeH * abs(vx) {
            var pick = rand(0, edgeH)
            var on = world[0]
            for r in world where upwind(r) {
                on = r
                if pick < r.h { break }
                pick -= r.h
            }
            xs[i] = vx > 0 ? on.x - rand(2, 80) : on.maxX + rand(2, 80)
            ys[i] = rand(on.y, on.maxY)
        } else {
            var pick = rand(0, topW)
            var on = world[0]
            for r in world {
                on = r
                if pick < r.w { break }
                pick -= r.w
            }
            xs[i] = rand(on.x, on.maxX)
            ys[i] = on.y - rand(2, 80)
        }
        phases[i] = rand(0, 6.28)
        exs[i] = 0; eys[i] = 0
        state[i] = 1
        assignKind(i)
    }

    /// The big soft foreground flakes are always balls: they are there for depth. Sleet and hail fall faster.
    func assignKind(_ i: Int) {
        var k = UInt8(snowType.rawValue)
        if sizes[i] > 5 {
            k = 0
        } else if snowType == .mixed {
            let r = rand()
            k = r < 0.30 ? 0 : r < 0.52 ? 1 : r < 0.67 ? 2 : r < 0.82 ? 3 : 4
        }
        kinds[i] = k
        vys[i] = (22 + 14 * sizes[i]) * (k == 2 ? 1.5 : k == 4 ? 1.25 : 1)
    }

    // MARK: surfaces

    /// Replace the set of resting places. Piles survive where a surface has not moved; where one moved or vanished, its snow drops.
    public func setSurfaces(_ inputs: [SurfaceInput]) {
        var seen = Set<Int>()
        for input in inputs where input.rect.w >= 8 {
            seen.insert(input.id)
            if let old = surfaces[input.id] {
                let r = old.rect, n = input.rect
                if abs(r.x - n.x) > 1.5 || abs(r.y - n.y) > 1.5 || abs(r.w - n.w) > 1.5 {
                    release(old)
                    surfaces[input.id] = makeSurface(input)
                } else {
                    old.visible = visibility(of: input)
                }
            } else {
                surfaces[input.id] = makeSurface(input)
            }
        }
        for (id, s) in surfaces {
            if seen.contains(id) { missing[id] = nil; continue }
            let misses = (missing[id] ?? 0) + 1
            if misses <= Sim.grace { missing[id] = misses; continue }   // a window can drop out of one poll: keep its snow
            release(s)
            surfaces[id] = nil
            sweepAcc[id] = nil
            missing[id] = nil
        }
        order = Array(surfaces.keys).sorted()
        list = order.compactMap { surfaces[$0] }
    }

    func makeSurface(_ input: SurfaceInput) -> Surface {
        let cols = max(1, Int((input.rect.w / Sim.columnWidth).rounded(.up)))
        let s = Surface(rect: input.rect, isFloor: input.isFloor,
                        heights: [Float](repeating: 0, count: cols), visible: visibility(of: input))
        s.radius = input.radius
        s.ploughable = input.isFloor && input.plough
        return s
    }

    func visibility(of input: SurfaceInput) -> [Bool] {
        let cols = max(1, Int((input.rect.w / Sim.columnWidth).rounded(.up)))
        var out = [Bool](repeating: true, count: cols)
        let y = input.rect.y - 1
        for c in 0..<cols {
            let x = input.rect.x + (Float(c) + 0.5) * Sim.columnWidth
            if !input.isFloor && !world.contains(where: { $0.contains(x, y) }) { out[c] = false; continue }
            for o in input.occluders where o.contains(x, y) { out[c] = false; break }
        }
        return out
    }

    /// Turn a pile into falling snow.
    func release(_ s: Surface) {
        var budget = 140
        for (c, h) in s.heights.enumerated() where h > 0.6 && budget > 0 {
            let n = min(Int(h * 0.8), budget)
            let x = s.rect.x + (Float(c) + 0.5) * Sim.columnWidth
            for _ in 0..<n where budget > 0 {
                spawnDebris(x: x + rand(-2, 2), y: s.rect.y - h * rand(0.2, 1), vx: rand(-25, 25), vy: rand(-10, 40))
                budget -= 1
            }
        }
    }

    func spawnDebris(x: Float, y: Float, vx: Float, vy: Float) {
        let total = xs.count
        guard total > normalCount else { return }
        for k in 0..<Sim.debrisCapacity {
            var i = debrisCursor + k
            if i >= total { i -= Sim.debrisCapacity }
            if state[i] == 0 {
                state[i] = 2
                xs[i] = x; ys[i] = y; vxs[i] = vx; vys[i] = vy
                sizes[i] = rand(1.6, 3.0); exs[i] = 0; eys[i] = 0; phases[i] = rand(0, 6.28)
                debrisCursor = i + 1 >= total ? normalCount : i + 1
                return
            }
        }
    }

    /// Let go of every pile at once.
    public func clearPiles() {
        for s in list {
            release(s)
            for c in s.heights.indices { s.heights[c] = 0 }
        }
    }

    // MARK: stepping

    /// Advance by `dt` seconds. `pointer` is the cursor in the same coordinates, nil when unknown.
    public func step(dt rawDt: Float, pointer: (x: Float, y: Float)? = nil) {
        guard !xs.isEmpty else { return }
        let dt = min(max(rawDt, 0), 0.1)
        clock += dt
        let gust = wind * (1 + 0.3 * sin(clock * 0.45))

        var pv: (x: Float, y: Float) = (0, 0)
        var speed: Float = 0
        if let p = pointer, let q = lastPointer, dt > 0 {
            pv = ((p.x - q.x) / dt, (p.y - q.y) / dt)
            speed = (pv.x * pv.x + pv.y * pv.y).squareRoot()
        }
        lastPointer = pointer

        let drag = exp(-3 * dt)
        let radius: Float = 70
        for i in 0..<xs.count where state[i] != 0 {
            let prevY = ys[i]
            if state[i] == 2 {
                vys[i] = min(vys[i] + 420 * dt, 240)
                cvx[i] = vxs[i] + exs[i]
                xs[i] += cvx[i] * dt
                vxs[i] *= exp(-1.2 * dt)
            } else {
                phases[i] += dt * 0.9
                let sway = sin(phases[i]) * (6 + sizes[i] * 2)
                cvx[i] = gust * (0.55 + 0.1 * sizes[i]) + sway + exs[i]
                xs[i] += cvx[i] * dt
            }
            cvy[i] = vys[i] + eys[i]
            ys[i] += cvy[i] * dt

            if let p = pointer, speed > 20 {
                let dx = xs[i] - p.x, dy = ys[i] - p.y
                let d2 = dx * dx + dy * dy
                if d2 < radius * radius && d2 > 0.01 {
                    let d = d2.squareRoot()
                    let k = (1 - d / radius) * (120 + min(speed, 1500) * 0.25)
                    exs[i] = clamp(exs[i] + dx / d * k * dt * 8, 400)
                    eys[i] = clamp(eys[i] + dy / d * k * dt * 8, 400)
                }
            }
            exs[i] *= drag; eys[i] *= drag

            if land(i, prevY) { continue }

            if ys[i] > worldBox.maxY + 6 || xs[i] < worldBox.x - 300 || xs[i] > worldBox.maxX + 300 {
                if state[i] == 2 { state[i] = 0 } else { respawn(i) }
            }
        }

        settle(dt: dt)
        if let p = pointer, speed > 120 { sweep(at: p, velocity: pv, speed: speed, dt: dt) }
        if extras { stepExtras(dt: dt, pointer: pointer, velocity: pv, speed: speed) }
    }

    @inline(__always) func clamp(_ v: Float, _ m: Float) -> Float { max(-m, min(m, v)) }

    /// Returns true when the flake came to rest (and was recycled).
    func land(_ i: Int, _ prevY: Float) -> Bool {
        if sizes[i] > 5 { return false }
        let x = xs[i], y = ys[i]
        for s in list {
            // Cheap rejects first: the edge, then the tallest a pile can be.
            if y < s.rect.y - Sim.floorCap || prevY > s.rect.y || x < s.rect.x || x >= s.rect.maxX { continue }
            let c = min(s.heights.count - 1, Int((x - s.rect.x) / Sim.columnWidth))
            guard s.visible[c] else { continue }
            let top = s.rect.y - s.heights[c]
            guard prevY <= top && y >= top else { continue }
            let cap = s.cap
            // Find the lowest visible neighbour so a full column hands the flake sideways instead of stacking spikes.
            var best = c
            for d in -2...2 {
                let n = c + d
                if n >= 0 && n < s.heights.count && s.visible[n] && s.heights[n] < s.heights[best] { best = n }
            }
            if s.heights[best] < cap {
                s.heights[best] = min(cap, s.heights[best] + (state[i] == 2 ? 1.0 : 0.3 * sizes[i]))
            }
            if state[i] == 2 { state[i] = 0 } else { respawn(i) }
            return true
        }
        return false
    }

    /// Slump steep slopes, shed over edges, melt.
    func settle(dt: Float) {
        for s in list {
            let n = s.heights.count
            let slope: Float = s.isFloor ? 3.0 : 2.2
            for pass in 0..<2 {
                for k in 0..<(n - 1) {
                    let i = pass == 0 ? k : n - 2 - k
                    guard s.visible[i] && s.visible[i + 1] else { continue }
                    let d = s.heights[i] - s.heights[i + 1]
                    if d > slope { let t = (d - slope) * 0.25; s.heights[i] -= t; s.heights[i + 1] += t }
                    else if d < -slope { let t = (-d - slope) * 0.25; s.heights[i] += t; s.heights[i + 1] -= t }
                }
            }
            if !s.isFloor {
                let edgeCap: Float = 3.0
                for (c, dir) in [(0, Float(-1)), (n - 1, Float(1))] where s.heights[c] > edgeCap {
                    let take = (s.heights[c] - edgeCap) * min(1, 2.5 * dt)
                    s.heights[c] -= take
                    s.spill += take
                    while s.spill >= 1 {
                        s.spill -= 1
                        let x = dir < 0 ? s.rect.x - 1 : s.rect.maxX + 1
                        spawnDebris(x: x, y: s.rect.y - 2, vx: dir * rand(8, 40), vy: rand(0, 25))
                    }
                }
            }
            if melts {
                let f = exp(-0.012 * dt)
                for c in 0..<n { s.heights[c] = max(0, s.heights[c] * f - 0.03 * dt) }
            }
        }
    }

    /// A moving cursor over a pile throws some of it.
    func sweep(at p: (x: Float, y: Float), velocity v: (x: Float, y: Float), speed: Float, dt: Float) {
        let reach: Float = 38
        for id in order {
            guard let s = surfaces[id], abs(p.y - s.rect.y) < 34,
                  p.x > s.rect.x - reach, p.x < s.rect.maxX + reach else { continue }
            var thrown: Float = sweepAcc[id] ?? 0
            let lo = max(0, Int((p.x - reach - s.rect.x) / Sim.columnWidth))
            let hi = min(s.heights.count - 1, Int((p.x + reach - s.rect.x) / Sim.columnWidth))
            if lo <= hi {
                for c in lo...hi where s.visible[c] && s.heights[c] > 0.05 {
                    let cx = s.rect.x + (Float(c) + 0.5) * Sim.columnWidth
                    let w = 1 - abs(cx - p.x) / reach
                    guard w > 0 else { continue }
                    let take = min(s.heights[c], s.heights[c] * 0.5 * w + 0.15 * w)
                    s.heights[c] -= take
                    thrown += take
                }
            }
            while thrown >= 1 {
                thrown -= 1
                spawnDebris(x: p.x + rand(-reach, reach), y: s.rect.y - rand(1, 6),
                            vx: v.x * 0.3 + rand(-45, 45), vy: -rand(40, 150))
            }
            sweepAcc[id] = thrown
        }
    }

    // MARK: reading state

    public var totalSnowOnSurfaces: Float {
        surfaces.values.reduce(0) { $0 + $1.heights.reduce(0, +) }
    }

    public var liveFlakeCount: Int { state.reduce(0) { $0 + ($1 != 0 ? 1 : 0) } }
    public var debrisCount: Int { state.indices.filter { $0 >= normalCount && state[$0] == 2 }.count }
    public var surfaceCount: Int { surfaces.count }
    public var flakeCapacity: Int { xs.count }

    public func heights(of id: Int) -> [Float]? { surfaces[id]?.heights }

    // MARK: render data

    /// Eight floats per flake: x, y, diameter, alpha, then vx, vy and two spare. The velocity lets the renderer glide a flake
    /// between steps, so the model can step at 30 Hz while the screen draws at 60. Returns the flake count.
    @discardableResult
    public func fillFlakes(_ out: inout [Float]) -> Int {
        var n = 0
        out.removeAll(keepingCapacity: true)
        for i in 0..<xs.count where state[i] != 0 && !hidden(xs[i], ys[i]) {
            let s = sizes[i]
            out.append(xs[i]); out.append(ys[i]); out.append(s * 1.6 + 1); out.append(s > 5 ? 0.3 : min(1, 0.45 + s * 0.14))
            out.append(cvx[i]); out.append(cvy[i]); out.append(Float(kinds[i])); out.append(phases[i])
            n += 1
        }
        return n + fillExtras(&out)
    }

    /// Four floats per vertex: x, y, alpha, shade (0 lit, 1 shadowed). Triangles, twelve vertices per column: the body,
    /// then a feather above it so the top edge fades over a few points instead of cutting. The profile is smoothed over
    /// about 24 points, and every end that is not at a screen edge is rounded. Returns the vertex count.
    @discardableResult
    public func fillPiles(_ out: inout [Float]) -> Int {
        out.removeAll(keepingCapacity: true)
        var count = 0
        @inline(__always) func v(_ x: Float, _ y: Float, _ a: Float, _ shade: Float) {
            out.append(x); out.append(y); out.append(a); out.append(shade)
            count += 1
        }
        let k: [Float] = [1, 5, 10, 10, 5, 1]
        for s in list {
            let n = s.heights.count
            if smoothed.count < n + 1 { smoothed = [Float](repeating: 0, count: n + 1) }
            func h(_ i: Int) -> Float {
                if i < 0 || i >= n { return s.isFloor ? s.heights[max(0, min(n - 1, i))] : 0 }
                return s.visible[i] ? s.heights[i] : 0
            }
            for i in 0...n {
                var t: Float = 0
                for j in 0..<6 { t += k[j] * h(i - 3 + j) }
                smoothed[i] = t / 32
            }
            let feather: Float = 2.5
            let top: Float = 0.72, bottom: Float = 0.97, deep: Float = 0.7
            // An end of a pile that is not at the edge of a screen is rounded off: the snow follows the corner of the window
            // (or the Dock) down, and its top curves down to meet it, so no end is a straight cut. A floor runs on to the
            // screen edge at full height.
            let openL = !(s.isFloor && atScreenEdge(s.rect.x)), openR = !(s.isFloor && atScreenEdge(s.rect.maxX))
            let rad = s.radius
            func shape(_ x: Float, _ h: Float) -> (top: Float, base: Float) {
                var d = Float.greatestFiniteMagnitude
                if openL { d = min(d, x - s.rect.x) }
                if openR { d = min(d, s.rect.maxX - x) }
                var drop: Float = 0
                if rad > 0 && d < rad { let k = rad - max(0, d); drop = rad - max(0, rad * rad - k * k).squareRoot() }
                let len = min(26, max(4, h * 1.3))
                var f: Float = 1
                if d < len { let t = 1 - max(0, d) / len; f = max(0, 1 - t * t).squareRoot() }
                return (s.rect.y + drop - h * f, s.rect.y + drop + 0.5)
            }
            for c in 0..<n where s.visible[c] {
                let l = smoothed[c], r = smoothed[c + 1]
                if l < 0.12 && r < 0.12 { continue }
                let x0 = s.rect.x + Float(c) * Sim.columnWidth
                let x1 = min(s.rect.maxX, x0 + Sim.columnWidth)
                if hidden((x0 + x1) * 0.5, s.rect.y - 1) { continue }
                // Near a rounded end, draw the column in 1-point slices so the curve is smooth, not faceted.
                let near = min(openL ? x1 - s.rect.x : .greatestFiniteMagnitude, openR ? s.rect.maxX - x0 : .greatestFiniteMagnitude)
                let slices = near < 30 ? 4 : 1
                for j in 0..<slices {
                    let fa = Float(j) / Float(slices), fb = Float(j + 1) / Float(slices)
                    let xa = x0 + (x1 - x0) * fa, xb = x0 + (x1 - x0) * fb
                    let (ya, ba) = shape(xa, l + (r - l) * fa), (yb, bb) = shape(xb, l + (r - l) * fb)
                    v(xa, ya, top, 0); v(xa, ba, bottom, deep); v(xb, yb, top, 0)
                    v(xb, yb, top, 0); v(xa, ba, bottom, deep); v(xb, bb, bottom, deep)
                    v(xa, ya - feather, 0, 0); v(xa, ya, top, 0); v(xb, yb - feather, 0, 0)
                    v(xb, yb - feather, 0, 0); v(xa, ya, top, 0); v(xb, yb, top, 0)
                }
            }
        }
        return count
    }
}

// Test hooks.
extension Sim {
    static var windowCapForTests: Float { windowCap }
    static var floorCapForTests: Float { floorCap }
    func setHeightForTests(id: Int, column: Int, value: Float) {
        surfaces[id]?.heights[column] = value
    }
}
