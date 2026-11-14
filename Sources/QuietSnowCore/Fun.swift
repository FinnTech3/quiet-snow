// Snowball splats and fights, animals and footprints, icicles, snow angels, flying hats, presents, fairy lights, the idle
// blizzard and the New Year's Eve fireworks. Ported by hand to windows/sim.hpp: keep the two in step, including the
// order of every rand() call.

import Foundation

public enum Season: Int, CaseIterable { case winter, autumn, spring }

/// Things that make a sound. The app decides whether to play them.
public enum SoundEvent: Int, CaseIterable { case bells, engine, chime, pop, splat }

/// A whole window, front to back, so snowballs can splat on it and icicles can hang from it.
public struct WindowRect {
    public var id: Int
    public var rect: Rect
    public init(id: Int, rect: Rect) { self.id = id; self.rect = rect }
}

struct Splat { var x: Float, y: Float, size: Float, life: Float, ang: Float }
struct Animal {
    var sid: Int, x: Float, dir: Float, robin: Bool, rx: Float, ry: Float
    var walked: Float = 0, leftFoot = false
}
struct Footprint { var sid: Int, x: Float, life: Float, robin: Bool, dir: Float, left: Bool }
struct Icicle { var wid: Int, x: Float, len: Float, maxLen: Float, drip: Float, rx: Float, bottom: Float }
struct Drop { var x: Float, y: Float, vy: Float }
struct Angel { var sid: Int, x: Float, life: Float }
struct Wiggle {
    var x: Float = 0, y: Float = 0, lastX: Float = 0, sign: Float = 0, flips = 0, start: Float = 0
    var sid: Int?
    var active = false
}
struct Hat { var x: Float, y: Float, vx: Float, vy: Float, spin: Float, life: Float, size: Float }

extension Sim {
    func emit(_ e: SoundEvent) { if sounds.count < 16 { sounds.append(e) } }

    /// The sounds since the last call. The app plays them if sound is on.
    public func takeSounds() -> [SoundEvent] {
        let out = sounds
        sounds = []
        return out
    }

    /// Every ordinary window, front to back. Icicles on a window that moved or closed go with it.
    public func setWindows(_ w: [WindowRect]) {
        windows = w
        icicles.removeAll { ic in !w.contains { $0.id == ic.wid && abs($0.rect.x - ic.rx) < 1.5 && abs($0.rect.maxY - ic.bottom) < 1.5 } }
    }

    /// Fireworks for `seconds`, over the main display. The app calls it at midnight on New Year's Eve.
    public func celebrate(seconds: Float) {
        celebrateLeft = seconds
        fireworkClock = 0
    }

    /// Santa flies over and drops a present on a window. The app calls it once a day. Click the present to open it.
    @discardableResult
    public func deliverPresent() -> Bool {
        guard extras, sleigh == nil, showT < 0, !world.isEmpty else { return false }
        let cands = zip(order, list).filter { !$0.1.isFloor && $0.1.rect.w >= 120 }
        guard !cands.isEmpty else { return false }
        let (id, s) = cands[min(cands.count - 1, Int(rand(0, Float(cands.count))))]
        let d = display(of: s)
        let y = d.y + 60
        let t = (2 * max(0, s.rect.y - y) / 520).squareRoot()
        sleigh = Sleigh(sid: id, x: d.x - 220, from: d.x - 220, to: d.maxX + 60, level: 0, mountain: false, y: y,
                        delivering: true, deliverX: s.rect.x + s.rect.w / 2 - 90 * t)
        emit(.bells)
        return true
    }

    func stepFun(dt: Float, pointer: (x: Float, y: Float)?) {
        stepFight(dt: dt, pointer: pointer)
        stepAnimal(dt: dt)
        stepIcicles(dt: dt)
        for i in splats.indices { splats[i].life -= dt; splats[i].y += 5 * dt }
        splats.removeAll { $0.life <= 0 }
        for i in prints.indices { prints[i].life -= dt }
        prints.removeAll { $0.life <= 0 || surfaces[$0.sid] == nil }
        for i in angels.indices { angels[i].life -= dt }
        angels.removeAll { $0.life <= 0 || surfaces[$0.sid] == nil }
        let blow = windNow + gust
        var i = 0
        while i < hats.count {
            var h = hats[i]
            h.vy += 200 * dt
            h.vx += (blow - h.vx) * 0.8 * dt
            h.x += h.vx * dt
            h.y += h.vy * dt
            h.spin += dt * 4 * (h.vx >= 0 ? 1 : -1)
            h.life -= dt
            if h.life <= 0 || h.y > worldBox.maxY + 30 || h.x < worldBox.x - 300 || h.x > worldBox.maxX + 300 { hats.remove(at: i); continue }
            hats[i] = h
            i += 1
        }
    }

    // MARK: the snowball fight

    /// After you throw a snowball, the snowmen throw back at the cursor for 20 seconds.
    func stepFight(dt: Float, pointer: (x: Float, y: Float)?) {
        guard fightLeft > 0 else { return }
        fightLeft -= dt
        throwClock -= dt
        guard throwClock <= 0, let p = pointer else { return }
        throwClock = rand(1.2, 2.5)
        var best: Snowman?
        var bestD: Float = 900
        for m in snowmen where m.build >= 1 {
            let d = hypot(p.x - m.x, p.y - m.top)
            if d < bestD { bestD = d; best = m }
        }
        guard let m = best, balls.count < 30 else { return }
        let sy = m.top - m.size * 0.9
        let dx = p.x - m.x, dy = p.y - sy
        let t = min(1.4, max(0.5, hypot(dx, dy) / 500))
        balls.append(Ball(x: m.x, y: sy, vx: dx / t, vy: dy / t - 260 * t, r: 4, life: 3, enemy: true))
    }

    func poof(_ x: Float, _ y: Float) {
        for _ in 0..<8 { spawnDebris(x: x + rand(-3, 3), y: y + rand(-3, 3), vx: rand(-60, 60), vy: -rand(20, 100)) }
    }

    func splat(_ b: Ball) {
        splats.append(Splat(x: b.x, y: b.y, size: b.r * 3.2 + 14, life: 6, ang: abs(b.x * 0.137).truncatingRemainder(dividingBy: 1) * 6.28))
        if splats.count > 30 { splats.removeFirst() }
        emit(.splat)
    }

    // MARK: animals and footprints

    /// Every couple of minutes a fox or a robin crosses the floor, leaving footprints in the snow.
    func stepAnimal(dt: Float) {
        guard var a = animal else {
            animalWait -= dt
            guard animalWait <= 0 else { return }
            animalWait = rand(90, 180)
            var pick: (Int, Surface)?
            var most: Float = 0
            for (id, s) in zip(order, list) where s.ploughable {
                let total = s.heights.reduce(0, +)
                if total / Float(s.heights.count) >= 2 && total > most { most = total; pick = (id, s) }
            }
            guard let pk = pick else { return }
            let (id, s) = pk
            let robin = rand() < 0.5
            let left = atScreenEdge(s.rect.x), right = atScreenEdge(s.rect.maxX)
            let dir: Float = left && (!right || rand() < 0.5) ? 1 : (right ? -1 : 1)
            animal = Animal(sid: id, x: dir > 0 ? s.rect.x - 20 : s.rect.maxX + 20, dir: dir, robin: robin, rx: s.rect.x, ry: s.rect.y)
            return
        }
        guard let s = surfaces[a.sid], !moved(s, a.rx, a.ry) else { animal = nil; return }
        let speed: Float = a.robin ? 38 : 46
        a.x += a.dir * speed * dt
        a.walked += speed * dt
        let stride: Float = a.robin ? 9 : 13
        if a.walked >= stride {
            a.walked -= stride
            if a.x >= s.rect.x && a.x < s.rect.maxX {
                let c = column(s, a.x)
                if s.heights[c] > 1 {
                    s.heights[c] = max(0, s.heights[c] - 0.4)
                    a.leftFoot.toggle()
                    prints.append(Footprint(sid: a.sid, x: a.x, life: 25, robin: a.robin, dir: a.dir, left: a.leftFoot))
                    if prints.count > 160 { prints.removeFirst() }
                }
            }
        }
        if (a.dir > 0 && a.x > s.rect.maxX + 30) || (a.dir < 0 && a.x < s.rect.x - 30) { animal = nil; return }
        animal = a
    }

    // MARK: icicles

    /// Icicles grow slowly from the bottoms of windows and drip.
    func stepIcicles(dt: Float) {
        if season != .winter { icicles = [] }            // icicles only in winter
        icicleClock -= dt
        if icicleClock <= 0 && !windows.isEmpty && season == .winter {
            icicleClock = 3
            let w = windows[min(windows.count - 1, Int(rand(0, Float(windows.count))))]
            let have = icicles.filter { $0.wid == w.id }.count
            let onScreen = world.contains { $0.contains(w.rect.x + w.rect.w / 2, w.rect.maxY + 1) }
            if w.rect.w >= 80 && have < Int(w.rect.w / 45) && icicles.count < 120 && onScreen {
                icicles.append(Icicle(wid: w.id, x: rand(w.rect.x + 14, w.rect.maxX - 14), len: 0, maxLen: rand(8, 26),
                                      drip: rand(3, 9), rx: w.rect.x, bottom: w.rect.maxY))
            }
        }
        for i in icicles.indices {
            icicles[i].len = min(icicles[i].maxLen, icicles[i].len + dt * 0.3)
            guard icicles[i].len >= 6 else { continue }
            icicles[i].drip -= dt
            if icicles[i].drip <= 0 {
                icicles[i].drip = rand(4, 12)
                if drops.count < 60 { drops.append(Drop(x: icicles[i].x, y: icicles[i].bottom + icicles[i].len, vy: 0)) }
            }
        }
        var i = 0
        while i < drops.count {
            var d = drops[i]
            let prevY = d.y
            d.vy = min(d.vy + 600 * dt, 700)
            d.y += d.vy * dt
            var gone = d.y > worldBox.maxY + 5
            for s in list where d.x >= s.rect.x && d.x < s.rect.maxX {
                let top = s.rect.y - s.heights[column(s, d.x)]
                if prevY <= top && d.y >= top { gone = true; break }
            }
            if gone { drops.remove(at: i); continue }
            drops[i] = d
            i += 1
        }
    }

    // MARK: snow angels

    /// Press on a pile and wiggle the mouse from side to side without moving away: a snow angel is pressed into the snow.
    func wiggled(x: Float, y: Float) -> Bool {
        guard wiggle.active else { return false }
        if hypot(x - wiggle.x, y - wiggle.y) > 50 || clock - wiggle.start > 2.5 { wiggle.active = false; return false }
        let dx = x - wiggle.lastX
        if abs(dx) > 1.5 {
            let sg: Float = dx > 0 ? 1 : -1
            if sg != wiggle.sign {
                if wiggle.sign != 0 { wiggle.flips += 1 }
                wiggle.sign = sg
            }
            wiggle.lastX = x
        }
        guard wiggle.flips >= 6, let sid = wiggle.sid, let s = surfaces[sid] else { return false }
        wiggle.active = false
        angels.append(Angel(sid: sid, x: wiggle.x, life: 90))
        if angels.count > 12 { angels.removeFirst() }
        for c in column(s, wiggle.x - 12)...column(s, wiggle.x + 12) {
            s.heights[c] = max(0, s.heights[c] - 1.5)
        }
        return true
    }

    // MARK: the idle blizzard

    /// While the computer is idle the blizzard thickens (twice the flakes, a swirling wind), and every 30 seconds the snow
    /// globe is shaken: every pile flies up and the air churns.
    func stepIdle(dt: Float) {
        idleT += dt
        shakeClock -= dt
        guard shakeClock <= 0 else { return }
        shakeClock = 30
        clearPiles()
        for i in 0..<xs.count where state[i] != 0 {
            exs[i] = clamp(exs[i] + rand(-350, 350), 400)
            eys[i] = clamp(eys[i] + rand(-320, 40), 400)
        }
        emit(.chime)
    }

    // MARK: drawing

    /// Fairy lights (kind 22), footprints (13), snow angels (17), the animal (14), icicles (15), drips (16), splats (12)
    /// and flying hats (18).
    func fillFun(_ out: inout [Float]) -> Int {
        var n = 0
        func put(_ x: Float, _ y: Float, _ d: Float, _ a: Float, _ vx: Float, _ vy: Float, _ kind: Float, _ ang: Float) {
            out.append(x); out.append(y); out.append(d); out.append(a)
            out.append(vx); out.append(vy); out.append(kind); out.append(ang)
            n += 1
        }
        if lights {
            var k = 0
            for s in list where !s.isFloor {
                var x = s.rect.x + 10
                while x < s.rect.maxX - 6 && k < 500 {
                    if s.visible[column(s, x)] && !hidden(x, s.rect.y - 1) {
                        put(x, s.rect.y + 2, 20, 1, 0, 0, 22, Float(k % 5) + (Float(k) * 0.37).truncatingRemainder(dividingBy: 1))
                    }
                    x += 24
                    k += 1
                }
            }
        }
        guard extras else { return n }
        func top(_ sid: Int, _ x: Float) -> Float? {
            guard let s = surfaces[sid] else { return nil }
            return s.rect.y - s.heights[column(s, x)]
        }
        for f in prints {
            guard let t = top(f.sid, f.x) else { continue }
            put(f.x, t + 1.5, f.robin ? 7 : 9, min(1, f.life / 6) * 0.9, 0, 0, 13, (f.robin ? 1 : 0) + (f.left ? 2 : 0) + (f.dir > 0 ? 4 : 0))
        }
        for a in angels {
            guard let t = top(a.sid, a.x) else { continue }
            put(a.x, t + 4, 40, min(1, a.life / 6), 0, 0, 17, 0)
        }
        if let a = animal, let t = top(a.sid, a.x) {
            let size: Float = a.robin ? 20 : 34
            put(a.x, t - size / 2 + 2, size, 1, a.dir * (a.robin ? 38 : 46), 0, 14, (a.robin ? 1 : 0) + (a.dir > 0 ? 2 : 0))
        }
        for ic in icicles where ic.len >= 1 {
            let j = windows.firstIndex { $0.id == ic.wid } ?? 0
            if windows[0..<j].contains(where: { $0.rect.contains(ic.x, ic.bottom + 2) }) { continue }   // a window in front
            let size = ic.len + 4
            put(ic.x, ic.bottom - 1 + size / 2, size, 0.9, 0, 0, 15, 0)
        }
        for d in drops { put(d.x, d.y, 4, 0.8, 0, d.vy, 16, 0) }
        for s in splats { put(s.x, s.y, s.size, min(1, s.life / 1.5), 0, 5, 12, s.ang) }
        for h in hats { put(h.x, h.y, h.size, min(1, h.life), h.vx, h.vy, 18, h.spin) }
        return n
    }

    /// For the README pictures (`--snapshot --toys`): one of every toy, placed at once instead of waited for. Expects a
    /// floor and at least one window surface, and the window list.
    public func stageForPicture() {
        extras = true
        lights = true
        if let (id, s) = zip(order, list).first(where: { $0.1.isFloor && $0.1.ploughable && $0.1.rect.w > 200 }) {
            let x = s.rect.x + 40
            animal = Animal(sid: id, x: x + 150, dir: 1, robin: false, rx: s.rect.x, ry: s.rect.y)
            for k in 0..<10 { prints.append(Footprint(sid: id, x: x + Float(k) * 13, life: 20, robin: false, dir: 1, left: k % 2 == 0)) }
            angels.append(Angel(sid: id, x: x + 230, life: 60))
        }
        if let (id, s) = zip(order, list).first(where: { !$0.1.isFloor }) {
            nextBallId += 1
            balls.append(Ball(x: s.rect.x + s.rect.w * 0.3, y: s.rect.y - s.heights[column(s, s.rect.x + s.rect.w * 0.3)] - 9, vx: 0, vy: 0, r: 9, life: 0,
                              on: id, rx: s.rect.x, ry: s.rect.y, id: nextBallId, placed: true, present: true, colour: 0))
            hats.append(Hat(x: s.rect.x + s.rect.w * 0.6, y: s.rect.y - 60, vx: 40, vy: 0, spin: 0.6, life: 5, size: 16))
        }
        for w in windows {
            for k in 0..<4 { icicles.append(Icicle(wid: w.id, x: w.rect.x + 40 + Float(k) * 70, len: 10 + Float(k) * 4, maxLen: 26, drip: 5, rx: w.rect.x, bottom: w.rect.maxY)) }
            splats.append(Splat(x: w.rect.x + w.rect.w * 0.7, y: w.rect.y + w.rect.h * 0.45, size: 30, life: 5, ang: 0.7))
        }
    }

    public var animalActive: Bool { animal != nil }
    public var footprintCount: Int { prints.count }
    public var icicleCount: Int { icicles.count }
    public var splatCount: Int { splats.count }
    public var angelCount: Int { angels.count }
    public var hatCount: Int { hats.count }
    public var presentCount: Int { balls.filter { $0.present }.count }
}
