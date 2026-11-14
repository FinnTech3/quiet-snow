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

/// A top edge snow can rest on. `rect.y` is the edge. `occluders` are the windows in front of it.
public struct SurfaceInput {
    public var id: Int
    public var rect: Rect
    public var occluders: [Rect]
    public var isFloor: Bool
    public init(id: Int, rect: Rect, occluders: [Rect] = [], isFloor: Bool = false) {
        self.id = id; self.rect = rect; self.occluders = occluders; self.isFloor = isFloor
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

final class Surface {
    var rect: Rect
    var isFloor: Bool
    var heights: [Float]
    var visible: [Bool]
    var spill: Float = 0
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
    public var weather: Weather = .steady { didSet { if weather != oldValue { rebuildFlakes() } } }
    public var densityScale: Float = 1 { didSet { if densityScale != oldValue { rebuildFlakes() } } }

    var clock: Float = 0
    var rng: UInt64
    var lastPointer: (x: Float, y: Float)?
    var sweepAcc: [Int: Float] = [:]
    var smoothed: [Float] = []

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

    func targetCount() -> Int {
        var area: Float = 0
        for s in world { area += s.w * s.h }
        let n = Int(weather.density * densityScale * area / 1_000_000)
        return max(60, min(n, 3500))
    }

    func rebuildFlakes() {
        guard !world.isEmpty else { return }
        normalCount = targetCount()
        let total = normalCount + Sim.debrisCapacity
        xs = [Float](repeating: 0, count: total)
        ys = xs; vys = xs; sizes = xs; phases = xs; exs = xs; eys = xs; vxs = xs; cvx = xs; cvy = xs
        state = [UInt8](repeating: 0, count: total)
        for i in 0..<normalCount {
            respawn(i)
            ys[i] = rand(worldBox.y, worldBox.maxY)   // spread over the screen so it is already snowing
            state[i] = 1
        }
        debrisCursor = normalCount
    }

    func respawn(_ i: Int) {
        // One in twenty is a big, soft, fast flake in the foreground. It never lands: it is there for depth.
        let size = rand() < 0.05 ? rand(5.5, 8.5) : rand(1.4, 4.2)
        sizes[i] = size
        vys[i] = 22 + 14 * size
        xs[i] = rand(worldBox.x - 120, worldBox.maxX + 120)
        ys[i] = worldBox.y - rand(2, 80)
        phases[i] = rand(0, 6.28)
        exs[i] = 0; eys[i] = 0
        state[i] = 1
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
        for (id, s) in surfaces where !seen.contains(id) {
            release(s)
            surfaces[id] = nil
            sweepAcc[id] = nil
        }
        order = Array(surfaces.keys).sorted()
        list = order.compactMap { surfaces[$0] }
    }

    func makeSurface(_ input: SurfaceInput) -> Surface {
        let cols = max(1, Int((input.rect.w / Sim.columnWidth).rounded(.up)))
        return Surface(rect: input.rect, isFloor: input.isFloor,
                       heights: [Float](repeating: 0, count: cols), visible: visibility(of: input))
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
        for i in 0..<xs.count where state[i] != 0 {
            let s = sizes[i]
            out.append(xs[i]); out.append(ys[i]); out.append(s * 1.6 + 1); out.append(s > 5 ? 0.3 : min(1, 0.45 + s * 0.14))
            out.append(cvx[i]); out.append(cvy[i]); out.append(0); out.append(0)
            n += 1
        }
        return n
    }

    /// Four floats per vertex: x, y, alpha, shade (0 lit, 1 shadowed). Triangles, six vertices per column. Returns the vertex count.
    @discardableResult
    public func fillPiles(_ out: inout [Float]) -> Int {
        out.removeAll(keepingCapacity: true)
        var count = 0
        @inline(__always) func v(_ x: Float, _ y: Float, _ a: Float, _ shade: Float) {
            out.append(x); out.append(y); out.append(a); out.append(shade)
            count += 1
        }
        for s in list {
            let n = s.heights.count
            if smoothed.count < n + 1 { smoothed = [Float](repeating: 0, count: n + 1) }
            // Edge heights between columns, smoothed with the neighbours, so the pile reads as one soft shape.
            for i in 0...n {
                let l = s.heights[max(0, min(n - 1, i - 1))], r = s.heights[max(0, min(n - 1, i))]
                let ll = s.heights[max(0, min(n - 1, i - 2))], rr = s.heights[max(0, min(n - 1, i + 1))]
                smoothed[i] = (ll + 3 * l + 3 * r + rr) * 0.125
            }
            let base = s.rect.y + 0.5
            for c in 0..<n where s.visible[c] {
                let l = smoothed[c], r = smoothed[c + 1]
                if l < 0.12 && r < 0.12 { continue }
                let x0 = s.rect.x + Float(c) * Sim.columnWidth
                let x1 = min(s.rect.maxX, x0 + Sim.columnWidth)
                let y0 = s.rect.y - l, y1 = s.rect.y - r
                let top: Float = 0.72, bottom: Float = 0.97, deep: Float = 0.7
                v(x0, y0, top, 0); v(x0, base, bottom, deep); v(x1, y1, top, 0)
                v(x1, y1, top, 0); v(x0, base, bottom, deep); v(x1, base, bottom, deep)
            }
        }
        return count
    }
}

// Test hooks.
extension Sim {
    static var windowCapForTests: Float { windowCap }
    func setHeightForTests(id: Int, column: Int, value: Float) {
        surfaces[id]?.heights[column] = value
    }
}
