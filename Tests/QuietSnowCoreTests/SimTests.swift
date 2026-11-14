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
        XCTAssertEqual(huge.normalCount, 5000, "ceiling of 5000 flakes")
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
        XCTAssertEqual(s.surfaceCount, 1, "one missed poll is forgiven: a window can drop out for a moment")
        s.setSurfaces([]); s.setSurfaces([])
        XCTAssertEqual(s.surfaceCount, 0)
        XCTAssertGreaterThan(s.debrisCount, 20)
    }

    func testAWindowThatDropsOutOfOnePollKeepsItsSnow() {
        let s = makeSim(.flurry)
        s.setSurfaces([windowTop])
        run(s, seconds: 60)
        let before = s.heights(of: 1)!.reduce(0, +)
        s.setSurfaces([]); s.setSurfaces([windowTop])
        XCTAssertEqual(s.heights(of: 1)!.reduce(0, +), before, accuracy: 0.001)
        s.setSurfaces([]); s.setSurfaces([]); s.setSurfaces([windowTop])
        XCTAssertEqual(s.heights(of: 1)!.reduce(0, +), before, accuracy: 0.001, "back in time: still the same pile")
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
        for _ in 0..<3 { s.setSurfaces([SurfaceInput(id: -1, rect: Rect(x: 0, y: 880, w: 1440, h: 0), isFloor: true)]) }
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
        XCTAssertEqual(n % 12, 0)
        XCTAssertEqual(v.count, n * 4)
        XCTAssertTrue(v.allSatisfy { $0.isFinite })
    }

    func testWindDoesNotLeaveOneSideOfTheScreenBare() {
        for wind: Float in [220, -220, 60, -60] {
            let s = Sim(seed: 11)
            s.setWorld([Rect(x: 0, y: 0, w: 1920, h: 1080)])
            s.wind = wind
            run(s, seconds: 40)
            var band = [Int](repeating: 0, count: 10)
            for _ in 0..<40 {   // add up forty snapshots so chance does not decide the result
                run(s, seconds: 1)
                var f: [Float] = []
                s.fillFlakes(&f)
                for i in stride(from: 0, to: f.count, by: 8) where f[i] >= 0 && f[i] < 1920 && f[i + 1] >= 0 && f[i + 1] < 1080 {
                    band[Int(f[i] / 192)] += 1
                }
            }
            XCTAssertGreaterThan(Float(band.min()!), Float(band.max()!) * 0.7, "wind \(wind): the emptiest tenth holds at least 70% of the fullest")
        }
    }

    func testEveryScreenGetsTheSameDensityWhateverTheLayout() {
        let r = [Rect(x: 0, y: 0, w: 1920, h: 1080), Rect(x: 1920, y: 400, w: 1280, h: 720), Rect(x: -1080, y: -300, w: 1080, h: 1920)]
        let s = Sim(seed: 5)
        s.setWorld(r)
        run(s, seconds: 30)
        var f: [Float] = []
        s.fillFlakes(&f)
        var d: [Double] = [0, 0, 0]
        for i in stride(from: 0, to: f.count, by: 8) {
            for k in 0..<3 where r[k].contains(f[i], f[i + 1]) { d[k] += 1 / Double(r[k].w * r[k].h) }
        }
        for k in 1..<3 { XCTAssertTrue(d[k] > d[0] * 0.7 && d[k] < d[0] * 1.3, "screen \(k) density \(d[k]) vs \(d[0])") }
    }

    func testAPileHasNoSharpStepAndRoundsOffAtTheEndsOfAWindow() {
        let s = makeSim(.light)
        s.setSurfaces([windowTop])
        for c in 0..<150 { s.setHeightForTests(id: 1, column: c, value: 14) }
        var v: [Float] = []
        s.fillPiles(&v)
        var maxStep: Float = 0, first: Float = 99, firstX: Float = 1e9
        for i in stride(from: 0, to: v.count - 47, by: 48) {   // one column: the body's top-left and top-right vertices
            maxStep = max(maxStep, abs(v[i + 1] - v[i + 9]))
            if v[i] < firstX { firstX = v[i]; first = 300 - v[i + 1] }
        }
        XCTAssertLessThan(maxStep, 5.5, "a pile 14 points tall rises over about 20 points, never in one jump")
        XCTAssertLessThan(first, 3, "the first column of a window pile is nearly bare: the end is rounded")
    }

    func testAFloorKeepsItsHeightOutToTheScreenEdges() {
        let s = makeSim(.light)
        s.setSurfaces([SurfaceInput(id: -1, rect: Rect(x: 0, y: 880, w: 1440, h: 0), isFloor: true)])
        for c in 0..<360 { s.setHeightForTests(id: -1, column: c, value: 20) }
        var v: [Float] = []
        s.fillPiles(&v)
        var leftTop: Float = 0, rightTop: Float = 0, leftX: Float = 1e9, rightX: Float = -1e9
        for i in stride(from: 0, to: v.count - 47, by: 48) {
            if v[i] < leftX { leftX = v[i]; leftTop = 880 - v[i + 1] }
            if v[i + 8] > rightX { rightX = v[i + 8]; rightTop = 880 - v[i + 9] }
        }
        XCTAssertEqual(leftX, 0, accuracy: 0.01)
        XCTAssertEqual(rightX, 1440, accuracy: 0.01)
        XCTAssertEqual(leftTop, 20, accuracy: 0.5)
        XCTAssertEqual(rightTop, 20, accuracy: 0.5)
    }

    func testWindowsHideTheSnowBehindThemInDesktopMode() {
        let s = makeSim(.flurry)
        s.setSurfaces([SurfaceInput(id: -1, rect: Rect(x: 0, y: 880, w: 1440, h: 0), isFloor: true)])
        run(s, seconds: 20)
        s.setOccluders([Rect(x: 200, y: 100, w: 600, h: 500)])
        var f: [Float] = []
        s.fillFlakes(&f)
        var inside = 0, outside = 0
        for i in stride(from: 0, to: f.count, by: 8) {
            if f[i] >= 200 && f[i] < 800 && f[i + 1] >= 100 && f[i + 1] < 600 { inside += 1 } else { outside += 1 }
        }
        XCTAssertEqual(inside, 0)
        XCTAssertGreaterThan(outside, 100)
        for c in 0..<360 { s.setHeightForTests(id: -1, column: c, value: 10) }
        s.setOccluders([Rect(x: 200, y: 0, w: 600, h: 900)])
        var v: [Float] = []
        s.fillPiles(&v)
        XCTAssertFalse(stride(from: 0, to: v.count, by: 4).contains { v[$0] > 204 && v[$0] < 796 })
    }

    func testEachSnowTypeDrawsItsOwnKindAndBigFlakesStayBalls() {
        for t in [SnowType.balls, .flakes, .sleet, .sparkles, .hail] {
            let s = makeSim()
            s.snowType = t
            run(s, seconds: 5)
            var f: [Float] = []
            s.fillFlakes(&f)
            var match = 0, big = 0, bigBalls = 0, other = 0
            for i in stride(from: 0, to: f.count, by: 8) {
                if f[i + 2] > 5 * 1.6 + 1 { big += 1; if f[i + 6] == 0 { bigBalls += 1 } }
                else if f[i + 6] == Float(t.rawValue) { match += 1 } else { other += 1 }
            }
            XCTAssertGreaterThan(match, 0)
            XCTAssertEqual(other, 0, "\(t)")
            XCTAssertEqual(bigBalls, big)
        }
    }

    func testMixedSnowUsesEveryKindAndChangingTheTypeChangesLiveFlakes() {
        let s = makeSim()
        s.snowType = .mixed
        run(s, seconds: 5)
        var f: [Float] = []
        s.fillFlakes(&f)
        var seen = Set<Int>()
        for i in stride(from: 0, to: f.count, by: 8) { seen.insert(Int(f[i + 6])) }
        XCTAssertEqual(seen, [0, 1, 2, 3, 4])
        s.snowType = .hail
        s.fillFlakes(&f)
        for i in stride(from: 0, to: f.count, by: 8) {
            XCTAssertTrue(f[i + 6] == 0 || f[i + 6] == 4)
            XCTAssertTrue(f[i + 7].isFinite)
        }
    }

    func testSleetFallsFasterThanBalls() {
        func meanVy(_ t: SnowType) -> Double {
            let s = makeSim(.steady, seed: 3)
            s.snowType = t
            run(s, seconds: 3)
            var f: [Float] = []
            s.fillFlakes(&f)
            var sum = 0.0, n = 0.0
            for i in stride(from: 0, to: f.count, by: 8) where f[i + 2] <= 5 * 1.6 + 1 { sum += Double(f[i + 5]); n += 1 }
            return sum / n
        }
        XCTAssertGreaterThan(meanVy(.sleet), meanVy(.balls) * 1.3)
    }

    func testADraggedWindowStirsTheSnowNearItAndNotFarAway() {
        let s = makeSim()
        run(s, seconds: 5)
        var before: [Float] = [], after: [Float] = []
        s.fillFlakes(&before)
        s.stir(Rect(x: 300, y: 300, w: 400, h: 200), vx: 600, vy: 0)
        s.step(dt: 1.0 / 30)
        s.fillFlakes(&after)
        var near = 0.0, far = 0.0, nn = 0.0, nf = 0.0
        for i in stride(from: 0, to: min(before.count, after.count) - 7, by: 8) {
            let d = Double(after[i + 4] - before[i + 4])
            if before[i] > 280 && before[i] < 720 && before[i + 1] > 280 && before[i + 1] < 520 { near += d; nn += 1 }
            else if before[i] > 1000 { far += d; nf += 1 }
        }
        XCTAssertGreaterThan(nn, 0); XCTAssertGreaterThan(nf, 0)
        XCTAssertGreaterThan(near / nn, 20)
        XCTAssertLessThan(abs(far / nf), 20)
    }

    /// The same scenario runs in windows/tests.cpp. CI compares the two summaries, so the ports cannot drift apart.
    func testGoldenSummary() {
        let s = Sim(seed: 42)
        s.weather = .steady; s.wind = 30
        s.setWorld([Rect(x: 0, y: 0, w: 1280, h: 800)])
        s.setSurfaces([SurfaceInput(id: 1, rect: Rect(x: 560, y: 300, w: 560, h: 0)),
                       SurfaceInput(id: 2, rect: Rect(x: 120, y: 150, w: 620, h: 0), occluders: [Rect(x: 560, y: 300, w: 560, h: 340)]),
                       SurfaceInput(id: -1, rect: Rect(x: 0, y: 742, w: 1280, h: 0), isFloor: true)])
        s.weather = .flurry
        for _ in 0..<(30 * 120) { s.step(dt: 1.0 / 30) }
        s.weather = .steady
        for _ in 0..<(30 * 10) { s.step(dt: 1.0 / 30) }
        var f: [Float] = []
        let n = s.fillFlakes(&f)
        var sx = 0.0, sy = 0.0
        var band = [0, 0, 0, 0]
        for i in stride(from: 0, to: f.count, by: 8) {
            sx += Double(f[i]); sy += Double(f[i + 1])
            if f[i] >= 0 && f[i] < 1280 { band[min(3, Int(f[i] / 320))] += 1 }
        }
        func total(_ id: Int) -> Double { Double(s.heights(of: id)!.reduce(0, +)) }
        print(String(format: "GOLDEN {\"normal\": %d, \"live\": %d, \"mean_x\": %.1f, \"mean_y\": %.1f, \"b0\": %d, \"b1\": %d, \"b2\": %d, \"b3\": %d, \"pile1\": %.1f, \"pile2\": %.1f, \"floor\": %.1f}",
                     s.normalCount, n, sx / Double(n), sy / Double(n), band[0], band[1], band[2], band[3], total(1), total(2), total(-1)))
    }

    /// The same scenario with snowmen and the plough, also compared with the C++ port in CI.
    func testExtrasSummary() {
        let s = Sim(seed: 42)
        s.setWorld([Rect(x: 0, y: 0, w: 1280, h: 800)])
        s.extras = true
        s.setSurfaces([SurfaceInput(id: 1, rect: Rect(x: 200, y: 300, w: 700, h: 0), radius: 10),
                       SurfaceInput(id: -1, rect: Rect(x: 0, y: 742, w: 1280, h: 0), isFloor: true)])
        for c in 0..<175 { s.setHeightForTests(id: 1, column: c, value: 8) }
        for c in 0..<320 { s.setHeightForTests(id: -1, column: c, value: 8) }
        for _ in 0..<(30 * 50) { s.step(dt: 1.0 / 30) }
        let men = s.snowmanCount, plough = s.ploughX == nil ? 0 : 1
        func total(_ id: Int) -> Double { Double(s.heights(of: id)!.reduce(0, +)) }
        let window = total(1), floor = total(-1)
        s.setSurfaces([SurfaceInput(id: 1, rect: Rect(x: 260, y: 300, w: 700, h: 0), radius: 10),
                       SurfaceInput(id: -1, rect: Rect(x: 0, y: 742, w: 1280, h: 0), isFloor: true)])
        s.step(dt: 1.0 / 30)
        print(String(format: "EXTRAS {\"snowmen\": %d, \"plough\": %d, \"balls\": %d, \"window\": %.1f, \"floor\": %.1f}",
                     men, plough, s.snowballCount, window, floor))
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

    func testWindowPileEndsAreRoundedAndFollowTheCorner() {
        let s = makeSim(.light)
        s.setSurfaces([SurfaceInput(id: 1, rect: Rect(x: 300, y: 300, w: 600, h: 400), radius: 10)])
        for c in 0..<150 { s.setHeightForTests(id: 1, column: c, value: 8) }
        var v: [Float] = []
        s.fillPiles(&v)
        // Vertex 0 of the first slice is the top at the left end: no snow standing up there, and it sits on the corner.
        XCTAssertEqual(v[0], 300)
        XCTAssertGreaterThanOrEqual(v[1], 300 + 10 - 0.01, "the very end is down the corner, not level with the top edge")
        // In the middle the pile is level with the window top and stands up off it.
        let mid = stride(from: 0, to: v.count, by: 48).first { v[$0] >= 600 }!
        XCTAssertLessThan(v[mid + 1], 300 - 4)
        XCTAssertEqual(v[mid + 5], 300.5, "its base is the window top")
    }

    func testAFloorAtTheScreenEdgeRunsOnButOneThatStopsShortIsRounded() {
        let s = makeSim(.light)
        let floor = SurfaceInput(id: -1, rect: Rect(x: 0, y: 850, w: 1440, h: 0), isFloor: true)
        let dock = SurfaceInput(id: -2, rect: Rect(x: 400, y: 820, w: 640, h: 0), isFloor: true, radius: 12, plough: false)
        s.setSurfaces([floor, dock])
        for c in 0..<360 { s.setHeightForTests(id: -1, column: c, value: 8) }
        for c in 0..<160 { s.setHeightForTests(id: -2, column: c, value: 8) }
        var v: [Float] = []
        s.fillPiles(&v)
        // Columns of the dock surface come first (id -2 sorts before -1).
        XCTAssertGreaterThan(v[1], 820, "the Dock pile starts down its rounded corner")
        let floorStart = stride(from: 0, to: v.count, by: 4).first { v[$0] == 0 }!
        XCTAssertLessThan(v[floorStart + 1], 850 - 6, "the floor keeps its height at the screen edge")
    }

    func testASnowmanBuildsOnADeepPileAndFallsWhenTheWindowMoves() {
        let s = makeSim(.light, seed: 3)
        s.extras = true
        s.setSurfaces([windowTop])
        for c in 0..<150 { s.setHeightForTests(id: 1, column: c, value: 8) }
        run(s, seconds: 30)
        XCTAssertEqual(s.snowmanCount, 1)
        var out: [Float] = []
        let n = s.fillFlakes(&out)
        XCTAssertTrue(stride(from: 6, to: n * 8, by: 8).contains { out[$0] == 5 }, "drawn as kind 5")
        s.setSurfaces([SurfaceInput(id: 1, rect: Rect(x: 500, y: 300, w: 600, h: 400))])
        s.step(dt: 1.0 / 30)
        XCTAssertEqual(s.snowmanCount, 0)
        XCTAssertEqual(s.snowballCount, 3, "a finished snowman breaks into its three balls")
        run(s, seconds: 12)
        XCTAssertEqual(s.snowballCount, 0, "the balls melt or fall away")
    }

    func testTheCursorKnocksASnowmanOver() {
        let s = makeSim(.light, seed: 3)
        s.extras = true
        s.setSurfaces([windowTop])
        for c in 0..<150 { s.setHeightForTests(id: 1, column: c, value: 8) }
        run(s, seconds: 30)
        XCTAssertEqual(s.snowmanCount, 1)
        var out: [Float] = []
        let n = s.fillFlakes(&out)
        guard let k = stride(from: 6, to: n * 8, by: 8).first(where: { out[$0] == 5 }) else { return XCTFail() }
        let x = out[k - 6], y = out[k - 5]
        s.step(dt: 1.0 / 30, pointer: (x - 40, y))
        s.step(dt: 1.0 / 30, pointer: (x, y))
        XCTAssertEqual(s.snowmanCount, 0)
    }

    func testNoExtrasUnlessAskedFor() {
        let s = makeSim(.light, seed: 3)
        s.setSurfaces([windowTop, SurfaceInput(id: -1, rect: Rect(x: 0, y: 850, w: 1440, h: 0), isFloor: true)])
        for c in 0..<150 { s.setHeightForTests(id: 1, column: c, value: 8) }
        for c in 0..<360 { s.setHeightForTests(id: -1, column: c, value: 8) }
        run(s, seconds: 60)
        XCTAssertEqual(s.snowmanCount, 0)
        XCTAssertNil(s.ploughX)
    }

    func testThePloughClearsTheFloorAndPushesTheSnowOffTheScreen() {
        let s = makeSim(.light, seed: 3), control = makeSim(.light, seed: 3)
        s.extras = true
        for sim in [s, control] {
            sim.setSurfaces([SurfaceInput(id: -1, rect: Rect(x: 0, y: 850, w: 1440, h: 0), isFloor: true)])
            for c in 0..<360 { sim.setHeightForTests(id: -1, column: c, value: 8) }
        }
        var seen = false
        for _ in 0..<(30 * 90) {
            s.step(dt: 1.0 / 30)
            control.step(dt: 1.0 / 30)
            if s.ploughX != nil { seen = true }
            if seen && s.ploughX == nil { break }
        }
        XCTAssertTrue(seen, "a plough comes within 30 seconds")
        XCTAssertNil(s.ploughX, "and leaves again")
        XCTAssertLessThan(s.totalSnowOnSurfaces, control.totalSnowOnSurfaces * 0.6)
        for h in s.heights(of: -1)! { XCTAssertLessThanOrEqual(h, Sim.floorCapForTests + 0.01) }
    }

    func testThePloughLeavesAHeapAgainstTheDockAndNeverDrivesOnIt() {
        let s = makeSim(.light, seed: 3)
        s.extras = true
        let left = SurfaceInput(id: -3, rect: Rect(x: 0, y: 850, w: 400, h: 0), isFloor: true)
        let dock = SurfaceInput(id: -2, rect: Rect(x: 400, y: 820, w: 640, h: 0), isFloor: true, radius: 12, plough: false)
        s.setSurfaces([left, dock])
        for c in 0..<100 { s.setHeightForTests(id: -3, column: c, value: 6) }
        for c in 0..<160 { s.setHeightForTests(id: -2, column: c, value: 6) }
        let dockBefore = s.heights(of: -2)!.reduce(0, +)
        var maxX: Float = -1000
        for _ in 0..<(30 * 90) {
            s.step(dt: 1.0 / 30)
            if let x = s.ploughX { maxX = max(maxX, x) } else if maxX > -1000 { break }
        }
        XCTAssertGreaterThan(maxX, 300, "it drove along the floor")
        XCTAssertLessThan(maxX, 400, "it stopped at the Dock")
        let h = s.heights(of: -3)!
        XCTAssertGreaterThan(h[95..<100].reduce(0, +), h[10..<15].reduce(0, +) + 10, "the heap is at the Dock end")
        XCTAssertGreaterThanOrEqual(s.heights(of: -2)!.reduce(0, +), dockBefore - 0.01, "the Dock was not ploughed")
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
