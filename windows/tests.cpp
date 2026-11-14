// Tests for sim.hpp, mirroring Tests/QuietSnowCoreTests. Plain asserts, no framework: build and run, exit code 0 means pass.
#include <chrono>
#include <cstdio>
#include <functional>
#include <string>

#include "sim.hpp"

using namespace qs;

static int failures = 0, tests = 0;
#define CHECK(cond)                                                                     \
    do {                                                                                \
        if (!(cond)) { std::printf("  FAIL line %d: %s\n", __LINE__, #cond); ++failures; } \
    } while (0)
#define NEAR(a, b, tol) CHECK(std::fabs((double)(a) - (double)(b)) <= (tol))

static const Rect screen{0, 0, 1440, 900};
static const SurfaceInput windowTop{1, {300, 300, 600, 0}, {}, false};
static SurfaceInput floorInput() { return {-1, {0, 880, 1440, 0}, {}, true}; }

static Sim makeSim(Weather w = Weather::Steady, uint64_t seed = 7) {
    Sim s(seed);
    s.setWeather(w);
    s.setWorld({screen});
    return s;
}
static void run(Sim& s, float seconds, const Pointer* p = nullptr) {
    for (float t = 0; t < seconds; t += 1.f / 30) s.step(1.f / 30, p);
}
static float sum(const std::vector<float>* v) { float t = 0; for (float x : *v) t += x; return t; }

static void test(const char* name, const std::function<void()>& fn) {
    ++tests;
    const int before = failures;
    fn();
    std::printf("%s %s\n", failures == before ? "ok  " : "FAIL", name);
}

int main() {
    test("flakes fall", [] {
        Sim s = makeSim();
        std::vector<float> a, b;
        s.fillFlakes(a); s.step(0.1f); s.fillFlakes(b);
        int down = 0;
        for (size_t i = 1; i < std::min(a.size(), b.size()); i += 8) down += b[i] > a[i];
        CHECK(down > (int)(a.size() / 8) * 9 / 10);
    });
    test("count follows weather and screen size", [] {
        Sim light = makeSim(Weather::Light), flurry = makeSim(Weather::Flurry);
        CHECK(light.normalCount() < flurry.normalCount());
        Sim tiny; tiny.setWorld({{0, 0, 100, 100}});
        CHECK(tiny.normalCount() == 60);
        Sim huge; huge.setWeather(Weather::Flurry); huge.setWorld({{0, 0, 7680, 4320}});
        CHECK(huge.normalCount() == 5000);
    });
    test("snow lands on a window top", [] {
        Sim s = makeSim(); s.setSurfaces({windowTop}); run(s, 60);
        CHECK(sum(s.heights(1)) > 20);
    });
    test("pile never passes the cap or goes negative", [] {
        Sim s = makeSim(Weather::Flurry); s.setSurfaces({windowTop}); run(s, 240);
        for (float h : *s.heights(1)) { CHECK(h <= Sim::kWindowCap + 0.5f); CHECK(h >= 0); }
    });
    test("occluded columns stay bare", [] {
        Sim s = makeSim(Weather::Flurry);
        SurfaceInput in = windowTop; in.occluders = {{250, 200, 350, 200}};
        s.setSurfaces({in}); run(s, 90);
        const auto& h = *s.heights(1);
        float covered = 0;
        for (int c = 0; c < (int)((600 - 300) / Sim::kColumnWidth) - 2; ++c) covered += h[c];
        NEAR(covered, 0, 0.001);
        CHECK(sum(&h) > 10);
    });
    test("off-screen edge gets no snow", [] {
        Sim s = makeSim(Weather::Flurry);
        s.setSurfaces({{1, {1300, 300, 400, 0}, {}, false}}); run(s, 90);
        const auto& h = *s.heights(1);
        float off = 0;
        for (size_t c = (size_t)((1440 - 1300) / Sim::kColumnWidth) + 2; c < h.size(); ++c) off += h[c];
        NEAR(off, 0, 0.001);
    });
    test("moving a window drops its snow", [] {
        Sim s = makeSim(Weather::Flurry); s.setSurfaces({windowTop}); run(s, 90);
        CHECK(sum(s.heights(1)) > 20);
        s.setSurfaces({{1, {340, 340, 600, 0}, {}, false}});
        NEAR(sum(s.heights(1)), 0, 0.001);
        CHECK(s.debrisCount() > 20);
    });
    test("closing a window drops its snow", [] {
        Sim s = makeSim(Weather::Flurry); s.setSurfaces({windowTop}); run(s, 90);
        s.setSurfaces({});
        CHECK(s.surfaceCount() == 1);   // one missed poll is forgiven: a window can drop out for a moment
        s.setSurfaces({}); s.setSurfaces({});
        CHECK(s.surfaceCount() == 0);
        CHECK(s.debrisCount() > 20);
    });
    test("a window that drops out of one poll keeps its snow", [] {
        Sim s = makeSim(Weather::Flurry); s.setSurfaces({windowTop}); run(s, 60);
        const float before = sum(s.heights(1));
        s.setSurfaces({}); s.setSurfaces({windowTop});
        NEAR(sum(s.heights(1)), before, 0.001);
        s.setSurfaces({}); s.setSurfaces({}); s.setSurfaces({windowTop});   // back in time: still the same pile
        NEAR(sum(s.heights(1)), before, 0.001);
    });
    test("resize drops the pile, sub-point jitter keeps it", [] {
        Sim s = makeSim(Weather::Flurry); s.setSurfaces({windowTop}); run(s, 60);
        const float before = sum(s.heights(1));
        s.setSurfaces({{1, {300.5f, 300.4f, 600, 0}, {}, false}});
        NEAR(sum(s.heights(1)), before, 0.001);
        s.setSurfaces({{1, {300, 300, 700, 0}, {}, false}});
        NEAR(sum(s.heights(1)), 0, 0.001);
    });
    test("debris lands on the floor below", [] {
        Sim s = makeSim(Weather::Light);
        s.setSurfaces({windowTop, floorInput()}); run(s, 200);
        const float floorBefore = sum(s.heights(-1));
        for (int i = 0; i < 3; ++i) s.setSurfaces({floorInput()});
        run(s, 8);
        CHECK(sum(s.heights(-1)) > floorBefore);
        CHECK(s.debrisCount() == 0);
    });
    test("slumping flattens a spike and keeps the snow", [] {
        Sim s = makeSim(Weather::Light); s.setSurfaces({windowTop});
        (*s.heightsMutable(1))[50] = 11;
        const float mass = sum(s.heights(1));
        for (int i = 0; i < 60; ++i) s.step(1.f / 30);
        const auto& h = *s.heights(1);
        CHECK(h[50] < 6);
        CHECK(h[49] + h[51] > 4);
        CHECK(sum(&h) + 0.5f >= mass * 0.9f);
    });
    test("snow sheds over the edge of a window", [] {
        Sim s = makeSim(Weather::Light); s.setSurfaces({windowTop});
        (*s.heightsMutable(1))[0] = 12;
        for (int i = 0; i < 150; ++i) s.step(1.f / 30);
        CHECK((*s.heights(1))[0] < 4);
    });
    test("melting shrinks piles, off does not", [] {
        Sim keep = makeSim(Weather::Light, 5), melt = makeSim(Weather::Light, 5);
        melt.melts = true;
        for (Sim* sim : {&keep, &melt}) {
            sim->setSurfaces({windowTop});
            for (int c = 20; c < 60; ++c) (*sim->heightsMutable(1))[c] = 6;
        }
        run(keep, 120); run(melt, 120);
        CHECK(melt.totalSnowOnSurfaces() < keep.totalSnowOnSurfaces() * 0.5f);
        CHECK(keep.totalSnowOnSurfaces() > 200);
    });
    test("sweeping throws snow, hovering does not", [] {
        Sim s = makeSim(Weather::Light); s.setSurfaces({windowTop});
        for (int c = 40; c < 80; ++c) (*s.heightsMutable(1))[c] = 8;
        const float m0 = s.totalSnowOnSurfaces();
        Pointer hover{400, 299};
        for (int i = 0; i < 60; ++i) s.step(1.f / 30, &hover);
        CHECK(s.totalSnowOnSurfaces() > m0 * 0.95f);
        Pointer p{300, 299};
        for (int i = 0; i < 40; ++i) { s.step(1.f / 30, &p); p.x += 12; }
        CHECK(s.totalSnowOnSurfaces() < m0 * 0.8f);
    });
    test("wind pushes flakes sideways", [] {
        Sim calm = makeSim(Weather::Steady, 3), windy = makeSim(Weather::Steady, 3);
        windy.wind = 200;
        for (int i = 0; i < 60; ++i) { calm.step(1.f / 30); windy.step(1.f / 30); }
        std::vector<float> a, b;
        calm.fillFlakes(a); windy.fillFlakes(b);
        auto meanX = [](const std::vector<float>& v) { float t = 0; int n = 0; for (size_t i = 0; i < v.size(); i += 8) { t += v[i]; ++n; } return t / n; };
        CHECK(meanX(b) - meanX(a) > 20);
    });
    test("nothing goes NaN or out of bounds over a long run", [] {
        Sim s = makeSim(Weather::Flurry); s.wind = -150; s.melts = true;
        s.setSurfaces({windowTop, {2, {900, 500, 300, 0}, {}, false}, floorInput()});
        for (int i = 0; i < 6000; ++i) {
            Pointer p{(float)((i * 37) % 1440), (float)((i * 53) % 900)};
            s.step(1.f / 30, i % 7 == 0 ? nullptr : &p);
            if (i % 1000 == 0) s.clearPiles();
        }
        std::vector<float> f; s.fillFlakes(f);
        bool finite = true; for (float x : f) finite = finite && std::isfinite(x);
        CHECK(finite);
        CHECK(s.liveFlakeCount() <= s.flakeCapacity());
        for (int id : {1, 2, -1}) for (float h : *s.heights(id)) CHECK(std::isfinite(h) && h >= 0);
    });
    test("pile geometry is whole triangles", [] {
        Sim s = makeSim(Weather::Light); s.setSurfaces({windowTop});
        for (int c = 10; c < 30; ++c) (*s.heightsMutable(1))[c] = 5;
        std::vector<float> v; const int n = s.fillPiles(v);
        CHECK(n > 0); CHECK(n % 12 == 0); CHECK((int)v.size() == n * 4);
        bool finite = true; for (float x : v) finite = finite && std::isfinite(x);
        CHECK(finite);
    });
    test("empty world does nothing", [] {
        Sim s; s.step(0.03f);
        std::vector<float> f;
        CHECK(s.fillFlakes(f) == 0);
        s.setSurfaces({windowTop});
        CHECK(s.fillPiles(f) == 0);
    });
    test("same seed gives the same snow", [] {
        Sim a = makeSim(Weather::Steady, 11), b = makeSim(Weather::Steady, 11);
        run(a, 5); run(b, 5);
        std::vector<float> fa, fb; a.fillFlakes(fa); b.fillFlakes(fb);
        CHECK(fa == fb);
    });
    test("huge time step is clamped", [] {
        Sim s = makeSim(); s.step(500);
        std::vector<float> f; s.fillFlakes(f);
        bool finite = true; for (float x : f) finite = finite && std::isfinite(x);
        CHECK(finite);
    });
    test("foreground flakes never land", [] {
        Sim s = makeSim(Weather::Flurry); s.setSurfaces({windowTop}); run(s, 5);
        int checked = 0;
        for (int i = 0; i < s.normalCount(); ++i) {
            if (s.sizes()[i] > 5) { s.ys()[i] = windowTop.rect.y - 0.5f; s.xs()[i] = 400; CHECK(!s.landForTest(i, windowTop.rect.y - 5)); ++checked; }
        }
        CHECK(checked > 0);
    });
    test("recorded velocity matches movement", [] {
        Sim s = makeSim(Weather::Steady, 21);
        std::vector<float> a, b;
        s.step(1.f / 30); s.fillFlakes(a); s.step(1.f / 30); s.fillFlakes(b);
        int checked = 0;
        for (size_t i = 0; i + 8 <= std::min(a.size(), b.size()); i += 8) {
            const float dy = b[i + 1] - a[i + 1];
            if (dy < 0 || dy > 20) continue;
            NEAR(dy, b[i + 5] / 30, 0.05);
            ++checked;
        }
        CHECK(checked > s.normalCount() / 2);
    });
    test("shade and alpha stay in range", [] {
        Sim s = makeSim(Weather::Light); s.setSurfaces({windowTop});
        for (int c = 10; c < 30; ++c) (*s.heightsMutable(1))[c] = 9;
        std::vector<float> v; s.fillPiles(v);
        bool inRange = true, shaded = false;
        for (size_t i = 0; i + 4 <= v.size(); i += 4) { inRange = inRange && v[i + 2] >= 0 && v[i + 2] <= 1 && v[i + 3] >= 0 && v[i + 3] <= 1; shaded = shaded || v[i + 3] > 0.5f; }
        CHECK(inRange); CHECK(shaded);
    });
    test("wind does not leave one side of the screen bare", [] {
        for (float wind : {220.f, -220.f, 60.f, -60.f}) {
            Sim s(11); s.setWorld({{0, 0, 1920, 1080}}); s.wind = wind;
            run(s, 40);
            int band[10] = {};
            for (int look = 0; look < 40; ++look) {   // add up forty snapshots so chance does not decide the result
                run(s, 1);
                std::vector<float> f; s.fillFlakes(f);
                for (size_t i = 0; i < f.size(); i += 8) if (f[i] >= 0 && f[i] < 1920 && f[i + 1] >= 0 && f[i + 1] < 1080) ++band[(int)(f[i] / 192)];
            }
            int lo = band[0], hi = band[0];
            for (int b : band) { lo = std::min(lo, b); hi = std::max(hi, b); }
            CHECK(lo > hi * 0.7);   // the emptiest tenth of the screen holds at least 70% of the fullest
        }
    });
    test("every screen gets the same density whatever the layout", [] {
        Sim s(5); s.setWorld({{0, 0, 1920, 1080}, {1920, 400, 1280, 720}, {-1080, -300, 1080, 1920}});
        run(s, 30);
        std::vector<float> f; s.fillFlakes(f);
        const Rect r[3] = {{0, 0, 1920, 1080}, {1920, 400, 1280, 720}, {-1080, -300, 1080, 1920}};
        double d[3] = {0, 0, 0};
        for (size_t i = 0; i < f.size(); i += 8) for (int k = 0; k < 3; ++k) if (r[k].contains(f[i], f[i + 1])) d[k] += 1 / (r[k].w * r[k].h);
        for (int k = 1; k < 3; ++k) CHECK(d[k] > d[0] * 0.7 && d[k] < d[0] * 1.3);
    });
    test("a pile has no sharp step and rounds off at the ends of a window", [] {
        Sim s = makeSim(Weather::Light); s.setSurfaces({windowTop});
        auto& h = *s.heightsMutable(1);
        for (size_t c = 0; c < h.size(); ++c) h[c] = 14;
        std::vector<float> v; s.fillPiles(v);
        float maxStep = 0, first = 99, firstX = 1e9;
        for (size_t i = 0; i + 48 <= v.size(); i += 48) {   // one column: the body's top-left and top-right vertices
            maxStep = std::max(maxStep, std::fabs(v[i + 1] - v[i + 9]));
            if (v[i] < firstX) { firstX = v[i]; first = 300 - v[i + 1]; }
        }
        CHECK(maxStep < 5.5f);   // a pile 14 px tall rises over about 20 px, never in one jump
        CHECK(first < 3);        // the first column of a window pile is nearly bare: the end is rounded
    });
    test("a floor keeps its height out to the screen edges", [] {
        Sim s = makeSim(Weather::Light); s.setSurfaces({floorInput()});
        auto& h = *s.heightsMutable(-1);
        for (size_t c = 0; c < h.size(); ++c) h[c] = 20;
        std::vector<float> v; s.fillPiles(v);
        float leftTop = 0, rightTop = 0, leftX = 1e9, rightX = -1e9;
        for (size_t i = 0; i + 48 <= v.size(); i += 48) {
            if (v[i] < leftX) { leftX = v[i]; leftTop = 880 - v[i + 1]; }
            if (v[i + 8] > rightX) { rightX = v[i + 8]; rightTop = 880 - v[i + 9]; }
        }
        NEAR(leftX, 0, 0.01); NEAR(rightX, 1440, 0.01);
        NEAR(leftTop, 20, 0.5); NEAR(rightTop, 20, 0.5);
    });
    test("windows hide the snow behind them in desktop mode", [] {
        Sim s = makeSim(Weather::Flurry); s.setSurfaces({floorInput()}); run(s, 20);
        s.setOccluders({{200, 100, 600, 500}});
        std::vector<float> f; s.fillFlakes(f);
        int inside = 0, outside = 0;
        for (size_t i = 0; i < f.size(); i += 8) (f[i] >= 200 && f[i] < 800 && f[i + 1] >= 100 && f[i + 1] < 600) ? ++inside : ++outside;
        CHECK(inside == 0); CHECK(outside > 100);
        for (size_t c = 0; c < s.heights(-1)->size(); ++c) (*s.heightsMutable(-1))[c] = 10;
        s.setOccluders({{200, 0, 600, 900}});
        std::vector<float> v; s.fillPiles(v);
        bool under = false;
        for (size_t i = 0; i < v.size(); i += 4) under = under || (v[i] > 204 && v[i] < 796);
        CHECK(!under);
    });
    test("each snow type draws its own kind, and the big foreground flakes stay balls", [] {
        const SnowType all[] = {SnowType::Balls, SnowType::Flakes, SnowType::Sleet, SnowType::Sparkles, SnowType::Hail};
        for (SnowType t : all) {
            Sim s = makeSim(); s.setSnowType(t); run(s, 5);
            std::vector<float> f; s.fillFlakes(f);
            int match = 0, big = 0, bigBalls = 0, other = 0;
            for (size_t i = 0; i < f.size(); i += 8) {
                const bool isBig = f[i + 2] > 5 * 1.6f + 1;
                if (isBig) { ++big; bigBalls += f[i + 6] == 0; } else if (f[i + 6] == (float)(int)t) ++match; else ++other;
            }
            CHECK(match > 0 && other == 0);
            CHECK(bigBalls == big);
        }
    });
    test("mixed snow uses every kind and changing the type changes live flakes", [] {
        Sim s = makeSim(); s.setSnowType(SnowType::Mixed); run(s, 5);
        std::vector<float> f; s.fillFlakes(f);
        bool seen[5] = {};
        for (size_t i = 0; i < f.size(); i += 8) seen[(int)f[i + 6]] = true;
        for (bool b : seen) CHECK(b);
        s.setSnowType(SnowType::Hail);
        s.fillFlakes(f);
        for (size_t i = 0; i < f.size(); i += 8) CHECK(f[i + 6] == 0 || f[i + 6] == 4);
        for (size_t i = 0; i < f.size(); i += 8) CHECK(std::isfinite(f[i + 7]));
    });
    test("sleet falls faster than balls", [] {
        auto meanVy = [](SnowType t) {
            Sim s = makeSim(Weather::Steady, 3); s.setSnowType(t); run(s, 3);
            std::vector<float> f; s.fillFlakes(f);
            double sum = 0; int n = 0;
            for (size_t i = 0; i < f.size(); i += 8) if (f[i + 2] <= 5 * 1.6f + 1) { sum += f[i + 5]; ++n; }
            return sum / n;
        };
        CHECK(meanVy(SnowType::Sleet) > meanVy(SnowType::Balls) * 1.3);
    });
    test("a dragged window stirs the snow near it and not far away", [] {
        Sim s = makeSim(); run(s, 5);
        std::vector<float> before, after; s.fillFlakes(before);
        s.stir({300, 300, 400, 200}, 600, 0);
        s.step(1.f / 30);
        s.fillFlakes(after);
        double near = 0, far = 0; int nn = 0, nf = 0;
        for (size_t i = 0; i + 8 <= before.size() && i + 8 <= after.size(); i += 8) {
            const double d = after[i + 4] - before[i + 4];   // the glide velocity along x
            if (before[i] > 280 && before[i] < 720 && before[i + 1] > 280 && before[i + 1] < 520) { near += d; ++nn; }
            else if (before[i] > 1000) { far += d; ++nf; }
        }
        CHECK(nn > 0 && nf > 0);
        CHECK(near / nn > 20);
        CHECK(std::fabs(far / nf) < 20);
    });
    test("a step stays cheap", [] {
        Sim s(9); s.setWeather(Weather::Flurry); s.setWorld({{0, 0, 2560, 1440}});
        std::vector<SurfaceInput> in;
        for (int i = 0; i < 30; ++i) in.push_back({i, {(float)(i * 70 % 2000), (float)(100 + i * 40 % 1200), 500, 0}, {}, false});
        s.setSurfaces(in);
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 600; ++i) s.step(1.f / 30);
        const double per = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() / 600;
        std::printf("  step_ms=%.3f\n", per * 1000);
        CHECK(per < 0.004);
    });

    test("window pile ends are rounded and follow the corner", [] {
        Sim s = makeSim(Weather::Light);
        SurfaceInput w = windowTop; w.radius = 10;
        s.setSurfaces({w});
        auto* h = s.heightsMutable(1);
        for (int c = 0; c < 150; ++c) (*h)[c] = 8;
        std::vector<float> v; s.fillPiles(v);
        CHECK(v[0] == 300);
        CHECK(v[1] >= 300 + 10 - 0.01f);   // the very end is down the corner, not level with the top edge
        size_t mid = 0;
        while (mid < v.size() && v[mid] < 600) mid += 48;
        CHECK(v[mid + 1] < 300 - 4);
        CHECK(v[mid + 5] == 300.5f);
    });
    test("a floor at the screen edge runs on, one that stops short is rounded", [] {
        Sim s = makeSim(Weather::Light);
        SurfaceInput floor{-1, {0, 850, 1440, 0}, {}, true};
        SurfaceInput dock{-2, {400, 820, 640, 0}, {}, true, 12, false};
        s.setSurfaces({floor, dock});
        for (auto& x : *s.heightsMutable(-1)) x = 8;
        for (auto& x : *s.heightsMutable(-2)) x = 8;
        std::vector<float> v; s.fillPiles(v);
        CHECK(v[1] > 820);                  // ids sort: the Dock (-2) comes first
        size_t at = 0;
        while (at < v.size() && v[at] != 0) at += 4;
        CHECK(at < v.size() && v[at + 1] < 850 - 6);
    });
    auto deepWindow = [] {
        Sim s = makeSim(Weather::Light, 3);
        s.setExtras(true);
        s.setSurfaces({windowTop});
        for (int c = 0; c < 150; ++c) (*s.heightsMutable(1))[c] = 8;
        run(s, 30);
        return s;
    };
    test("a snowman builds on a deep pile and falls when the window moves", [&] {
        Sim s = deepWindow();
        CHECK(s.snowmanCount() == 1);
        std::vector<float> f; const int n = s.fillFlakes(f);
        bool drawn = false;
        for (int i = 0; i < n; ++i) drawn |= f[i * 8 + 6] == 5;
        CHECK(drawn);
        s.setSurfaces({{1, {500, 300, 600, 0}, {}, false}});
        s.step(1.f / 30);
        CHECK(s.snowmanCount() == 0);
        CHECK(s.snowballCount() == 3);
        run(s, 12);
        CHECK(s.snowballCount() == 0);
    });
    test("the cursor knocks a snowman over", [&] {
        Sim s = deepWindow();
        CHECK(s.snowmanCount() == 1);
        std::vector<float> f; const int n = s.fillFlakes(f);
        int k = -1;
        for (int i = 0; i < n; ++i) if (f[i * 8 + 6] == 5) k = i;
        CHECK(k >= 0);
        if (k < 0) return;
        Pointer a{f[k * 8] - 40, f[k * 8 + 1]}, b{f[k * 8], f[k * 8 + 1]};
        s.step(1.f / 30, &a); s.step(1.f / 30, &b);
        CHECK(s.snowmanCount() == 0);
    });
    test("no extras unless asked for", [] {
        Sim s = makeSim(Weather::Light, 3);
        s.setSurfaces({windowTop, {-1, {0, 850, 1440, 0}, {}, true}});
        for (int c = 0; c < 150; ++c) (*s.heightsMutable(1))[c] = 8;
        for (auto& x : *s.heightsMutable(-1)) x = 8;
        run(s, 60);
        CHECK(s.snowmanCount() == 0);
        CHECK(!s.ploughActive());
    });
    test("the plough clears the floor and pushes the snow off the screen", [] {
        Sim s = makeSim(Weather::Light, 3), control = makeSim(Weather::Light, 3);
        s.setExtras(true);
        for (Sim* sim : {&s, &control}) {
            sim->setSurfaces({{-1, {0, 850, 1440, 0}, {}, true}});
            for (auto& x : *sim->heightsMutable(-1)) x = 8;
        }
        bool seen = false;
        for (int i = 0; i < 30 * 90; ++i) {
            s.step(1.f / 30); control.step(1.f / 30);
            if (s.ploughActive()) seen = true;
            if (seen && !s.ploughActive()) break;
        }
        CHECK(seen);
        CHECK(!s.ploughActive());
        CHECK(s.totalSnowOnSurfaces() < control.totalSnowOnSurfaces() * 0.6f);
        for (float h : *s.heights(-1)) CHECK(h <= s.cap(-1) + 0.01f);
    });
    test("the plough leaves a heap against the Dock and never drives on it", [] {
        Sim s = makeSim(Weather::Light, 3);
        s.setExtras(true);
        s.setSurfaces({{-3, {0, 850, 400, 0}, {}, true}, {-2, {400, 820, 640, 0}, {}, true, 12, false}});
        for (auto& x : *s.heightsMutable(-3)) x = 6;
        for (auto& x : *s.heightsMutable(-2)) x = 6;
        const float dockBefore = sum(s.heights(-2));
        float maxX = -1000;
        for (int i = 0; i < 30 * 90; ++i) {
            s.step(1.f / 30);
            if (s.ploughActive()) maxX = std::max(maxX, s.ploughX()); else if (maxX > -1000) break;
        }
        CHECK(maxX > 300);
        CHECK(maxX < 400);
        const auto& h = *s.heights(-3);
        float end = 0, start = 0;
        for (int c = 95; c < 100; ++c) end += h[c];
        for (int c = 10; c < 15; ++c) start += h[c];
        CHECK(end > start + 10);
        CHECK(sum(s.heights(-2)) >= dockBefore - 0.01f);
    });

    auto groundSim = [](uint64_t seed) {
        Sim s = makeSim(Weather::Light, seed);
        s.setSurfaces({{-1, {0, 850, 1440, 0}, {}, true}});
        for (auto& x : *s.heightsMutable(-1)) x = 10;
        return s;
    };
    test("a floor holds a third of its screen, a window forty", [] {
        Sim s = makeSim(Weather::Light);
        s.setSurfaces({windowTop, {-1, {0, 850, 1440, 0}, {}, true}});
        CHECK(s.cap(-1) == 300);
        CHECK(s.cap(1) == 40);
    });
    test("clicks go to the snow only where there is snow", [&] {
        Sim s = groundSim(7);
        CHECK(!s.snowAt(100, 845));
        s.setExtras(true);
        CHECK(s.snowAt(100, 845));
        CHECK(!s.snowAt(100, 500));
    });
    test("dragging a ball through snow grows it and it rests where dropped", [&] {
        Sim s = groundSim(7);
        s.setExtras(true);
        CHECK(!s.grab(100, 500));
        CHECK(s.grab(100, 845));
        const float before = s.totalSnowOnSurfaces();
        for (int k = 0; k < 120; ++k) s.drag(100 + k * 4.f, 843);
        CHECK(s.carriedRadius() > 15);
        CHECK(s.totalSnowOnSurfaces() < before - 100);
        s.drop(0, 0);
        CHECK(!s.carrying());
        run(s, 30);
        CHECK(s.snowballCount() == 1);
    });
    test("three balls stacked make a snowman", [&] {
        Sim s = groundSim(7);
        s.setExtras(true);
        const int starts[3] = {100, 700, 1100}, lens[3] = {120, 60, 30};
        for (int j = 0; j < 3; ++j) {
            CHECK(s.grab((float)starts[j], 845));
            for (int k = 0; k < lens[j]; ++k) s.drag((float)(starts[j] + k * 3), 843);
            s.drag(600, 500);
            s.drop(0, 0);
            run(s, 3);
        }
        CHECK(s.snowballCount() == 0);
        CHECK(s.snowmanCount() == 1);
        run(s, 20);
        CHECK(s.snowmanCount() == 1);
    });
    test("Santa clears a deep window", [] {
        Sim s = makeSim(Weather::Light, 4);
        s.setExtras(true);
        s.setSurfaces({windowTop});
        for (auto& x : *s.heightsMutable(1)) x = 30;
        bool seen = false;
        for (int i = 0; i < 30 * 40; ++i) {
            s.step(1.f / 30);
            if (s.sleighActive()) seen = true;
            if (seen && !s.sleighActive()) break;
        }
        CHECK(seen);
        CHECK(sum(s.heights(1)) < 150 * 3);
    });
    test("Santa flattens a mountain a layer at a time, then the plough comes", [] {
        Sim s = makeSim(Weather::Light, 4);
        s.setExtras(true);
        s.setSurfaces({{-1, {0, 850, 1440, 0}, {}, true}});
        auto& h = *s.heightsMutable(-1);
        for (int c = 0; c < 360; ++c) h[c] = (float)std::max(0, 290 - std::abs(c - 180) * 2);
        int passes = 0;
        float last = -1e9f;
        bool ploughAfter = false;
        for (int i = 0; i < 30 * 150; ++i) {
            s.step(1.f / 30);
            if (s.sleighActive()) { if (s.sleighX() < last) ++passes; last = s.sleighX(); }
            if (!s.sleighActive() && passes > 0 && s.ploughActive()) { ploughAfter = true; break; }
        }
        CHECK(passes >= 4);
        float m = 0;
        for (float x : *s.heights(-1)) m = std::max(m, x);
        CHECK(m <= 62);
        CHECK(ploughAfter);
    });

    // The same scenario runs in the Swift tests. CI compares the two summaries, so the ports cannot drift apart.
    {
        Sim s(42);
        s.setWeather(Weather::Steady); s.wind = 30; s.setWorld({{0, 0, 1280, 800}});
        s.setSurfaces({{1, {560, 300, 560, 0}, {}, false}, {2, {120, 150, 620, 0}, {{560, 300, 560, 340}}, false}, {-1, {0, 742, 1280, 0}, {}, true}});
        s.setWeather(Weather::Flurry);
        for (int i = 0; i < 30 * 120; ++i) s.step(1.f / 30);
        s.setWeather(Weather::Steady);
        for (int i = 0; i < 30 * 10; ++i) s.step(1.f / 30);
        std::vector<float> f; const int n = s.fillFlakes(f);
        double sx = 0, sy = 0; int band[4] = {};
        for (size_t i = 0; i < f.size(); i += 8) { sx += f[i]; sy += f[i + 1]; if (f[i] >= 0 && f[i] < 1280) ++band[std::min(3, (int)(f[i] / 320))]; }
        std::printf("GOLDEN {\"normal\": %d, \"live\": %d, \"mean_x\": %.1f, \"mean_y\": %.1f, \"b0\": %d, \"b1\": %d, \"b2\": %d, \"b3\": %d, \"pile1\": %.1f, \"pile2\": %.1f, \"floor\": %.1f}\n",
                    s.normalCount(), n, sx / n, sy / n, band[0], band[1], band[2], band[3], sum(s.heights(1)), sum(s.heights(2)), sum(s.heights(-1)));
    }
    // The same scenario with snowmen and the plough (testExtrasSummary in the Swift tests).
    {
        Sim s(42);
        s.setWorld({{0, 0, 1280, 800}});
        s.setExtras(true);
        SurfaceInput w{1, {200, 300, 700, 0}, {}, false, 10};
        SurfaceInput fl{-1, {0, 742, 1280, 0}, {}, true};
        s.setSurfaces({w, fl});
        for (auto& x : *s.heightsMutable(1)) x = 8;
        for (auto& x : *s.heightsMutable(-1)) x = 8;
        for (int i = 0; i < 30 * 50; ++i) s.step(1.f / 30);
        const int men = s.snowmanCount(), plough = s.ploughActive() ? 1 : 0;
        const float window = sum(s.heights(1)), floor = sum(s.heights(-1));
        w.rect.x = 260;
        s.setSurfaces({w, fl});
        s.step(1.f / 30);
        const int balls = s.snowballCount();
        // Roll a ball along the floor, then raise a mountain and let Santa flatten it.
        s.grab(100, 740);
        for (int k = 0; k < 100; ++k) s.drag(100 + k * 4.f, 739);
        const float radius = s.carrying() ? s.carriedRadius() : 0;
        s.drop(0, 0);
        auto& h = *s.heightsMutable(-1);
        for (int c = 0; c < 320; ++c) h[c] = (float)std::max(0, 260 - std::abs(c - 160) * 2);
        for (int i = 0; i < 30 * 40; ++i) s.step(1.f / 30);
        float mountain = 0;
        for (float x : *s.heights(-1)) mountain = std::max(mountain, x);
        std::printf("EXTRAS {\"snowmen\": %d, \"plough\": %d, \"balls\": %d, \"window\": %.1f, \"floor\": %.1f, \"radius\": %.2f, \"mountain\": %.1f}\n",
                    men, plough, balls, window, floor, radius, mountain);
    }
    std::printf("%d tests, %d failure(s)\n", tests, failures);
    return failures == 0 ? 0 : 1;
}
