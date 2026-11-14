// Metal side: shaders compiled at launch (so building needs no Xcode Metal toolchain) and the two draw calls.

#if canImport(AppKit) && canImport(MetalKit)
import MetalKit

// MARK: shaders (compiled at launch, so no Xcode Metal toolchain is needed to build)

let shaderSource = """
#include <metal_stdlib>
using namespace metal;

struct U { float2 origin; float2 size; float t; float time; float scale; float pink; float season; float pad1; float pad2; float pad3; float4 tint; };
struct V { float4 pos [[position]]; float size [[point_size]]; float a; float shade; float kind [[flat]]; float ang [[flat]]; float2 dir [[flat]]; float speed [[flat]]; };

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
    float d = f.z * (kind == 1.0 ? 2.4 : kind == 2.0 ? 3.2 : kind == 3.0 ? 2.0 : kind == 4.0 ? 0.8 : kind == 19.0 ? 3.0 : kind == 20.0 ? 2.4 : 1.0);
    V o; o.pos = clip(f, u); o.size = d * u.scale; o.a = f.w; o.shade = 0.0;
    o.kind = kind; o.ang = m.w; o.dir = normalize(m.xy + float2(0.0001, 0.0001)); o.speed = length(m.xy);
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
static float2 turn(float2 p, float a) { float c = cos(a), s = sin(a); return float2(p.x * c - p.y * s, p.x * s + p.y * c); }
static float4 snowman(float2 u, float tag, float time) {
    // The tag: build progress, plus 2 without a hat, plus 4 times when it waves.
    float wave = floor(tag / 4.0);
    float rest = tag - wave * 4.0;
    bool hatless = rest >= 2.0;
    float b = hatless ? rest - 2.0 : rest;
    float waving = step(fract(time * 0.1 + wave / 8.0), 0.25);
    float2 hand = float2(0.62, -0.28);
    if (waving > 0.5) hand = float2(0.26, -0.02) + turn(float2(0.0, -0.42), sin(time * 8.0) * 0.5 + 0.35);
    float g1 = smoothstep(0.0, 0.15, b), g2 = smoothstep(0.33, 0.48, b), g3 = smoothstep(0.6, 0.75, b), g4 = smoothstep(0.85, 1.0, b);
    float4 o = float4(0.0, 0.0, 0.0, 0.0);
    float arms = max(smoothstep(0.05, 0.02, segDist(u, float2(0.26, -0.02), hand)),
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
    float hat = hatless ? 0.0 : g4;
    o = over(o, float3(0.85, 0.1, 0.12), smoothstep(0.02, -0.02, length(pa - ba * t) - mix(0.17, 0.035, t)) * hat);
    o = over(o, float3(0.98, 0.98, 1.0), max(box(u, float2(0.0, -0.62), float2(0.22, 0.05), 0.04, 0.02), circ(u, float2(0.29, -0.93), 0.07, 0.02)) * hat);
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

// A snowball splat (kind 12): a ragged star of snow with a drip.
static float4 splatShape(float2 u, float ang) {
    float2 p = turn(u, ang);
    float a = atan2(p.y, p.x);
    float edge = 0.66 + 0.14 * sin(a * 5.0) + 0.07 * sin(a * 11.0 + 1.0);
    float m = max(smoothstep(edge, edge - 0.08, length(p)), max(circ(u, float2(0.08, 0.62), 0.12, 0.04), circ(u, float2(0.08, 0.86), 0.08, 0.04)));
    return float4(snowShade(u, 0.8) * m, m);
}
// A footprint in the snow (kind 13): a fox's pad and toes, or a robin's three-toed print.
static float4 footprint(float2 u, float tag) {
    bool robin = fmod(tag, 2.0) >= 1.0, left = fmod(floor(tag / 2.0), 2.0) >= 1.0, right = tag >= 4.0;
    u.y += left ? -0.18 : 0.18;
    if (!right) u.x = -u.x;
    float m;
    if (robin) {
        m = max(max(stroke(u, float2(-0.5, 0.0), float2(0.4, 0.0), 0.08), stroke(u, float2(0.0, 0.0), float2(0.45, -0.35), 0.08)),
                max(stroke(u, float2(0.0, 0.0), float2(0.45, 0.35), 0.08), 0.0));
    } else {
        m = max(oval(u, float2(-0.1, 0.0), float2(0.32, 0.28)),
                max(max(circ(u, float2(0.35, -0.3), 0.13, 0.04), circ(u, float2(0.45, 0.0), 0.13, 0.04)), circ(u, float2(0.35, 0.3), 0.13, 0.04)));
    }
    return float4(float3(0.42, 0.5, 0.72) * m * 0.85, m * 0.85);
}
// A fox walking, or a robin hopping (kind 14). Tag: 1 for the robin, plus 2 when it faces right.
static float4 animalShape(float2 u, float tag, float time) {
    bool robin = fmod(tag, 2.0) >= 1.0;
    if (tag < 2.0) u.x = -u.x;
    float4 o = float4(0.0, 0.0, 0.0, 0.0);
    if (robin) {
        u.y += abs(sin(time * 8.0)) * 0.25;
        float3 brown = float3(0.45, 0.3, 0.18);
        o = over(o, float3(0.2, 0.15, 0.1), max(stroke(u, float2(-0.05, 0.4), float2(-0.1, 0.7), 0.03), stroke(u, float2(0.1, 0.4), float2(0.12, 0.7), 0.03)));
        o = over(o, brown, oval(turn(u - float2(-0.45, -0.05), -0.4), float2(0.0, 0.0), float2(0.24, 0.08)));
        o = over(o, brown, circ(u, float2(0.0, 0.1), 0.38, 0.03));
        o = over(o, float3(0.93, 0.35, 0.12), circ(u, float2(0.16, 0.12), 0.24, 0.03) * circ(u, float2(0.0, 0.1), 0.38, 0.03));
        o = over(o, brown, circ(u, float2(0.28, -0.22), 0.22, 0.03));
        o = over(o, float3(0.95, 0.75, 0.2), stroke(u, float2(0.48, -0.22), float2(0.64, -0.19), 0.04));
        o = over(o, float3(0.05, 0.05, 0.05), circ(u, float2(0.34, -0.27), 0.04, 0.015));
    } else {
        float s = sin(time * 10.0);
        float3 orange = float3(0.92, 0.45, 0.12), dark = float3(0.18, 0.12, 0.1);
        float legs = max(max(stroke(u, float2(0.25, 0.25), float2(0.25 + s * 0.12, 0.62), 0.045), stroke(u, float2(0.3, 0.25), float2(0.3 - s * 0.12, 0.62), 0.045)),
                         max(stroke(u, float2(-0.25, 0.25), float2(-0.25 - s * 0.12, 0.62), 0.045), stroke(u, float2(-0.3, 0.25), float2(-0.3 + s * 0.12, 0.62), 0.045)));
        o = over(o, dark, legs);
        o = over(o, orange, oval(turn(u - float2(-0.58, 0.02), 0.25), float2(0.0, 0.0), float2(0.3, 0.13)));
        o = over(o, float3(0.97, 0.97, 0.97), circ(u, float2(-0.84, 0.08), 0.08, 0.03));
        o = over(o, orange, oval(u, float2(0.0, 0.15), float2(0.42, 0.2)));
        o = over(o, float3(0.97, 0.95, 0.9), oval(u, float2(0.3, 0.22), float2(0.12, 0.12)));
        o = over(o, orange, max(circ(u, float2(0.45, -0.1), 0.17, 0.03), stroke(u, float2(0.5, -0.06), float2(0.72, 0.0), 0.06)));
        o = over(o, orange, max(stroke(u, float2(0.38, -0.2), float2(0.36, -0.38), 0.05), stroke(u, float2(0.5, -0.2), float2(0.52, -0.38), 0.05)));
        o = over(o, dark, max(circ(u, float2(0.74, 0.0), 0.04, 0.015), circ(u, float2(0.52, -0.13), 0.03, 0.012)));
    }
    return o;
}
// An icicle hanging from the top of its sprite (kind 15), and a drip (kind 16).
static float4 icicle(float2 u) {
    float w = 0.34 * (1.0 - (u.y + 1.0) * 0.5);
    float m = smoothstep(0.03, -0.03, abs(u.x) - w) * step(-1.0, u.y);
    float3 c = mix(float3(0.78, 0.88, 1.0), float3(1.0, 1.0, 1.0), smoothstep(0.0, -w, u.x - w * 0.2) * 0.6);
    return float4(c * m * 0.85, m * 0.85);
}
static float4 drip(float2 u) {
    float m = max(circ(u, float2(0.0, 0.25), 0.55, 0.06), smoothstep(0.03, -0.03, abs(u.x) - 0.5 * saturate((u.y + 0.8) / 1.0)) * step(-0.8, u.y) * step(u.y, 0.25));
    return float4(float3(0.65, 0.82, 1.0) * m * 0.8, m * 0.8);
}
// A snow angel pressed into a pile (kind 17).
static float4 angelShape(float2 u) {
    float wings = max(oval(turn(u - float2(-0.35, -0.05), 0.5), float2(0.0, 0.0), float2(0.45, 0.22)),
                      oval(turn(u - float2(0.35, -0.05), -0.5), float2(0.0, 0.0), float2(0.45, 0.22)));
    float body = max(circ(u, float2(0.0, -0.45), 0.18, 0.03), max(oval(u, float2(0.0, 0.2), float2(0.28, 0.5)), wings));
    return float4(float3(0.5, 0.6, 0.82) * body * 0.8, body * 0.8);
}
// A Santa hat blown off a snowman, tumbling (kind 18).
static float4 hatShape(float2 u, float spin) {
    float2 p = turn(u, spin);
    float4 o = float4(0.0, 0.0, 0.0, 0.0);
    float2 ha = float2(0.0, 0.3), hb = float2(0.45, -0.6);
    float2 pa = p - ha, ba = hb - ha;
    float t = saturate(dot(pa, ba) / dot(ba, ba));
    o = over(o, float3(0.85, 0.1, 0.12), smoothstep(0.03, -0.03, length(pa - ba * t) - mix(0.45, 0.08, t)));
    o = over(o, float3(0.98, 0.98, 1.0), max(box(p, float2(0.0, 0.35), float2(0.55, 0.13), 0.1, 0.03), circ(p, float2(0.48, -0.65), 0.17, 0.03)));
    return o;
}
// An autumn leaf (kind 19) and a blossom petal (kind 20), turning over as they fall.
static float4 leaf(float2 u, float ang, float time) {
    float flip = cos(time * 2.0 + ang * 3.0);
    float2 p = turn(u, ang + time * 1.5);
    p.x /= max(0.25, abs(flip));
    float m = min(circ(p, float2(0.45, 0.0), 0.75, 0.05), circ(p, float2(-0.45, 0.0), 0.75, 0.05));
    float h = fract(ang * 1.7);
    float3 c = h < 0.25 ? float3(0.95, 0.5, 0.1) : h < 0.5 ? float3(0.85, 0.2, 0.1) : h < 0.75 ? float3(0.95, 0.75, 0.2) : float3(0.6, 0.35, 0.15);
    c = mix(c * 0.7, c, smoothstep(0.0, 0.08, abs(p.x)));
    return float4(c * m, m);
}
static float4 petal(float2 u, float ang, float time) {
    float flip = cos(time * 2.2 + ang * 3.0);
    float2 p = turn(u, ang + time * 1.2);
    p.x /= max(0.3, abs(flip));
    float m = oval(p, float2(0.0, 0.05), float2(0.45, 0.7)) * (1.0 - circ(p, float2(0.0, -0.78), 0.2, 0.05));
    float3 c = mix(float3(1.0, 0.98, 0.99), float3(1.0, 0.7, 0.84), smoothstep(-0.6, 0.6, p.y));
    return float4(c * m, m);
}
// A present from Santa (kind 21): a box, a lid, a ribbon and a bow.
static float4 presentShape(float2 u, float colour) {
    float3 c = colour < 0.5 ? float3(0.85, 0.12, 0.15) : colour < 1.5 ? float3(0.1, 0.6, 0.3) : colour < 2.5 ? float3(0.2, 0.4, 0.9)
             : colour < 3.5 ? float3(0.95, 0.75, 0.15) : float3(0.6, 0.3, 0.85);
    float3 ribbon = colour < 3.5 && colour > 2.5 ? float3(0.85, 0.12, 0.15) : float3(1.0, 0.85, 0.3);
    float4 o = float4(0.0, 0.0, 0.0, 0.0);
    o = over(o, ribbon, max(smoothstep(0.06, 0.03, abs(length(u - float2(-0.2, -0.62)) - 0.13)), smoothstep(0.06, 0.03, abs(length(u - float2(0.2, -0.62)) - 0.13))));
    o = over(o, c * 0.85, box(u, float2(0.0, 0.2), float2(0.62, 0.5), 0.05, 0.03));
    o = over(o, c, box(u, float2(0.0, -0.36), float2(0.72, 0.13), 0.04, 0.03));
    o = over(o, ribbon, max(box(u, float2(0.0, 0.1), float2(0.1, 0.62), 0.0, 0.03), box(u, float2(0.0, -0.36), float2(0.72, 0.05), 0.0, 0.03)));
    return o;
}
// A fairy light (kind 22): a coloured bulb that twinkles, with a glow.
static float4 fairyLight(float2 u, float tag, float time) {
    float k = floor(tag);
    float3 c = k < 0.5 ? float3(1.0, 0.25, 0.25) : k < 1.5 ? float3(0.25, 1.0, 0.4) : k < 2.5 ? float3(0.3, 0.55, 1.0) : k < 3.5 ? float3(1.0, 0.8, 0.25) : float3(1.0, 0.45, 0.85);
    float tw = 0.6 + 0.4 * sin(time * 3.0 + (tag - k) * 6.2831);
    float4 o = float4(0.0, 0.0, 0.0, 0.0);
    o = over(o, c, exp(-dot(u - float2(0.0, 0.1), u - float2(0.0, 0.1)) * 5.0) * 0.6 * tw);
    o = over(o, mix(c, float3(1.0, 1.0, 1.0), 0.35 * tw), oval(u, float2(0.0, 0.12), float2(0.2, 0.3)));
    o = over(o, float3(0.15, 0.2, 0.15), box(u, float2(0.0, -0.22), float2(0.1, 0.07), 0.02, 0.02));
    return o;
}
// A firework spark (kind 11): a white-hot core in a coloured glow.
static float4 spark(float2 u, float hue) {
    float3 c = hue < 0.5 ? float3(1.0, 0.25, 0.3) : hue < 1.5 ? float3(1.0, 0.8, 0.25) : hue < 2.5 ? float3(0.3, 1.0, 0.45)
             : hue < 3.5 ? float3(0.35, 0.6, 1.0) : hue < 4.5 ? float3(1.0, 0.45, 0.8) : hue < 5.5 ? float3(0.7, 0.4, 1.0) : float3(1.0, 1.0, 1.0);
    float r = length(u);
    float core = smoothstep(0.35, 0.0, r);
    float a = saturate(core + exp(-r * r * 4.0) * 0.85) * smoothstep(1.0, 0.8, r);
    return float4(mix(c, float3(1.0, 1.0, 1.0), core * 0.7) * a, a);
}

fragment float4 flakeF(V in [[stage_in]], float2 pc [[point_coord]], constant U &u [[buffer(1)]]) {
    float2 q0 = (pc - 0.5) * 2.0;
    if (in.kind > 21.5) return fairyLight(q0, in.ang, u.time) * in.a;
    if (in.kind > 20.5) return presentShape(q0, in.ang) * in.a;
    if (in.kind > 19.5) return petal(q0, in.ang, u.time) * in.a;
    if (in.kind > 18.5) return leaf(q0, in.ang, u.time) * in.a;
    if (in.kind > 17.5) return hatShape(q0, in.ang) * in.a;
    if (in.kind > 16.5) return angelShape(q0) * in.a;
    if (in.kind > 15.5) return drip(q0) * in.a;
    if (in.kind > 14.5) return icicle(q0) * in.a;
    if (in.kind > 13.5) return animalShape(q0, in.ang, u.time) * in.a;
    if (in.kind > 12.5) return footprint(q0, in.ang) * in.a;
    if (in.kind > 11.5) return splatShape(q0, in.ang) * in.a;
    if (in.kind > 10.5) return spark(q0, in.ang) * in.a;
    if (in.kind > 9.5) return gem(q0, in.ang, u.time) * in.a;
    if (in.kind > 8.5) return reindeer(q0, in.ang, u.time) * in.a;
    if (in.kind > 7.5) return sleigh(q0) * in.a;
    if (in.kind > 6.5) return snowball(q0) * in.a;
    if (in.kind > 5.5) return plough(q0, in.ang, u.time) * in.a;
    if (in.kind > 4.5) return snowman(q0, in.ang, u.time) * in.a;
    float m = 0.0, alpha = in.a, rim = 0.0;
    if (alpha > 1.5) { alpha -= 2.0; rim = smoothstep(1.0, 0.5, length(q0)) * 0.55; }   // the show's dots
    float3 tint = float3(0.94, 0.97, 1.0);
    if (in.kind < 0.5) {                         // ball: a soft disc, drawn out along its path when it moves fast
        float k = mix(1.0, 0.6, saturate((in.speed - 140.0) / 360.0));
        float2 s = float2(dot(q0, in.dir), dot(q0, float2(-in.dir.y, in.dir.x)));
        m = smoothstep(1.0, 0.25, length(float2(s.x * k, s.y / k)));
        tint = mix(tint, float3(1.0, 1.0, 1.0), smoothstep(0.6, 0.0, length(q0 - float2(-0.3, -0.3))) * 0.35);
    } else if (in.kind < 1.5) {                  // snowflake: six arms, each with two pairs of branches
        float c = cos(in.ang), sn = sin(in.ang);
        float2 q = float2(q0.x * c - q0.y * sn, q0.x * sn + q0.y * c);
        float face = cos(u.time * 1.3 + in.ang);     // it turns over as it falls, and glints when it faces you
        q.x /= max(0.2, abs(face));
        float sector = 3.14159265 / 3.0;
        float a = abs(fmod(atan2(q.y, q.x) + 6.2831853 + sector * 0.5, sector) - sector * 0.5);
        float2 p = float2(cos(a), sin(a)) * length(q);
        float arm = p.x < 0.92 ? abs(p.y) : length(p - float2(0.92, 0.0));
        float dd = min(arm, min(segDist(p, float2(0.40, 0.0), float2(0.53, 0.22)), segDist(p, float2(0.66, 0.0), float2(0.76, 0.17))));
        m = smoothstep(0.20, 0.07, dd) * step(length(q), 1.0) * (1.0 + 0.8 * pow(abs(face), 12.0));
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
    tint = mix(tint, tint * u.tint.rgb, u.tint.a);         // a touch of the wallpaper's colour
    float a = m * alpha;
    return float4(tint * a + float3(0.04, 0.05, 0.12) * rim * (1.0 - a), a + rim * (1.0 - a));
}

vertex V pileV(uint i [[vertex_id]], constant float4 *p [[buffer(0)]], constant U &u [[buffer(1)]]) {
    V o; o.pos = clip(p[i], u); o.size = 1.0; o.a = p[i].z; o.shade = p[i].w; o.kind = 0.0; o.ang = 0.0; o.dir = float2(0.0, 0.0); o.speed = 0.0; return o;
}
fragment float4 pileF(V in [[stage_in]], constant U &u [[buffer(1)]]) {
    float a = in.a;
    // Snow, or a pile of autumn leaves, or of blossom; pink after the show; touched with the wallpaper's colour.
    float3 lit = u.season > 1.5 ? float3(1.0, 0.9, 0.94) : u.season > 0.5 ? float3(0.92, 0.62, 0.3) : float3(0.97, 0.985, 1.0);
    float3 shadow = u.season > 1.5 ? float3(0.9, 0.65, 0.78) : u.season > 0.5 ? float3(0.62, 0.32, 0.12) : float3(0.66, 0.76, 0.93);
    lit = mix(lit, float3(1.0, 0.8, 0.92), u.pink);
    shadow = mix(shadow, float3(0.95, 0.5, 0.76), u.pink);
    lit = mix(lit, lit * u.tint.rgb, u.tint.a);
    shadow = mix(shadow, shadow * u.tint.rgb, u.tint.a);
    return float4(mix(lit, shadow, in.shade) * a, a);
}
"""



struct Uniforms { var origin: SIMD2<Float>; var size: SIMD2<Float>; var t: Float; var time: Float = 0; var scale: Float = 1; var pink: Float = 0
    var season: Float = 0; var pad1: Float = 0, pad2: Float = 0, pad3: Float = 0; var tint: SIMD4<Float> = .zero }

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
                season: Float = 0, tint: SIMD4<Float> = .zero,
                piles: MTLBuffer?, pileCount: Int, flakes: MTLBuffer?, flakeCount: Int) {
        var u = Uniforms(origin: origin, size: size, t: t, time: time, scale: scale, pink: pink, season: season, tint: tint)
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
