// The snow model for Windows: a C++ port of Sources/QuietSnowCore/Sim.swift, kept in step with it by hand.
// Header only, standard library only, no Windows headers, so tests/tests.cpp builds anywhere.
// Coordinates are pixels, origin top-left of the virtual screen, y grows downwards.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace qs {

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    float maxX() const { return x + w; }
    float maxY() const { return y + h; }
    bool contains(float px, float py) const { return px >= x && px < maxX() && py >= y && py < maxY(); }
};

// A top edge snow can rest on. `radius` is the corner radius of the thing underneath (a window or the Dock), so the
// snow can follow the corner down. `plough` lets the snow plough drive along a floor.
struct SurfaceInput {
    int id = 0;
    Rect rect;
    std::vector<Rect> occluders;
    bool isFloor = false;
    float radius = 0;
    bool plough = true;
};

enum class Weather { Light, Steady, Flurry };

// What a flake looks like. The model only records which kind each flake is; the shader draws it.
enum class SnowType : uint8_t { Balls = 0, Flakes, Sleet, Sparkles, Hail, Mixed };

inline float density(Weather w) {
    switch (w) {
        case Weather::Light: return 190;
        case Weather::Steady: return 500;
        case Weather::Flurry: return 1400;
    }
    return 500;
}

struct Surface {
    Rect rect;
    bool isFloor = false;
    std::vector<float> heights;
    std::vector<char> visible;
    float spill = 0;
    float radius = 0;
    bool ploughable = false;
    float cap = 40;   // a floor can grow a mountain a third of its screen high
};

struct Pointer { float x = 0, y = 0; };

// Snowmen, snowballs and the snow plough: see Sources/QuietSnowCore/Extras.swift.
struct Snowman { int sid; float x, rx, ry, build, top; float size = 30; bool made = false; };
struct Ball {
    float x = 0, y = 0, vx = 0, vy = 0, r = 0, life = 0;
    bool hasOn = false; int on = 0; float rx = 0, ry = 0;
    int id = 0; bool placed = false;
    bool hasSupport = false; int support = 0;
};
struct Plough { int sid; float x, dir; bool backing; float rx, ry, vx = 0; };
struct Sleigh { int sid; float x, from, to, level; bool mountain; float y, pause = 0; };
struct ShowParticle { float x, y, vx, vy, size; uint8_t kind; float amount, delay, phase; bool landed = false; };
struct Spark { float x, y, vx, vy, life, maxLife, hue, size; bool rocket; };
struct Gem { int sid = 0; float x = 0, depth = 0, tag = 0, size = 0; bool sparkle = false; };

class Sim {
public:
    static constexpr float kColumnWidth = 4;
    static constexpr float kWindowCap = 40;
    static constexpr float kFloorCap = 30;   // the least a floor can hold; see makeSurface
    static constexpr int kDebrisCapacity = 600;
    static constexpr int kGrace = 2;   // polls a surface may miss before its snow is let go
    static constexpr float kSnowmanSize = 30;
    static constexpr float kPloughSize = 56;
    static constexpr float kSleighSize = 56;
    static constexpr float kReindeerSize = 40;
    static constexpr int kMaxPlaced = 12;
    static constexpr float kPinkSeconds = 15;

    float wind = 0;
    bool melts = false;

    explicit Sim(uint64_t seed = 0x9E3779B97F4A7C15ull) : rng_(seed == 0 ? 1 : seed) {}

    SnowType snowType() const { return type_; }
    void setSnowType(SnowType t) {
        if (t == type_) return;
        type_ = t;
        for (int i = 0; i < normalCount_; ++i) assignKind(i);
    }

    // Snowmen on window tops, the snowballs they break into, and a snow plough on the floor. Off by default.
    bool extras() const { return extras_; }
    void setExtras(bool on) {
        extras_ = on;
        if (!on) { snowmen_.clear(); balls_.clear(); hasPlough_ = false; hasSleigh_ = false; carrying_ = false; }
    }

    Weather weather() const { return weather_; }
    void setWeather(Weather w) { if (w != weather_) { weather_ = w; rebuildFlakes(); } }
    void setDensityScale(float s) { if (s != densityScale_) { densityScale_ = s; rebuildFlakes(); } }

    void setWorld(const std::vector<Rect>& screens) {
        if (screens.empty()) return;
        world_ = screens;
        float minX = screens[0].x, minY = screens[0].y, maxX = screens[0].maxX(), maxY = screens[0].maxY();
        for (const auto& s : screens) {
            minX = std::min(minX, s.x); minY = std::min(minY, s.y);
            maxX = std::max(maxX, s.maxX()); maxY = std::max(maxY, s.maxY());
        }
        box_ = Rect{minX, minY, maxX - minX, maxY - minY};
        rebuildFlakes();
    }

    // Windows that hide the snow behind them (desktop-only mode). Empty means the snow shows everywhere.
    void setOccluders(const std::vector<Rect>& r) { occluders_ = r; }

    // A window being dragged stirs the snow near it: flakes within 60 px of its edges are pushed the way it moves
    // (velocity in px per second).
    void stir(const Rect& r, float vx, float vy) {
        const float m = 60;
        for (size_t i = 0; i < xs_.size(); ++i) {
            if (state_[i] == 0 || xs_[i] < r.x - m || xs_[i] > r.maxX() + m || ys_[i] < r.y - m || ys_[i] > r.maxY() + m) continue;
            exs_[i] = clamp(exs_[i] + vx * 0.3f, 400);
            eys_[i] = clamp(eys_[i] + vy * 0.3f, 400);
        }
    }

    const std::vector<Rect>& world() const { return world_; }
    int normalCount() const { return normalCount_; }

    void setSurfaces(const std::vector<SurfaceInput>& inputs) {
        std::vector<int> seen;
        for (const auto& in : inputs) {
            if (in.rect.w < 8) continue;
            seen.push_back(in.id);
            auto it = surfaces_.find(in.id);
            if (it != surfaces_.end()) {
                Surface& old = *it->second;
                if (std::fabs(old.rect.x - in.rect.x) > 1.5f || std::fabs(old.rect.y - in.rect.y) > 1.5f ||
                    std::fabs(old.rect.w - in.rect.w) > 1.5f) {
                    release(old);
                    it->second = makeSurface(in);
                } else {
                    old.visible = visibility(in);
                }
            } else {
                surfaces_[in.id] = makeSurface(in);
            }
        }
        for (auto it = surfaces_.begin(); it != surfaces_.end();) {
            if (std::find(seen.begin(), seen.end(), it->first) != seen.end()) {
                missing_.erase(it->first);
                ++it;
            } else if (++missing_[it->first] <= kGrace) {
                ++it;   // a window can drop out of one poll (an animation, a cloak): keep its snow for a moment
            } else {
                missing_.erase(it->first);
                release(*it->second);
                sweepAcc_.erase(it->first);
                it = surfaces_.erase(it);
            }
        }
        list_.clear();
        for (auto& kv : surfaces_) list_.push_back(kv.second.get());   // std::map keeps ids sorted
    }

    void clearPiles() {
        for (Surface* s : list_) {
            release(*s);
            std::fill(s->heights.begin(), s->heights.end(), 0.f);
        }
    }

    void step(float rawDt, const Pointer* pointer = nullptr) {
        if (xs_.empty()) return;
        const float dt = std::min(std::max(rawDt, 0.f), 0.1f);
        clock_ += dt;
        if (showT_ >= 0) { stepShow(dt); return; }
        stepPink(dt);
        if (!sparks_.empty()) stepFireworks(dt, false);   // the last sparks fade after the show
        const float gust = wind * (1 + 0.3f * std::sin(clock_ * 0.45f));

        float pvx = 0, pvy = 0, speed = 0;
        if (pointer && hasLast_ && dt > 0) {
            pvx = (pointer->x - last_.x) / dt;
            pvy = (pointer->y - last_.y) / dt;
            speed = std::sqrt(pvx * pvx + pvy * pvy);
        }
        if (pointer) { last_ = *pointer; hasLast_ = true; } else { hasLast_ = false; }

        const float drag = std::exp(-3 * dt);
        const float radius = 70;
        const size_t count = xs_.size();
        for (size_t i = 0; i < count; ++i) {
            if (state_[i] == 0) continue;
            const float prevY = ys_[i];
            if (state_[i] == 2) {
                vys_[i] = std::min(vys_[i] + 420 * dt, 240.f);
                cvx_[i] = vxs_[i] + exs_[i];
                xs_[i] += cvx_[i] * dt;
                vxs_[i] *= std::exp(-1.2f * dt);
            } else {
                phases_[i] += dt * 0.9f;
                const float sway = std::sin(phases_[i]) * (6 + sizes_[i] * 2);
                cvx_[i] = gust * (0.55f + 0.1f * sizes_[i]) + sway + exs_[i];
                xs_[i] += cvx_[i] * dt;
            }
            cvy_[i] = vys_[i] + eys_[i];
            ys_[i] += cvy_[i] * dt;

            if (pointer && speed > 20) {
                const float dx = xs_[i] - pointer->x, dy = ys_[i] - pointer->y;
                const float d2 = dx * dx + dy * dy;
                if (d2 < radius * radius && d2 > 0.01f) {
                    const float d = std::sqrt(d2);
                    const float k = (1 - d / radius) * (120 + std::min(speed, 1500.f) * 0.25f);
                    exs_[i] = clamp(exs_[i] + dx / d * k * dt * 8, 400);
                    eys_[i] = clamp(eys_[i] + dy / d * k * dt * 8, 400);
                }
            }
            exs_[i] *= drag; eys_[i] *= drag;

            if (land(i, prevY)) continue;

            if (ys_[i] > box_.maxY() + 6 || xs_[i] < box_.x - 300 || xs_[i] > box_.maxX() + 300) {
                if (state_[i] == 2) state_[i] = 0; else respawn(i);
            }
        }
        settle(dt);
        if (pointer && speed > 120 && !carrying_) sweep(*pointer, pvx, pvy);
        if (extras_) stepExtras(dt, pointer, pvx, speed);
    }

    // Eight floats per flake: x, y, diameter, alpha, vx, vy, kind (a SnowType value, never Mixed), a per-flake angle.
    int fillFlakes(std::vector<float>& out) const {
        out.clear();
        if (showT_ >= 0) return fillShow(out) + fillExtras(out);
        int n = 0;
        for (size_t i = 0; i < xs_.size(); ++i) {
            if (state_[i] == 0 || hidden(xs_[i], ys_[i])) continue;
            const float s = sizes_[i];
            out.push_back(xs_[i]); out.push_back(ys_[i]); out.push_back(s * 1.6f + 1);
            out.push_back(s > 5 ? 0.3f : std::min(1.f, 0.45f + s * 0.14f));
            out.push_back(cvx_[i]); out.push_back(cvy_[i]); out.push_back((float)kinds_[i]); out.push_back(phases_[i]);
            ++n;
        }
        return n + fillExtras(out) + fillShow(out);
    }

    // Four floats per vertex: x, y, alpha, shade. Triangles, twelve vertices per column: the body, then a feather above
    // it so the top edge fades over a few pixels instead of cutting. The profile is smoothed over about 24 pixels, and
    // every end that is not at a screen edge is rounded.
    int fillPiles(std::vector<float>& out) {
        out.clear();
        int count = 0;
        auto v = [&](float x, float y, float a, float shade) {
            out.push_back(x); out.push_back(y); out.push_back(a); out.push_back(shade);
            ++count;
        };
        static const float k[6] = {1, 5, 10, 10, 5, 1};
        for (const Surface* s : list_) {
            const int n = (int)s->heights.size();
            if ((int)smoothed_.size() < n + 1) smoothed_.assign(n + 1, 0.f);
            auto h = [&](int i) {
                if (i < 0 || i >= n) return s->isFloor ? s->heights[std::max(0, std::min(n - 1, i))] : 0.f;
                return s->visible[i] ? s->heights[i] : 0.f;
            };
            for (int i = 0; i <= n; ++i) {
                float t = 0;
                for (int j = 0; j < 6; ++j) t += k[j] * h(i - 3 + j);
                smoothed_[i] = t / 32.f;
            }
            const float feather = 2.5f;
            const float top = 0.72f, bottom = 0.97f, deep = 0.7f;
            // An end of a pile that is not at the edge of a screen is rounded off: the snow follows the corner of the
            // window (or the Dock) down, and its top curves down to meet it. A floor runs on to the screen edge.
            const bool openL = !(s->isFloor && atScreenEdge(s->rect.x)), openR = !(s->isFloor && atScreenEdge(s->rect.maxX()));
            const float rad = s->radius;
            const float kNone = 3.0e38f;
            auto shape = [&](float x, float h, float& yTop, float& yBase) {
                float d = kNone;
                if (openL) d = std::min(d, x - s->rect.x);
                if (openR) d = std::min(d, s->rect.maxX() - x);
                float drop = 0;
                if (rad > 0 && d < rad) { const float k = rad - std::max(0.f, d); drop = rad - std::sqrt(std::max(0.f, rad * rad - k * k)); }
                const float len = std::min(26.f, std::max(4.f, h * 1.3f));
                float f = 1;
                if (d < len) { const float t = 1 - std::max(0.f, d) / len; f = std::sqrt(std::max(0.f, 1 - t * t)); }
                yTop = s->rect.y + drop - h * f;
                yBase = s->rect.y + drop + 0.5f;
            };
            for (int c = 0; c < n; ++c) {
                if (!s->visible[c]) continue;
                const float l = smoothed_[c], r = smoothed_[c + 1];
                if (l < 0.12f && r < 0.12f) continue;
                const float x0 = s->rect.x + c * kColumnWidth;
                const float x1 = std::min(s->rect.maxX(), x0 + kColumnWidth);
                if (hidden((x0 + x1) * 0.5f, s->rect.y - 1)) continue;
                // Near a rounded end, draw the column in 1-pixel slices so the curve is smooth, not faceted.
                const float toL = openL ? x1 - s->rect.x : kNone, toR = openR ? s->rect.maxX() - x0 : kNone;
                const int slices = std::min(toL, toR) < 30 ? 4 : 1;
                for (int j = 0; j < slices; ++j) {
                    const float fa = (float)j / slices, fb = (float)(j + 1) / slices;
                    const float xa = x0 + (x1 - x0) * fa, xb = x0 + (x1 - x0) * fb;
                    float ya, ba, yb, bb;
                    shape(xa, l + (r - l) * fa, ya, ba);
                    shape(xb, l + (r - l) * fb, yb, bb);
                    v(xa, ya, top, 0); v(xa, ba, bottom, deep); v(xb, yb, top, 0);
                    v(xb, yb, top, 0); v(xa, ba, bottom, deep); v(xb, bb, bottom, deep);
                    v(xa, ya - feather, 0, 0); v(xa, ya, top, 0); v(xb, yb - feather, 0, 0);
                    v(xb, yb - feather, 0, 0); v(xa, ya, top, 0); v(xb, yb, top, 0);
                }
            }
        }
        return count;
    }

    // Reading state, for tests and the status line.
    float totalSnowOnSurfaces() const {
        float t = 0;
        for (const Surface* s : list_) for (float h : s->heights) t += h;
        return t;
    }
    int liveFlakeCount() const { int n = 0; for (auto s : state_) n += s != 0; return n; }
    int debrisCount() const { int n = 0; for (size_t i = normalCount_; i < state_.size(); ++i) n += state_[i] == 2; return n; }
    int surfaceCount() const { return (int)surfaces_.size(); }
    int flakeCapacity() const { return (int)xs_.size(); }
    const std::vector<float>* heights(int id) const {
        auto it = surfaces_.find(id);
        return it == surfaces_.end() ? nullptr : &it->second->heights;
    }
    std::vector<float>* heightsMutable(int id) {
        auto it = surfaces_.find(id);
        return it == surfaces_.end() ? nullptr : &it->second->heights;
    }
    // True where a click should go to the snow rather than the window underneath: on a pile, a snowball, or anywhere
    // while one is being carried.
    bool snowAt(float x, float y) const {
        if (!extras_) return false;
        if (carrying_) return true;
        for (const auto& b : balls_) {
            const float dx = x - b.x, dy = y - b.y;
            if (b.placed && dx * dx + dy * dy < (b.r + 3) * (b.r + 3)) return true;
        }
        int c;
        return pileAt(x, y, c) != nullptr;
    }

    // The mouse went down at (x, y). Picks up a snowball there, or scoops a small one out of the pile. True if it did.
    bool grab(float x, float y) {
        if (!extras_ || carrying_) return false;
        for (int k = (int)balls_.size() - 1; k >= 0; --k) {
            Ball b = balls_[k];
            const float dx = x - b.x, dy = y - b.y;
            if (!(b.placed && dx * dx + dy * dy < (b.r + 3) * (b.r + 3))) continue;
            balls_.erase(balls_.begin() + k);
            unsupport(balls_, b.id);
            b.x = x; b.y = y; b.hasOn = false; b.hasSupport = false;
            carried_ = b; carrying_ = true;
            return true;
        }
        int c;
        Surface* s = pileAt(x, y, c);
        if (!s) return false;
        for (int k = std::max(0, c - 1); k <= std::min((int)s->heights.size() - 1, c + 1); ++k) s->heights[k] = std::max(0.f, s->heights[k] - 1.5f);
        Ball b;
        b.x = x; b.y = y; b.r = 5; b.id = ++nextBallId_; b.placed = true;
        carried_ = b; carrying_ = true;
        return true;
    }

    // The mouse moved with the button down. The ball follows it and picks up the snow it rolls through.
    void drag(float x, float y) {
        if (!carrying_) return;
        Ball& b = carried_;
        b.x = x; b.y = y;
        float taken = 0;
        for (Surface* s : list_) {
            if (!(x + b.r > s->rect.x && x - b.r < s->rect.maxX())) continue;
            const int lo = column(*s, x - b.r), hi = column(*s, x + b.r);
            for (int c = lo; c <= hi; ++c) {
                if (!s->visible[c] || !(s->heights[c] > 0.2f)) continue;
                const float top = s->rect.y - s->heights[c];
                if (!(y + b.r > top + 0.5f && y - b.r < s->rect.y)) continue;
                const float take = std::min(std::min(s->heights[c], (y + b.r - top) * 0.25f), 1.f);
                s->heights[c] -= take;
                taken += take;
            }
        }
        b.r = std::min(45.f, std::sqrt(b.r * b.r + taken * kColumnWidth / 3.14159265f));
    }

    // The mouse went up. The ball drops from there, thrown a little by how the mouse was moving.
    void drop(float vx, float vy) {
        if (!carrying_) return;
        carrying_ = false;
        Ball b = carried_;
        b.vx = std::max(-600.f, std::min(600.f, vx)); b.vy = std::max(-600.f, std::min(600.f, vy));
        int placed = 0;
        for (const auto& q : balls_) placed += q.placed;
        if (placed >= kMaxPlaced) {
            for (size_t k = 0; k < balls_.size(); ++k) {
                if (!balls_[k].placed) continue;
                const int old = balls_[k].id;
                balls_.erase(balls_.begin() + k);
                unsupport(balls_, old);
                break;
            }
        }
        balls_.push_back(b);
    }

    bool showActive() const { return showT_ >= 0; }
    float showTime() const { return showT_; }
    // 0 normally, 1 while the snow is pink, easing in and out over a second.
    float pinkAmount() const { return pinkLeft_ <= 0 ? 0 : std::max(0.f, std::min(std::min(1.f, pinkLeft_), kPinkSeconds - pinkLeft_)); }
    int gemCount() const { return (int)gems_.size(); }
    int sparkCount() const { return (int)sparks_.size(); }
    int rocketsLaunched() const { return rocketsLaunched_; }
    const std::vector<ShowParticle>& showParts() const { return showParts_; }
    const std::vector<float>& showTargets(int k) const { return showTargets_[k]; }

    // Start the show. Up to 60% of the dots come from snow in the air, the rest rise out of every pile and mountain
    // (which empties them); if that is still not enough, more falls in from above.
    void startShow() {
        if (world_.empty() || showT_ >= 0) return;
        const Rect d = world_[0];
        showTargets_ = {textPoints({"Merry Christmas", "Alice"}, d), textPoints({"My Favourite", "Ho Ho Ho"}, d)};
        const int want = std::min(2500, (int)std::max(showTargets_[0].size(), showTargets_[1].size()) / 2);
        showParts_.clear();
        const int fromAir = want * 6 / 10;
        for (size_t i = 0; i < xs_.size(); ++i) {
            if (state_[i] == 0 || (int)showParts_.size() >= fromAir) continue;
            ShowParticle p{xs_[i], ys_[i], cvx_[i], cvy_[i], std::min(sizes_[i], 4.2f), kinds_[i], state_[i] == 2 ? 1.f : 0.3f * sizes_[i], 0, 0};
            p.delay = rand(0, 1.2f);
            p.phase = rand(0, 6.28f);
            showParts_.push_back(p);
        }
        std::fill(state_.begin(), state_.end(), 0);
        float total = 0;
        for (Surface* s : list_) for (size_t c = 0; c < s->heights.size(); ++c) if (s->visible[c]) total += s->heights[c];
        const int quota = want - (int)showParts_.size();
        if (quota > 0 && total > 0) {
            const float per = std::max(0.05f, total / quota);
            float acc = 0;
            for (Surface* s : list_) {
                for (size_t c = 0; c < s->heights.size(); ++c) {
                    if (!s->visible[c]) continue;
                    acc += s->heights[c];
                    while (acc >= per && (int)showParts_.size() < want) {
                        acc -= per;
                        const float take = std::min(s->heights[c], per);
                        s->heights[c] -= take;
                        ShowParticle p{};
                        p.x = s->rect.x + (c + 0.5f) * kColumnWidth + rand(-2, 2);
                        p.y = s->rect.y - s->heights[c] - rand(0, 2);
                        p.vx = rand(-20, 20);
                        p.vy = -rand(30, 90);
                        p.size = rand(1.6f, 3.4f);
                        p.kind = showKind();
                        p.amount = take;
                        p.delay = rand(0.3f, 2.0f);
                        p.phase = rand(0, 6.28f);
                        showParts_.push_back(p);
                    }
                }
            }
        }
        // Whatever the even shares left behind goes with the snow that rose, so the piles are really empty.
        const int firstPile = std::min((int)showParts_.size(), fromAir);
        if ((int)showParts_.size() > firstPile) {
            float rest = 0;
            for (Surface* s : list_) for (size_t c = 0; c < s->heights.size(); ++c) if (s->visible[c]) { rest += s->heights[c]; s->heights[c] = 0; }
            const float each = rest / (float)(showParts_.size() - firstPile);
            for (size_t i = firstPile; i < showParts_.size(); ++i) showParts_[i].amount += each;
        }
        while ((int)showParts_.size() < want) {
            ShowParticle p{};
            p.x = rand(d.x, d.maxX());
            p.y = d.y - rand(10, 200);
            p.vy = rand(30, 60);
            p.size = rand(1.4f, 3.2f);
            p.kind = showKind();
            p.delay = rand(0, 2.5f);
            p.phase = rand(0, 6.28f);
            showParts_.push_back(p);
        }
        showT_ = 0;
        fireworkClock_ = 0;
    }

    int snowmanCount() const { return (int)snowmen_.size(); }
    bool sleighActive() const { return hasSleigh_; }
    float sleighX() const { return sleigh_.x; }
    bool carrying() const { return carrying_; }
    float carriedRadius() const { return carried_.r; }
    float cap(int id) const { auto it = surfaces_.find(id); return it == surfaces_.end() ? 0 : it->second->cap; }
    int snowballCount() const { return (int)balls_.size(); }
    bool ploughActive() const { return hasPlough_; }
    float ploughX() const { return plough_.x; }

    // Test hooks.
    std::vector<float>& sizes() { return sizes_; }
    std::vector<float>& xs() { return xs_; }
    std::vector<float>& ys() { return ys_; }
    bool landForTest(int i, float prevY) { return land((size_t)i, prevY); }
    const std::vector<float>& cvyForTest() const { return cvy_; }

private:
    std::vector<float> xs_, ys_, vys_, sizes_, phases_, exs_, eys_, vxs_, cvx_, cvy_;
    std::vector<uint8_t> state_, kinds_;
    SnowType type_ = SnowType::Balls;
    int normalCount_ = 0;
    int debrisCursor_ = 0;
    std::map<int, std::unique_ptr<Surface>> surfaces_;
    std::vector<Surface*> list_;
    std::vector<Rect> world_;
    Rect box_{0, 0, 1, 1};
    Weather weather_ = Weather::Steady;
    float densityScale_ = 1;
    float clock_ = 0;
    uint64_t rng_;
    Pointer last_;
    bool hasLast_ = false;
    std::map<int, float> sweepAcc_;
    std::vector<float> smoothed_;
    std::vector<Rect> occluders_;
    std::map<int, int> missing_;
    bool extras_ = false;
    std::vector<Snowman> snowmen_;
    std::vector<Ball> balls_;
    Plough plough_{};
    bool hasPlough_ = false;
    float snowmanClock_ = 0;
    float ploughWait_ = 20;
    bool carrying_ = false;
    Ball carried_;
    int nextBallId_ = 0;
    Sleigh sleigh_{};
    bool hasSleigh_ = false;
    float sleighWait_ = 20;
    float sleighCheck_ = 0;
    float showT_ = -1;
    std::vector<ShowParticle> showParts_;
    std::vector<std::vector<float>> showTargets_;
    float pinkLeft_ = 0;
    std::vector<Gem> gems_;
    std::vector<Spark> sparks_;
    float fireworkClock_ = 0;
    int rocketsLaunched_ = 0;

    static float clamp(float v, float m) { return std::max(-m, std::min(m, v)); }

    float rand() {
        rng_ ^= rng_ << 13; rng_ ^= rng_ >> 7; rng_ ^= rng_ << 17;
        return (float)(rng_ >> 40) / (float)(1 << 24);
    }
    float rand(float a, float b) { return a + (b - a) * rand(); }

    int targetCount() const {
        float area = 0;
        for (const auto& s : world_) area += s.w * s.h;
        const int n = (int)(density(weather_) * densityScale_ * area / 1000000.f);
        return std::max(60, std::min(n, 5000));
    }

    bool hidden(float x, float y) const {
        for (const auto& o : occluders_) if (o.contains(x, y)) return true;
        return false;
    }

    void rebuildFlakes() {
        if (world_.empty()) return;
        normalCount_ = targetCount();
        const size_t total = (size_t)normalCount_ + kDebrisCapacity;
        for (auto* v : {&xs_, &ys_, &vys_, &sizes_, &phases_, &exs_, &eys_, &vxs_, &cvx_, &cvy_}) v->assign(total, 0.f);
        state_.assign(total, 0);
        kinds_.assign(total, 0);
        float area = 0;
        for (const auto& r : world_) area += r.w * r.h;
        for (int i = 0; i < normalCount_; ++i) {
            respawn(i);
            float pick = rand(0, area);
            const Rect* on = &world_.back();
            for (const auto& r : world_) { if (pick < r.w * r.h) { on = &r; break; } pick -= r.w * r.h; }
            xs_[i] = rand(on->x, on->maxX());   // start spread over the screens, not over the gaps between them
            ys_[i] = rand(on->y, on->maxY());
            state_[i] = 1;
        }
        debrisCursor_ = normalCount_;
    }

    void respawn(size_t i) {
        const float size = rand() < 0.05f ? rand(5.5f, 8.5f) : rand(1.4f, 4.2f);
        sizes_[i] = size;
        vys_[i] = 22 + 14 * size;
        // Snow comes in over the top of each screen and, with wind, over the upwind edge, in proportion to what crosses
        // each, so every part of every screen gets the same density whatever the size, layout or wind.
        const float vx = wind * (0.55f + 0.1f * size);
        float topW = 0, edgeH = 0;
        for (const auto& r : world_) {
            topW += r.w;
            if (vx > 0 ? r.x == box_.x : r.maxX() == box_.maxX()) edgeH += r.h;
        }
        if (edgeH > 0 && vx != 0 && rand() * (topW * vys_[i] + edgeH * std::fabs(vx)) < edgeH * std::fabs(vx)) {
            float pick = rand(0, edgeH);
            const Rect* on = &world_[0];
            for (const auto& r : world_) {
                if (!(vx > 0 ? r.x == box_.x : r.maxX() == box_.maxX())) continue;
                on = &r;
                if (pick < r.h) break;
                pick -= r.h;
            }
            xs_[i] = vx > 0 ? on->x - rand(2, 80) : on->maxX() + rand(2, 80);
            ys_[i] = rand(on->y, on->maxY());
        } else {
            float pick = rand(0, topW);
            const Rect* on = &world_[0];
            for (const auto& r : world_) { on = &r; if (pick < r.w) break; pick -= r.w; }
            xs_[i] = rand(on->x, on->maxX());
            ys_[i] = on->y - rand(2, 80);
        }
        phases_[i] = rand(0, 6.28f);
        exs_[i] = 0; eys_[i] = 0;
        state_[i] = 1;
        assignKind(i);
    }

    // The big soft foreground flakes are always balls: they are there for depth. Sleet and hail fall faster.
    void assignKind(size_t i) {
        uint8_t k = (uint8_t)type_;
        if (sizes_[i] > 5) {
            k = 0;
        } else if (type_ == SnowType::Mixed) {
            const float r = rand();
            k = r < 0.30f ? 0 : r < 0.52f ? 1 : r < 0.67f ? 2 : r < 0.82f ? 3 : 4;
        }
        kinds_[i] = k;
        vys_[i] = (22 + 14 * sizes_[i]) * (k == 2 ? 1.5f : k == 4 ? 1.25f : 1.f);
    }

    bool atScreenEdge(float x) const {
        for (const auto& w : world_) if (std::fabs(w.x - x) < 2 || std::fabs(w.maxX() - x) < 2) return true;
        return false;
    }

    static int column(const Surface& s, float x) {
        return std::max(0, std::min((int)s.heights.size() - 1, (int)((x - s.rect.x) / kColumnWidth)));
    }

    static bool moved(const Surface& s, float rx, float ry) { return std::fabs(s.rect.x - rx) > 1.5f || std::fabs(s.rect.y - ry) > 1.5f; }

    Surface* find(int id) const {
        auto it = surfaces_.find(id);
        return it == surfaces_.end() ? nullptr : it->second.get();
    }

    // The display a surface belongs to.
    Rect display(const Surface& s) const {
        const float mx = s.rect.x + s.rect.w / 2;
        for (const auto& w : world_) if (w.x <= mx && mx < w.maxX() && w.y < s.rect.y && s.rect.y <= w.maxY() + 1) return w;
        return world_.empty() ? Rect{} : world_[0];
    }

    static float maxHeight(const Surface& s) { float m = 0; for (float h : s.heights) m = std::max(m, h); return m; }

    Ball* ballById(int id) { for (auto& b : balls_) if (b.id == id) return &b; return nullptr; }
    static bool resting(const Ball& b) { return b.hasOn || b.hasSupport; }

    void stepExtras(float dt, const Pointer* pointer, float pvx, float speed) {
        const Pointer* p = carrying_ ? nullptr : pointer;   // while you carry a snowball the cursor knocks nothing over
        stepSnowmen(dt, p, pvx, speed);
        stepBalls(dt, p, pvx, speed);
        buildStacks();
        snowmanClock_ += dt;
        if (snowmanClock_ >= 2.5f) { snowmanClock_ -= 2.5f; trySpawnSnowman(); }
        stepSleigh(dt);
        stepPlough(dt);
    }

    // A snowman builds itself where a window has a deep, even stretch of snow away from its ends. At most three at once.
    void trySpawnSnowman() {
        int wild = 0;
        for (const auto& m : snowmen_) wild += !m.made;
        if (wild >= 3) return;
        const int span = 10, margin = 8;
        for (auto& kv : surfaces_) {
            const int id = kv.first;
            Surface& s = *kv.second;
            if (s.isFloor || s.rect.w < 140) continue;
            bool taken = false;
            for (const auto& m : snowmen_) if (m.sid == id) taken = true;
            if (taken) continue;
            const int n = (int)s.heights.size();
            if (n < span + 2 * margin) continue;
            int best = -1;
            float bestMin = 0;
            for (int start = margin; start <= n - margin - span; ++start) {
                float lo = 3.0e38f;
                for (int c = start; c < start + span; ++c) lo = std::min(lo, s.visible[c] ? s.heights[c] : 0.f);
                if (lo > bestMin) { bestMin = lo; best = start; }
            }
            if (best >= 0 && bestMin >= 4 && rand() < 0.4f) {
                const float x = s.rect.x + (best + span / 2.f) * kColumnWidth;
                snowmen_.push_back(Snowman{id, x, s.rect.x, s.rect.y, 0, s.rect.y - bestMin});
                return;
            }
        }
    }

    void stepSnowmen(float dt, const Pointer* pointer, float pvx, float speed) {
        for (size_t i = 0; i < snowmen_.size();) {
            Snowman m = snowmen_[i];
            bool fall = false;
            float push = 0;
            Surface* s = find(m.sid);
            if (s && !moved(*s, m.rx, m.ry)) {
                const int c = column(*s, m.x);
                m.top = s->rect.y - s->heights[c];
                if (!m.made && s->heights[c] < 1.5f) fall = true;   // the snow under it was swept or melted away
                m.build = std::min(1.f, m.build + dt / 2.5f);
                if (pointer && speed > 120 && std::fabs(pointer->x - m.x) < m.size * 0.45f && pointer->y < m.top + 2 &&
                    pointer->y > m.top - m.size - 4) {
                    fall = true;
                    push = pvx;
                }
            } else {
                fall = true;                             // its window moved or closed
            }
            if (fall) {
                breakSnowman(m, push);
                snowmen_.erase(snowmen_.begin() + i);
                continue;
            }
            snowmen_[i] = m;
            ++i;
        }
    }

    // The balls it was built from tumble off, with a puff of loose snow.
    void breakSnowman(const Snowman& m, float push) {
        const float half = m.size / 2;
        const float cy = m.top - half + 1;
        const float parts[3][3] = {{0.55f, 0.42f, 0}, {0.0f, 0.31f, 0.33f}, {-0.42f, 0.22f, 0.6f}};
        for (const auto& p : parts) {
            if (!(m.build > p[2])) continue;
            if (balls_.size() >= 24) break;
            Ball b;
            b.x = m.x; b.y = cy + p[0] * half; b.r = p[1] * half;
            b.vx = push * 0.3f + rand(-40, 40);
            b.vy = -rand(20, 70);
            b.life = rand(5, 8);
            balls_.push_back(b);
        }
        for (int k = 0; k < 6; ++k) {
            const float x = m.x + rand(-6, 6);
            const float y = cy + rand(-8, 8);
            const float vx = push * 0.2f + rand(-50, 50);
            const float vy = -rand(20, 90);
            spawnDebris(x, y, vx, vy);
        }
    }

    // Three balls resting on top of each other become a snowman, sized by the bottom one.
    void buildStacks() {
        for (const Ball& c : balls_) {
            if (!c.hasSupport) continue;
            const Ball* b = ballById(c.support);
            if (!b || !b->hasSupport) continue;
            const Ball* a = ballById(b->support);
            if (!a || !a->hasOn) continue;
            const Surface* s = find(a->on);
            if (!s) continue;
            Snowman m{a->on, a->x, s->rect.x, s->rect.y, 0.6f, a->y + a->r};
            m.size = std::min(140.f, std::max(24.f, a->r * 2 / 0.42f));
            m.made = true;
            snowmen_.push_back(m);
            const int ia = a->id, ib = b->id, ic = c.id;
            balls_.erase(std::remove_if(balls_.begin(), balls_.end(), [&](const Ball& q) { return q.id == ia || q.id == ib || q.id == ic; }), balls_.end());
            return;                                      // one per step is plenty
        }
    }

    void stepBalls(float dt, const Pointer* pointer, float pvx, float speed) {
        for (size_t i = 0; i < balls_.size();) {
            Ball b = balls_[i];
            if (pointer && speed > 120) {
                const float dx = b.x - pointer->x, dy = b.y - pointer->y;
                if (dx * dx + dy * dy < (b.r + 12) * (b.r + 12)) {
                    b.vx += pvx * 0.4f;
                    b.vy = std::min(b.vy, 0.f) - 120;
                    b.hasOn = false;
                    b.hasSupport = false;
                }
            }
            Surface* on = b.hasOn ? find(b.on) : nullptr;
            if (b.hasSupport) {
                // Sitting on another ball: follow it, or fall if it has gone or is falling itself.
                const Ball* a = ballById(b.support);
                if (a && resting(*a)) {
                    b.x = a->x;
                    b.y = a->y - (a->r + b.r) * 0.92f;
                } else {
                    b.hasSupport = false;
                }
            } else if (on && !moved(*on, b.rx, b.ry)) {
                b.vx *= std::exp(-1.8f * dt);
                b.x += b.vx * dt;
                if (b.x < on->rect.x || b.x >= on->rect.maxX()) b.hasOn = false;   // rolled off the end
                else b.y = on->rect.y - on->heights[column(*on, b.x)] - b.r;
            } else {
                b.hasOn = false;
                const float prevY = b.y;
                b.vy = std::min(b.vy + 520 * dt, 420.f);
                b.x += b.vx * dt;
                b.y += b.vy * dt;
                b.vx *= std::exp(-0.6f * dt);
                // Land on a resting ball: stack on it if it hits near the top, roll off if not.
                for (const Ball& a : balls_) {
                    if (a.id == b.id || !resting(a)) continue;
                    const float dx = b.x - a.x;
                    if (!(std::fabs(dx) < a.r + b.r && prevY + b.r <= a.y - a.r * 0.5f && b.y + b.r >= a.y - a.r * 0.9f)) continue;
                    if (std::fabs(dx) < a.r * 0.9f) {
                        b.hasSupport = true; b.support = a.id;
                        b.vx = 0; b.vy = 0;
                        b.x = a.x;
                        b.y = a.y - (a.r + b.r) * 0.92f;
                    } else {
                        b.vx += (dx < 0 ? -1 : 1) * 60;
                    }
                    break;
                }
                if (!b.hasSupport) {
                    for (auto& kv : surfaces_) {
                        Surface& s = *kv.second;
                        if (!(b.x >= s.rect.x && b.x < s.rect.maxX())) continue;
                        const int c = column(s, b.x);
                        if (!s.visible[c]) continue;
                        const float top = s.rect.y - s.heights[c];
                        // Its middle was above the edge and it now touches the snow (dropped inside a pile, it pops up).
                        if (!(prevY <= s.rect.y && b.y + b.r >= top)) continue;
                        b.y = top - b.r;
                        if (b.vy > 90) {
                            b.vy = -b.vy * 0.35f;
                            b.vx *= 0.8f;
                        } else {
                            b.vy = 0;
                            b.hasOn = true; b.on = kv.first; b.rx = s.rect.x; b.ry = s.rect.y;
                        }
                        break;
                    }
                }
            }
            if (!b.placed) b.life -= dt;
            bool gone = b.y > box_.maxY() + 20 || b.x < box_.x - 300 || b.x > box_.maxX() + 300;
            if (!gone && !b.placed && b.life <= 0) {
                // It melts back into the pile it is sitting on.
                if (Surface* s = b.hasOn ? find(b.on) : nullptr) {
                    const int c = column(*s, b.x);
                    const int lo = std::max(0, c - 2), hi = std::min((int)s->heights.size() - 1, c + 2);
                    const float add = b.r * 1.2f / (hi - lo + 1);
                    for (int k = lo; k <= hi; ++k) s->heights[k] = std::min(s->cap, s->heights[k] + add);
                }
                gone = true;
            }
            if (gone) { balls_.erase(balls_.begin() + i); continue; }
            balls_[i] = b;
            ++i;
        }
    }

    Surface* pileAt(float x, float y, int& col) const {
        for (Surface* s : list_) {
            if (!(x >= s->rect.x && x < s->rect.maxX())) continue;
            const int c = column(*s, x);
            const float h = s->heights[c];
            if (s->visible[c] && h >= 2.5f && y >= s->rect.y - h - 4 && y <= s->rect.y + 2 && !hidden(x, s->rect.y - 1)) { col = c; return s; }
        }
        return nullptr;
    }

    static void unsupport(std::vector<Ball>& balls, int id) { for (auto& q : balls) if (q.hasSupport && q.support == id) q.hasSupport = false; }

    // Santa flies when snow gets deep. A floor with a mountain a third of the screen high is flattened a layer at a time,
    // pass after pass, left to right, until it is low enough for the plough. Otherwise, now and then, he clears the
    // deepest window.
    void stepSleigh(float dt) {
        if (!hasSleigh_) {
            sleighWait_ -= dt;
            sleighCheck_ += dt;
            if (sleighCheck_ < 1) return;
            sleighCheck_ = 0;
            for (auto& kv : surfaces_) {
                Surface& s = *kv.second;
                if (!s.isFloor) continue;
                const float m = maxHeight(s);
                if (m >= s.cap * 0.9f) { startSleigh(kv.first, s, true, std::max(20.f, m - 40)); return; }
            }
            if (sleighWait_ > 0) return;
            int pickId = 0;
            Surface* pick = nullptr;
            float deepest = 10;
            for (auto& kv : surfaces_) {
                Surface& s = *kv.second;
                if (s.isFloor) continue;
                float total = 0;
                for (float h : s.heights) total += h;
                const float mean = total / s.heights.size();
                if (mean >= deepest) { deepest = mean; pick = &s; pickId = kv.first; }
            }
            if (!pick) return;
            sleighWait_ = rand(45, 90);
            startSleigh(pickId, *pick, false, 1);
            return;
        }
        Sleigh& g = sleigh_;
        if (g.pause > 0) { g.pause -= dt; return; }
        const float oldX = g.x;
        g.x += 300 * dt;
        if (Surface* s = find(g.sid)) {
            g.y = s->rect.y - g.level;
            float removed = 0;
            const int lo = column(*s, std::min(oldX, g.x) - 24), hi = column(*s, g.x + 24);
            if (g.x + 24 >= s->rect.x && g.x - 24 < s->rect.maxX()) {
                for (int c = lo; c <= hi; ++c) {
                    if (!(s->heights[c] > g.level)) continue;
                    removed += s->heights[c] - g.level;
                    s->heights[c] = g.level;
                }
            }
            float thrown = removed * 0.03f;              // a spray behind it; the rest is carried away
            while (thrown >= 1) {
                thrown -= 1;
                const float x = g.x + rand(-20, 20);
                const float y = g.y - rand(0, 10);
                const float vx = rand(80, 220);
                const float vy = -rand(40, 120);
                spawnDebris(x, y, vx, vy);
            }
        }
        if (g.x > g.to) {
            Surface* s = find(g.sid);
            if (g.mountain && s && maxHeight(*s) > 40) {
                g.level = std::max(20.f, maxHeight(*s) - 40);   // round again, a layer lower
                g.x = g.from;
                g.pause = 1;
            } else {
                hasSleigh_ = false;
                if (g.mountain) ploughWait_ = std::min(ploughWait_, 3.f);   // now the plough can do the rest
            }
        }
    }

    void startSleigh(int id, const Surface& s, bool mountain, float level) {
        const Rect d = display(s);
        sleigh_ = Sleigh{id, d.x - 220, d.x - 220, d.maxX() + 60, level, mountain, s.rect.y - level, 0};
        hasSleigh_ = true;
    }

    // Now and then a plough drives along the floor with the most snow, from a screen edge, pushing the snow into a heap
    // ahead of its blade. At a screen edge the heap goes off the screen with it; against the Dock it leaves the heap
    // there and reverses out. It waits while a mountain is being flattened.
    void stepPlough(float dt) {
        if (!hasPlough_) {
            ploughWait_ -= dt;
            if (ploughWait_ > 0 || hasSleigh_) return;
            ploughWait_ = rand(35, 70);
            int pickId = 0;
            Surface* pick = nullptr;
            float most = 0;
            for (auto& kv : surfaces_) {
                Surface& s = *kv.second;
                if (!s.ploughable || maxHeight(s) > 60) continue;
                float total = 0;
                for (float h : s.heights) total += h;
                if (total / s.heights.size() >= 2 && total > most) { most = total; pick = &s; pickId = kv.first; }
            }
            if (!pick) return;
            const bool left = atScreenEdge(pick->rect.x), right = atScreenEdge(pick->rect.maxX());
            if (!left && !right) return;
            const float dir = left && (!right || rand() < 0.5f) ? 1.f : -1.f;
            plough_ = Plough{pickId, dir > 0 ? pick->rect.x - 24 : pick->rect.maxX() + 24, dir, false, pick->rect.x, pick->rect.y, 0};
            hasPlough_ = true;
            return;
        }
        Plough& p = plough_;
        Surface* sp = find(p.sid);
        if (!sp || moved(*sp, p.rx, p.ry)) { hasPlough_ = false; return; }
        Surface& s = *sp;
        const int n = (int)s.heights.size();
        const float speed = p.backing ? 80.f : 55.f;
        const float oldFront = p.x + p.dir * 20;
        p.vx = (p.backing ? -p.dir : p.dir) * speed;
        p.x += p.vx * dt;
        if (p.backing) {
            if ((p.dir > 0 && p.x < s.rect.x - 30) || (p.dir < 0 && p.x > s.rect.maxX() + 30)) hasPlough_ = false;
            return;
        }
        const float front = p.x + p.dir * 20;
        const float end = p.dir > 0 ? s.rect.maxX() : s.rect.x;
        const bool past = p.dir > 0 ? front >= end : front <= end;
        // Scrape the columns the blade passed this step down to a thin layer.
        float carry = 0;
        const int a = (int)std::floor((std::min(oldFront, front) - s.rect.x) / kColumnWidth);
        const int b = (int)std::floor((std::max(oldFront, front) - s.rect.x) / kColumnWidth);
        if (b >= 0 && a < n) {
            for (int c = std::max(0, a); c <= std::min(n - 1, b); ++c) {
                if (!(s.heights[c] > 0.4f)) continue;
                carry += s.heights[c] - 0.4f;
                s.heights[c] = 0.4f;
            }
        }
        if (past) {
            if (atScreenEdge(end)) {
                // Pushed off the screen. Drive on until the plough is gone too.
                if ((p.dir > 0 && p.x > end + 30) || (p.dir < 0 && p.x < end - 30)) hasPlough_ = false;
            } else {
                // Leave the heap against the Dock and back out.
                deposit(s, p.dir > 0 ? n - 1 : 0, -p.dir, carry, p.x, false);
                p.backing = true;
            }
        } else {
            deposit(s, column(s, front) + (int)p.dir, p.dir, carry, front, atScreenEdge(end) || std::fabs(end - front) > 60);
        }
    }

    // Pile `amount` into the four columns from `from` in direction `dir`, most in the first, no higher than the blade can
    // push (48). Past the end of a floor that stops short of the screen edge (at the Dock) the snow piles against the
    // end; past a screen edge it is gone. What will not fit is pushed aside, off the road: a quarter of it is thrown over
    // the blade as loose snow (not at the Dock, where it would land on the Dock), the rest is gone.
    void deposit(Surface& s, int from, float dir, float amount, float x, bool throwing = true) {
        if (!(amount > 0)) return;
        static const float weights[4] = {0.4f, 0.3f, 0.2f, 0.1f};
        const int n = (int)s.heights.size();
        const float limit = std::min(s.cap, 48.f);
        float left = amount;
        for (int k = 0; k < 4; ++k) {
            int c = from + (int)dir * k;
            if (c < 0 || c >= n) {
                if (atScreenEdge(c < 0 ? s.rect.x : s.rect.maxX())) continue;
                c = std::max(0, std::min(n - 1, c));
            }
            const float add = std::min(limit - s.heights[c], amount * weights[k]);
            if (add > 0) { s.heights[c] += add; left -= add; }
        }
        float thrown = throwing ? left * 0.25f : 0.f;
        while (thrown >= 1) {
            thrown -= 1;
            const float dx = x + rand(-4, 4);
            const float dy = s.rect.y - rand(10, 24);
            const float vx = dir * rand(20, 70);
            const float vy = -rand(60, 140);
            spawnDebris(dx, dy, vx, vy);
        }
    }

    // Snowmen (kind 5: build progress in the angle slot), the plough (kind 6: direction), snowballs (kind 7), the sleigh
    // (kind 8) and its reindeer (kind 9: their place in the team, plus 10 for Rudolph at the front), in the same
    // eight-float layout as flakes.
    int fillExtras(std::vector<float>& out) const {
        if (!extras_) return 0;
        int n = 0;
        auto put = [&](float x, float y, float d, float vx, float vy, float kind, float ang) {
            out.push_back(x); out.push_back(y); out.push_back(d); out.push_back(1);
            out.push_back(vx); out.push_back(vy); out.push_back(kind); out.push_back(ang);
            ++n;
        };
        for (const auto& m : snowmen_) {
            const Surface* s = find(m.sid);
            if (!s || !s->visible[column(*s, m.x)]) continue;
            const float y = s->rect.y - s->heights[column(*s, m.x)] - m.size / 2 + 1;   // on the snow as it is now
            if (!hidden(m.x, y)) put(m.x, y, m.size, 0, 0, 5, m.build);
        }
        for (const auto& b : balls_) if (!hidden(b.x, b.y)) put(b.x, b.y, b.r * 2, b.vx, resting(b) ? 0 : b.vy, 7, 0);
        if (carrying_) put(carried_.x, carried_.y, carried_.r * 2, 0, 0, 7, 0);
        if (hasPlough_) {
            if (const Surface* s = find(plough_.sid)) {
                const float y = s->rect.y - s->heights[column(*s, plough_.x)] - kPloughSize / 2 + 2;
                if (!hidden(plough_.x, y)) put(plough_.x, y, kPloughSize, plough_.vx, 0, 6, plough_.dir);
            }
        }
        if (hasSleigh_ && sleigh_.pause <= 0) {
            const float bob = std::sin(clock_ * 3) * 3;
            const float y = sleigh_.y - kSleighSize * 0.3f + bob;
            put(sleigh_.x, y, kSleighSize, 300, 0, 8, 0);
            for (int k = 0; k < 4; ++k) {
                const float ry = y - 8 - k * 3 + std::sin(clock_ * 3 + k) * 2;
                put(sleigh_.x + 52 + k * 36, ry, kReindeerSize, 300, 0, 9, (float)k + (k == 3 ? 10 : 0));
            }
        }
        return n;
    }

    // ---------------------------------------------------------------- the hidden show (Sources/QuietSnowCore/Show.swift)

    // A 5 by 7 pixel font, just the letters the messages need.
    static const char* const* glyph(char ch) {
        static const char* M[] = {"#...#", "##.##", "#.#.#", "#.#.#", "#...#", "#...#", "#...#"};
        static const char* C[] = {".###.", "#...#", "#....", "#....", "#....", "#...#", ".###."};
        static const char* A[] = {".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"};
        static const char* F[] = {"#####", "#....", "#....", "####.", "#....", "#....", "#...."};
        static const char* H[] = {"#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"};
        static const char* e[] = {".....", ".....", ".###.", "#...#", "#####", "#....", ".###."};
        static const char* r[] = {".....", ".....", "#.##.", "##..#", "#....", "#....", "#...."};
        static const char* y[] = {".....", ".....", "#...#", "#...#", ".####", "....#", ".###."};
        static const char* h[] = {"#....", "#....", "#.##.", "##..#", "#...#", "#...#", "#...#"};
        static const char* i[] = {"..#..", ".....", ".##..", "..#..", "..#..", "..#..", ".###."};
        static const char* s[] = {".....", ".....", ".####", "#....", ".###.", "....#", "####."};
        static const char* t[] = {".#...", ".#...", "####.", ".#...", ".#...", ".#..#", "..##."};
        static const char* m[] = {".....", ".....", "##.#.", "#.#.#", "#.#.#", "#.#.#", "#.#.#"};
        static const char* a[] = {".....", ".....", ".###.", "....#", ".####", "#...#", ".####"};
        static const char* l[] = {".##..", "..#..", "..#..", "..#..", "..#..", "..#..", ".###."};
        static const char* c[] = {".....", ".....", ".###.", "#....", "#....", "#...#", ".###."};
        static const char* v[] = {".....", ".....", "#...#", "#...#", "#...#", ".#.#.", "..#.."};
        static const char* o[] = {".....", ".....", ".###.", "#...#", "#...#", "#...#", ".###."};
        static const char* u[] = {".....", ".....", "#...#", "#...#", "#...#", "#..##", ".##.#"};
        switch (ch) {
            case 'M': return M; case 'C': return C; case 'A': return A; case 'F': return F; case 'H': return H;
            case 'e': return e; case 'r': return r; case 'y': return y; case 'h': return h; case 'i': return i;
            case 's': return s; case 't': return t; case 'm': return m; case 'a': return a; case 'l': return l;
            case 'c': return c; case 'v': return v; case 'o': return o; case 'u': return u;
        }
        return nullptr;
    }

    // Points that spell `lines`, centred in the upper part of a display, a few points apart.
    static std::vector<float> textPoints(const std::vector<std::string>& lines, const Rect& d) {
        size_t widestN = 1;
        for (const auto& l : lines) widestN = std::max(widestN, l.size());
        const float widest = (float)widestN;
        const float px = std::min(d.w * 0.84f / (widest * 6), d.h * 0.46f / (float)(lines.size() * 9));
        const int sub = std::max(1, (int)(px / 5));
        const float step = px / sub;
        const float top = d.y + d.h * 0.36f - (float)(lines.size() * 9) * px / 2;
        std::vector<float> out;
        for (size_t row = 0; row < lines.size(); ++row) {
            const std::string& line = lines[row];
            const float left = d.x + (d.w - (float)(line.size() * 6 - 1) * px) / 2;
            for (size_t col = 0; col < line.size(); ++col) {
                const char* const* g = glyph(line[col]);
                if (!g) continue;
                for (int gy = 0; gy < 7; ++gy) {
                    for (int gx = 0; gx < 5; ++gx) {
                        if (g[gy][gx] != '#') continue;
                        for (int sy = 0; sy < sub; ++sy)
                            for (int sx = 0; sx < sub; ++sx) {
                                out.push_back(left + (float)(col * 6 + gx) * px + (sx + 0.5f) * step);
                                out.push_back(top + (float)(row * 9 + gy) * px + (sy + 0.5f) * step);
                            }
                    }
                }
            }
        }
        return out;
    }

    uint8_t showKind() { return (uint8_t)(type_ == SnowType::Mixed ? (int)rand(0, 5) : (int)type_); }

    // Rockets go up from the bottom of the screen, trailing gold, and burst into coloured sparks that fall and fade.
    void stepFireworks(float dt, bool launching) {
        if (launching && !world_.empty()) {
            const Rect d = world_[0];
            fireworkClock_ -= dt;
            if (fireworkClock_ <= 0) {
                fireworkClock_ = rand(0.3f, 0.6f);
                Spark r{};
                r.x = rand(d.x + d.w * 0.08f, d.maxX() - d.w * 0.08f);
                r.y = d.maxY();
                r.vx = rand(-40, 40);
                r.vy = -rand(480, 660);
                r.life = 3; r.maxLife = 3;
                r.hue = (float)(int)rand(0, 7);
                r.size = 3.2f; r.rocket = true;
                sparks_.push_back(r);
                ++rocketsLaunched_;
            }
        }
        std::vector<Spark> born;
        for (size_t i = 0; i < sparks_.size();) {
            Spark s = sparks_[i];
            if (s.rocket) {
                s.vy += 300 * dt;
                s.x += s.vx * dt;
                s.y += s.vy * dt;
                Spark t{};
                t.x = s.x + rand(-1, 1);
                t.y = s.y + 4;
                t.vx = rand(-10, 10);
                t.vy = rand(10, 40);
                t.life = 0.45f; t.maxLife = 0.45f; t.hue = 1; t.size = 1.6f; t.rocket = false;
                born.push_back(t);
                if (s.vy > -80) {                        // the top of its climb: burst
                    const int n = 60 + (int)rand(0, 30);
                    const float second = (float)(int)rand(0, 7);
                    born.push_back(Spark{s.x, s.y, 0, 0, 0.25f, 0.25f, 6, 12, false});   // the flash
                    for (int k = 0; k < n; ++k) {
                        const float a = (float)k / n * 6.2832f + rand(-0.05f, 0.05f);
                        const float speed = rand(160, 320);
                        const float life = rand(1.0f, 1.6f);
                        Spark b{};
                        b.x = s.x; b.y = s.y;
                        b.vx = std::cos(a) * speed; b.vy = std::sin(a) * speed;
                        b.life = life; b.maxLife = life;
                        b.hue = k % 3 == 0 ? second : s.hue;
                        b.size = rand(2.5f, 4);
                        b.rocket = false;
                        born.push_back(b);
                    }
                    sparks_.erase(sparks_.begin() + i);
                    continue;
                }
            } else {
                const float drag = std::exp(-1.2f * dt);
                s.vy += 120 * dt;
                s.vx *= drag; s.vy *= drag;
                s.x += s.vx * dt;
                s.y += s.vy * dt;
                s.life -= dt;
                if (s.life <= 0) { sparks_.erase(sparks_.begin() + i); continue; }
            }
            sparks_[i] = s;
            ++i;
        }
        const size_t room = sparks_.size() < 1500 ? 1500 - sparks_.size() : 0;
        sparks_.insert(sparks_.end(), born.begin(), born.begin() + std::min(room, born.size()));
    }

    void stepShow(float dt) {
        showT_ += dt;
        stepFireworks(dt, showT_ >= 9 && showT_ < 17.5f);   // while "My Favourite Ho Ho Ho" is up
        const int n = (int)showParts_.size();
        if (showT_ < 15) {
            const std::vector<float>& targets = showTargets_[showT_ < 9 ? 0 : 1];
            const int t = (int)targets.size() / 2;
            if (t <= 0) { endShow(); return; }
            for (int i = 0; i < n; ++i) {
                ShowParticle& p = showParts_[i];
                if (showT_ >= p.delay) {
                    const int k = (int)((long long)i * t / n);
                    const float tx = targets[2 * k] + std::sin(showT_ * 2 + p.phase) * 0.6f;
                    const float ty = targets[2 * k + 1] + std::cos(showT_ * 1.7f + p.phase) * 0.6f;
                    p.vx += (10 * (tx - p.x) - 6 * p.vx) * dt;
                    p.vy += (10 * (ty - p.y) - 6 * p.vy) * dt;
                } else {
                    p.vx *= std::exp(-dt);
                }
                p.x += p.vx * dt;
                p.y += p.vy * dt;
            }
            return;
        }
        // Everything drops to the floor.
        int left = 0;
        for (auto& p : showParts_) {
            if (p.landed) continue;
            p.vy = std::min(p.vy + 500 * dt, 600.f);
            p.vx *= std::exp(-dt);
            p.x += p.vx * dt;
            p.y += p.vy * dt;
            for (Surface* s : list_) {
                if (!(s->isFloor && p.x >= s->rect.x && p.x < s->rect.maxX())) continue;
                const int c = column(*s, p.x);
                if (p.y >= s->rect.y - s->heights[c]) {
                    s->heights[c] = std::min(s->cap, s->heights[c] + p.amount);
                    p.landed = true;
                    break;
                }
            }
            if (p.y > box_.maxY() + 10) p.landed = true;
            if (!p.landed) ++left;
        }
        if (left == 0 || showT_ > 24) endShow();
    }

    void endShow() {
        showT_ = -1;
        showParts_.clear();
        for (int i = 0; i < normalCount_; ++i) respawn(i);
        pinkLeft_ = kPinkSeconds;
        gems_.clear();
        for (auto& kv : surfaces_) {
            Surface& s = *kv.second;
            for (int c = 2; c < (int)s.heights.size(); c += 4) {
                if (!(s.visible[c] && s.heights[c] >= 2 && gems_.size() < 320)) continue;
                Gem g;
                g.sid = kv.first;
                g.x = s.rect.x + (c + 0.5f) * kColumnWidth + rand(-2, 2);
                g.depth = rand(0.15f, 0.85f);
                g.tag = (float)(int)rand(0, 6);
                g.tag += rand(0, 0.99f);
                g.size = rand(10, 16);
                g.sparkle = rand() < 0.35f;
                gems_.push_back(g);
            }
        }
    }

    void stepPink(float dt) {
        if (pinkLeft_ <= 0) return;
        pinkLeft_ -= dt;
        if (pinkLeft_ <= 0) { pinkLeft_ = 0; gems_.clear(); }
    }

    // The show's dots (in place of the flakes), and the gems and sparkles while the snow is pink.
    int fillShow(std::vector<float>& out) const {
        int n = 0;
        auto put = [&](float x, float y, float d, float a, float vx, float vy, float kind, float ang) {
            out.push_back(x); out.push_back(y); out.push_back(d); out.push_back(a);
            out.push_back(vx); out.push_back(vy); out.push_back(kind); out.push_back(ang);
            ++n;
        };
        for (const auto& s : sparks_) put(s.x, s.y, s.size * 3.5f, s.rocket ? 1.f : std::max(0.f, s.life / s.maxLife), s.vx, s.vy, 11, s.hue);
        for (const auto& p : showParts_)
            if (!p.landed) put(p.x, p.y, p.size * 1.6f + 1, std::min(1.f, 0.55f + p.size * 0.14f) + 2, p.vx, p.vy, (float)p.kind, p.phase);   // + 2: a faint dark rim
        for (const auto& g : gems_) {
            const Surface* s = find(g.sid);
            if (!s) continue;
            const float h = s->heights[column(*s, g.x)];
            if (h < 1) continue;
            put(g.x, s->rect.y - h * (1 - g.depth), g.sparkle ? g.size * 0.8f : g.size, 1, 0, 0, g.sparkle ? 3.f : 10.f, g.sparkle ? g.tag * 6.28f : g.tag);
        }
        return n;
    }

    static int columns(const Rect& r) { return std::max(1, (int)std::ceil(r.w / kColumnWidth)); }

    std::unique_ptr<Surface> makeSurface(const SurfaceInput& in) {
        auto s = std::make_unique<Surface>();
        s->rect = in.rect;
        s->isFloor = in.isFloor;
        s->radius = in.radius;
        s->ploughable = in.isFloor && in.plough;
        if (in.isFloor) s->cap = std::max(kFloorCap, display(*s).h / 3);
        s->heights.assign(columns(in.rect), 0.f);
        s->visible = visibility(in);
        return s;
    }

    std::vector<char> visibility(const SurfaceInput& in) const {
        const int cols = columns(in.rect);
        std::vector<char> out(cols, 1);
        const float y = in.rect.y - 1;
        for (int c = 0; c < cols; ++c) {
            const float x = in.rect.x + (c + 0.5f) * kColumnWidth;
            if (!in.isFloor) {
                bool onScreen = false;
                for (const auto& w : world_) if (w.contains(x, y)) { onScreen = true; break; }
                if (!onScreen) { out[c] = 0; continue; }
            }
            for (const auto& o : in.occluders) if (o.contains(x, y)) { out[c] = 0; break; }
        }
        return out;
    }

    void release(const Surface& s) {
        int budget = 140;
        for (size_t c = 0; c < s.heights.size() && budget > 0; ++c) {
            const float h = s.heights[c];
            if (!(h > 0.6f)) continue;
            const int n = std::min((int)(h * 0.8f), budget);
            const float x = s.rect.x + (c + 0.5f) * kColumnWidth;
            for (int k = 0; k < n && budget > 0; ++k) {
                spawnDebris(x + rand(-2, 2), s.rect.y - h * rand(0.2f, 1), rand(-25, 25), rand(-10, 40));
                --budget;
            }
        }
    }

    void spawnDebris(float x, float y, float vx, float vy) {
        const int total = (int)xs_.size();
        if (total <= normalCount_) return;
        for (int k = 0; k < kDebrisCapacity; ++k) {
            int i = debrisCursor_ + k;
            if (i >= total) i -= kDebrisCapacity;
            if (state_[i] == 0) {
                state_[i] = 2;
                xs_[i] = x; ys_[i] = y; vxs_[i] = vx; vys_[i] = vy;
                sizes_[i] = rand(1.6f, 3.0f); exs_[i] = 0; eys_[i] = 0; phases_[i] = rand(0, 6.28f);
                debrisCursor_ = i + 1 >= total ? normalCount_ : i + 1;
                return;
            }
        }
    }

    bool land(size_t i, float prevY) {
        if (sizes_[i] > 5) return false;
        const float x = xs_[i], y = ys_[i];
        for (Surface* s : list_) {
            if (y < s->rect.y - s->cap || prevY > s->rect.y || x < s->rect.x || x >= s->rect.maxX()) continue;
            const int n = (int)s->heights.size();
            const int c = std::min(n - 1, (int)((x - s->rect.x) / kColumnWidth));
            if (!s->visible[c]) continue;
            const float top = s->rect.y - s->heights[c];
            if (!(prevY <= top && y >= top)) continue;
            const float cap = s->cap;
            int best = c;
            for (int d = -2; d <= 2; ++d) {
                const int m = c + d;
                if (m >= 0 && m < n && s->visible[m] && s->heights[m] < s->heights[best]) best = m;
            }
            if (s->heights[best] < cap) {
                // Floors gather twice as fast, so mountains can grow.
                s->heights[best] = std::min(cap, s->heights[best] + (state_[i] == 2 ? 1.0f : 0.3f * sizes_[i]) * (s->isFloor ? 2.f : 1.f));
            }
            if (state_[i] == 2) state_[i] = 0; else respawn(i);
            return true;
        }
        return false;
    }

    void settle(float dt) {
        for (Surface* s : list_) {
            const int n = (int)s->heights.size();
            const float slope = s->isFloor ? 3.0f : 2.2f;
            for (int pass = 0; pass < 2; ++pass) {
                for (int k = 0; k < n - 1; ++k) {
                    const int i = pass == 0 ? k : n - 2 - k;
                    if (!s->visible[i] || !s->visible[i + 1]) continue;
                    const float d = s->heights[i] - s->heights[i + 1];
                    if (d > slope) { const float t = (d - slope) * 0.25f; s->heights[i] -= t; s->heights[i + 1] += t; }
                    else if (d < -slope) { const float t = (-d - slope) * 0.25f; s->heights[i] += t; s->heights[i + 1] -= t; }
                }
            }
            if (!s->isFloor) {
                const float edgeCap = 3.0f;
                const int cols[2] = {0, n - 1};
                const float dirs[2] = {-1, 1};
                for (int e = 0; e < 2; ++e) {
                    const int c = cols[e];
                    if (!(s->heights[c] > edgeCap)) continue;
                    const float take = (s->heights[c] - edgeCap) * std::min(1.f, 2.5f * dt);
                    s->heights[c] -= take;
                    s->spill += take;
                    while (s->spill >= 1) {
                        s->spill -= 1;
                        const float x = dirs[e] < 0 ? s->rect.x - 1 : s->rect.maxX() + 1;
                        spawnDebris(x, s->rect.y - 2, dirs[e] * rand(8, 40), rand(0, 25));
                    }
                }
            }
            if (melts) {
                const float f = std::exp(-0.012f * dt);
                for (int c = 0; c < n; ++c) s->heights[c] = std::max(0.f, s->heights[c] * f - 0.03f * dt);
            }
        }
    }

    void sweep(const Pointer& p, float vx, float /*vy*/) {
        const float reach = 38;
        for (Surface* s : list_) {
            int id = 0;
            for (auto& kv : surfaces_) if (kv.second.get() == s) { id = kv.first; break; }
            if (!(p.x > s->rect.x - reach && p.x < s->rect.maxX() + reach && p.y > s->rect.y - s->heights[column(*s, p.x)] - 34 &&
                  p.y < s->rect.y + 6)) continue;
            float thrown = sweepAcc_.count(id) ? sweepAcc_[id] : 0.f;
            const int n = (int)s->heights.size();
            const int lo = std::max(0, (int)((p.x - reach - s->rect.x) / kColumnWidth));
            const int hi = std::min(n - 1, (int)((p.x + reach - s->rect.x) / kColumnWidth));
            if (lo <= hi) {
                for (int c = lo; c <= hi; ++c) {
                    if (!s->visible[c] || !(s->heights[c] > 0.05f)) continue;
                    const float cx = s->rect.x + (c + 0.5f) * kColumnWidth;
                    const float w = 1 - std::fabs(cx - p.x) / reach;
                    if (!(w > 0)) continue;
                    const float take = std::min(s->heights[c], s->heights[c] * 0.5f * w + 0.15f * w);
                    s->heights[c] -= take;
                    thrown += take;
                }
            }
            while (thrown >= 1) {
                thrown -= 1;
                spawnDebris(p.x + rand(-reach, reach), s->rect.y - rand(1, 6), vx * 0.3f + rand(-45, 45), -rand(40, 150));
            }
            sweepAcc_[id] = thrown;
        }
    }
};

}  // namespace qs
