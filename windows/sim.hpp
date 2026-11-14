// The snow model for Windows: a C++ port of Sources/QuietSnowCore/Sim.swift, kept in step with it by hand.
// Header only, standard library only, no Windows headers, so tests/tests.cpp builds anywhere.
// Coordinates are pixels, origin top-left of the virtual screen, y grows downwards.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

namespace qs {

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    float maxX() const { return x + w; }
    float maxY() const { return y + h; }
    bool contains(float px, float py) const { return px >= x && px < maxX() && py >= y && py < maxY(); }
};

struct SurfaceInput {
    int id = 0;
    Rect rect;
    std::vector<Rect> occluders;
    bool isFloor = false;
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
    float cap() const { return isFloor ? 30.f : 16.f; }
};

struct Pointer { float x = 0, y = 0; };

class Sim {
public:
    static constexpr float kColumnWidth = 4;
    static constexpr float kWindowCap = 16;
    static constexpr float kFloorCap = 30;
    static constexpr int kDebrisCapacity = 600;
    static constexpr int kGrace = 2;   // polls a surface may miss before its snow is let go

    float wind = 0;
    bool melts = false;

    explicit Sim(uint64_t seed = 0x9E3779B97F4A7C15ull) : rng_(seed == 0 ? 1 : seed) {}

    SnowType snowType() const { return type_; }
    void setSnowType(SnowType t) {
        if (t == type_) return;
        type_ = t;
        for (int i = 0; i < normalCount_; ++i) assignKind(i);
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
        if (pointer && speed > 120) sweep(*pointer, pvx, pvy);
    }

    // Eight floats per flake: x, y, diameter, alpha, vx, vy, kind (a SnowType value, never Mixed), a per-flake angle.
    int fillFlakes(std::vector<float>& out) const {
        out.clear();
        int n = 0;
        for (size_t i = 0; i < xs_.size(); ++i) {
            if (state_[i] == 0 || hidden(xs_[i], ys_[i])) continue;
            const float s = sizes_[i];
            out.push_back(xs_[i]); out.push_back(ys_[i]); out.push_back(s * 1.6f + 1);
            out.push_back(s > 5 ? 0.3f : std::min(1.f, 0.45f + s * 0.14f));
            out.push_back(cvx_[i]); out.push_back(cvy_[i]); out.push_back((float)kinds_[i]); out.push_back(phases_[i]);
            ++n;
        }
        return n;
    }

    // Four floats per vertex: x, y, alpha, shade. Triangles, twelve vertices per column: the body, then a feather above
    // it so the top edge fades over a few pixels instead of cutting. The profile is smoothed over about 24 pixels,
    // which rounds the ends of a pile on a window, while a floor keeps its full height out to the screen edges.
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
            const float base = s->rect.y + 0.5f;
            const float feather = 2.5f;
            // A pile on a window slopes down to nothing over its last 20 pixels; a floor runs on to the screen edge.
            auto end = [&](float x) {
                if (s->isFloor) return 1.f;
                const float t = std::min(1.f, std::min(x - s->rect.x, s->rect.maxX() - x) / 20.f);
                return t * t * (3 - 2 * t);
            };
            const float top = 0.72f, bottom = 0.97f, deep = 0.7f;
            for (int c = 0; c < n; ++c) {
                if (!s->visible[c]) continue;
                const float l = smoothed_[c], r = smoothed_[c + 1];
                if (l < 0.12f && r < 0.12f) continue;
                const float x0 = s->rect.x + c * kColumnWidth;
                const float x1 = std::min(s->rect.maxX(), x0 + kColumnWidth);
                if (hidden((x0 + x1) * 0.5f, s->rect.y - 1)) continue;
                float y0 = s->rect.y - l * end(x0), y1 = s->rect.y - r * end(x1);
                v(x0, y0, top, 0); v(x0, base, bottom, deep); v(x1, y1, top, 0);
                v(x1, y1, top, 0); v(x0, base, bottom, deep); v(x1, base, bottom, deep);
                v(x0, y0 - feather, 0, 0); v(x0, y0, top, 0); v(x1, y1 - feather, 0, 0);
                v(x1, y1 - feather, 0, 0); v(x0, y0, top, 0); v(x1, y1, top, 0);
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

    static int columns(const Rect& r) { return std::max(1, (int)std::ceil(r.w / kColumnWidth)); }

    std::unique_ptr<Surface> makeSurface(const SurfaceInput& in) {
        auto s = std::make_unique<Surface>();
        s->rect = in.rect;
        s->isFloor = in.isFloor;
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
            if (y < s->rect.y - kFloorCap || prevY > s->rect.y || x < s->rect.x || x >= s->rect.maxX()) continue;
            const int n = (int)s->heights.size();
            const int c = std::min(n - 1, (int)((x - s->rect.x) / kColumnWidth));
            if (!s->visible[c]) continue;
            const float top = s->rect.y - s->heights[c];
            if (!(prevY <= top && y >= top)) continue;
            const float cap = s->cap();
            int best = c;
            for (int d = -2; d <= 2; ++d) {
                const int m = c + d;
                if (m >= 0 && m < n && s->visible[m] && s->heights[m] < s->heights[best]) best = m;
            }
            if (s->heights[best] < cap) {
                s->heights[best] = std::min(cap, s->heights[best] + (state_[i] == 2 ? 1.0f : 0.3f * sizes_[i]));
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
            if (!(std::fabs(p.y - s->rect.y) < 34 && p.x > s->rect.x - reach && p.x < s->rect.maxX() + reach)) continue;
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
