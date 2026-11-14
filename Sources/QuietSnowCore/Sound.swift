// The sounds, made from sine waves and noise when first needed: no audio files. Mono, 16-bit, 22,050 samples a second,
// as WAV bytes the app can hand straight to the system. windows/sound.hpp makes the same sounds for Windows.

import Foundation

public enum SoundBank {
    static let rate: Float = 22050

    public static func wav(_ e: SoundEvent) -> [UInt8] {
        let s = samples(e)
        var out: [UInt8] = []
        out.reserveCapacity(44 + s.count * 2)
        func u32(_ v: UInt32) { for k in 0..<4 { out.append(UInt8((v >> (8 * UInt32(k))) & 0xFF)) } }
        func u16(_ v: UInt16) { out.append(UInt8(v & 0xFF)); out.append(UInt8(v >> 8)) }
        out += Array("RIFF".utf8); u32(UInt32(36 + s.count * 2)); out += Array("WAVE".utf8)
        out += Array("fmt ".utf8); u32(16); u16(1); u16(1); u32(UInt32(rate)); u32(UInt32(rate) * 2); u16(2); u16(16)
        out += Array("data".utf8); u32(UInt32(s.count * 2))
        for v in s { u16(UInt16(bitPattern: Int16(max(-1, min(1, v)) * 0.8 * 32767))) }
        return out
    }

    static func samples(_ e: SoundEvent) -> [Float] {
        let dur: Float
        switch e {
        case .bells: dur = 1.6
        case .engine: dur = 1.8
        case .chime: dur = 2.4
        case .pop: dur = 0.6
        case .splat: dur = 0.25
        }
        let n = Int(dur * rate)
        var out = [Float](repeating: 0, count: n)
        var seed: UInt32 = 12345
        func noise() -> Float {
            seed = seed &* 1664525 &+ 1013904223
            return Float(seed >> 8) / 16_777_216 * 2 - 1
        }
        let tau: Float = 6.2831853
        var low: Float = 0
        for i in 0..<n {
            let t = Float(i) / rate
            var v: Float = 0
            switch e {
            case .bells:                                  // a jingle: seven strikes of a small bright bell
                for (k, at) in [Float(0), 0.12, 0.24, 0.5, 0.62, 0.74, 1.0].enumerated() where t >= at {
                    let d = t - at, detune = 1 + 0.01 * Float(k % 3)
                    let env = exp(-d * 18)
                    v += env * (sin(tau * 2400 * detune * d) + 0.6 * sin(tau * 3530 * detune * d)
                                + 0.4 * sin(tau * 4900 * detune * d) + 0.25 * sin(tau * 6200 * detune * d)) * 0.3
                }
            case .engine:                                 // a little diesel putt-putt
                let saw = 2 * (t * 55 - (t * 55).rounded(.down)) - 1
                let am = 0.55 + 0.45 * max(0, sin(tau * 9 * t))
                low += 0.08 * ((saw * 0.7 + noise() * 0.3) * am - low)
                let env = min(1, t / 0.1) * min(1, (dur - t) / 0.3)
                v = low * env * 1.6
            case .chime:                                  // four bells going up
                for (k, f) in [Float(1046.5), 1318.5, 1568.0, 2093.0].enumerated() {
                    let at = Float(k) * 0.18
                    guard t >= at else { continue }
                    let d = t - at
                    v += (sin(tau * f * d) + 0.3 * sin(tau * 2.76 * f * d)) * exp(-d * 2.5) * 0.22
                }
            case .pop:                                    // a thump and a crackle
                v = noise() * exp(-t * 9) * 0.5 + sin(tau * 70 * t) * exp(-t * 14) * 0.6
                if t > 0.12 && noise() > 0.985 { v += noise() * 0.6 * exp(-(t - 0.12) * 4) }
            case .splat:                                  // a soft wet thud
                low += 0.15 * (noise() - low)
                v = low * exp(-t * 22) * 2.2
            }
            out[i] = v
        }
        return out
    }
}
