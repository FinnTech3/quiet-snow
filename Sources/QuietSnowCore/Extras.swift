// Snowmen, snowballs you roll yourself, the snow plough and Santa's sleigh. Ported by hand to windows/sim.hpp: keep the
// two in step, including the order of every rand() call.

import Foundation

struct Snowman {
    var sid: Int          // the surface it stands on
    var x: Float
    var rx: Float, ry: Float   // where that surface was when it was built: if it moves, the snowman falls
    var build: Float      // 0 to 1: bottom ball, then the middle, the head, then the hat and scarf
    var top: Float        // the snow it stands on, updated every step
    var size: Float = Sim.snowmanSize
    var made = false      // stacked by the user from three snowballs: it stays even on bare ground
}

struct Ball {
    var x: Float, y: Float, vx: Float, vy: Float, r: Float, life: Float
    var on: Int?          // the surface it is resting on, if any
    var rx: Float = 0, ry: Float = 0
    var id = 0
    var placed = false    // rolled and dropped by the user: it does not melt
    var support: Int?     // the ball it is resting on, if any
}

struct Plough {
    var sid: Int
    var x: Float          // the middle of the plough
    var dir: Float        // +1 drives right, -1 left
    var backing: Bool     // reversing out after leaving a heap against the Dock
    var rx: Float, ry: Float
    var vx: Float = 0
}

struct Sleigh {
    var sid: Int
    var x: Float          // the sleigh itself; the reindeer run ahead of it
    var from: Float, to: Float
    var level: Float      // the height of snow it leaves behind on this pass
    var mountain: Bool    // flattening a mountain on a floor, a layer per pass, rather than clearing one window
    var y: Float          // the line the runners skim
    var pause: Float = 0  // between passes over a mountain
}

extension Sim {
    static let snowmanSize: Float = 30
    static let ploughSize: Float = 56
    static let sleighSize: Float = 56
    static let reindeerSize: Float = 40
    static let maxPlaced = 12

    func atScreenEdge(_ x: Float) -> Bool {
        world.contains { abs($0.x - x) < 2 || abs($0.maxX - x) < 2 }
    }

    func column(_ s: Surface, _ x: Float) -> Int {
        max(0, min(s.heights.count - 1, Int((x - s.rect.x) / Sim.columnWidth)))
    }

    func moved(_ s: Surface, _ rx: Float, _ ry: Float) -> Bool { abs(s.rect.x - rx) > 1.5 || abs(s.rect.y - ry) > 1.5 }

    /// The display a surface belongs to.
    func display(of s: Surface) -> Rect {
        let mx = s.rect.x + s.rect.w / 2
        return world.first { $0.x <= mx && mx < $0.maxX && $0.y < s.rect.y && s.rect.y <= $0.maxY + 1 } ?? world.first ?? Rect(x: 0, y: 0, w: 0, h: 0)
    }

    func maxHeight(_ s: Surface) -> Float { s.heights.max() ?? 0 }

    func stepExtras(dt: Float, pointer: (x: Float, y: Float)?, velocity pv: (x: Float, y: Float), speed: Float) {
        let p = carrying == nil ? pointer : nil      // while you carry a snowball the cursor knocks nothing over
        stepSnowmen(dt: dt, pointer: p, pvx: pv.x, speed: speed)
        stepBalls(dt: dt, pointer: p, pvx: pv.x, speed: speed)
        buildStacks()
        snowmanClock += dt
        if snowmanClock >= 2.5 { snowmanClock -= 2.5; trySpawnSnowman() }
        stepSleigh(dt: dt)
        stepPlough(dt: dt)
    }

    // MARK: snowmen

    /// A snowman builds itself where a window has a deep, even stretch of snow away from its ends. At most three at once.
    func trySpawnSnowman() {
        guard snowmen.filter({ !$0.made }).count < 3 else { return }
        let span = 10, margin = 8
        for (id, s) in zip(order, list) where !s.isFloor && s.rect.w >= 140 {
            if snowmen.contains(where: { $0.sid == id }) { continue }
            let n = s.heights.count
            guard n >= span + 2 * margin else { continue }
            var best = -1
            var bestMin: Float = 0
            for start in margin...(n - margin - span) {
                var lo = Float.greatestFiniteMagnitude
                for c in start..<(start + span) { lo = min(lo, s.visible[c] ? s.heights[c] : 0) }
                if lo > bestMin { bestMin = lo; best = start }
            }
            if best >= 0 && bestMin >= 4 && rand() < 0.4 {
                let x = s.rect.x + (Float(best) + Float(span) / 2) * Sim.columnWidth
                snowmen.append(Snowman(sid: id, x: x, rx: s.rect.x, ry: s.rect.y, build: 0, top: s.rect.y - bestMin))
                return
            }
        }
    }

    func stepSnowmen(dt: Float, pointer: (x: Float, y: Float)?, pvx: Float, speed: Float) {
        var i = 0
        while i < snowmen.count {
            var m = snowmen[i]
            var fall = false
            var push: Float = 0
            if let s = surfaces[m.sid], !moved(s, m.rx, m.ry) {
                let c = column(s, m.x)
                m.top = s.rect.y - s.heights[c]
                if !m.made && s.heights[c] < 1.5 { fall = true }   // the snow under it was swept or melted away
                m.build = min(1, m.build + dt / 2.5)
                if let p = pointer, speed > 120, abs(p.x - m.x) < m.size * 0.45, p.y < m.top + 2, p.y > m.top - m.size - 4 {
                    fall = true
                    push = pvx
                }
            } else {
                fall = true                              // its window moved or closed
            }
            if fall {
                breakSnowman(m, push: push)
                snowmen.remove(at: i)
                continue
            }
            snowmen[i] = m
            i += 1
        }
    }

    /// The balls it was built from tumble off, with a puff of loose snow.
    func breakSnowman(_ m: Snowman, push: Float) {
        let half = m.size / 2
        let cy = m.top - half + 1
        let parts: [(y: Float, r: Float, at: Float)] = [(0.55, 0.42, 0), (0.0, 0.31, 0.33), (-0.42, 0.22, 0.6)]
        for p in parts where m.build > p.at {
            if balls.count >= 24 { break }
            balls.append(Ball(x: m.x, y: cy + p.y * half, vx: push * 0.3 + rand(-40, 40), vy: -rand(20, 70),
                              r: p.r * half, life: rand(5, 8)))
        }
        for _ in 0..<6 {
            spawnDebris(x: m.x + rand(-6, 6), y: cy + rand(-8, 8), vx: push * 0.2 + rand(-50, 50), vy: -rand(20, 90))
        }
    }

    /// Three balls resting on top of each other become a snowman, sized by the bottom one.
    func buildStacks() {
        for c in balls {
            guard let bid = c.support, let b = balls.first(where: { $0.id == bid }),
                  let aid = b.support, let a = balls.first(where: { $0.id == aid }), let sid = a.on,
                  let s = surfaces[sid] else { continue }
            let size = min(140, max(24, a.r * 2 / 0.42))
            snowmen.append(Snowman(sid: sid, x: a.x, rx: s.rect.x, ry: s.rect.y, build: 0.6, top: a.y + a.r, size: size, made: true))
            balls.removeAll { $0.id == a.id || $0.id == b.id || $0.id == c.id }
            return                                       // one per step is plenty
        }
    }

    // MARK: snowballs

    func stepBalls(dt: Float, pointer: (x: Float, y: Float)?, pvx: Float, speed: Float) {
        var i = 0
        while i < balls.count {
            var b = balls[i]
            if let p = pointer, speed > 120 {
                let dx = b.x - p.x, dy = b.y - p.y
                if dx * dx + dy * dy < (b.r + 12) * (b.r + 12) {
                    b.vx += pvx * 0.4
                    b.vy = min(b.vy, 0) - 120
                    b.on = nil
                    b.support = nil
                }
            }
            if let sup = b.support {
                // Sitting on another ball: follow it, or fall if it has gone or is falling itself.
                if let a = balls.first(where: { $0.id == sup }), a.on != nil || a.support != nil {
                    b.x = a.x
                    b.y = a.y - (a.r + b.r) * 0.92
                } else {
                    b.support = nil
                }
            } else if let id = b.on, let s = surfaces[id], !moved(s, b.rx, b.ry) {
                b.vx *= exp(-1.8 * dt)
                b.x += b.vx * dt
                if b.x < s.rect.x || b.x >= s.rect.maxX {
                    b.on = nil                           // rolled off the end
                } else {
                    b.y = s.rect.y - s.heights[column(s, b.x)] - b.r
                }
            } else {
                b.on = nil
                let prevY = b.y
                b.vy = min(b.vy + 520 * dt, 420)
                b.x += b.vx * dt
                b.y += b.vy * dt
                b.vx *= exp(-0.6 * dt)
                // Land on a resting ball: stack on it if it hits near the top, roll off if not.
                for a in balls where a.id != b.id && (a.on != nil || a.support != nil) {
                    let dx = b.x - a.x
                    guard abs(dx) < a.r + b.r, prevY + b.r <= a.y - a.r * 0.5, b.y + b.r >= a.y - a.r * 0.9 else { continue }
                    if abs(dx) < a.r * 0.9 {
                        b.support = a.id
                        b.vx = 0; b.vy = 0
                        b.x = a.x
                        b.y = a.y - (a.r + b.r) * 0.92
                    } else {
                        b.vx += (dx < 0 ? -1 : 1) * 60
                    }
                    break
                }
                if b.support == nil {
                    for (id, s) in zip(order, list) where b.x >= s.rect.x && b.x < s.rect.maxX {
                        let c = column(s, b.x)
                        guard s.visible[c] else { continue }
                        let top = s.rect.y - s.heights[c]
                        // Its middle was above the edge and it now touches the snow (dropped inside a pile, it pops up).
                        guard prevY <= s.rect.y && b.y + b.r >= top else { continue }
                        b.y = top - b.r
                        if b.vy > 90 {
                            b.vy = -b.vy * 0.35
                            b.vx *= 0.8
                        } else {
                            b.vy = 0
                            b.on = id; b.rx = s.rect.x; b.ry = s.rect.y
                        }
                        break
                    }
                }
            }
            if !b.placed { b.life -= dt }
            var gone = b.y > worldBox.maxY + 20 || b.x < worldBox.x - 300 || b.x > worldBox.maxX + 300
            if !gone && !b.placed && b.life <= 0 {
                // It melts back into the pile it is sitting on.
                if let id = b.on, let s = surfaces[id] {
                    let c = column(s, b.x)
                    let lo = max(0, c - 2), hi = min(s.heights.count - 1, c + 2)
                    let add = b.r * 1.2 / Float(hi - lo + 1)
                    for k in lo...hi { s.heights[k] = min(s.cap, s.heights[k] + add) }
                }
                gone = true
            }
            if gone { balls.remove(at: i); continue }
            balls[i] = b
            i += 1
        }
    }

    // MARK: rolling your own

    /// True where a click should go to the snow rather than the window underneath: on a pile, a snowball, or anywhere
    /// while one is being carried.
    public func snowAt(x: Float, y: Float) -> Bool {
        guard extras else { return false }
        if carrying != nil { return true }
        for b in balls where b.placed {
            let dx = x - b.x, dy = y - b.y
            if dx * dx + dy * dy < (b.r + 3) * (b.r + 3) { return true }
        }
        return pile(atX: x, y: y) != nil
    }

    func pile(atX x: Float, y: Float) -> (Surface, Int)? {
        for s in list where x >= s.rect.x && x < s.rect.maxX {
            let c = column(s, x)
            let h = s.heights[c]
            if s.visible[c] && h >= 2.5 && y >= s.rect.y - h - 4 && y <= s.rect.y + 2 && !hidden(x, s.rect.y - 1) { return (s, c) }
        }
        return nil
    }

    /// The mouse went down at (x, y). Picks up a snowball there, or scoops a small one out of the pile. True if it did.
    @discardableResult
    public func grab(x: Float, y: Float) -> Bool {
        guard extras, carrying == nil else { return false }
        if let k = balls.lastIndex(where: { b in
            let dx = x - b.x, dy = y - b.y
            return b.placed && dx * dx + dy * dy < (b.r + 3) * (b.r + 3)
        }) {
            var b = balls.remove(at: k)
            for j in balls.indices where balls[j].support == b.id { balls[j].support = nil }
            b.x = x; b.y = y; b.on = nil; b.support = nil
            carrying = b
            return true
        }
        guard let hit = pile(atX: x, y: y) else { return false }
        let (s, c) = hit
        for k in max(0, c - 1)...min(s.heights.count - 1, c + 1) { s.heights[k] = max(0, s.heights[k] - 1.5) }
        nextBallId += 1
        carrying = Ball(x: x, y: y, vx: 0, vy: 0, r: 5, life: 0, id: nextBallId, placed: true)
        return true
    }

    /// The mouse moved with the button down. The ball follows it and picks up the snow it rolls through.
    public func drag(x: Float, y: Float) {
        guard var b = carrying else { return }
        b.x = x; b.y = y
        var taken: Float = 0
        for s in list where x + b.r > s.rect.x && x - b.r < s.rect.maxX {
            let lo = column(s, x - b.r), hi = column(s, x + b.r)
            for c in lo...hi where s.visible[c] && s.heights[c] > 0.2 {
                let top = s.rect.y - s.heights[c]
                guard y + b.r > top + 0.5 && y - b.r < s.rect.y else { continue }
                let take = min(s.heights[c], (y + b.r - top) * 0.25, 1)
                s.heights[c] -= take
                taken += take
            }
        }
        b.r = min(45, (b.r * b.r + taken * Sim.columnWidth / Float.pi).squareRoot())
        carrying = b
    }

    /// The mouse went up. The ball drops from there, thrown a little by how the mouse was moving.
    public func drop(vx: Float, vy: Float) {
        guard var b = carrying else { return }
        carrying = nil
        b.vx = max(-600, min(600, vx)); b.vy = max(-600, min(600, vy))
        if balls.filter({ $0.placed }).count >= Sim.maxPlaced, let k = balls.firstIndex(where: { $0.placed }) {
            let old = balls.remove(at: k)
            for j in balls.indices where balls[j].support == old.id { balls[j].support = nil }
        }
        balls.append(b)
    }

    // MARK: Santa's sleigh

    /// Santa flies when snow gets deep. A floor with a mountain a third of the screen high is flattened a layer at a time,
    /// pass after pass, left to right, until it is low enough for the plough. Otherwise, now and then, he clears the
    /// deepest window.
    func stepSleigh(dt: Float) {
        guard var g = sleigh else {
            sleighWait -= dt
            sleighCheck += dt
            guard sleighCheck >= 1 else { return }
            sleighCheck = 0
            for (id, s) in zip(order, list) where s.isFloor {
                let m = maxHeight(s)
                if m >= s.cap * 0.9 { startSleigh(id, s, mountain: true, level: max(20, m - 40)); return }
            }
            guard sleighWait <= 0 else { return }
            var pick: (Int, Surface)?
            var deepest: Float = 10
            for (id, s) in zip(order, list) where !s.isFloor {
                let mean = s.heights.reduce(0, +) / Float(s.heights.count)
                if mean >= deepest { deepest = mean; pick = (id, s) }
            }
            guard let pk = pick else { return }
            sleighWait = rand(45, 90)
            startSleigh(pk.0, pk.1, mountain: false, level: 1)
            return
        }
        if g.pause > 0 { g.pause -= dt; sleigh = g; return }
        let oldX = g.x
        g.x += 300 * dt
        if let s = surfaces[g.sid] {
            g.y = s.rect.y - g.level
            var removed: Float = 0
            let lo = column(s, min(oldX, g.x) - 24), hi = column(s, g.x + 24)
            if g.x + 24 >= s.rect.x && g.x - 24 < s.rect.maxX {
                for c in lo...hi where s.heights[c] > g.level {
                    removed += s.heights[c] - g.level
                    s.heights[c] = g.level
                }
            }
            var thrown = removed * 0.03                  // a spray behind it; the rest is carried away
            while thrown >= 1 {
                thrown -= 1
                spawnDebris(x: g.x + rand(-20, 20), y: g.y - rand(0, 10), vx: rand(80, 220), vy: -rand(40, 120))
            }
        }
        if g.x > g.to {
            if g.mountain, let s = surfaces[g.sid], maxHeight(s) > 40 {
                g.level = max(20, maxHeight(s) - 40)    // round again, a layer lower
                g.x = g.from
                g.pause = 1
            } else {
                sleigh = nil
                if g.mountain { ploughWait = min(ploughWait, 3) }   // now the plough can do the rest
                return
            }
        }
        sleigh = g
    }

    func startSleigh(_ id: Int, _ s: Surface, mountain: Bool, level: Float) {
        let d = display(of: s)
        sleigh = Sleigh(sid: id, x: d.x - 220, from: d.x - 220, to: d.maxX + 60, level: level, mountain: mountain,
                        y: s.rect.y - level)
    }

    // MARK: the plough

    /// Now and then a plough drives along the floor with the most snow, from a screen edge, pushing the snow into a
    /// heap ahead of its blade. At a screen edge the heap goes off the screen with it; against the Dock it leaves the
    /// heap there and reverses out. It waits while a mountain is being flattened.
    func stepPlough(dt: Float) {
        guard var p = plough else {
            ploughWait -= dt
            guard ploughWait <= 0, sleigh == nil else { return }
            ploughWait = rand(35, 70)
            var pick: (Int, Surface)?
            var most: Float = 0
            for (id, s) in zip(order, list) where s.ploughable && maxHeight(s) <= 60 {
                let total = s.heights.reduce(0, +)
                if total / Float(s.heights.count) >= 2 && total > most { most = total; pick = (id, s) }
            }
            guard let pk = pick else { return }
            let (id, s) = pk
            let left = atScreenEdge(s.rect.x), right = atScreenEdge(s.rect.maxX)
            guard left || right else { return }
            let dir: Float = left && (!right || rand() < 0.5) ? 1 : -1
            plough = Plough(sid: id, x: dir > 0 ? s.rect.x - 24 : s.rect.maxX + 24, dir: dir, backing: false,
                            rx: s.rect.x, ry: s.rect.y)
            return
        }
        guard let s = surfaces[p.sid], !moved(s, p.rx, p.ry) else { plough = nil; return }
        let n = s.heights.count
        let speed: Float = p.backing ? 80 : 55
        let oldFront = p.x + p.dir * 20
        p.vx = (p.backing ? -p.dir : p.dir) * speed
        p.x += p.vx * dt
        if p.backing {
            if (p.dir > 0 && p.x < s.rect.x - 30) || (p.dir < 0 && p.x > s.rect.maxX + 30) { plough = nil; return }
            plough = p
            return
        }
        let front = p.x + p.dir * 20
        let end = p.dir > 0 ? s.rect.maxX : s.rect.x
        let past = p.dir > 0 ? front >= end : front <= end
        // Scrape the columns the blade passed this step down to a thin layer.
        var carry: Float = 0
        let a = Int(((min(oldFront, front) - s.rect.x) / Sim.columnWidth).rounded(.down))
        let b = Int(((max(oldFront, front) - s.rect.x) / Sim.columnWidth).rounded(.down))
        if b >= 0 && a < n {
            for c in max(0, a)...min(n - 1, b) where s.heights[c] > 0.4 {
                carry += s.heights[c] - 0.4
                s.heights[c] = 0.4
            }
        }
        if past {
            if atScreenEdge(end) {
                // Pushed off the screen. Drive on until the plough is gone too.
                if (p.dir > 0 && p.x > end + 30) || (p.dir < 0 && p.x < end - 30) { plough = nil; return }
            } else {
                // Leave the heap against the Dock and back out.
                let last = p.dir > 0 ? n - 1 : 0
                deposit(s, from: last, dir: -p.dir, amount: carry, over: p.x, throwing: false)
                p.backing = true
            }
        } else {
            let ahead = column(s, front) + Int(p.dir)
            deposit(s, from: ahead, dir: p.dir, amount: carry, over: front, throwing: atScreenEdge(end) || abs(end - front) > 60)
        }
        plough = p
    }

    /// Pile `amount` into the four columns from `from` in direction `dir`, most in the first, no higher than the blade
    /// can push (48). Past the end of a floor that stops short of the screen edge (at the Dock) the snow piles against
    /// the end; past a screen edge it is gone. What will not fit is pushed aside, off the road: a quarter of it is thrown
    /// over the blade as loose snow (not at the Dock, where it would land on the Dock), the rest is gone.
    func deposit(_ s: Surface, from: Int, dir: Float, amount: Float, over x: Float, throwing: Bool = true) {
        guard amount > 0 else { return }
        let weights: [Float] = [0.4, 0.3, 0.2, 0.1]
        let n = s.heights.count
        let limit = min(s.cap, 48)
        var left = amount
        for (k, w) in weights.enumerated() {
            var c = from + Int(dir) * k
            if c < 0 || c >= n {
                if atScreenEdge(c < 0 ? s.rect.x : s.rect.maxX) { continue }
                c = max(0, min(n - 1, c))
            }
            let add = min(limit - s.heights[c], amount * w)
            if add > 0 { s.heights[c] += add; left -= add }
        }
        var thrown = throwing ? left * 0.25 : 0
        while thrown >= 1 {
            thrown -= 1
            spawnDebris(x: x + rand(-4, 4), y: s.rect.y - rand(10, 24), vx: dir * rand(20, 70), vy: -rand(60, 140))
        }
    }

    // MARK: drawing

    /// Snowmen (kind 5: build progress in the angle slot), the plough (kind 6: direction), snowballs (kind 7), the sleigh
    /// (kind 8) and its reindeer (kind 9: their place in the team, plus 10 for Rudolph at the front), in the same
    /// eight-float layout as flakes. Returns how many were added.
    func fillExtras(_ out: inout [Float]) -> Int {
        guard extras else { return 0 }
        var n = 0
        func put(_ x: Float, _ y: Float, _ d: Float, _ vx: Float, _ vy: Float, _ kind: Float, _ ang: Float) {
            out.append(x); out.append(y); out.append(d); out.append(1)
            out.append(vx); out.append(vy); out.append(kind); out.append(ang)
            n += 1
        }
        for m in snowmen {
            guard let s = surfaces[m.sid], s.visible[column(s, m.x)] else { continue }
            let y = s.rect.y - s.heights[column(s, m.x)] - m.size / 2 + 1   // on the snow as it is now
            if !hidden(m.x, y) { put(m.x, y, m.size, 0, 0, 5, m.build) }
        }
        for b in balls where !hidden(b.x, b.y) { put(b.x, b.y, b.r * 2, b.vx, b.on == nil && b.support == nil ? b.vy : 0, 7, 0) }
        if let b = carrying { put(b.x, b.y, b.r * 2, 0, 0, 7, 0) }
        if let p = plough, let s = surfaces[p.sid] {
            let y = s.rect.y - s.heights[column(s, p.x)] - Sim.ploughSize / 2 + 2
            if !hidden(p.x, y) { put(p.x, y, Sim.ploughSize, p.vx, 0, 6, p.dir) }
        }
        if let g = sleigh, g.pause <= 0 {
            let bob = sin(clock * 3) * 3
            let y = g.y - Sim.sleighSize * 0.3 + bob
            put(g.x, y, Sim.sleighSize, 300, 0, 8, 0)
            for k in 0..<4 {
                let ry = y - 8 - Float(k) * 3 + sin(clock * 3 + Float(k)) * 2
                put(g.x + 52 + Float(k) * 36, ry, Sim.reindeerSize, 300, 0, 9, Float(k) + (k == 3 ? 10 : 0))
            }
        }
        return n
    }

    // MARK: reading state

    public var snowmanCount: Int { snowmen.count }
    public var snowballCount: Int { balls.count }
    public var ploughX: Float? { plough?.x }
    public var sleighX: Float? { sleigh?.x }
    public var isCarrying: Bool { carrying != nil }
    public var carriedRadius: Float? { carrying?.r }
}
