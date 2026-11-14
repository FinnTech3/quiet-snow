// The sounds, made from sine waves and noise when first needed: no audio files. Mono, 16-bit, 22,050 samples a second,
// as WAV bytes for PlaySound. Sources/QuietSnowCore/Sound.swift makes the same sounds for the Mac.
#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

#include "sim.hpp"

namespace qs {

inline std::vector<float> soundSamples(SoundEvent e) {
    const float rate = 22050;
    const float dur = e == SoundEvent::Bells ? 1.6f : e == SoundEvent::Engine ? 1.8f : e == SoundEvent::Chime ? 2.4f
                    : e == SoundEvent::Pop ? 0.6f : 0.25f;
    const int n = (int)(dur * rate);
    std::vector<float> out((size_t)n, 0.f);
    uint32_t seed = 12345;
    auto noise = [&] { seed = seed * 1664525u + 1013904223u; return (float)(seed >> 8) / 16777216.f * 2 - 1; };
    const float tau = 6.2831853f;
    float low = 0;
    for (int i = 0; i < n; ++i) {
        const float t = i / rate;
        float v = 0;
        switch (e) {
            case SoundEvent::Bells: {   // a jingle: seven strikes of a small bright bell
                const float at[7] = {0, 0.12f, 0.24f, 0.5f, 0.62f, 0.74f, 1.0f};
                for (int k = 0; k < 7; ++k) {
                    if (t < at[k]) continue;
                    const float d = t - at[k], detune = 1 + 0.01f * (k % 3);
                    v += std::exp(-d * 18) * (std::sin(tau * 2400 * detune * d) + 0.6f * std::sin(tau * 3530 * detune * d)
                                              + 0.4f * std::sin(tau * 4900 * detune * d) + 0.25f * std::sin(tau * 6200 * detune * d)) * 0.3f;
                }
                break;
            }
            case SoundEvent::Engine: {  // a little diesel putt-putt
                const float saw = 2 * (t * 55 - std::floor(t * 55)) - 1;
                const float am = 0.55f + 0.45f * std::max(0.f, std::sin(tau * 9 * t));
                low += 0.08f * ((saw * 0.7f + noise() * 0.3f) * am - low);
                const float env = std::min(1.f, t / 0.1f) * std::min(1.f, (dur - t) / 0.3f);
                v = low * env * 1.6f;
                break;
            }
            case SoundEvent::Chime: {   // four bells going up
                const float f[4] = {1046.5f, 1318.5f, 1568.0f, 2093.0f};
                for (int k = 0; k < 4; ++k) {
                    const float at = k * 0.18f;
                    if (t < at) continue;
                    const float d = t - at;
                    v += (std::sin(tau * f[k] * d) + 0.3f * std::sin(tau * 2.76f * f[k] * d)) * std::exp(-d * 2.5f) * 0.22f;
                }
                break;
            }
            case SoundEvent::Pop:       // a thump and a crackle
                v = noise() * std::exp(-t * 9) * 0.5f + std::sin(tau * 70 * t) * std::exp(-t * 14) * 0.6f;
                if (t > 0.12f && noise() > 0.985f) v += noise() * 0.6f * std::exp(-(t - 0.12f) * 4);
                break;
            case SoundEvent::Splat:     // a soft wet thud
                low += 0.15f * (noise() - low);
                v = low * std::exp(-t * 22) * 2.2f;
                break;
        }
        out[(size_t)i] = v;
    }
    return out;
}

inline std::vector<uint8_t> soundWav(SoundEvent e) {
    const std::vector<float> s = soundSamples(e);
    std::vector<uint8_t> out;
    out.reserve(44 + s.size() * 2);
    auto u32 = [&](uint32_t v) { for (int k = 0; k < 4; ++k) out.push_back((uint8_t)(v >> (8 * k))); };
    auto u16 = [&](uint16_t v) { out.push_back((uint8_t)(v & 0xFF)); out.push_back((uint8_t)(v >> 8)); };
    auto tag = [&](const char* t) { for (int k = 0; k < 4; ++k) out.push_back((uint8_t)t[k]); };
    tag("RIFF"); u32((uint32_t)(36 + s.size() * 2)); tag("WAVE");
    tag("fmt "); u32(16); u16(1); u16(1); u32(22050); u32(44100); u16(2); u16(16);
    tag("data"); u32((uint32_t)(s.size() * 2));
    for (float v : s) u16((uint16_t)(int16_t)(std::max(-1.f, std::min(1.f, v)) * 0.8f * 32767));
    return out;
}

}  // namespace qs
