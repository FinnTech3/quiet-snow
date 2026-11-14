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
        CHECK(huge.normalCount() == 3500);
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
        CHECK(s.surfaceCount() == 0);
        CHECK(s.debrisCount() > 20);
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
        s.setSurfaces({floorInput()}); run(s, 8);
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
        CHECK(n > 0); CHECK(n % 6 == 0); CHECK((int)v.size() == n * 4);
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

    std::printf("%d tests, %d failure(s)\n", tests, failures);
    return failures == 0 ? 0 : 1;
}
