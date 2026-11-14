// The hidden show: every flake in the air and all the snow in the piles rises up and spells two messages, then drops
// to the floor, which turns pink and sparkles with gems for 15 seconds. Ported by hand to windows/sim.hpp: keep the two
// in step, including the order of every rand() call.

import Foundation

struct ShowParticle {
    var x: Float, y: Float, vx: Float, vy: Float, size: Float
    var kind: UInt8
    var amount: Float     // the snow it carries, put back on the floor where it lands
    var delay: Float      // when it starts for its place in the words
    var phase: Float
    var landed = false
}

struct Spark {
    var x: Float, y: Float, vx: Float, vy: Float, life: Float, maxLife: Float
    var hue: Float               // which colour: red, gold, green, blue, pink, purple, white
    var size: Float
    var rocket: Bool             // still climbing; it bursts at the top
}

struct Gem {
    var sid: Int
    var x: Float, depth: Float   // depth: how far down into the pile, 0 top to 1 bottom
    var tag: Float               // colour (whole part) and twinkle phase (fraction)
    var size: Float
    var sparkle: Bool            // a twinkling star rather than a gem
}

/// A 5 by 7 pixel font, just the letters the messages need.
let showFont: [Character: [String]] = [
    "M": ["#...#", "##.##", "#.#.#", "#.#.#", "#...#", "#...#", "#...#"],
    "C": [".###.", "#...#", "#....", "#....", "#....", "#...#", ".###."],
    "A": [".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"],
    "F": ["#####", "#....", "#....", "####.", "#....", "#....", "#...."],
    "H": ["#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"],
    "e": [".....", ".....", ".###.", "#...#", "#####", "#....", ".###."],
    "r": [".....", ".....", "#.##.", "##..#", "#....", "#....", "#...."],
    "y": [".....", ".....", "#...#", "#...#", ".####", "....#", ".###."],
    "h": ["#....", "#....", "#.##.", "##..#", "#...#", "#...#", "#...#"],
    "i": ["..#..", ".....", ".##..", "..#..", "..#..", "..#..", ".###."],
    "s": [".....", ".....", ".####", "#....", ".###.", "....#", "####."],
    "t": [".#...", ".#...", "####.", ".#...", ".#...", ".#..#", "..##."],
    "m": [".....", ".....", "##.#.", "#.#.#", "#.#.#", "#.#.#", "#.#.#"],
    "a": [".....", ".....", ".###.", "....#", ".####", "#...#", ".####"],
    "l": [".##..", "..#..", "..#..", "..#..", "..#..", "..#..", ".###."],
    "c": [".....", ".....", ".###.", "#....", "#....", "#...#", ".###."],
    "v": [".....", ".....", "#...#", "#...#", "#...#", ".#.#.", "..#.."],
    "o": [".....", ".....", ".###.", "#...#", "#...#", "#...#", ".###."],
    "u": [".....", ".....", "#...#", "#...#", "#...#", "#..##", ".##.#"],
]

extension Sim {
    static let showMessages = [["Merry Christmas", "Alice"], ["My Favourite", "Ho Ho Ho"]]
    static let pinkSeconds: Float = 15

    /// Points that spell `lines`, centred in the upper part of a display, a few points apart.
    static func textPoints(_ lines: [String], on d: Rect) -> [Float] {
        let widest = Float(lines.map { $0.count }.max() ?? 1)
        let px = min(d.w * 0.84 / (widest * 6), d.h * 0.46 / Float(lines.count * 9))
        let sub = max(1, Int(px / 5))
        let step = px / Float(sub)
        let top = d.y + d.h * 0.36 - Float(lines.count * 9) * px / 2
        var out: [Float] = []
        for (row, line) in lines.enumerated() {
            let left = d.x + (d.w - Float(line.count * 6 - 1) * px) / 2
            for (col, ch) in line.enumerated() {
                guard let g = showFont[ch] else { continue }
                for (gy, bits) in g.enumerated() {
                    for (gx, b) in bits.enumerated() where b == "#" {
                        for sy in 0..<sub {
                            for sx in 0..<sub {
                                out.append(left + Float(col * 6 + gx) * px + (Float(sx) + 0.5) * step)
                                out.append(top + Float(row * 9 + gy) * px + (Float(sy) + 0.5) * step)
                            }
                        }
                    }
                }
            }
        }
        return out
    }

    public var showActive: Bool { showT >= 0 }
    public var showTime: Float { showT }

    /// 0 normally, 1 while the snow is pink, easing in and out over a second.
    public var pinkAmount: Float { pinkLeft <= 0 ? 0 : max(0, min(1, pinkLeft, Sim.pinkSeconds - pinkLeft)) }

    /// Start the show. Up to 60% of the dots come from snow in the air, the rest rise out of every pile and mountain
    /// (which empties them); if that is still not enough, more falls in from above.
    public func startShow() {
        guard !world.isEmpty, showT < 0 else { return }
        let d = world[0]
        showTargets = Sim.showMessages.map { Sim.textPoints($0, on: d) }
        let want = min(2500, max(showTargets[0].count, showTargets[1].count) / 2)
        showParts = []
        let fromAir = want * 6 / 10
        for i in 0..<xs.count where state[i] != 0 && showParts.count < fromAir {
            showParts.append(ShowParticle(x: xs[i], y: ys[i], vx: cvx[i], vy: cvy[i], size: min(sizes[i], 4.2), kind: kinds[i],
                                          amount: state[i] == 2 ? 1 : 0.3 * sizes[i], delay: rand(0, 1.2), phase: rand(0, 6.28)))
        }
        for i in 0..<xs.count { state[i] = 0 }
        var total: Float = 0
        for s in list { for c in s.heights.indices where s.visible[c] { total += s.heights[c] } }
        let quota = want - showParts.count
        if quota > 0 && total > 0 {
            let per = max(0.05, total / Float(quota))
            var acc: Float = 0
            for s in list {
                for c in s.heights.indices where s.visible[c] {
                    acc += s.heights[c]
                    while acc >= per && showParts.count < want {
                        acc -= per
                        let take = min(s.heights[c], per)
                        s.heights[c] -= take
                        let x = s.rect.x + (Float(c) + 0.5) * Sim.columnWidth + rand(-2, 2)
                        showParts.append(ShowParticle(x: x, y: s.rect.y - s.heights[c] - rand(0, 2), vx: rand(-20, 20),
                                                      vy: -rand(30, 90), size: rand(1.6, 3.4), kind: UInt8(snowType == .mixed ? Int(rand(0, 5)) : snowType.rawValue),
                                                      amount: take, delay: rand(0.3, 2.0), phase: rand(0, 6.28)))
                    }
                }
            }
        }
        // Whatever the even shares left behind goes with the snow that rose, so the piles are really empty.
        let firstPile = min(showParts.count, fromAir)
        if showParts.count > firstPile {
            var rest: Float = 0
            for s in list { for c in s.heights.indices where s.visible[c] { rest += s.heights[c]; s.heights[c] = 0 } }
            let each = rest / Float(showParts.count - firstPile)
            for i in firstPile..<showParts.count { showParts[i].amount += each }
        }
        while showParts.count < want {
            showParts.append(ShowParticle(x: rand(d.x, d.maxX), y: d.y - rand(10, 200), vx: 0, vy: rand(30, 60), size: rand(1.4, 3.2),
                                          kind: UInt8(snowType == .mixed ? Int(rand(0, 5)) : snowType.rawValue), amount: 0,
                                          delay: rand(0, 2.5), phase: rand(0, 6.28)))
        }
        showT = 0
        fireworkClock = 0
        emit(.chime)
    }

    /// A white flash and a ring of 60 to 90 sparks, a third of them in a second colour.
    func burst(x: Float, y: Float, hue: Float) -> [Spark] {
        var out: [Spark] = []
        let n = 60 + Int(rand(0, 30))
        let second = Float(Int(rand(0, 7)))
        out.append(Spark(x: x, y: y, vx: 0, vy: 0, life: 0.25, maxLife: 0.25, hue: 6, size: 12, rocket: false))   // the flash
        for k in 0..<n {
            let a = Float(k) / Float(n) * 6.2832 + rand(-0.05, 0.05)
            let speed = rand(160, 320)
            let life = rand(1.0, 1.6)
            out.append(Spark(x: x, y: y, vx: cos(a) * speed, vy: sin(a) * speed, life: life, maxLife: life,
                             hue: k % 3 == 0 ? second : hue, size: rand(2.5, 4), rocket: false))
        }
        return out
    }

    /// Rockets go up from the bottom of the screen, trailing gold, and burst into coloured sparks that fall and fade.
    func stepFireworks(dt: Float, launching: Bool) {
        if launching, let d = world.first {
            fireworkClock -= dt
            if fireworkClock <= 0 {
                fireworkClock = rand(0.3, 0.6)
                sparks.append(Spark(x: rand(d.x + d.w * 0.08, d.maxX - d.w * 0.08), y: d.maxY, vx: rand(-40, 40), vy: -rand(480, 660),
                                    life: 3, maxLife: 3, hue: Float(Int(rand(0, 7))), size: 3.2, rocket: true))
                rocketsLaunched += 1
            }
        }
        var born: [Spark] = []
        var i = 0
        while i < sparks.count {
            var s = sparks[i]
            if s.rocket {
                s.vy += 300 * dt
                s.x += s.vx * dt
                s.y += s.vy * dt
                born.append(Spark(x: s.x + rand(-1, 1), y: s.y + 4, vx: rand(-10, 10), vy: rand(10, 40), life: 0.45, maxLife: 0.45,
                                  hue: 1, size: 1.6, rocket: false))
                if s.vy > -80 {                          // the top of its climb: burst
                    born.append(contentsOf: burst(x: s.x, y: s.y, hue: s.hue))
                    if clock - lastPop > 0.12 { lastPop = clock; emit(.pop) }
                    sparks.remove(at: i)
                    continue
                }
            } else {
                let drag = exp(-1.2 * dt)
                s.vy += 120 * dt
                s.vx *= drag; s.vy *= drag
                s.x += s.vx * dt
                s.y += s.vy * dt
                s.life -= dt
                if s.life <= 0 { sparks.remove(at: i); continue }
            }
            sparks[i] = s
            i += 1
        }
        sparks.append(contentsOf: born.prefix(max(0, 1500 - sparks.count)))
    }

    func stepShow(dt: Float) {
        showT += dt
        stepFireworks(dt: dt, launching: showT >= 9 && showT < 17.5)   // while "My Favourite Ho Ho Ho" is up
        let n = showParts.count
        if showT < 15 {
            let targets = showTargets[showT < 9 ? 0 : 1]
            let t = targets.count / 2
            guard t > 0 else { endShow(); return }
            for i in 0..<n {
                var p = showParts[i]
                if showT >= p.delay {
                    let k = i * t / n
                    let tx = targets[2 * k] + sin(showT * 2 + p.phase) * 0.6
                    let ty = targets[2 * k + 1] + cos(showT * 1.7 + p.phase) * 0.6
                    p.vx += (10 * (tx - p.x) - 6 * p.vx) * dt
                    p.vy += (10 * (ty - p.y) - 6 * p.vy) * dt
                } else {
                    p.vx *= exp(-dt)
                }
                p.x += p.vx * dt
                p.y += p.vy * dt
                showParts[i] = p
            }
            return
        }
        // Everything drops to the floor.
        var left = 0
        for i in 0..<n where !showParts[i].landed {
            var p = showParts[i]
            p.vy = min(p.vy + 500 * dt, 600)
            p.vx *= exp(-dt)
            p.x += p.vx * dt
            p.y += p.vy * dt
            for s in list where s.isFloor && p.x >= s.rect.x && p.x < s.rect.maxX {
                let c = column(s, p.x)
                if p.y >= s.rect.y - s.heights[c] {
                    s.heights[c] = min(s.cap, s.heights[c] + p.amount)
                    p.landed = true
                    break
                }
            }
            if p.y > worldBox.maxY + 10 { p.landed = true }
            if !p.landed { left += 1 }
            showParts[i] = p
        }
        if left == 0 || showT > 24 { endShow() }
    }

    func endShow() {
        showT = -1
        showParts = []
        for i in 0..<normalCount { respawn(i) }
        pinkLeft = Sim.pinkSeconds
        gems = []
        for (id, s) in zip(order, list) {
            for c in stride(from: 2, to: s.heights.count, by: 4) where s.visible[c] && s.heights[c] >= 2 && gems.count < 320 {
                gems.append(Gem(sid: id, x: s.rect.x + (Float(c) + 0.5) * Sim.columnWidth + rand(-2, 2), depth: rand(0.15, 0.85),
                                tag: Float(Int(rand(0, 6))) + rand(0, 0.99), size: rand(10, 16), sparkle: rand() < 0.35))
            }
        }
    }

    func stepPink(dt: Float) {
        guard pinkLeft > 0 else { return }
        pinkLeft -= dt
        if pinkLeft <= 0 { pinkLeft = 0; gems = [] }
    }

    /// The show's dots (in place of the flakes), its fireworks (kind 11), and the gems and sparkles while the snow is pink.
    func fillShow(_ out: inout [Float]) -> Int {
        var n = 0
        for s in sparks {
            out.append(s.x); out.append(s.y); out.append(s.size * 3.5); out.append(s.rocket ? 1 : max(0, s.life / s.maxLife))
            out.append(s.vx); out.append(s.vy); out.append(11); out.append(s.hue)
            n += 1
        }
        for p in showParts where !p.landed {
            // Alpha above 1 asks the shader for a faint dark rim, so the words show over a white window too.
            out.append(p.x); out.append(p.y); out.append(p.size * 1.6 + 1); out.append(min(1, 0.55 + p.size * 0.14) + 2)
            out.append(p.vx); out.append(p.vy); out.append(Float(p.kind)); out.append(p.phase)
            n += 1
        }
        for g in gems {
            guard let s = surfaces[g.sid] else { continue }
            let h = s.heights[column(s, g.x)]
            guard h >= 1 else { continue }
            out.append(g.x); out.append(s.rect.y - h * (1 - g.depth)); out.append(g.sparkle ? g.size * 0.8 : g.size); out.append(1)
            out.append(0); out.append(0); out.append(g.sparkle ? 3 : 10); out.append(g.sparkle ? g.tag * 6.28 : g.tag)
            n += 1
        }
        return n
    }

    public var gemCount: Int { gems.count }
    public var sparkCount: Int { sparks.count }
}
