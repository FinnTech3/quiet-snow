import XCTest
@testable import QuietSnowCore

final class SimTests: XCTestCase {
    let screen = Rect(x: 0, y: 0, w: 1440, h: 900)

    func makeSim(_ w: Weather = .steady, seed: UInt64 = 7) -> Sim {
        let s = Sim(seed: seed)
        s.weather = w
        s.setWorld([screen])
        return s
    }

    func run(_ s: Sim, seconds: Float, pointer: (x: Float, y: Float)? = nil) {
        var t: Float = 0
        while t < seconds { s.step(dt: 1.0 / 30, pointer: pointer); t += 1.0 / 30 }
    }

    let windowTop = SurfaceInput(id: 1, rect: Rect(x: 300, y: 300, w: 600, h: 400))

    func testFlakesFall() {
        let s = makeSim()
        var a: [Float] = [], b: [Float] = []
        s.fillFlakes(&a)
        s.step(dt: 0.1)
        s.fillFlakes(&b)
        var down = 0
        for i in stride(from: 1, to: min(a.count, b.count), by: 8) where b[i] > a[i] { down += 1 }
        XCTAssertGreaterThan(down, a.count / 8 * 9 / 10)
    }

    func testFlakeCountFollowsWeatherAndScreenSize() {
        let light = makeSim(.light), flurry = makeSim(.flurry)
        XCTAssertLessThan(light.normalCount, flurry.normalCount)
        let tiny = Sim(); tiny.setWorld([Rect(x: 0, y: 0, w: 100, h: 100)])
        XCTAssertEqual(tiny.normalCount, 60, "floor of 60 flakes")
        let huge = Sim(); huge.weather = .flurry; huge.setWorld([Rect(x: 0, y: 0, w: 7680, h: 4320)])
        XCTAssertEqual(huge.normalCount, 3500, "ceiling of 3500 flakes")
    }

    func testSnowLandsOnAWindowTop() {
        let s = makeSim()
        s.setSurfaces([windowTop])
        run(s, seconds: 60)
        XCTAssertGreaterThan(s.heights(of: 1)!.reduce(0, +), 20)
    }

    func testPileNeverPassesTheCapOrGoesNegative() {
        let s = makeSim(.flurry)
        s.setSurfaces([windowTop])
        run(s, seconds: 240)
        for h in s.heights(of: 1)! {
            XCTAssertLessThanOrEqual(h, Sim.windowCapForTests + 0.5)
            XCTAssertGreaterThanOrEqual(h, 0)
        }
    }

    func testOccludedColumnsStayBare() {
        let s = makeSim(.flurry)
        // Another window sits over the left half of the top edge.
        let cover = Rect(x: 250, y: 200, w: 350, h: 200)
        s.setSurfaces([SurfaceInput(id: 1, rect: windowTop.rect, occluders: [cover])])
        run(s, seconds: 90)
        let h = s.heights(of: 1)!
        let covered = h[0..<Int((cover.maxX - windowTop.rect.x) / Sim.columnWidth) - 2]
        XCTAssertEqual(covered.reduce(0, +), 0, accuracy: 0.001)
        XCTAssertGreaterThan(h.reduce(0, +), 10)
    }

    func testEdgeOffScreenGetsNoSnow() {
        let s = makeSim(.flurry)
        s.setSurfaces([SurfaceInput(id: 1, rect: Rect(x: 1300, y: 300, w: 400, h: 300))])
        run(s, seconds: 90)
        let h = s.heights(of: 1)!
        let from = Int((1440 - 1300) / Sim.columnWidth) + 2
        XCTAssertEqual(h[from...].reduce(0, +), 0, accuracy: 0.001)
    }

    func testMovingAWindowDropsItsSnow() {
        let s = makeSim(.flurry)
        s.setSurfaces([windowTop])
        run(s, seconds: 90)
        let before = s.heights(of: 1)!.reduce(0, +)
        XCTAssertGreaterThan(before, 20)
        s.setSurfaces([SurfaceInput(id: 1, rect: Rect(x: 340, y: 340, w: 600, h: 400))])
        XCTAssertEqual(s.heights(of: 1)!.reduce(0, +), 0, accuracy: 0.001)
        XCTAssertGreaterThan(s.debrisCount, 20, "the old pile became falling snow")
    }

    func testClosingAWindowDropsItsSnow() {
        let s = makeSim(.flurry)
        s.setSurfaces([windowTop])
        run(s, seconds: 90)
        s.setSurfaces([])
        XCTAssertEqual(s.surfaceCount, 0)
        XCTAssertGreaterThan(s.debrisCount, 20)
    }

    func testResizingAndSmallJittersAreHandledDifferently() {
        let s = makeSim(.flurry)
        s.setSurfaces([windowTop])
        run(s, seconds: 60)
        let before = s.heights(of: 1)!.reduce(0, +)
        s.setSurfaces([SurfaceInput(id: 1, rect: Rect(x: 300.5, y: 300.4, w: 600, h: 400))])
        XCTAssertEqual(s.heights(of: 1)!.reduce(0, +), before, accuracy: 0.001, "sub-point jitter keeps the pile")
        s.setSurfaces([SurfaceInput(id: 1, rect: Rect(x: 300, y: 300, w: 700, h: 400))])
        XCTAssertEqual(s.heights(of: 1)!.reduce(0, +), 0, accuracy: 0.001, "a resize drops it")
    }

    func testDebrisLandsOnTheFloorBelow() {
        let s = makeSim(.light)
        s.setSurfaces([windowTop, SurfaceInput(id: -1, rect: Rect(x: 0, y: 880, w: 1440, h: 0), isFloor: true)])
        run(s, seconds: 200)
        let floorBefore = s.heights(of: -1)!.reduce(0, +)
        s.setSurfaces([SurfaceInput(id: -1, rect: Rect(x: 0, y: 880, w: 1440, h: 0), isFloor: true)])
        run(s, seconds: 8)
        XCTAssertGreaterThan(s.heights(of: -1)!.reduce(0, +), floorBefore, "released snow ends up on the floor")
        XCTAssertEqual(s.debrisCount, 0, "and none is left in the air")
    }

    func testSlumpingFlattensASpikeAndKeepsTheSnow() {
        let s = makeSim(.light)
        s.setSurfaces([windowTop])
        s.setHeightForTests(id: 1, column: 50, value: 11)
        let mass = s.heights(of: 1)!.reduce(0, +)
        for _ in 0..<60 { s.step(dt: 1.0 / 30) }
        let h = s.heights(of: 1)!
        XCTAssertLessThan(h[50], 6)
        XCTAssertGreaterThan(h[49] + h[51], 4)
        XCTAssertGreaterThanOrEqual(h.reduce(0, +) + 0.5, mass * 0.9, "slumping moves snow, it does not delete it")
    }

    func testSnowSheddsOverTheEdgeOfAWindow() {
        let s = makeSim(.light)
        s.setSurfaces([windowTop])
        s.setHeightForTests(id: 1, column: 0, value: 12)
        for _ in 0..<150 { s.step(dt: 1.0 / 30) }
        XCTAssertLessThan(s.heights(of: 1)![0], 4)
    }

    func testMeltingShrinksPilesAndOffDoesNot() {
        let keep = makeSim(.light, seed: 5), melt = makeSim(.light, seed: 5)
        melt.melts = true
        for sim in [keep, melt] {
            sim.setSurfaces([windowTop])
            for c in 20..<60 { sim.setHeightForTests(id: 1, column: c, value: 6) }
        }
        run(keep, seconds: 120); run(melt, seconds: 120)
        XCTAssertLessThan(melt.totalSnowOnSurfaces, keep.totalSnowOnSurfaces * 0.5)
        XCTAssertGreaterThan(keep.totalSnowOnSurfaces, 200, "without melt the pile stays")
    }

    func testSweepingThrowsSnowButAHoveringCursorDoesNot() {
        let s = makeSim(.light)
        s.setSurfaces([windowTop])
        for c in 40..<80 { s.setHeightForTests(id: 1, column: c, value: 8) }
        let m0 = s.totalSnowOnSurfaces
        // Hover without moving.
        for _ in 0..<60 { s.step(dt: 1.0 / 30, pointer: (x: 400, y: 299)) }
        XCTAssertGreaterThan(s.totalSnowOnSurfaces, m0 * 0.95)
        // Sweep across.
        var x: Float = 300
        for _ in 0..<40 { s.step(dt: 1.0 / 30, pointer: (x: x, y: 299)); x += 12 }
        XCTAssertLessThan(s.totalSnowOnSurfaces, m0 * 0.8)
    }

    func testWindPushesFlakesSideways() {
        let calm = makeSim(.steady, seed: 3), windy = makeSim(.steady, seed: 3)
        windy.wind = 200
        var a: [Float] = [], b: [Float] = []
        calm.fillFlakes(&a); windy.fillFlakes(&b)
        for _ in 0..<60 { calm.step(dt: 1.0 / 30); windy.step(dt: 1.0 / 30) }
        calm.fillFlakes(&a); windy.fillFlakes(&b)
        func meanX(_ v: [Float]) -> Float { stride(from: 0, to: v.count, by: 8).map { v[$0] }.reduce(0, +) / Float(v.count / 8) }
        XCTAssertGreaterThan(meanX(b) - meanX(a), 20)
    }

    func testNothingGoesNaNOrOutOfBoundsOverALongRun() {
        let s = makeSim(.flurry)
        s.wind = -150
        s.melts = true
        s.setSurfaces([windowTop, SurfaceInput(id: 2, rect: Rect(x: 900, y: 500, w: 300, h: 0)),
                       SurfaceInput(id: -1, rect: Rect(x: 0, y: 880, w: 1440, h: 0), isFloor: true)])
        var x: Float = 0
        for i in 0..<6000 {
            x = Float((i * 37) % 1440)
            s.step(dt: 1.0 / 30, pointer: i % 7 == 0 ? nil : (x: x, y: Float((i * 53) % 900)))
            if i % 1000 == 0 { s.clearPiles() }
        }
        var f: [Float] = []
        s.fillFlakes(&f)
        XCTAssertTrue(f.allSatisfy { $0.isFinite })
        XCTAssertLessThanOrEqual(s.liveFlakeCount, s.flakeCapacity)
        for id in [1, 2, -1] { XCTAssertTrue(s.heights(of: id)!.allSatisfy { $0.isFinite && $0 >= 0 }) }
    }

    func testPileGeometryIsWholeTriangles() {
        let s = makeSim(.light)
        s.setSurfaces([windowTop])
        for c in 10..<30 { s.setHeightForTests(id: 1, column: c, value: 5) }
        var v: [Float] = []
        let n = s.fillPiles(&v)
        XCTAssertGreaterThan(n, 0)
        XCTAssertEqual(n % 6, 0)
        XCTAssertEqual(v.count, n * 4)
        XCTAssertTrue(v.allSatisfy { $0.isFinite })
    }

    func testForegroundFlakesNeverLand() {
        let s = makeSim(.flurry)
        s.setSurfaces([windowTop])
        run(s, seconds: 60)
        var f: [Float] = []
        s.fillFlakes(&f)
        let big = stride(from: 2, to: f.count, by: 8).filter { f[$0] > 5 * 1.6 + 1 }.count
        XCTAssertGreaterThan(big, 0, "there are foreground flakes")
        // Landing code refuses them outright.
        for i in 0..<s.normalCount where s.sizes[i] > 5 {
            s.ys[i] = windowTop.rect.y - 0.5; s.xs[i] = 400
            XCTAssertFalse(s.land(i, windowTop.rect.y - 5))
        }
    }

    func testFlakeVelocityMatchesWhereItMoves() {
        let s = makeSim(.steady, seed: 21)
        var a: [Float] = [], b: [Float] = []
        s.step(dt: 1.0 / 30); s.fillFlakes(&a)
        s.step(dt: 1.0 / 30); s.fillFlakes(&b)
        var checked = 0
        for i in stride(from: 0, to: min(a.count, b.count), by: 8) {
            let dy = b[i + 1] - a[i + 1]
            if dy < 0 || dy > 20 { continue }                        // skip respawned flakes
            XCTAssertEqual(dy, b[i + 5] / 30, accuracy: 0.05)         // the velocity recorded for the step just taken
            checked += 1
        }
        XCTAssertGreaterThan(checked, s.normalCount / 2)
    }

    func testPileShadeAndAlphaStayInRange() {
        let s = makeSim(.light)
        s.setSurfaces([windowTop])
        for c in 10..<30 { s.setHeightForTests(id: 1, column: c, value: 9) }
        var v: [Float] = []
        s.fillPiles(&v)
        for i in stride(from: 0, to: v.count, by: 4) {
            XCTAssertTrue((0...1).contains(v[i + 2]) && (0...1).contains(v[i + 3]))
        }
        XCTAssertTrue(stride(from: 3, to: v.count, by: 4).contains { v[$0] > 0.5 }, "the base of a pile is shaded")
    }

    func testEmptyWorldDoesNothing() {
        let s = Sim()
        s.step(dt: 0.03)
        var f: [Float] = []
        XCTAssertEqual(s.fillFlakes(&f), 0)
        s.setSurfaces([windowTop])
        XCTAssertEqual(s.fillPiles(&f), 0)
    }

    func testSameSeedGivesTheSameSnow() {
        let a = makeSim(.steady, seed: 11), b = makeSim(.steady, seed: 11)
        run(a, seconds: 5); run(b, seconds: 5)
        var fa: [Float] = [], fb: [Float] = []
        a.fillFlakes(&fa); b.fillFlakes(&fb)
        XCTAssertEqual(fa, fb)
    }

    func testHugeTimeStepIsClamped() {
        let s = makeSim()
        s.step(dt: 500)
        var f: [Float] = []
        s.fillFlakes(&f)
        XCTAssertTrue(f.allSatisfy { $0.isFinite })
    }
}

final class SpeedTests: XCTestCase {
    /// 30 windows on a big screen in a flurry: a step must stay far below the 33 ms frame budget.
    func testStepIsCheap() {
        let s = Sim(seed: 9)
        s.weather = .flurry
        s.setWorld([Rect(x: 0, y: 0, w: 2560, h: 1440)])
        s.setSurfaces((0..<30).map { SurfaceInput(id: $0, rect: Rect(x: Float($0 * 70 % 2000), y: Float(100 + $0 * 40 % 1200), w: 500, h: 300)) })
        let t0 = Date()
        for _ in 0..<600 { s.step(dt: 1.0 / 30) }
        let perStep = Date().timeIntervalSince(t0) / 600
        var buf: [Float] = []
        for c in 0..<30 { for k in 0..<120 { s.setHeightForTests(id: c, column: k, value: 8) } }
        let t1 = Date()
        for _ in 0..<300 { s.fillPiles(&buf); s.fillFlakes(&buf) }
        let perFill = Date().timeIntervalSince(t1) / 300
        print("step_ms=\(perStep * 1000) fill_ms=\(perFill * 1000) flakes=\(s.liveFlakeCount)")
        XCTAssertLessThan(perFill, 0.004, "building the draw data stays cheap")
        XCTAssertLessThan(perStep, 0.004, "under 4 ms per step even in an optimised build")
    }
}
