// Snowmen, snowballs and the snow plough. Ported by hand to windows/sim.hpp: keep the two in step, including the order
// of every rand() call.

import Foundation

struct Snowman {
    var sid: Int          // the surface it stands on
    var x: Float
    var rx: Float, ry: Float   // where that surface was when it was built: if it moves, the snowman falls
    var build: Float      // 0 to 1: bottom ball, then the middle, the head, then the hat and scarf
    var top: Float        // the snow it stands on, updated every step
}

struct Ball {
    var x: Float, y: Float, vx: Float, vy: Float, r: Float, life: Float
    var on: Int?          // the surface it is resting on, if any
    var rx: Float = 0, ry: Float = 0
}

struct Plough {
    var sid: Int
    var x: Float          // the middle of the plough
    var dir: Float        // +1 drives right, -1 left
    var backing: Bool     // reversing out after leaving a heap against the Dock
    var rx: Float, ry: Float
    var vx: Float = 0
}

extension Sim {
    static let snowmanSize: Float = 30
    static let ploughSize: Float = 40

    func atScreenEdge(_ x: Float) -> Bool {
        world.contains { abs($0.x - x) < 2 || abs($0.maxX - x) < 2 }
    }

    func column(_ s: Surface, _ x: Float) -> Int {
        max(0, min(s.heights.count - 1, Int((x - s.rect.x) / Sim.columnWidth)))
    }

    func moved(_ s: Surface, _ rx: Float, _ ry: Float) -> Bool { abs(s.rect.x - rx) > 1.5 || abs(s.rect.y - ry) > 1.5 }

    func stepExtras(dt: Float, pointer: (x: Float, y: Float)?, velocity pv: (x: Float, y: Float), speed: Float) {
        stepSnowmen(dt: dt, pointer: pointer, pvx: pv.x, speed: speed)
        stepBalls(dt: dt, pointer: pointer, pvx: pv.x, speed: speed)
        snowmanClock += dt
        if snowmanClock >= 2.5 { snowmanClock -= 2.5; trySpawnSnowman() }
        stepPlough(dt: dt)
    }

    // MARK: snowmen

    /// A snowman builds itself where a window has a deep, even stretch of snow away from its ends. At most three at once.
    func trySpawnSnowman() {
        guard snowmen.count < 3 else { return }
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
                if s.heights[c] < 1.5 { fall = true }   // the snow under it was swept or melted away
                m.build = min(1, m.build + dt / 2.5)
                if let p = pointer, speed > 120, abs(p.x - m.x) < 14, p.y < m.top + 2, p.y > m.top - Sim.snowmanSize - 4 {
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
        let half = Sim.snowmanSize / 2
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
                }
            }
            if let id = b.on, let s = surfaces[id], !moved(s, b.rx, b.ry) {
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
                for (id, s) in zip(order, list) where b.x >= s.rect.x && b.x < s.rect.maxX {
                    let c = column(s, b.x)
                    guard s.visible[c] else { continue }
                    let top = s.rect.y - s.heights[c]
                    guard prevY + b.r <= top + 0.5 && b.y + b.r >= top else { continue }
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
            b.life -= dt
            var gone = b.y > worldBox.maxY + 20 || b.x < worldBox.x - 300 || b.x > worldBox.maxX + 300
            if !gone && b.life <= 0 {
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

    // MARK: the plough

    /// Now and then a plough drives along the floor with the most snow, from a screen edge, pushing the snow into a
    /// heap ahead of its blade. At a screen edge the heap goes off the screen with it; against the Dock it leaves the
    /// heap there and reverses out.
    func stepPlough(dt: Float) {
        guard var p = plough else {
            ploughWait -= dt
            guard ploughWait <= 0 else { return }
            ploughWait = rand(70, 140)
            var pick: (Int, Surface)?
            var most: Float = 0
            for (id, s) in zip(order, list) where s.ploughable {
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

    /// Pile `amount` into the four columns from `from` in direction `dir`, most in the first. Past the end of a floor that
    /// stops short of the screen edge (at the Dock) the snow piles against the end; past a screen edge it is gone. What
    /// will not fit is pushed aside, off the road: a quarter of it is thrown over the blade as loose snow (not at the Dock,
    /// where it would land on the Dock), the rest is gone.
    func deposit(_ s: Surface, from: Int, dir: Float, amount: Float, over x: Float, throwing: Bool = true) {
        guard amount > 0 else { return }
        let weights: [Float] = [0.4, 0.3, 0.2, 0.1]
        let n = s.heights.count
        var left = amount
        for (k, w) in weights.enumerated() {
            var c = from + Int(dir) * k
            if c < 0 || c >= n {
                if atScreenEdge(c < 0 ? s.rect.x : s.rect.maxX) { continue }
                c = max(0, min(n - 1, c))
            }
            let add = min(s.cap - s.heights[c], amount * w)
            if add > 0 { s.heights[c] += add; left -= add }
        }
        var thrown = throwing ? left * 0.25 : 0
        while thrown >= 1 {
            thrown -= 1
            spawnDebris(x: x + rand(-4, 4), y: s.rect.y - rand(10, 24), vx: dir * rand(20, 70), vy: -rand(60, 140))
        }
    }

    // MARK: drawing

    /// Snowmen (kind 5, build progress in the angle slot), the plough (kind 6, direction in the angle slot) and snowballs
    /// (kind 7), in the same eight-float layout as flakes. Returns how many were added.
    func fillExtras(_ out: inout [Float]) -> Int {
        guard extras else { return 0 }
        var n = 0
        for m in snowmen {
            guard let s = surfaces[m.sid], s.visible[column(s, m.x)] else { continue }
            let y = m.top - Sim.snowmanSize / 2 + 1
            if hidden(m.x, y) { continue }
            out.append(m.x); out.append(y); out.append(Sim.snowmanSize); out.append(1)
            out.append(0); out.append(0); out.append(5); out.append(m.build)
            n += 1
        }
        for b in balls where !hidden(b.x, b.y) {
            out.append(b.x); out.append(b.y); out.append(b.r * 2); out.append(1)
            out.append(b.vx); out.append(b.on == nil ? b.vy : 0); out.append(7); out.append(0)
            n += 1
        }
        if let p = plough, let s = surfaces[p.sid] {
            let y = s.rect.y - s.heights[column(s, p.x)] - Sim.ploughSize / 2 + 2
            if !hidden(p.x, y) {
                out.append(p.x); out.append(y); out.append(Sim.ploughSize); out.append(1)
                out.append(p.vx); out.append(0); out.append(6); out.append(p.dir)
                n += 1
            }
        }
        return n
    }

    // MARK: reading state

    public var snowmanCount: Int { snowmen.count }
    public var snowballCount: Int { balls.count }
    public var ploughX: Float? { plough?.x }
}
