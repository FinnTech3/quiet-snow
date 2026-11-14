// Metal side: shaders compiled at launch (so building needs no Xcode Metal toolchain) and the two draw calls.

#if canImport(AppKit) && canImport(MetalKit)
import MetalKit

// MARK: shaders (compiled at launch, so no Xcode Metal toolchain is needed to build)

let shaderSource = """
#include <metal_stdlib>
using namespace metal;

struct U { float2 origin; float2 size; float t; float time; float scale; float pink; };
struct V { float4 pos [[position]]; float size [[point_size]]; float a; float shade; float kind [[flat]]; float ang [[flat]]; float2 dir [[flat]]; };

static float4 clip(float4 f, constant U &u) {
    float2 q = (f.xy - u.origin) / u.size;
    return float4(q.x * 2.0 - 1.0, 1.0 - q.y * 2.0, 0.0, 1.0);
}

// kind: 0 ball, 1 snowflake, 2 sleet, 3 sparkle, 4 hail (the SnowType values in the model); 5 snowman, 6 plough, 7 snowball, 8 sleigh, 9 reindeer.
vertex V flakeV(uint i [[vertex_id]], constant float4 *p [[buffer(0)]], constant U &u [[buffer(1)]]) {
    float4 f = p[2 * i];
    float4 m = p[2 * i + 1];
    f.xy += m.xy * u.t;                       // glide on from the last model step
    float kind = m.z;
    float d = f.z * (kind == 1.0 ? 2.4 : kind == 2.0 ? 3.2 : kind == 3.0 ? 2.0 : kind == 4.0 ? 0.8 : 1.0);
    V o; o.pos = clip(f, u); o.size = d * u.scale; o.a = f.w; o.shade = 0.0;
    o.kind = kind; o.ang = m.w; o.dir = normalize(m.xy + float2(0.0001, 0.0001));
    return o;
}

static float segDist(float2 p, float2 a, float2 b) {
    float2 pa = p - a, ba = b - a;
    return length(pa - ba * saturate(dot(pa, ba) / dot(ba, ba)));
}

// Snowman (5), plough (6), snowball (7), sleigh (8) and reindeer (9): drawn from shapes, premultiplied colour out.
static float circ(float2 p, float2 c, float r, float aa) { return smoothstep(r + aa, r - aa, length(p - c)); }
static float box(float2 p, float2 c, float2 h, float rr, float aa) {
    float2 q = abs(p - c) - h + rr;
    return smoothstep(aa, -aa, length(max(q, float2(0.0, 0.0))) + min(max(q.x, q.y), 0.0) - rr);
}
static float oval(float2 p, float2 c, float2 r) { return smoothstep(1.06, 0.94, length((p - c) / r)); }
static float stroke(float2 p, float2 a, float2 b, float w) { return smoothstep(w + 0.02, w - 0.02, segDist(p, a, b)); }
static float4 over(float4 dst, float3 c, float m) { return float4(mix(dst.rgb, c, m), mix(dst.a, 1.0, m)); }
static float3 snowShade(float2 p, float k) {
    return mix(float3(0.97, 0.98, 1.0), float3(0.70, 0.78, 0.92), saturate(dot(p, float2(0.6, 0.8)) * k + 0.2));
}
static float4 snowman(float2 u, float b) {
    float g1 = smoothstep(0.0, 0.15, b), g2 = smoothstep(0.33, 0.48, b), g3 = smoothstep(0.6, 0.75, b), g4 = smoothstep(0.85, 1.0, b);
    float4 o = float4(0.0, 0.0, 0.0, 0.0);
    float arms = max(smoothstep(0.05, 0.02, segDist(u, float2(0.26, -0.02), float2(0.62, -0.28))),
                     smoothstep(0.05, 0.02, segDist(u, float2(-0.26, -0.02), float2(-0.62, -0.24))));
    o = over(o, float3(0.42, 0.27, 0.15), arms * g4);
    o = over(o, snowShade(u - float2(0.0, 0.55), 1.6), circ(u, float2(0.0, 0.55), 0.42, 0.04) * g1);
    o = over(o, snowShade(u, 2.0), circ(u, float2(0.0, 0.0), 0.31, 0.04) * g2);
    o = over(o, snowShade(u - float2(0.0, -0.42), 2.6), circ(u, float2(0.0, -0.42), 0.22, 0.04) * g3);
    float dark = max(max(circ(u, float2(0.0, -0.04), 0.04, 0.02), circ(u, float2(0.0, 0.11), 0.04, 0.02)),
                     max(circ(u, float2(-0.08, -0.47), 0.035, 0.015), circ(u, float2(0.08, -0.47), 0.035, 0.015)));
    o = over(o, float3(0.13, 0.13, 0.16), dark * g4);
    o = over(o, float3(0.95, 0.5, 0.1), smoothstep(0.04, 0.015, segDist(u, float2(0.0, -0.40), float2(0.2, -0.37))) * g4);
    float scarf = max(box(u, float2(0.0, -0.21), float2(0.21, 0.05), 0.04, 0.02), box(u, float2(0.12, -0.1), float2(0.05, 0.11), 0.03, 0.02));
    o = over(o, float3(0.82, 0.12, 0.14), scarf * g4);
    float2 ha = float2(0.0, -0.7), hb = float2(0.26, -0.92);
    float2 pa = u - ha, ba = hb - ha;
    float t = saturate(dot(pa, ba) / dot(ba, ba));
    o = over(o, float3(0.85, 0.1, 0.12), smoothstep(0.02, -0.02, length(pa - ba * t) - mix(0.17, 0.035, t)) * g4);
    o = over(o, float3(0.98, 0.98, 1.0), max(box(u, float2(0.0, -0.62), float2(0.22, 0.05), 0.04, 0.02), circ(u, float2(0.29, -0.93), 0.07, 0.02)) * g4);
    return o;
}
// A pink plough that bounces and rocks as it goes, with steam puffing from a pipe on the cab.
static float4 plough(float2 u, float dir, float time) {
    u.x *= dir < 0.0 ? -1.0 : 1.0;   // the blade is always at the front
    float bounce = abs(sin(time * 9.0)) * 0.05, rock = sin(time * 9.0) * 0.05;
    float2 p = (u - float2(0.0, 0.316 - bounce)) / 0.72;
    float2 pivot = float2(-0.2, 0.8);
    float2 d = p - pivot;
    p = pivot + float2(d.x * cos(rock) - d.y * sin(rock), d.x * sin(rock) + d.y * cos(rock));
    float3 pink = float3(1.0, 0.45, 0.72), light = float3(1.0, 0.62, 0.84), dark = float3(0.22, 0.18, 0.22), steel = float3(0.62, 0.63, 0.68);
    float4 o = float4(0.0, 0.0, 0.0, 0.0);
    for (int k = 0; k < 3; k++) {
        float ph = fract(time * 0.8 + float(k) / 3.0);
        float2 c = float2(-0.05 - 0.35 * ph, -0.56 - 0.85 * ph);
        float r = 0.08 + 0.16 * ph;
        o = over(o, float3(0.96, 0.96, 0.98), smoothstep(r, r * 0.35, length(p - c)) * (1.0 - ph) * 0.8);
    }
    o = over(o, dark, box(p, float2(0.28, 0.5), float2(0.14, 0.04), 0.02, 0.02));
    o = over(o, dark, box(p, float2(-0.2, 0.72), float2(0.48, 0.16), 0.15, 0.02));
    float wheels = max(max(circ(p, float2(-0.55, 0.72), 0.09, 0.02), circ(p, float2(-0.32, 0.72), 0.09, 0.02)),
                       max(circ(p, float2(-0.09, 0.72), 0.09, 0.02), circ(p, float2(0.14, 0.72), 0.09, 0.02)));
    o = over(o, steel, wheels);
    o = over(o, pink, box(p, float2(-0.22, 0.38), float2(0.42, 0.17), 0.05, 0.02));
    o = over(o, dark, box(p, float2(-0.05, -0.36), float2(0.045, 0.14), 0.02, 0.02));
    o = over(o, light, box(p, float2(-0.32, 0.0), float2(0.24, 0.24), 0.05, 0.02));
    o = over(o, float3(0.62, 0.82, 0.95), box(p, float2(-0.28, -0.02), float2(0.15, 0.14), 0.03, 0.02));
    o = over(o, mix(steel, float3(0.88, 0.89, 0.92), saturate(p.x * 4.0 - 1.6)), box(p, float2(0.5, 0.55), float2(0.06, 0.33), 0.04, 0.02));
    return o;
}
static float4 snowball(float2 u) {
    float m = smoothstep(1.0, 0.86, length(u));
    return float4(snowShade(u, 0.9) * m, m);
}
// Santa in his sleigh with a sack of presents, flying right.
static float4 sleigh(float2 u) {
    float3 red = float3(0.82, 0.08, 0.12), gold = float3(0.98, 0.8, 0.25), white = float3(0.98, 0.98, 1.0);
    float4 o = float4(0.0, 0.0, 0.0, 0.0);
    o = over(o, float3(0.55, 0.38, 0.2), circ(u, float2(-0.5, -0.08), 0.3, 0.03));
    o = over(o, float3(0.35, 0.22, 0.1), stroke(u, float2(-0.62, -0.32), float2(-0.42, -0.36), 0.03));
    o = over(o, red, circ(u, float2(-0.1, 0.0), 0.27, 0.03));
    o = over(o, float3(1.0, 0.82, 0.7), circ(u, float2(-0.02, -0.33), 0.13, 0.03));
    o = over(o, white, circ(u, float2(0.01, -0.21), 0.12, 0.03));
    o = over(o, float3(0.15, 0.1, 0.08), circ(u, float2(0.04, -0.36), 0.022, 0.01));
    float2 ha = float2(-0.04, -0.45), hb = float2(-0.27, -0.7);
    float2 pa = u - ha, ba = hb - ha;
    float t = saturate(dot(pa, ba) / dot(ba, ba));
    o = over(o, red, smoothstep(0.02, -0.02, length(pa - ba * t) - mix(0.13, 0.03, t)));
    o = over(o, white, max(box(u, float2(-0.02, -0.44), float2(0.15, 0.04), 0.03, 0.02), circ(u, float2(-0.28, -0.71), 0.06, 0.02)));
    o = over(o, red, max(box(u, float2(-0.1, 0.32), float2(0.55, 0.2), 0.1, 0.02), circ(u, float2(0.48, 0.12), 0.16, 0.02)));
    o = over(o, gold, box(u, float2(-0.1, 0.15), float2(0.55, 0.03), 0.02, 0.02));
    float runner = max(stroke(u, float2(-0.65, 0.66), float2(0.55, 0.66), 0.035),
                       smoothstep(0.05, 0.02, abs(length(u - float2(0.58, 0.53)) - 0.13)) * step(0.55, u.x));
    runner = max(runner, max(stroke(u, float2(-0.4, 0.5), float2(-0.4, 0.66), 0.03), stroke(u, float2(0.3, 0.5), float2(0.3, 0.66), 0.03)));
    o = over(o, gold, runner);
    return o;
}
// A reindeer at a gallop. Rudolph (tag 10 and up) leads, with a glowing red nose.
static float4 reindeer(float2 u, float tag, float time) {
    float t = time * 10.0 + tag, s = sin(t);
    float3 brown = float3(0.6, 0.4, 0.22), dark = float3(0.38, 0.24, 0.12), horn = float3(0.86, 0.76, 0.56);
    float4 o = float4(0.0, 0.0, 0.0, 0.0);
    if (tag >= 10.0) {
        float g = exp(-dot(u - float2(0.72, -0.28), u - float2(0.72, -0.28)) / 0.03) * (0.75 + 0.25 * sin(time * 6.0));
        o = over(o, float3(1.0, 0.25, 0.2), g * 0.7);
    }
    o = over(o, float3(0.7, 0.1, 0.1), stroke(u, float2(-1.0, 0.05), float2(-0.3, 0.05), 0.025));
    float legs = max(max(stroke(u, float2(0.25, 0.2), float2(0.25 + s * 0.18, 0.62), 0.045), stroke(u, float2(0.32, 0.2), float2(0.32 - s * 0.12, 0.62), 0.045)),
                     max(stroke(u, float2(-0.25, 0.2), float2(-0.28 - s * 0.18, 0.62), 0.045), stroke(u, float2(-0.32, 0.2), float2(-0.32 + s * 0.12, 0.62), 0.045)));
    o = over(o, dark, legs);
    o = over(o, float3(0.92, 0.86, 0.76), circ(u, float2(-0.42, 0.0), 0.07, 0.02));
    o = over(o, brown, max(oval(u, float2(0.0, 0.08), float2(0.42, 0.2)), stroke(u, float2(0.3, 0.0), float2(0.46, -0.25), 0.09)));
    o = over(o, brown, oval(u, float2(0.55, -0.3), float2(0.17, 0.1)));
    float antler = max(max(stroke(u, float2(0.48, -0.38), float2(0.42, -0.64), 0.025), stroke(u, float2(0.44, -0.52), float2(0.32, -0.62), 0.025)),
                       stroke(u, float2(0.43, -0.58), float2(0.52, -0.72), 0.025));
    o = over(o, horn, antler);
    o = over(o, float3(0.98, 0.8, 0.25), circ(u, float2(-0.3, 0.12), 0.05, 0.02));
    o = over(o, float3(0.1, 0.08, 0.06), circ(u, float2(0.58, -0.33), 0.025, 0.012));
    o = over(o, tag >= 10.0 ? float3(1.0, 0.12, 0.1) : float3(0.15, 0.1, 0.08), circ(u, float2(0.72, -0.28), tag >= 10.0 ? 0.065 : 0.045, 0.015));
    return o;
}

// A cut gem (kind 10) that catches the light now and then. The whole part of the tag picks the stone, the fraction
// when it glints.
static float4 gem(float2 u, float tag, float time) {
    float hue = floor(tag), ph = (tag - hue) * 6.2831;
    float3 c = hue < 0.5 ? float3(0.95, 0.1, 0.3) : hue < 1.5 ? float3(0.15, 0.35, 1.0) : hue < 2.5 ? float3(0.1, 0.85, 0.45)
             : hue < 3.5 ? float3(0.68, 0.25, 0.95) : hue < 4.5 ? float3(1.0, 0.8, 0.2) : float3(0.2, 0.9, 0.95);
    float2 q = abs(u);
    float m = smoothstep(0.86, 0.78, q.x * 1.15 + q.y);
    float3 col = c * (0.7 + 0.5 * saturate(-u.x * 0.6 - u.y * 0.8 + 0.3));
    col = mix(col, float3(1.0, 1.0, 1.0), smoothstep(0.28, 0.16, q.x * 1.15 + q.y) * 0.45);
    float4 o = float4(col * m, m);
    float tw = pow(max(0.0, sin(time * 3.0 + ph)), 6.0);
    float star = max(smoothstep(0.08, 0.0, q.y) * smoothstep(1.0, 0.0, q.x), smoothstep(0.08, 0.0, q.x) * smoothstep(1.0, 0.0, q.y)) * tw;
    return over(o, float3(1.0, 1.0, 1.0), star);
}

fragment float4 flakeF(V in [[stage_in]], float2 pc [[point_coord]], constant U &u [[buffer(1)]]) {
    float2 q0 = (pc - 0.5) * 2.0;
    if (in.kind > 9.5) return gem(q0, in.ang, u.time) * in.a;
    if (in.kind > 8.5) return reindeer(q0, in.ang, u.time) * in.a;
    if (in.kind > 7.5) return sleigh(q0) * in.a;
    if (in.kind > 6.5) return snowball(q0) * in.a;
    if (in.kind > 5.5) return plough(q0, in.ang, u.time) * in.a;
    if (in.kind > 4.5) return snowman(q0, in.ang) * in.a;
    float m = 0.0, alpha = in.a, rim = 0.0;
    if (alpha > 1.5) { alpha -= 2.0; rim = smoothstep(1.0, 0.5, length(q0)) * 0.55; }   // the show's dots
    float3 tint = float3(0.94, 0.97, 1.0);
    if (in.kind < 0.5) {                         // ball: a soft disc
        m = smoothstep(1.0, 0.25, length(q0));
    } else if (in.kind < 1.5) {                  // snowflake: six arms, each with two pairs of branches
        float c = cos(in.ang), sn = sin(in.ang);
        float2 q = float2(q0.x * c - q0.y * sn, q0.x * sn + q0.y * c);
        float sector = 3.14159265 / 3.0;
        float a = abs(fmod(atan2(q.y, q.x) + 6.2831853 + sector * 0.5, sector) - sector * 0.5);
        float2 p = float2(cos(a), sin(a)) * length(q);
        float arm = p.x < 0.92 ? abs(p.y) : length(p - float2(0.92, 0.0));
        float dd = min(arm, min(segDist(p, float2(0.40, 0.0), float2(0.53, 0.22)), segDist(p, float2(0.66, 0.0), float2(0.76, 0.17))));
        m = smoothstep(0.20, 0.07, dd) * step(length(q), 1.0);
    } else if (in.kind < 2.5) {                  // sleet: a streak with a brighter head, along the way the flake travels
        float2 perp = float2(-in.dir.y, in.dir.x);
        float2 s = float2(dot(q0, in.dir), dot(q0, perp));
        m = smoothstep(1.0, 0.2, length(float2(s.x * 0.85, s.y / 0.125))) * mix(0.45, 1.0, saturate(s.x * 0.5 + 0.5));
        alpha *= 0.85;
    } else if (in.kind < 3.5) {                  // sparkle: a four-point star that twinkles
        float2 q = abs(q0);
        float st = sqrt(q.x) + sqrt(q.y);
        m = max(smoothstep(1.0, 0.55, st), smoothstep(0.4, 0.0, length(q0)));
        m *= 0.55 + 0.45 * sin(u.time * 2.6 + in.ang * 5.0);
        tint = float3(1.0, 0.99, 0.93);
    } else {                                     // hail: a small hard pellet with a highlight
        m = smoothstep(1.0, 0.78, length(q0));
        tint = mix(float3(0.78, 0.87, 0.97), float3(1.0, 1.0, 1.0), smoothstep(0.6, 0.0, length(q0 - float2(-0.35, -0.35))));
        alpha = min(1.0, alpha * 1.6);
    }
    tint = mix(tint, float3(1.0, 0.66, 0.86), u.pink);   // the snow turns pink after the show
    float a = m * alpha;
    return float4(tint * a + float3(0.04, 0.05, 0.12) * rim * (1.0 - a), a + rim * (1.0 - a));
}

vertex V pileV(uint i [[vertex_id]], constant float4 *p [[buffer(0)]], constant U &u [[buffer(1)]]) {
    V o; o.pos = clip(p[i], u); o.size = 1.0; o.a = p[i].z; o.shade = p[i].w; return o;
}
fragment float4 pileF(V in [[stage_in]], constant U &u [[buffer(1)]]) {
    float a = in.a;
    float3 lit = mix(float3(0.97, 0.985, 1.0), float3(1.0, 0.8, 0.92), u.pink);
    float3 shadow = mix(float3(0.66, 0.76, 0.93), float3(0.95, 0.5, 0.76), u.pink);
    return float4(mix(lit, shadow, in.shade) * a, a);
}
"""



struct Uniforms { var origin: SIMD2<Float>; var size: SIMD2<Float>; var t: Float; var time: Float = 0; var scale: Float = 1; var pink: Float = 0 }

final class Renderer {
    let queue: MTLCommandQueue
    let pointPSO: MTLRenderPipelineState
    let trianglePSO: MTLRenderPipelineState

    init(device: MTLDevice) throws {
        guard let q = device.makeCommandQueue() else { throw NSError(domain: "QuietSnow", code: 1, userInfo: [NSLocalizedDescriptionKey: "no command queue"]) }
        queue = q
        let lib = try device.makeLibrary(source: shaderSource, options: nil)
        func pso(_ v: String, _ f: String) throws -> MTLRenderPipelineState {
            let d = MTLRenderPipelineDescriptor()
            d.vertexFunction = lib.makeFunction(name: v)
            d.fragmentFunction = lib.makeFunction(name: f)
            let c = d.colorAttachments[0]!
            c.pixelFormat = .bgra8Unorm
            c.isBlendingEnabled = true
            c.rgbBlendOperation = .add
            c.alphaBlendOperation = .add
            c.sourceRGBBlendFactor = .one                      // the shaders return premultiplied colour
            c.sourceAlphaBlendFactor = .one
            c.destinationRGBBlendFactor = .oneMinusSourceAlpha
            c.destinationAlphaBlendFactor = .oneMinusSourceAlpha
            return try device.makeRenderPipelineState(descriptor: d)
        }
        pointPSO = try pso("flakeV", "flakeF")
        trianglePSO = try pso("pileV", "pileF")
    }

    func encode(_ enc: MTLRenderCommandEncoder, origin: SIMD2<Float>, size: SIMD2<Float>, t: Float = 0, time: Float = 0, scale: Float = 1, pink: Float = 0,
                piles: MTLBuffer?, pileCount: Int, flakes: MTLBuffer?, flakeCount: Int) {
        var u = Uniforms(origin: origin, size: size, t: t, time: time, scale: scale, pink: pink)
        if pileCount > 0, let buf = piles {
            enc.setRenderPipelineState(trianglePSO)
            enc.setVertexBuffer(buf, offset: 0, index: 0)
            enc.setVertexBytes(&u, length: MemoryLayout<Uniforms>.stride, index: 1)
            enc.setFragmentBytes(&u, length: MemoryLayout<Uniforms>.stride, index: 1)
            enc.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: pileCount)
        }
        if flakeCount > 0, let buf = flakes {
            enc.setRenderPipelineState(pointPSO)
            enc.setVertexBuffer(buf, offset: 0, index: 0)
            enc.setVertexBytes(&u, length: MemoryLayout<Uniforms>.stride, index: 1)
            enc.setFragmentBytes(&u, length: MemoryLayout<Uniforms>.stride, index: 1)
            enc.drawPrimitives(type: .point, vertexStart: 0, vertexCount: flakeCount)
        }
    }
}
#endif
