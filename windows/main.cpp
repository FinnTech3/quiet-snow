// Quiet Snow for Windows: a tray icon and one click-through overlay per display, drawn with Direct3D 11 into a
// DirectComposition swap chain. The snow itself lives in sim.hpp.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <dxgi1_3.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <mmsystem.h>
#include <wincodec.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wtsapi32.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <cstring>
#include <map>
#include <thread>
#include <string>
#include <vector>

#include "sim.hpp"
#include "sound.hpp"
#include "version.h"

#ifdef _MSC_VER
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#endif

static const wchar_t* kVersion = QS_VERSION_W;

// ---------------------------------------------------------------- shaders

static const char* kShader = R"HLSL(
#define mix lerp
#define fract frac
cbuffer U : register(b0) { float2 origin; float2 size; float t; float time; float pink; float season; float4 U_tint; };

struct FlakeIn { float4 a : A; float4 b : B; uint vid : SV_VertexID; };
struct V { float4 pos : SV_Position; float2 uv : UV; float alpha : AL; float shade : SH; float kind : K; float ang : AN; float3 vel : VL; };

// kind: 0 ball, 1 snowflake, 2 sleet, 3 sparkle, 4 hail (the SnowType values in sim.hpp); 5 snowman, 6 plough, 7 snowball, 8 sleigh, 9 reindeer.
V flakeV(FlakeIn i) {
    float2 corner = float2((i.vid & 1) ? 1.0 : -1.0, (i.vid & 2) ? 1.0 : -1.0);
    const float kind = i.b.z;
    const float d = i.a.z * (kind == 1.0 ? 2.4 : kind == 3.0 ? 2.0 : kind == 4.0 ? 0.8 : kind == 19.0 ? 3.0 : kind == 20.0 ? 2.4 : 1.0);
    float2 off = corner * 0.5 * d;
    if (kind == 2.0) {   // sleet is a streak along the way the flake is travelling
        float2 dir = normalize(i.b.xy + float2(0.0001, 0.0001));
        float2 perp = float2(-dir.y, dir.x);
        off = dir * corner.x * 0.5 * d * 3.2 + perp * corner.y * 0.5 * d * 0.4;
    }
    float2 p = i.a.xy + i.b.xy * t + off - origin;
    float2 q = p / size;
    V o;
    o.pos = float4(q.x * 2.0 - 1.0, 1.0 - q.y * 2.0, 0.0, 1.0);
    o.uv = corner;
    o.alpha = i.a.w;
    o.shade = 0.0;
    o.kind = kind;
    o.ang = i.b.w;
    o.vel = float3(normalize(i.b.xy + float2(0.0001, 0.0001)), length(i.b.xy));
    return o;
}

float segDist(float2 p, float2 a, float2 b) {
    float2 pa = p - a, ba = b - a;
    return length(pa - ba * saturate(dot(pa, ba) / dot(ba, ba)));
}

// Snowman (5), plough (6), snowball (7), sleigh (8) and reindeer (9): drawn from shapes, premultiplied colour out.
float circ(float2 p, float2 c, float r, float aa) { return smoothstep(r + aa, r - aa, length(p - c)); }
float box(float2 p, float2 c, float2 h, float rr, float aa) {
    float2 q = abs(p - c) - h + rr;
    return smoothstep(aa, -aa, length(max(q, float2(0.0, 0.0))) + min(max(q.x, q.y), 0.0) - rr);
}
float oval(float2 p, float2 c, float2 r) { return smoothstep(1.06, 0.94, length((p - c) / r)); }
float stroke(float2 p, float2 a, float2 b, float w) { return smoothstep(w + 0.02, w - 0.02, segDist(p, a, b)); }
float4 over(float4 dst, float3 c, float m) { return float4(mix(dst.rgb, c, m), mix(dst.a, 1.0, m)); }
float3 snowShade(float2 p, float k) {
    return mix(float3(0.97, 0.98, 1.0), float3(0.70, 0.78, 0.92), saturate(dot(p, float2(0.6, 0.8)) * k + 0.2));
}
float2 turn(float2 p, float a) { float c = cos(a), s = sin(a); return float2(p.x * c - p.y * s, p.x * s + p.y * c); }
float4 snowman(float2 u, float tag, float time) {
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
float4 plough(float2 u, float dir, float time) {
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
float4 snowball(float2 u) {
    float m = smoothstep(1.0, 0.86, length(u));
    return float4(snowShade(u, 0.9) * m, m);
}
// Santa in his sleigh with a sack of presents, flying right.
float4 sleigh(float2 u) {
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
float4 reindeer(float2 u, float tag, float time) {
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
float4 gem(float2 u, float tag, float time) {
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
float4 splatShape(float2 u, float ang) {
    float2 p = turn(u, ang);
    float a = atan2(p.y, p.x);
    float edge = 0.66 + 0.14 * sin(a * 5.0) + 0.07 * sin(a * 11.0 + 1.0);
    float m = max(smoothstep(edge, edge - 0.08, length(p)), max(circ(u, float2(0.08, 0.62), 0.12, 0.04), circ(u, float2(0.08, 0.86), 0.08, 0.04)));
    return float4(snowShade(u, 0.8) * m, m);
}
// A footprint in the snow (kind 13): a fox's pad and toes, or a robin's three-toed print.
float4 footprint(float2 u, float tag) {
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
float4 animalShape(float2 u, float tag, float time) {
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
float4 icicle(float2 u) {
    float w = 0.34 * (1.0 - (u.y + 1.0) * 0.5);
    float m = smoothstep(0.03, -0.03, abs(u.x) - w) * step(-1.0, u.y);
    float3 c = mix(float3(0.78, 0.88, 1.0), float3(1.0, 1.0, 1.0), smoothstep(0.0, -w, u.x - w * 0.2) * 0.6);
    return float4(c * m * 0.85, m * 0.85);
}
float4 drip(float2 u) {
    float m = max(circ(u, float2(0.0, 0.25), 0.55, 0.06), smoothstep(0.03, -0.03, abs(u.x) - 0.5 * saturate((u.y + 0.8) / 1.0)) * step(-0.8, u.y) * step(u.y, 0.25));
    return float4(float3(0.65, 0.82, 1.0) * m * 0.8, m * 0.8);
}
// A snow angel pressed into a pile (kind 17).
float4 angelShape(float2 u) {
    float wings = max(oval(turn(u - float2(-0.35, -0.05), 0.5), float2(0.0, 0.0), float2(0.45, 0.22)),
                      oval(turn(u - float2(0.35, -0.05), -0.5), float2(0.0, 0.0), float2(0.45, 0.22)));
    float body = max(circ(u, float2(0.0, -0.45), 0.18, 0.03), max(oval(u, float2(0.0, 0.2), float2(0.28, 0.5)), wings));
    return float4(float3(0.5, 0.6, 0.82) * body * 0.8, body * 0.8);
}
// A Santa hat blown off a snowman, tumbling (kind 18).
float4 hatShape(float2 u, float spin) {
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
float4 leaf(float2 u, float ang, float time) {
    float flip = cos(time * 2.0 + ang * 3.0);
    float2 p = turn(u, ang + time * 1.5);
    p.x /= max(0.25, abs(flip));
    float m = min(circ(p, float2(0.45, 0.0), 0.75, 0.05), circ(p, float2(-0.45, 0.0), 0.75, 0.05));
    float h = fract(ang * 1.7);
    float3 c = h < 0.25 ? float3(0.95, 0.5, 0.1) : h < 0.5 ? float3(0.85, 0.2, 0.1) : h < 0.75 ? float3(0.95, 0.75, 0.2) : float3(0.6, 0.35, 0.15);
    c = mix(c * 0.7, c, smoothstep(0.0, 0.08, abs(p.x)));
    return float4(c * m, m);
}
float4 petal(float2 u, float ang, float time) {
    float flip = cos(time * 2.2 + ang * 3.0);
    float2 p = turn(u, ang + time * 1.2);
    p.x /= max(0.3, abs(flip));
    float m = oval(p, float2(0.0, 0.05), float2(0.45, 0.7)) * (1.0 - circ(p, float2(0.0, -0.78), 0.2, 0.05));
    float3 c = mix(float3(1.0, 0.98, 0.99), float3(1.0, 0.7, 0.84), smoothstep(-0.6, 0.6, p.y));
    return float4(c * m, m);
}
// A present from Santa (kind 21): a box, a lid, a ribbon and a bow.
float4 presentShape(float2 u, float colour) {
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
float4 fairyLight(float2 u, float tag, float time) {
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
float4 spark(float2 u, float hue) {
    float3 c = hue < 0.5 ? float3(1.0, 0.25, 0.3) : hue < 1.5 ? float3(1.0, 0.8, 0.25) : hue < 2.5 ? float3(0.3, 1.0, 0.45)
             : hue < 3.5 ? float3(0.35, 0.6, 1.0) : hue < 4.5 ? float3(1.0, 0.45, 0.8) : hue < 5.5 ? float3(0.7, 0.4, 1.0) : float3(1.0, 1.0, 1.0);
    float r = length(u);
    float core = smoothstep(0.35, 0.0, r);
    float a = saturate(core + exp(-r * r * 4.0) * 0.85) * smoothstep(1.0, 0.8, r);
    return float4(mix(c, float3(1.0, 1.0, 1.0), core * 0.7) * a, a);
}

float4 flakeP(V i) : SV_Target {
    const float2 u = i.uv;
    if (i.kind > 21.5) return fairyLight(u, i.ang, time) * i.alpha;
    if (i.kind > 20.5) return presentShape(u, i.ang) * i.alpha;
    if (i.kind > 19.5) return petal(u, i.ang, time) * i.alpha;
    if (i.kind > 18.5) return leaf(u, i.ang, time) * i.alpha;
    if (i.kind > 17.5) return hatShape(u, i.ang) * i.alpha;
    if (i.kind > 16.5) return angelShape(u) * i.alpha;
    if (i.kind > 15.5) return drip(u) * i.alpha;
    if (i.kind > 14.5) return icicle(u) * i.alpha;
    if (i.kind > 13.5) return animalShape(u, i.ang, time) * i.alpha;
    if (i.kind > 12.5) return footprint(u, i.ang) * i.alpha;
    if (i.kind > 11.5) return splatShape(u, i.ang) * i.alpha;
    if (i.kind > 10.5) return spark(u, i.ang) * i.alpha;
    if (i.kind > 9.5) return gem(u, i.ang, time) * i.alpha;
    if (i.kind > 8.5) return reindeer(u, i.ang, time) * i.alpha;
    if (i.kind > 7.5) return sleigh(u) * i.alpha;
    if (i.kind > 6.5) return snowball(u) * i.alpha;
    if (i.kind > 5.5) return plough(u, i.ang, time) * i.alpha;
    if (i.kind > 4.5) return snowman(u, i.ang, time) * i.alpha;
    float m = 0.0, alpha = i.alpha, rim = 0.0;
    if (alpha > 1.5) { alpha -= 2.0; rim = smoothstep(1.0, 0.5, length(u)) * 0.55; }   // the show's dots
    float3 tint = float3(0.94, 0.97, 1.0);
    if (i.kind < 0.5) {                      // ball: a soft disc, drawn out along its path when it moves fast
        const float k = lerp(1.0, 0.6, saturate((i.vel.z - 140.0) / 360.0));
        const float2 s = float2(dot(u, i.vel.xy), dot(u, float2(-i.vel.y, i.vel.x)));
        m = smoothstep(1.0, 0.25, length(float2(s.x * k, s.y / k)));
        tint = lerp(tint, float3(1.0, 1.0, 1.0), smoothstep(0.6, 0.0, length(u - float2(-0.3, -0.3))) * 0.35);
    } else if (i.kind < 1.5) {               // snowflake: six arms, each with two pairs of branches
        const float c = cos(i.ang), sn = sin(i.ang);
        float2 q = float2(u.x * c - u.y * sn, u.x * sn + u.y * c);
        const float face = cos(time * 1.3 + i.ang);   // it turns over as it falls, and glints when it faces you
        q.x /= max(0.2, abs(face));
        const float sector = 3.14159265 / 3.0;
        const float a = abs(fmod(atan2(q.y, q.x) + 6.2831853 + sector * 0.5, sector) - sector * 0.5);
        const float2 p = float2(cos(a), sin(a)) * length(q);
        const float arm = p.x < 0.92 ? abs(p.y) : length(p - float2(0.92, 0.0));
        const float dd = min(arm, min(segDist(p, float2(0.40, 0.0), float2(0.53, 0.22)), segDist(p, float2(0.66, 0.0), float2(0.76, 0.17))));
        m = smoothstep(0.20, 0.07, dd) * step(length(q), 1.0) * (1.0 + 0.8 * pow(abs(face), 12.0));
    } else if (i.kind < 2.5) {               // sleet: a streak with a brighter head
        m = smoothstep(1.0, 0.2, length(float2(u.x * 0.85, u.y))) * lerp(0.45, 1.0, saturate(u.x * 0.5 + 0.5));
        alpha *= 0.85;
    } else if (i.kind < 3.5) {               // sparkle: a four-point star that twinkles
        const float2 q = abs(u);
        const float s = sqrt(q.x) + sqrt(q.y);
        m = max(smoothstep(1.0, 0.55, s), smoothstep(0.4, 0.0, length(u)));
        m *= 0.55 + 0.45 * sin(time * 2.6 + i.ang * 5.0);
        tint = float3(1.0, 0.99, 0.93);
    } else {                                 // hail: a small hard pellet with a highlight
        m = smoothstep(1.0, 0.78, length(u));
        tint = lerp(float3(0.78, 0.87, 0.97), float3(1.0, 1.0, 1.0), smoothstep(0.6, 0.0, length(u - float2(-0.35, -0.35))));
        alpha = min(1.0, alpha * 1.6);
    }
    tint = lerp(tint, float3(1.0, 0.66, 0.86), pink);   // the snow turns pink after the show
    tint = lerp(tint, tint * U_tint.rgb, U_tint.a);       // a touch of the wallpaper's colour
    const float a = m * alpha;
    return float4(tint * a + float3(0.04, 0.05, 0.12) * rim * (1.0 - a), a + rim * (1.0 - a));
}

struct PileIn { float4 v : P; };
V pileV(PileIn i) {
    float2 q = (i.v.xy - origin) / size;
    V o;
    o.pos = float4(q.x * 2.0 - 1.0, 1.0 - q.y * 2.0, 0.0, 1.0);
    o.uv = float2(0, 0);
    o.alpha = i.v.z;
    o.shade = i.v.w;
    o.kind = 0; o.ang = 0; o.vel = float3(0, 0, 0);
    return o;
}
float4 pileP(V i) : SV_Target {
    // Snow, or a pile of autumn leaves, or of blossom; pink after the show; touched with the wallpaper's colour.
    float3 lit = season > 1.5 ? float3(1.0, 0.9, 0.94) : season > 0.5 ? float3(0.92, 0.62, 0.3) : float3(0.97, 0.985, 1.0);
    float3 shadow = season > 1.5 ? float3(0.9, 0.65, 0.78) : season > 0.5 ? float3(0.62, 0.32, 0.12) : float3(0.66, 0.76, 0.93);
    lit = lerp(lit, float3(1.0, 0.8, 0.92), pink);
    shadow = lerp(shadow, float3(0.95, 0.5, 0.76), pink);
    lit = lerp(lit, lit * U_tint.rgb, U_tint.a);
    shadow = lerp(shadow, shadow * U_tint.rgb, U_tint.a);
    return float4(lerp(lit, shadow, i.shade) * i.alpha, i.alpha);
}
)HLSL";

struct Constants { float origin[2]; float size[2]; float t; float time; float pink; float season; float tint[4]; };

// ---------------------------------------------------------------- GPU

template <class T> static void release(T*& p) { if (p) { p->Release(); p = nullptr; } }

struct Gpu {
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    IDXGIDevice* dxgi = nullptr;
    IDXGIFactory2* factory = nullptr;
    IDCompositionDevice* dcomp = nullptr;
    ID3D11VertexShader *flakeVS = nullptr, *pileVS = nullptr;
    ID3D11PixelShader *flakePS = nullptr, *pilePS = nullptr;
    ID3D11InputLayout *flakeLayout = nullptr, *pileLayout = nullptr;
    ID3D11BlendState* blend = nullptr;
    ID3D11RasterizerState* raster = nullptr;
    ID3D11Buffer* cb = nullptr;
    ID3D11Buffer *flakeBuf = nullptr, *pileBuf = nullptr;
    UINT flakeCap = 0, pileCap = 0;
    bool warp = false;

    bool init(bool needDcomp) {
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
        D3D_FEATURE_LEVEL got;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 3, D3D11_SDK_VERSION, &dev, &got, &ctx);
        if (FAILED(hr)) {
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels, 3, D3D11_SDK_VERSION, &dev, &got, &ctx);
            warp = true;
        }
        if (FAILED(hr)) return false;
        if (FAILED(dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgi))) return false;
        if (needDcomp) {
            IDXGIAdapter* adapter = nullptr;
            if (FAILED(dxgi->GetAdapter(&adapter))) return false;
            hr = adapter->GetParent(__uuidof(IDXGIFactory2), (void**)&factory);
            adapter->Release();
            if (FAILED(hr)) return false;
            if (FAILED(DCompositionCreateDevice(dxgi, __uuidof(IDCompositionDevice), (void**)&dcomp))) return false;
        }
        return buildPipeline();
    }

    bool buildPipeline() {
        auto compile = [&](const char* entry, const char* target, ID3DBlob** out) {
            ID3DBlob* err = nullptr;
            HRESULT hr = D3DCompile(kShader, std::strlen(kShader), nullptr, nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out, &err);
            if (err) err->Release();
            return SUCCEEDED(hr);
        };
        ID3DBlob *fv = nullptr, *fp = nullptr, *pv = nullptr, *pp = nullptr;
        if (!compile("flakeV", "vs_4_0", &fv) || !compile("flakeP", "ps_4_0", &fp) ||
            !compile("pileV", "vs_4_0", &pv) || !compile("pileP", "ps_4_0", &pp)) return false;
        bool ok = SUCCEEDED(dev->CreateVertexShader(fv->GetBufferPointer(), fv->GetBufferSize(), nullptr, &flakeVS)) &&
                  SUCCEEDED(dev->CreatePixelShader(fp->GetBufferPointer(), fp->GetBufferSize(), nullptr, &flakePS)) &&
                  SUCCEEDED(dev->CreateVertexShader(pv->GetBufferPointer(), pv->GetBufferSize(), nullptr, &pileVS)) &&
                  SUCCEEDED(dev->CreatePixelShader(pp->GetBufferPointer(), pp->GetBufferSize(), nullptr, &pilePS));
        if (ok) {
            D3D11_INPUT_ELEMENT_DESC fl[] = {
                {"A", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1},
                {"B", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_INSTANCE_DATA, 1},
            };
            ok = SUCCEEDED(dev->CreateInputLayout(fl, 2, fv->GetBufferPointer(), fv->GetBufferSize(), &flakeLayout));
            D3D11_INPUT_ELEMENT_DESC pl[] = {{"P", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0}};
            ok = ok && SUCCEEDED(dev->CreateInputLayout(pl, 1, pv->GetBufferPointer(), pv->GetBufferSize(), &pileLayout));
        }
        fv->Release(); fp->Release(); pv->Release(); pp->Release();
        if (!ok) return false;

        D3D11_BLEND_DESC bd = {};
        auto& rt = bd.RenderTarget[0];
        rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D11_BLEND_ONE; rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA; rt.BlendOp = D3D11_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D11_BLEND_ONE; rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA; rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(dev->CreateBlendState(&bd, &blend))) return false;
        D3D11_RASTERIZER_DESC rd = {};
        rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
        if (FAILED(dev->CreateRasterizerState(&rd, &raster))) return false;
        D3D11_BUFFER_DESC cbd = {};
        cbd.ByteWidth = sizeof(Constants); cbd.Usage = D3D11_USAGE_DYNAMIC;
        cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        return SUCCEEDED(dev->CreateBuffer(&cbd, nullptr, &cb));
    }

    // Copy `data` into a dynamic vertex buffer, growing it when needed.
    bool upload(ID3D11Buffer*& buf, UINT& cap, const std::vector<float>& data) {
        const UINT bytes = (UINT)(data.size() * sizeof(float));
        if (bytes == 0) return false;
        if (!buf || cap < bytes) {
            release(buf);
            cap = std::max(bytes * 2, 4096u);
            D3D11_BUFFER_DESC d = {};
            d.ByteWidth = cap; d.Usage = D3D11_USAGE_DYNAMIC;
            d.BindFlags = D3D11_BIND_VERTEX_BUFFER; d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(dev->CreateBuffer(&d, nullptr, &buf))) { cap = 0; return false; }
        }
        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(ctx->Map(buf, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
        std::memcpy(m.pData, data.data(), bytes);
        ctx->Unmap(buf, 0);
        return true;
    }

    // ox, oy and the size are in points (pixels divided by `scale`); the viewport is in pixels.
    void draw(ID3D11RenderTargetView* rtv, UINT w, UINT h, float ox, float oy, float scale, float t, float time, float pink, float season,
              const float* tint, int pileN, int flakeN) {
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            Constants c = {{ox, oy}, {(float)w / scale, (float)h / scale}, t, time, pink, season, {tint[0], tint[1], tint[2], tint[3]}};
            std::memcpy(m.pData, &c, sizeof c);
            ctx->Unmap(cb, 0);
        }
        const float clear[4] = {0, 0, 0, 0};
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ctx->ClearRenderTargetView(rtv, clear);
        D3D11_VIEWPORT vp = {0, 0, (float)w, (float)h, 0, 1};
        ctx->RSSetViewports(1, &vp);
        ctx->RSSetState(raster);
        ctx->OMSetBlendState(blend, nullptr, 0xffffffff);
        ctx->VSSetConstantBuffers(0, 1, &cb);
        ctx->PSSetConstantBuffers(0, 1, &cb);   // the pixel shaders read the time (sparkles, steam, gallops) and the pink
        if (pileN > 0 && pileBuf) {
            UINT stride = 16, off = 0;
            ctx->IASetInputLayout(pileLayout);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx->IASetVertexBuffers(0, 1, &pileBuf, &stride, &off);
            ctx->VSSetShader(pileVS, nullptr, 0);
            ctx->PSSetShader(pilePS, nullptr, 0);
            ctx->Draw((UINT)pileN, 0);
        }
        if (flakeN > 0 && flakeBuf) {
            UINT stride = 32, off = 0;
            ctx->IASetInputLayout(flakeLayout);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
            ctx->IASetVertexBuffers(0, 1, &flakeBuf, &stride, &off);
            ctx->VSSetShader(flakeVS, nullptr, 0);
            ctx->PSSetShader(flakePS, nullptr, 0);
            ctx->DrawInstanced(4, (UINT)flakeN, 0, 0);
        }
    }
};

// ---------------------------------------------------------------- overlays

struct Overlay {
    HWND hwnd = nullptr;
    RECT mon{};          // monitor rectangle in virtual-screen pixels
    RECT work{};         // work area (monitor minus taskbar)
    IDXGISwapChain1* sc = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    IDCompositionTarget* target = nullptr;
    IDCompositionVisual* visual = nullptr;
    UINT w = 0, h = 0;
    bool shown = false;
    bool covered = false;   // desktop-only mode and an app is full screen on this display: nothing is drawn
    int clear = 0;          // polls in a row without a full screen app
};

static const wchar_t* kOverlayClass = L"QuietSnowOverlay";
static const wchar_t* kHostClass = L"QuietSnowHost";
static const UINT WM_TRAY = WM_APP + 1;

static Gpu g_gpu;
static qs::Sim g_sim;
static std::vector<Overlay> g_overlays;
static HWND g_host = nullptr;
static HICON g_icon = nullptr;
static UINT g_taskbarCreated = 0;

static bool g_paused = false;       // user chose Pause
static bool g_away = false;         // locked, display off or asleep
static float g_scale = 1;           // display scaling: the snow is measured in points, so it looks the same on every screen
static DWORD g_scope = 0;           // 0 over all apps and screens, 1 desktop only (snow hides behind windows)
static bool g_lowPower = false;
static bool g_presenting = false;   // Windows is in presentation mode: stay out of the way
static bool g_hotkey = false;       // Ctrl+Alt+Shift+S pauses and resumes, when the user turns it on
static bool g_sound = false;        // off until the user turns it on
static float g_tint[4] = {0, 0, 0, 0};   // the wallpaper's colour and how much of it the snow takes
static void playSound(qs::SoundEvent e);
static float g_radius = 0;          // window corner radius: 8 on Windows 11, square on Windows 10

// The real Windows version (GetVersionEx reports what the manifest declares, not what is running).
static OSVERSIONINFOW windowsVersion() {
    typedef LONG (WINAPI* RtlGetVersionFn)(OSVERSIONINFOW*);
    OSVERSIONINFOW vi = {sizeof vi};
    if (HMODULE nt = GetModuleHandleW(L"ntdll.dll"))
        if (auto fn = (RtlGetVersionFn)GetProcAddress(nt, "RtlGetVersion")) fn(&vi);
    return vi;
}
static std::vector<float> g_flakes, g_piles;
static int g_flakeN = 0, g_pileN = 0;
static double g_lastStep = 0, g_lastPoll = 0, g_lastSlow = 0, g_lastFrame = 0;
static int g_stepCount = 0;
static float g_glide = 0;

static double nowSeconds() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)freq.QuadPart;
}

// ---------------------------------------------------------------- settings

static const wchar_t* kKey = L"Software\\FinnTech3\\QuietSnow";
static DWORD getPref(const wchar_t* name, DWORD def) {
    DWORD v = def, sz = sizeof v;
    RegGetValueW(HKEY_CURRENT_USER, kKey, name, RRF_RT_REG_DWORD, nullptr, &v, &sz);
    return v;
}
static void setPref(const wchar_t* name, DWORD v) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE*)&v, sizeof v);
        RegCloseKey(k);
    }
}

static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static bool loginEnabled() {
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, L"QuietSnow", RRF_RT_REG_SZ, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}
static void setLogin(bool on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring cmd = L"\"" + std::wstring(path) + L"\"";
        RegSetValueExW(k, L"QuietSnow", 0, REG_SZ, (const BYTE*)cmd.c_str(), (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, L"QuietSnow");
    }
    RegCloseKey(k);
}

static float windValue() { return (float)(int)getPref(L"wind", 0) ; }   // stored as a signed value in a DWORD

// ---------------------------------------------------------------- overlays: create and destroy

// The overlays are click-through except while the cursor is over snow or a snowball: then they take the click, so you
// can scoop up a snowball and roll it. Only the window style changes; there is no mouse hook.
static bool g_interactive = false;
static POINT g_lastDrag = {};
static double g_lastDragAt = 0;
static float g_dragVx = 0, g_dragVy = 0;

static void setInteractive(bool on) {
    if (on == g_interactive) return;
    g_interactive = on;
    for (auto& o : g_overlays) {
        LONG_PTR ex = GetWindowLongPtrW(o.hwnd, GWL_EXSTYLE);
        ex = on ? (ex & ~(LONG_PTR)WS_EX_TRANSPARENT) : (ex | WS_EX_TRANSPARENT);
        SetWindowLongPtrW(o.hwnd, GWL_EXSTYLE, ex);
    }
}

static void dragTo(POINT p) {
    const double now = nowSeconds();
    const double dt = now - g_lastDragAt;
    if (dt > 0.001) {
        g_dragVx = (float)((p.x - g_lastDrag.x) / g_scale / dt);
        g_dragVy = (float)((p.y - g_lastDrag.y) / g_scale / dt);
    }
    g_lastDrag = p; g_lastDragAt = now;
    g_sim.drag(p.x / g_scale, p.y / g_scale);
}

static LRESULT CALLBACK overlayProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCHITTEST) return g_interactive ? HTCLIENT : HTTRANSPARENT;
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (m == WM_SETCURSOR && g_interactive) { SetCursor(LoadCursorW(nullptr, IDC_HAND)); return TRUE; }
    if (m == WM_LBUTTONDOWN) {
        POINT p; GetCursorPos(&p);
        if (g_sim.grab(p.x / g_scale, p.y / g_scale)) {
            SetCapture(h);
            g_lastDrag = p; g_lastDragAt = nowSeconds(); g_dragVx = g_dragVy = 0;
        }
        return 0;
    }
    if (m == WM_MOUSEMOVE && GetCapture() == h) { POINT p; GetCursorPos(&p); dragTo(p); return 0; }
    if (m == WM_LBUTTONUP && GetCapture() == h) { ReleaseCapture(); return 0; }
    if (m == WM_CAPTURECHANGED) {   // the button went up, or something took the mouse away: let the ball go
        if (nowSeconds() - g_lastDragAt > 0.1) g_dragVx = g_dragVy = 0;
        g_sim.drop(g_dragVx, g_dragVy);
        return 0;
    }
    if (m == WM_DPICHANGED) {   // keep covering the whole display exactly, whatever scaling Windows suggests
        for (auto& o : g_overlays) if (o.hwnd == h) SetWindowPos(h, nullptr, o.mon.left, o.mon.top, (int)o.w, (int)o.h, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

struct MonitorInfoLite { RECT mon, work; };
static BOOL CALLBACK enumMonitors(HMONITOR hm, HDC, LPRECT, LPARAM lp) {
    MONITORINFO mi = {sizeof mi};
    if (GetMonitorInfoW(hm, &mi)) reinterpret_cast<std::vector<MonitorInfoLite>*>(lp)->push_back({mi.rcMonitor, mi.rcWork});
    return TRUE;
}

static void destroyOverlays() {
    for (auto& o : g_overlays) {
        release(o.rtv); release(o.sc); release(o.visual); release(o.target);
        if (o.hwnd) DestroyWindow(o.hwnd);
    }
    g_overlays.clear();
    if (g_gpu.ctx) g_gpu.ctx->ClearState();
}

static bool createOverlay(const MonitorInfoLite& mi, Overlay& o) {
    o.mon = mi.mon; o.work = mi.work;
    o.w = (UINT)(mi.mon.right - mi.mon.left); o.h = (UINT)(mi.mon.bottom - mi.mon.top);
    o.hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_NOACTIVATE,
                             kOverlayClass, L"Quiet Snow", WS_POPUP, mi.mon.left, mi.mon.top, (int)o.w, (int)o.h,
                             nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!o.hwnd) return false;
    SetLayeredWindowAttributes(o.hwnd, 0, 255, LWA_ALPHA);
    SetWindowPos(o.hwnd, HWND_TOPMOST, mi.mon.left, mi.mon.top, (int)o.w, (int)o.h, SWP_NOACTIVATE);   // exact, whatever the scaling

    DXGI_SWAP_CHAIN_DESC1 d = {};
    d.Width = o.w; d.Height = o.h;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = 2;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    d.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    if (FAILED(g_gpu.factory->CreateSwapChainForComposition(g_gpu.dev, &d, nullptr, &o.sc))) return false;
    ID3D11Texture2D* back = nullptr;
    if (FAILED(o.sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back))) return false;
    HRESULT hr = g_gpu.dev->CreateRenderTargetView(back, nullptr, &o.rtv);
    back->Release();
    if (FAILED(hr)) return false;
    if (FAILED(g_gpu.dcomp->CreateTargetForHwnd(o.hwnd, TRUE, &o.target))) return false;
    if (FAILED(g_gpu.dcomp->CreateVisual(&o.visual))) return false;
    o.visual->SetContent(o.sc);
    o.target->SetRoot(o.visual);
    g_gpu.dcomp->Commit();
    ShowWindow(o.hwnd, SW_SHOWNOACTIVATE);
    o.shown = true;
    return true;
}

static bool buildOverlays() {
    destroyOverlays();
    g_interactive = false;   // new overlays start click-through
    g_scale = std::max(1.f, (float)GetDpiForSystem() / 96.f);
    std::vector<MonitorInfoLite> mons;
    EnumDisplayMonitors(nullptr, nullptr, enumMonitors, (LPARAM)&mons);
    std::vector<qs::Rect> world;
    for (const auto& m : mons) {
        Overlay o;
        if (!createOverlay(m, o)) { destroyOverlays(); return false; }
        g_overlays.push_back(o);
        world.push_back({m.mon.left / g_scale, m.mon.top / g_scale, (m.mon.right - m.mon.left) / g_scale, (m.mon.bottom - m.mon.top) / g_scale});
    }
    g_sim.setWorld(world);
    return !g_overlays.empty();
}

// ---------------------------------------------------------------- what is on screen

static void applyVisibility();

struct EnumCtx { std::vector<std::pair<int, qs::Rect>> rects; std::vector<qs::Rect> all; std::vector<qs::WindowRect> wins; std::vector<char> full; DWORD pid; };

// Collects every ordinary window (`all`, for desktop-only mode) and, of those, the ones snow can rest on (`rects`).
// Everything is in points: pixels divided by the display scaling.
static BOOL CALLBACK enumWindows(HWND hwnd, LPARAM lp) {
    auto* ctx = reinterpret_cast<EnumCtx*>(lp);
    if (ctx->all.size() >= 80) return FALSE;
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == ctx->pid) return TRUE;
    const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (ex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) return TRUE;
    int cloaked = 0;
    DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof cloaked);
    if (cloaked) return TRUE;
    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, 64);
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"Shell_TrayWnd") || !wcscmp(cls, L"Shell_SecondaryTrayWnd")) return TRUE;
    RECT r;
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof r))) GetWindowRect(hwnd, &r);
    for (size_t k = 0; k < g_overlays.size(); ++k) {   // a window that covers a whole display is a full screen app
        const RECT& m = g_overlays[k].mon;
        if (r.left <= m.left + 2 && r.top <= m.top + 2 && r.right >= m.right - 2 && r.bottom >= m.bottom - 2) ctx->full[k] = 1;
    }
    const qs::Rect rect{r.left / g_scale, r.top / g_scale, (r.right - r.left) / g_scale, (r.bottom - r.top) / g_scale};
    if (rect.w < 100 / g_scale || rect.h < 50 / g_scale) return TRUE;
    ctx->all.push_back(rect);
    ctx->wins.push_back({(int)(INT_PTR)hwnd, rect});   // for snowball splats and icicles
    if (IsZoomed(hwnd)) return TRUE;
    // A window pinned to the top and as wide as its screen is maximised or full screen: let the snow pass over it.
    for (const auto& m : g_sim.world()) {
        if (rect.w >= m.w * 0.9f && rect.y < m.y + 40 / g_scale && rect.x < m.maxX() && rect.maxX() > m.x) return TRUE;
    }
    if (ctx->rects.size() < 40) ctx->rects.push_back({(int)(INT_PTR)hwnd, rect});
    return TRUE;
}

// Polling is slow while nothing moves and fast for a moment after any window moves, which is what a drag looks like.
static double g_fastUntil = 0;
static std::vector<qs::Rect> g_lastAll;

static bool sameRect(const qs::Rect& a, const qs::Rect& b) { return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h; }

static void pollSurfaces() {
    EnumCtx ctx;
    ctx.pid = GetCurrentProcessId();
    ctx.full.assign(g_overlays.size(), 0);
    EnumWindows(enumWindows, (LPARAM)&ctx);          // front to back
    for (size_t k = 0; k < g_overlays.size(); ++k) { // desktop-only mode shows nothing on a display with a full screen app
        Overlay& o = g_overlays[k];
        if (g_scope == 1 && ctx.full[k]) { o.covered = true; o.clear = 0; }
        else if (o.covered && ++o.clear >= 2) o.covered = false;   // two clear polls in a row, so it cannot flicker
    }
    applyVisibility();
    bool moved = ctx.all.size() != g_lastAll.size();
    for (size_t i = 0; !moved && i < ctx.all.size(); ++i) moved = !sameRect(ctx.all[i], g_lastAll[i]);
    if (moved) g_fastUntil = nowSeconds() + 0.6;
    g_lastAll = ctx.all;
    // A window that is being dragged or resized stirs the snow around it, in the direction it moved.
    static std::map<int, qs::Rect> prev;
    static double prevAt = 0;
    const double at = nowSeconds();
    const double elapsed = at - prevAt;
    if (elapsed > 0.02 && elapsed < 0.5) {
        for (const auto& r : ctx.rects) {
            auto it = prev.find(r.first);
            if (it == prev.end()) continue;
            const float dx = r.second.x - it->second.x, dy = r.second.y - it->second.y;
            if (std::abs(dx) + std::abs(dy) >= 2 && std::abs(r.second.w - it->second.w) < 1 && std::abs(r.second.h - it->second.h) < 1)
                g_sim.stir(r.second, dx / (float)elapsed, dy / (float)elapsed);
        }
    }
    prev.clear();
    for (const auto& r : ctx.rects) prev[r.first] = r.second;
    prevAt = at;
    std::vector<qs::SurfaceInput> inputs;
    for (size_t i = 0; i < ctx.rects.size(); ++i) {
        qs::SurfaceInput in;
        in.id = ctx.rects[i].first;
        in.rect = {ctx.rects[i].second.x, ctx.rects[i].second.y, ctx.rects[i].second.w, 0};
        in.radius = g_radius;
        for (size_t k = 0; k < i; ++k) in.occluders.push_back(ctx.rects[k].second);
        inputs.push_back(std::move(in));
    }
    // The floor of each display: the top of the taskbar when it sits at the bottom, else the screen edge. The work area
    // is read fresh each time, so moving or resizing the taskbar moves the floor with it.
    for (size_t i = 0; i < g_overlays.size(); ++i) {
        const RECT& m = g_overlays[i].mon;
        bool stacked = false;
        for (size_t k = 0; k < g_overlays.size(); ++k) {
            if (k == i) continue;
            const RECT& o = g_overlays[k].mon;
            if (std::abs(o.top - m.bottom) < 2 && o.left < m.right && o.right > m.left) stacked = true;
        }
        if (stacked) continue;
        MONITORINFO mi = {sizeof mi};
        const LONG bottom = GetMonitorInfoW(MonitorFromRect(&m, MONITOR_DEFAULTTONEAREST), &mi) ? mi.rcWork.bottom : g_overlays[i].work.bottom;
        qs::SurfaceInput f;
        f.id = -(int)(i + 1);
        f.rect = {m.left / g_scale, bottom / g_scale, (m.right - m.left) / g_scale, 0};
        f.isFloor = true;
        inputs.push_back(std::move(f));
    }
    g_sim.setSurfaces(inputs);
    g_sim.setOccluders(g_scope == 1 ? ctx.all : std::vector<qs::Rect>{});
    g_sim.setWindows(ctx.wins);
}

// ---------------------------------------------------------------- frames

static bool anyShown() { for (const auto& o : g_overlays) if (o.shown) return true; return false; }
static bool running() { return !g_paused && !g_away && !g_presenting && anyShown(); }

// Show or hide each overlay: hidden while the screen is locked or off, and on a display with a full screen app in
// desktop-only mode. A window is only touched when its state really changes.
static void applyVisibility() {
    for (auto& o : g_overlays) {
        const bool want = !g_away && !g_presenting && !o.covered;
        if (o.shown == want) continue;
        ShowWindow(o.hwnd, want ? SW_SHOWNOACTIVATE : SW_HIDE);
        o.shown = want;
    }
}

static void frame() {
    const double now = nowSeconds();
    const double interval = g_lowPower ? 1.0 / 20 : 1.0 / 30;
    if (now - g_lastStep >= interval - 0.004) {
        const float dt = (float)(now - g_lastStep);
        g_lastStep = now;
        ++g_stepCount;
        if (now - g_lastPoll >= (now < g_fastUntil ? 0.05 : 0.15)) { g_lastPoll = now; pollSurfaces(); }

        POINT p;
        GetCursorPos(&p);
        qs::Pointer ptr{p.x / g_scale, p.y / g_scale};
        g_sim.step(dt, &ptr);
        setInteractive(g_sim.snowAt(ptr.x, ptr.y));
        for (qs::SoundEvent e : g_sim.takeSounds()) if (g_sound) playSound(e);
        g_flakeN = g_sim.fillFlakes(g_flakes);
        g_gpu.upload(g_gpu.flakeBuf, g_gpu.flakeCap, g_flakes);
        g_pileN = g_sim.fillPiles(g_piles);            // every step, so piles move smoothly
        g_gpu.upload(g_gpu.pileBuf, g_gpu.pileCap, g_piles);
    }
    g_glide = (float)std::min(std::max(now - g_lastStep, 0.0), 0.07);
    for (auto& o : g_overlays) {
        if (!o.shown) continue;
        g_gpu.draw(o.rtv, o.w, o.h, o.mon.left / g_scale, o.mon.top / g_scale, g_scale, g_glide, (float)std::fmod(now, 1000.0), g_sim.pinkAmount(),
                   (float)g_sim.season(), g_tint, g_pileN, g_flakeN);
        o.sc->Present(0, 0);
    }
}

// ---------------------------------------------------------------- sound, wallpaper, the show video, and chores

static void playSound(qs::SoundEvent e) {
    static std::map<int, std::vector<uint8_t>> cache;   // PlaySound reads from this memory while it plays, so it stays
    std::vector<uint8_t>& w = cache[(int)e];
    if (w.empty()) w = qs::soundWav(e);
    PlaySoundW((LPCWSTR)w.data(), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

// The desktop wallpaper scaled to W by H, top-down BGRA. False when there is none or it cannot be read.
static bool loadWallpaper(int W, int H, std::vector<uint32_t>& out) {
    wchar_t path[MAX_PATH] = {};
    if (!SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, path, 0) || !path[0]) return false;
    IWICImagingFactory* f = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICBitmapScaler* sc = nullptr;
    IWICFormatConverter* cv = nullptr;
    bool ok = false;
    out.assign((size_t)W * H, 0);
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))) &&
        SUCCEEDED(f->CreateDecoderFromFilename(path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(f->CreateBitmapScaler(&sc)) &&
        SUCCEEDED(sc->Initialize(frame, (UINT)W, (UINT)H, WICBitmapInterpolationModeFant)) && SUCCEEDED(f->CreateFormatConverter(&cv)) &&
        SUCCEEDED(cv->Initialize(sc, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) &&
        SUCCEEDED(cv->CopyPixels(nullptr, (UINT)W * 4, (UINT)(W * H * 4), (BYTE*)out.data())))
        ok = true;
    for (IUnknown* u : {(IUnknown*)cv, (IUnknown*)sc, (IUnknown*)frame, (IUnknown*)dec, (IUnknown*)f}) if (u) u->Release();
    return ok;
}

// The wallpaper's average colour, brightened so the snow keeps its light, with its strength last.
static void wallpaperTint(float* tint) {
    std::vector<uint32_t> px;
    tint[0] = tint[1] = tint[2] = tint[3] = 0;
    if (!loadWallpaper(8, 8, px)) return;
    float r = 0, g = 0, b = 0;
    for (uint32_t p : px) { b += (float)(p & 255); g += (float)((p >> 8) & 255); r += (float)((p >> 16) & 255); }
    const float m = std::max(std::max(r, g), std::max(b, 1.f));
    tint[0] = 0.5f + 0.5f * r / m; tint[1] = 0.5f + 0.5f * g / m; tint[2] = 0.5f + 0.5f * b / m; tint[3] = 0.35f;
}

// Record the hidden show over the wallpaper: 20 seconds of snow settling, then the show, the fireworks and the pink
// snow, at 30 frames a second, as an H.264 MP4. Returns 0 on success.
static int writeCard(const std::wstring& path, bool useWallpaper) {
    const int W = 1280, H = 720;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Gpu gpu;
    if (!gpu.init(false)) { std::fprintf(stderr, "no Direct3D 11 device\n"); return 1; }
    std::vector<uint32_t> bg;
    if (!useWallpaper || !loadWallpaper(W, H, bg)) {
        bg.assign((size_t)W * H, 0);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const float t = (x / (float)W + y / (float)H) * 0.5f;
                bg[(size_t)y * W + x] = (255u << 24) | ((uint32_t)(26 + 40 * t) << 16) | ((uint32_t)(36 + 20 * t) << 8) | (uint32_t)(61 + 41 * t);
            }
    }
    qs::Sim sim(42);
    sim.setWeather(qs::Weather::Steady);
    sim.setExtras(true);
    sim.setWorld({{0, 0, (float)W, (float)H}});
    sim.setSurfaces({{-2, {0, (float)H, (float)W, 0}, {}, true}});
    for (int i = 0; i < 30 * 20; ++i) sim.step(1.f / 30);
    if (FAILED(MFStartup(MF_VERSION))) { std::fprintf(stderr, "no Media Foundation\n"); return 1; }
    IMFSinkWriter* writer = nullptr;
    IMFMediaType *outType = nullptr, *inType = nullptr;
    DWORD stream = 0;
    HRESULT hr = MFCreateSinkWriterFromURL(path.c_str(), nullptr, nullptr, &writer);
    if (SUCCEEDED(hr)) hr = MFCreateMediaType(&outType);
    if (SUCCEEDED(hr)) {
        outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        outType->SetUINT32(MF_MT_AVG_BITRATE, 8000000);
        outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(outType, MF_MT_FRAME_SIZE, W, H);
        MFSetAttributeRatio(outType, MF_MT_FRAME_RATE, 30, 1);
        MFSetAttributeRatio(outType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        hr = writer->AddStream(outType, &stream);
    }
    if (SUCCEEDED(hr)) hr = MFCreateMediaType(&inType);
    if (SUCCEEDED(hr)) {
        inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(inType, MF_MT_FRAME_SIZE, W, H);
        MFSetAttributeRatio(inType, MF_MT_FRAME_RATE, 30, 1);
        MFSetAttributeRatio(inType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        hr = writer->SetInputMediaType(stream, inType, nullptr);
    }
    if (SUCCEEDED(hr)) hr = writer->BeginWriting();
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = W; td.Height = H; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D *tex = nullptr, *stage = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    if (SUCCEEDED(hr) && (FAILED(gpu.dev->CreateTexture2D(&td, nullptr, &tex)) || FAILED(gpu.dev->CreateRenderTargetView(tex, nullptr, &rtv)))) hr = E_FAIL;
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (SUCCEEDED(hr) && FAILED(gpu.dev->CreateTexture2D(&td, nullptr, &stage))) hr = E_FAIL;
    std::vector<float> flakes, piles;
    const float noTint[4] = {0, 0, 0, 0};
    sim.startShow();
    int frame = 0, after = 0;
    float time = 0;
    while (SUCCEEDED(hr) && after < 30 * 6 && frame < 30 * 45) {   // the show, then six seconds of pink snow
        sim.step(1.f / 30);
        time += 1.f / 30;
        if (!sim.showActive()) ++after;
        const int fn = sim.fillFlakes(flakes), pn = sim.fillPiles(piles);
        gpu.upload(gpu.flakeBuf, gpu.flakeCap, flakes);
        gpu.upload(gpu.pileBuf, gpu.pileCap, piles);
        gpu.draw(rtv, W, H, 0, 0, 1.f, 0, time, sim.pinkAmount(), (float)sim.season(), noTint, pn, fn);
        gpu.ctx->CopyResource(stage, tex);
        D3D11_MAPPED_SUBRESOURCE mp;
        if (FAILED(gpu.ctx->Map(stage, 0, D3D11_MAP_READ, 0, &mp))) { hr = E_FAIL; break; }
        IMFMediaBuffer* buf = nullptr;
        IMFSample* sample = nullptr;
        BYTE* dst = nullptr;
        hr = MFCreateMemoryBuffer((DWORD)(W * H * 4), &buf);
        if (SUCCEEDED(hr)) hr = buf->Lock(&dst, nullptr, nullptr);
        if (SUCCEEDED(hr)) {
            for (int y = 0; y < H; ++y) {   // RGB32 samples are bottom-up
                const uint32_t* src = (const uint32_t*)((const uint8_t*)mp.pData + (size_t)y * mp.RowPitch);
                uint32_t* row = (uint32_t*)dst + (size_t)(H - 1 - y) * W;
                const uint32_t* back = &bg[(size_t)y * W];
                for (int x = 0; x < W; ++x) {
                    const uint32_t sp = src[x], bp = back[x];
                    const float sa = ((sp >> 24) & 255) / 255.f;
                    auto ch = [&](int sh) { return (uint32_t)std::min(255.f, ((sp >> sh) & 255) + ((bp >> sh) & 255) * (1 - sa)); };
                    row[x] = (255u << 24) | (ch(16) << 16) | (ch(8) << 8) | ch(0);
                }
            }
            buf->Unlock();
            buf->SetCurrentLength((DWORD)(W * H * 4));
            hr = MFCreateSample(&sample);
        }
        gpu.ctx->Unmap(stage, 0);
        if (SUCCEEDED(hr)) hr = sample->AddBuffer(buf);
        if (SUCCEEDED(hr)) hr = sample->SetSampleTime((LONGLONG)frame * 333333);
        if (SUCCEEDED(hr)) hr = sample->SetSampleDuration(333333);
        if (SUCCEEDED(hr)) hr = writer->WriteSample(stream, sample);
        if (sample) sample->Release();
        if (buf) buf->Release();
        ++frame;
    }
    if (SUCCEEDED(hr)) hr = writer->Finalize();
    for (IUnknown* u : {(IUnknown*)stage, (IUnknown*)rtv, (IUnknown*)tex, (IUnknown*)inType, (IUnknown*)outType, (IUnknown*)writer}) if (u) u->Release();
    MFShutdown();
    if (FAILED(hr)) { std::fprintf(stderr, "card failed: 0x%08lx after %d frames\n", (unsigned long)hr, frame); return 1; }
    std::printf("card: %d frames\n", frame);
    return 0;
}

// Saves the show as a video on the Desktop, in the background, then shows it in Explorer.
static void saveVideo() {
    PWSTR desk = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desk))) return;
    const std::wstring path = std::wstring(desk) + L"\\Quiet Snow show.mp4";
    CoTaskMemFree(desk);
    std::thread([path] {
        if (writeCard(path, true) == 0) ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"/select,\"" + path + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
        else MessageBoxW(nullptr, L"The video could not be made.", L"Quiet Snow", MB_OK | MB_ICONINFORMATION);
    }).detach();
}

static void applySeason() {
    const DWORD pick = getPref(L"season", 0);
    qs::Season season = pick == 1 ? qs::Season::Autumn : pick == 2 ? qs::Season::Spring : qs::Season::Winter;
    if (pick == 3) {   // follow the calendar
        SYSTEMTIME st; GetLocalTime(&st);
        season = st.wMonth >= 3 && st.wMonth <= 5 ? qs::Season::Spring : st.wMonth >= 9 && st.wMonth <= 11 ? qs::Season::Autumn : qs::Season::Winter;
    }
    g_sim.setSeason(season);
}

// Every two seconds, with the other slow checks: idle blizzard, the season, the wallpaper's colour, New Year's Eve, and
// Santa's daily present.
static void chores() {
    LASTINPUTINFO li = {sizeof li};   // only when the last input was: nothing of what it was
    const bool idle = GetLastInputInfo(&li) && (GetTickCount() - li.dwTime) / 1000 >= 600;
    g_sim.setIdle(idle && running());
    if (getPref(L"season", 0) == 3) applySeason();
    static double lastTint = 0;
    if (getPref(L"tint", 0) && nowSeconds() - lastTint > 600) { lastTint = nowSeconds(); wallpaperTint(g_tint); }
    SYSTEMTIME st; GetLocalTime(&st);
    if (st.wMonth == 1 && st.wDay == 1 && st.wHour == 0 && st.wMinute < 2 && getPref(L"lastCelebration", 0) != st.wYear) {
        setPref(L"lastCelebration", st.wYear);
        g_sim.celebrate(90);   // Happy New Year
    }
    const DWORD day = (DWORD)st.wYear * 10000 + st.wMonth * 100 + st.wDay;
    if (running() && g_sim.extras() && getPref(L"lastPresent", 0) != day && g_sim.deliverPresent()) setPref(L"lastPresent", day);
}

// Slow checks, every two seconds: battery saver, and keeping the overlay above whatever has taken the top
// (the taskbar and the Start menu do) so the snow reaches the very edge of the screen.
static void slowChecks() {
    SYSTEM_POWER_STATUS sps = {};
    const bool low = GetSystemPowerStatus(&sps) && sps.SystemStatusFlag == 1;
    if (low != g_lowPower) { g_lowPower = low; g_sim.setDensityScale(low ? 0.6f : 1.f); }
    QUERY_USER_NOTIFICATION_STATE qn;
    const bool presenting = SUCCEEDED(SHQueryUserNotificationState(&qn)) && qn == QUNS_PRESENTATION_MODE;
    if (presenting != g_presenting) { g_presenting = presenting; applyVisibility(); if (running()) g_lastStep = nowSeconds(); }
    for (auto& o : g_overlays) {
        if (o.shown && GetWindow(o.hwnd, GW_HWNDPREV)) SetWindowPos(o.hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    chores();
}

// ---------------------------------------------------------------- tray

enum Id {
    ID_LIGHT = 1, ID_STEADY, ID_FLURRY,
    ID_WIND0, ID_WIND_R1, ID_WIND_L1, ID_WIND_R2, ID_WIND_L2,
    ID_MELT, ID_CLEAR, ID_PAUSE, ID_LOGIN, ID_QUIT, ID_SCOPE_ALL, ID_SCOPE_DESKTOP, ID_TYPE0,
    ID_HOTKEY = ID_TYPE0 + 6, ID_DIAG, ID_UPDATE, ID_RESET, ID_EXTRAS, ID_CODE,
    ID_LIGHTS, ID_TINT, ID_SOUND, ID_VIDEO, ID_SEASON0,
};

static const int kWindValues[5] = {0, 60, -60, 220, -220};

static void addTray() {
    NOTIFYICONDATAW n = {sizeof n};
    n.hWnd = g_host; n.uID = 1;
    n.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    n.uCallbackMessage = WM_TRAY;
    n.hIcon = g_icon ? g_icon : LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(n.szTip, L"Quiet Snow");
    Shell_NotifyIconW(NIM_ADD, &n);
}
static void removeTray() {
    NOTIFYICONDATAW n = {sizeof n};
    n.hWnd = g_host; n.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &n);
}

static void timerSync();


static const int kHotkeyId = 1;
static void applyHotkey() {
    UnregisterHotKey(g_host, kHotkeyId);
    if (g_hotkey) g_hotkey = RegisterHotKey(g_host, kHotkeyId, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, 'S') != 0;
}

static std::wstring diagnostics() {
    std::wstring t = L"Quiet Snow ";
    t += kVersion;
    t += L"\r\n";
    const OSVERSIONINFOW vi = windowsVersion();
    wchar_t b[256];
    swprintf_s(b, L"Windows %lu.%lu build %lu\r\n", vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber);
    t += b;
    swprintf_s(b, L"Adapter: %ls\r\n", g_gpu.warp ? L"software (WARP)" : L"hardware");
    t += b;
    swprintf_s(b, L"Scaling: %.2f\r\n", (double)g_scale);
    t += b;
    for (size_t i = 0; i < g_overlays.size(); ++i) {
        const RECT& m = g_overlays[i].mon;
        swprintf_s(b, L"Display %zu: %ld x %ld at %ld,%ld, %ls%ls\r\n", i + 1, m.right - m.left, m.bottom - m.top, m.left, m.top,
                   g_overlays[i].shown ? L"shown" : L"hidden", g_overlays[i].covered ? L" (full-screen app)" : L"");
        t += b;
    }
    static const wchar_t* weatherNames[3] = {L"Light", L"Steady", L"Flurry"};
    static const wchar_t* typeNames[6] = {L"Balls", L"Snowflakes", L"Sleet", L"Sparkles", L"Hail", L"Mix of all"};
    swprintf_s(b, L"Weather %ls, wind %d, snow type %ls, %ls\r\n", weatherNames[(int)g_sim.weather()], (int)g_sim.wind,
               typeNames[(int)g_sim.snowType()], g_scope == 1 ? L"desktop only" : L"over all apps");
    t += b;
    swprintf_s(b, L"Flakes %d, pile quads %d, paused %ls, presentation mode %ls, low power %ls\r\n", g_flakeN, g_pileN / 12,
               g_paused ? L"yes" : L"no", g_presenting ? L"yes" : L"no", g_lowPower ? L"yes" : L"no");
    t += b;
    swprintf_s(b, L"Snow toys %ls: %d snowmen, %d snowballs, plough %ls, sleigh %ls, window corners %.0f\r\n", g_sim.extras() ? L"on" : L"off",
               g_sim.snowmanCount(), g_sim.snowballCount(), g_sim.ploughActive() ? L"out" : L"away", g_sim.sleighActive() ? L"out" : L"away", (double)g_radius);
    t += b;
    return t;
}

static void copyText(const std::wstring& t) {
    if (!OpenClipboard(g_host)) return;
    EmptyClipboard();
    if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (t.size() + 1) * sizeof(wchar_t))) {
        if (void* p = GlobalLock(h)) {
            std::memcpy(p, t.c_str(), (t.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(h);
            if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);
        } else GlobalFree(h);
    }
    CloseClipboard();
}

// Back to the defaults a fresh install starts with.
static void resetSettings() {
    RegDeleteKeyW(HKEY_CURRENT_USER, kKey);
    g_sim.setWeather(qs::Weather::Steady);
    g_sim.wind = 0;
    g_sim.melts = false;
    g_sim.setSnowType(qs::SnowType::Balls);
    g_paused = false;
    g_scope = 0;
    g_hotkey = false;
    g_sim.setExtras(true);
    g_sim.lights = false;
    g_sound = false;
    g_tint[3] = 0;
    applySeason();
    applyHotkey();
    for (auto& o : g_overlays) o.covered = false;
    g_lastAll.clear();
    pollSurfaces();
    timerSync();
}

// A little box for a secret code. Only what is typed here is read: nothing listens to the keyboard anywhere else.
static std::wstring g_codeText;
static bool g_codeDone = false;
static LRESULT CALLBACK codeProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_COMMAND && (LOWORD(w) == IDOK || LOWORD(w) == IDCANCEL)) {
        if (LOWORD(w) == IDOK) {
            wchar_t buf[64] = {};
            GetWindowTextW(GetDlgItem(h, 100), buf, 64);
            g_codeText = buf;
        }
        DestroyWindow(h);
        return 0;
    }
    if (m == WM_CLOSE) { DestroyWindow(h); return 0; }
    if (m == WM_DESTROY) { g_codeDone = true; return 0; }
    return DefWindowProcW(h, m, w, l);
}

static std::wstring askForCode() {
    static bool registered = false;
    HINSTANCE hi = GetModuleHandleW(nullptr);
    if (!registered) {
        WNDCLASSEXW wc = {sizeof wc};
        wc.lpfnWndProc = codeProc; wc.hInstance = hi; wc.lpszClassName = L"QuietSnowCode";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.hIcon = g_icon;
        RegisterClassExW(&wc);
        registered = true;
    }
    const int W = 300, H = 140;
    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    HWND h = CreateWindowExW(WS_EX_TOPMOST | WS_EX_DLGMODALFRAME, L"QuietSnowCode", L"Quiet Snow", WS_POPUP | WS_CAPTION | WS_SYSMENU,
                             (wa.left + wa.right - W) / 2, (wa.top + wa.bottom - H) / 2, W, H, nullptr, nullptr, hi, nullptr);
    if (!h) return L"";
    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HWND label = CreateWindowExW(0, L"STATIC", L"Enter a code:", WS_CHILD | WS_VISIBLE, 14, 12, 260, 18, h, nullptr, hi, nullptr);
    HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_PASSWORD | ES_AUTOHSCROLL,
                                14, 34, 260, 24, h, (HMENU)100, hi, nullptr);
    HWND ok = CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 194, 68, 80, 26, h, (HMENU)IDOK, hi, nullptr);
    for (HWND c : {label, edit, ok}) SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
    g_codeText.clear();
    g_codeDone = false;
    ShowWindow(h, SW_SHOW);
    SetForegroundWindow(h);
    SetFocus(edit);
    MSG msg;
    while (!g_codeDone && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN && IsWindow(h)) { SendMessageW(h, WM_COMMAND, IDOK, 0); continue; }
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE && IsWindow(h)) { SendMessageW(h, WM_COMMAND, IDCANCEL, 0); continue; }
        if (!IsWindow(h) || !IsDialogMessageW(h, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    return g_codeText;
}

static void enterCode() {
    std::wstring t = askForCode();
    while (!t.empty() && t.back() == L' ') t.pop_back();
    while (!t.empty() && t.front() == L' ') t.erase(0, 1);
    for (auto& c : t) c = (wchar_t)towlower(c);
    if (t != L"daddy") return;
    if (g_paused) { g_paused = false; setPref(L"paused", 0); timerSync(); }
    setPref(L"unlocked", 1);   // from now on the menu offers to save the show as a video
    g_sim.startShow();
}

static void showMenu() {
    HMENU m = CreatePopupMenu();
    const qs::Weather w = g_sim.weather();
    AppendMenuW(m, MF_STRING | (w == qs::Weather::Light ? MF_CHECKED : 0), ID_LIGHT, L"Light");
    AppendMenuW(m, MF_STRING | (w == qs::Weather::Steady ? MF_CHECKED : 0), ID_STEADY, L"Steady");
    AppendMenuW(m, MF_STRING | (w == qs::Weather::Flurry ? MF_CHECKED : 0), ID_FLURRY, L"Flurry");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    const wchar_t* names[5] = {L"Still air", L"Breeze to the right", L"Breeze to the left", L"Gale to the right", L"Gale to the left"};
    for (int i = 0; i < 5; ++i) AppendMenuW(m, MF_STRING | ((int)g_sim.wind == kWindValues[i] ? MF_CHECKED : 0), ID_WIND0 + i, names[i]);
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    HMENU types = CreatePopupMenu();
    static const wchar_t* typeNames[6] = {L"Balls", L"Snowflakes", L"Sleet", L"Sparkles", L"Hail", L"Mix of all"};
    for (int i = 0; i < 6; ++i) AppendMenuW(types, MF_STRING | ((int)g_sim.snowType() == i ? MF_CHECKED : 0), ID_TYPE0 + i, typeNames[i]);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)types, L"Snow type");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (g_scope == 0 ? MF_CHECKED : 0), ID_SCOPE_ALL, L"Over all apps and screens");
    AppendMenuW(m, MF_STRING | (g_scope == 1 ? MF_CHECKED : 0), ID_SCOPE_DESKTOP, L"Desktop only");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (g_sim.extras() ? MF_CHECKED : 0), ID_EXTRAS, L"Snow toys: snowballs, snowmen, plough, sleigh");
    HMENU seasons = CreatePopupMenu();
    static const wchar_t* seasonNames[4] = {L"Winter snow", L"Autumn leaves", L"Spring blossom", L"Follow the calendar"};
    for (int i = 0; i < 4; ++i) AppendMenuW(seasons, MF_STRING | (getPref(L"season", 0) == (DWORD)i ? MF_CHECKED : 0), ID_SEASON0 + i, seasonNames[i]);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)seasons, L"Season");
    AppendMenuW(m, MF_STRING | (g_sim.lights ? MF_CHECKED : 0), ID_LIGHTS, L"Fairy lights");
    AppendMenuW(m, MF_STRING | (getPref(L"tint", 0) ? MF_CHECKED : 0), ID_TINT, L"Tint the snow with my wallpaper");
    AppendMenuW(m, MF_STRING | (g_sound ? MF_CHECKED : 0), ID_SOUND, L"Sound");
    AppendMenuW(m, MF_STRING | (g_sim.melts ? MF_CHECKED : 0), ID_MELT, L"Melt slowly");
    AppendMenuW(m, MF_STRING, ID_CLEAR, L"Clear piles");
    AppendMenuW(m, MF_STRING, ID_PAUSE, g_paused ? L"Resume" : L"Pause");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (loginEnabled() ? MF_CHECKED : 0), ID_LOGIN, L"Open at login");
    AppendMenuW(m, MF_STRING | (g_hotkey ? MF_CHECKED : 0), ID_HOTKEY, L"Pause with Ctrl+Alt+Shift+S");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_DIAG, L"Copy diagnostics");
    AppendMenuW(m, MF_STRING, ID_UPDATE, L"Check for updates");
    AppendMenuW(m, MF_STRING, ID_RESET, L"Reset settings");
    AppendMenuW(m, MF_STRING, ID_CODE, L"\u2744");   // a lone snowflake: the way in to the secret code
    if (getPref(L"unlocked", 0)) AppendMenuW(m, MF_STRING, ID_VIDEO, L"Save the show as a video");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_QUIT, L"Quit");
    POINT p;
    GetCursorPos(&p);
    SetForegroundWindow(g_host);
    const int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, p.x, p.y, 0, g_host, nullptr);
    PostMessageW(g_host, WM_NULL, 0, 0);
    DestroyMenu(m);
    switch (cmd) {
        case ID_LIGHT: g_sim.setWeather(qs::Weather::Light); setPref(L"weather", 0); break;
        case ID_STEADY: g_sim.setWeather(qs::Weather::Steady); setPref(L"weather", 1); break;
        case ID_FLURRY: g_sim.setWeather(qs::Weather::Flurry); setPref(L"weather", 2); break;
        case ID_WIND0: case ID_WIND_R1: case ID_WIND_L1: case ID_WIND_R2: case ID_WIND_L2: {
            const int v = kWindValues[cmd - ID_WIND0];
            g_sim.wind = (float)v; setPref(L"wind", (DWORD)v);
            break;
        }
        case ID_SCOPE_ALL: case ID_SCOPE_DESKTOP:
            g_scope = cmd == ID_SCOPE_DESKTOP ? 1 : 0; setPref(L"scope", g_scope);
            for (auto& o : g_overlays) o.covered = false;
            g_lastAll.clear(); pollSurfaces();
            break;
        case ID_TYPE0: case ID_TYPE0 + 1: case ID_TYPE0 + 2: case ID_TYPE0 + 3: case ID_TYPE0 + 4: case ID_TYPE0 + 5:
            g_sim.setSnowType((qs::SnowType)(cmd - ID_TYPE0)); setPref(L"snowType", (DWORD)(cmd - ID_TYPE0));
            break;
        case ID_MELT: g_sim.melts = !g_sim.melts; setPref(L"melt", g_sim.melts ? 1 : 0); break;
        case ID_CLEAR: g_sim.clearPiles(); break;
        case ID_PAUSE: g_paused = !g_paused; setPref(L"paused", g_paused ? 1 : 0); timerSync(); break;
        case ID_LOGIN: setLogin(!loginEnabled()); break;
        case ID_EXTRAS: g_sim.setExtras(!g_sim.extras()); setPref(L"extras", g_sim.extras() ? 1 : 0); break;
        case ID_HOTKEY: g_hotkey = !g_hotkey; applyHotkey(); setPref(L"hotkey", g_hotkey ? 1 : 0); break;
        case ID_DIAG: copyText(diagnostics()); break;
        case ID_UPDATE: ShellExecuteW(nullptr, L"open", L"https://github.com/FinnTech3/quiet-snow/releases/latest", nullptr, nullptr, SW_SHOWNORMAL); break;
        case ID_RESET: resetSettings(); break;
        case ID_CODE: enterCode(); break;
        case ID_VIDEO: saveVideo(); break;
        case ID_LIGHTS: g_sim.lights = !g_sim.lights; setPref(L"lights", g_sim.lights ? 1 : 0); break;
        case ID_TINT: setPref(L"tint", getPref(L"tint", 0) ? 0 : 1); if (getPref(L"tint", 0)) wallpaperTint(g_tint); else g_tint[3] = 0; break;
        case ID_SOUND: g_sound = !g_sound; setPref(L"sound", g_sound ? 1 : 0); break;
        case ID_SEASON0: case ID_SEASON0 + 1: case ID_SEASON0 + 2: case ID_SEASON0 + 3:
            setPref(L"season", (DWORD)(cmd - ID_SEASON0)); applySeason(); break;
        case ID_QUIT: PostQuitMessage(0); break;
    }
}

// ---------------------------------------------------------------- host window and pacing

static UINT_PTR kSlowTimer = 1;
static void timerSync() {
    if (running()) { g_lastStep = nowSeconds(); } else { setInteractive(false); }
    applyVisibility();
}

static HPOWERNOTIFY g_powerNote = nullptr;
// GUID_CONSOLE_DISPLAY_STATE, spelled out so no extra library is needed to link it.
static const GUID kConsoleDisplayState = {0x6fe69556, 0x704a, 0x47a0, {0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47}};

static LRESULT CALLBACK hostProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == g_taskbarCreated && m != 0) { addTray(); return 0; }
    switch (m) {
        case WM_TRAY:
            if (l == WM_RBUTTONUP || l == WM_LBUTTONUP || l == WM_CONTEXTMENU) showMenu();
            return 0;
        case WM_TIMER:
            if (w == kSlowTimer) { slowChecks(); if (!running()) pollSurfaces(); timerSync(); }
            return 0;
        case WM_WTSSESSION_CHANGE:
            if (w == WTS_SESSION_LOCK) { g_away = true; timerSync(); }
            else if (w == WTS_SESSION_UNLOCK) { g_away = false; timerSync(); }
            return 0;
        case WM_POWERBROADCAST:
            if (w == PBT_POWERSETTINGCHANGE) {
                auto* ps = (POWERBROADCAST_SETTING*)l;
                if (ps && ps->PowerSetting == kConsoleDisplayState && ps->DataLength >= sizeof(DWORD)) {
                    g_away = *(DWORD*)ps->Data == 0;   // 0 off, 1 on, 2 dimmed
                    timerSync();
                }
            } else if (w == PBT_APMSUSPEND) { g_away = true; timerSync(); }
            else if (w == PBT_APMRESUMEAUTOMATIC) { g_away = false; timerSync(); }
            return TRUE;
        case WM_DISPLAYCHANGE:
            if (g_gpu.dev) { buildOverlays(); pollSurfaces(); }
            return 0;
        case WM_HOTKEY:
            if (w == kHotkeyId) { g_paused = !g_paused; setPref(L"paused", g_paused ? 1 : 0); timerSync(); }
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// ---------------------------------------------------------------- snapshot (headless, for CI and the README)

#pragma pack(push, 1)
struct BmpHeader { uint16_t type = 0x4D42; uint32_t size; uint32_t reserved = 0; uint32_t offset = 54;
                   uint32_t hdr = 40; int32_t w; int32_t h; uint16_t planes = 1; uint16_t bpp = 32; uint32_t comp = 0;
                   uint32_t imgSize; int32_t xppm = 2835, yppm = 2835; uint32_t used = 0, important = 0; };
#pragma pack(pop)

// mode: 0 the plough, 1 mixed snow and the sleigh, 2 the hidden show's second words with fireworks, 3 the pink snow after it,
// 4 one of every snow toy, 5 autumn leaves.
static int snapshot(const wchar_t* path, int mode) {
    const bool mix = mode == 1;
    const int W = 1280, H = 800;
    Gpu gpu;
    if (!gpu.init(false)) { std::fprintf(stderr, "no Direct3D 11 device\n"); return 1; }
    qs::Sim sim(42);
    sim.setWeather(qs::Weather::Steady);
    if (mix) sim.setSnowType(qs::SnowType::Mixed);
    if (mode == 5) sim.setSeason(qs::Season::Autumn);
    sim.wind = 30;
    sim.setExtras(true);
    sim.setWorld({{0, 0, (float)W, (float)H}});
    const qs::Rect back{120, 150, 620, 420}, front{560, 300, 560, 340};
    const float dockTop = 742;
    sim.setWindows({{1, front}, {2, back}});
    // The drawn Dock, and the bare bottom edge of the screen either side of it, as on a Mac.
    sim.setSurfaces({{1, {front.x, front.y, front.w, 0}, {}, false, 8},
                     {2, {back.x, back.y, back.w, 0}, {front}, false, 8},
                     {-1, {340, dockTop, 600, 0}, {}, true, 16, false},
                     {-2, {0, (float)H, 340, 0}, {}, true},
                     {-3, {940, (float)H, (float)W - 940, 0}, {}, true}});
    sim.setWeather(qs::Weather::Flurry);
    for (int i = 0; i < 30 * 300; ++i) sim.step(1.f / 30);
    sim.setWeather(qs::Weather::Steady);
    for (int i = 0; i < 30 * 10; ++i) sim.step(1.f / 30);
    // Wait for the plough to be on its way to the Dock, so the picture shows it (it comes every one to two minutes).
    // The mixed picture shows Santa's sleigh instead.
    auto inShot = [&] { return mix ? sim.sleighActive() && sim.sleighX() > 520 && sim.sleighX() < 760
                                   : sim.ploughActive() && sim.ploughX() > 120 && sim.ploughX() < 260; };
    for (int i = 0; i < 30 * 200 && !inShot(); ++i) sim.step(1.f / 30);
    if (mode == 4) sim.stageForPicture();
    if (mode == 2 || mode == 3) {
        sim.startShow();
        for (int i = 0; i < 30 * 30 && (mode == 2 ? sim.showTime() < 12 : sim.showActive()); ++i) sim.step(1.f / 30);
        if (mode == 3) for (int i = 0; i < 30 * 3; ++i) sim.step(1.f / 30);
    }

    std::vector<float> flakes, piles;
    const int fn = sim.fillFlakes(flakes), pn = sim.fillPiles(piles);
    gpu.upload(gpu.flakeBuf, gpu.flakeCap, flakes);
    gpu.upload(gpu.pileBuf, gpu.pileCap, piles);

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = W; td.Height = H; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D *tex = nullptr, *stage = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    if (FAILED(gpu.dev->CreateTexture2D(&td, nullptr, &tex)) || FAILED(gpu.dev->CreateRenderTargetView(tex, nullptr, &rtv))) return 1;
    const float noTint[4] = {0, 0, 0, 0};
    gpu.draw(rtv, W, H, 0, 0, 1.f, 0, 0.8f, sim.pinkAmount(), (float)sim.season(), noTint, pn, fn);
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(gpu.dev->CreateTexture2D(&td, nullptr, &stage))) return 1;
    gpu.ctx->CopyResource(stage, tex);
    D3D11_MAPPED_SUBRESOURCE mp;
    if (FAILED(gpu.ctx->Map(stage, 0, D3D11_MAP_READ, 0, &mp))) return 1;

    // Composite the premultiplied snow over a drawn desktop, top-down BGRA.
    std::vector<uint32_t> out((size_t)W * H);
    auto lerpf = [](float a, float b, float t) { return a + (b - a) * t; };
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const float t = (x / (float)W + y / (float)H) * 0.5f;
            float r = lerpf(26, 66, t), g = lerpf(36, 56, t), b = lerpf(61, 102, t);
            auto inRect = [&](const qs::Rect& q) { return q.contains((float)x, (float)y); };
            if (inRect(back)) { const bool bar = y < back.y + 34; r = g = b = bar ? 224 : 245; if (!bar && (y - (int)back.y - 70) % 28 < 9 && y > back.y + 70 && x > back.x + 28 && x < back.x + back.w * 0.72f) r = g = b = 205; }
            if (inRect(front)) { const bool bar = y < front.y + 34; r = bar ? 51 : 28; g = bar ? 51 : 31; b = bar ? 51 : 38; if (!bar && (y - (int)front.y - 70) % 28 < 9 && y > front.y + 70 && x > front.x + 28 && x < front.x + front.w * 0.72f) { r = 70; g = 72; b = 78; } }
            if (y >= (int)dockTop + 8 && y < (int)dockTop + 58 && x > 340 && x < 940) { r = lerpf(r, 255, 0.16f); g = lerpf(g, 255, 0.16f); b = lerpf(b, 255, 0.16f); }
            const uint32_t s = *(uint32_t*)((uint8_t*)mp.pData + (size_t)y * mp.RowPitch + (size_t)x * 4);
            const float sa = ((s >> 24) & 255) / 255.f;
            const float sr = ((s >> 16) & 255), sg = ((s >> 8) & 255), sb = (s & 255);
            const float R = sr + r * (1 - sa), G = sg + g * (1 - sa), B = sb + b * (1 - sa);
            out[(size_t)y * W + x] = (255u << 24) | ((uint32_t)std::min(255.f, R) << 16) | ((uint32_t)std::min(255.f, G) << 8) | (uint32_t)std::min(255.f, B);
        }
    }
    gpu.ctx->Unmap(stage, 0);

    // BMP wants bottom-up rows.
    std::vector<uint32_t> flipped((size_t)W * H);
    for (int y = 0; y < H; ++y) std::memcpy(&flipped[(size_t)y * W], &out[(size_t)(H - 1 - y) * W], (size_t)W * 4);
    BmpHeader hd;
    hd.w = W; hd.h = H; hd.imgSize = (uint32_t)(W * H * 4); hd.size = 54 + hd.imgSize;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"wb") != 0 || !f) return 1;
    std::fwrite(&hd, sizeof hd, 1, f);
    std::fwrite(flipped.data(), 4, flipped.size(), f);
    std::fclose(f);
    std::printf("snapshot: %s adapter, %d flakes, %d pile quads, %d snowmen, plough %s, sleigh %s\n", gpu.warp ? "software (WARP)" : "hardware",
                fn, pn / 12, sim.snowmanCount(), sim.ploughActive() ? "on screen" : "away", sim.sleighActive() ? "on screen" : "away");
    return 0;
}

// ---------------------------------------------------------------- entry

// A GUI program has no console of its own: borrow the parent's unless output is already redirected.
static void attachConsole() {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!h || h == INVALID_HANDLE_VALUE) {
        AttachConsole(ATTACH_PARENT_PROCESS);
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
    }
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR cmdLine, int) {
    const std::wstring cmd = cmdLine ? cmdLine : L"";
    if (cmd.find(L"--version") != std::wstring::npos) {
        attachConsole();
        std::fwprintf(stdout, L"Quiet Snow %ls\n", kVersion);
        return 0;
    }
    const size_t cp = cmd.find(L"--card");
    if (cp != std::wstring::npos) {   // the show as a video, for CI: QuietSnow --card out.mp4
        attachConsole();
        std::wstring path = cmd.substr(cp + 6);
        while (!path.empty() && path[0] == L' ') path.erase(0, 1);
        while (!path.empty() && path.back() == L' ') path.pop_back();
        if (path.empty()) { std::fprintf(stderr, "usage: QuietSnow --card out.mp4\n"); return 2; }
        wchar_t full[MAX_PATH];
        GetFullPathNameW(path.c_str(), MAX_PATH, full, nullptr);
        return writeCard(full, false);
    }
    const size_t sp = cmd.find(L"--snapshot");
    if (sp != std::wstring::npos) {
        attachConsole();
        std::wstring path = cmd.substr(sp + 10);
        while (!path.empty() && path[0] == L' ') path.erase(0, 1);
        int mode = 0;
        const wchar_t* flags[5] = {L"--mix", L"--show", L"--pink", L"--toys", L"--autumn"};
        for (int f = 0; f < 5; ++f) {
            const size_t at = path.find(flags[f]);
            if (at != std::wstring::npos) { mode = f + 1; path = path.substr(0, at); }
        }
        while (!path.empty() && path.back() == L' ') path.pop_back();
        if (path.empty()) { std::fprintf(stderr, "usage: QuietSnow --snapshot out.bmp [--mix | --show | --pink | --toys | --autumn]\n"); return 2; }
        return snapshot(path.c_str(), mode);
    }

    HANDLE once = CreateMutexW(nullptr, TRUE, L"Local\\QuietSnowSingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    WNDCLASSEXW oc = {sizeof oc};
    oc.lpfnWndProc = overlayProc; oc.hInstance = hi; oc.lpszClassName = kOverlayClass;
    oc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&oc);
    WNDCLASSEXW hc = {sizeof hc};
    hc.lpfnWndProc = hostProc; hc.hInstance = hi; hc.lpszClassName = kHostClass;
    RegisterClassExW(&hc);

    if (!g_gpu.init(true)) {
        MessageBoxW(nullptr, L"Quiet Snow needs Direct3D 11 and Windows 10 or later.", L"Quiet Snow", MB_OK | MB_ICONINFORMATION);
        return 1;
    }
    g_host = CreateWindowExW(0, kHostClass, L"Quiet Snow", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hi, nullptr);
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    g_icon = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);

    // First run on a machine with animations switched off (Settings, Accessibility): start on the lightest weather.
    BOOL animations = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0);
    const DWORD weather = getPref(L"weather", animations ? 1 : 0);
    g_sim.setWeather(weather == 0 ? qs::Weather::Light : weather == 2 ? qs::Weather::Flurry : qs::Weather::Steady);
    g_sim.wind = windValue();
    g_sim.melts = getPref(L"melt", 0) != 0;
    g_paused = getPref(L"paused", 0) != 0;
    g_scope = getPref(L"scope", 0) == 1 ? 1 : 0;
    g_sim.setSnowType((qs::SnowType)std::min<DWORD>(getPref(L"snowType", 0), 5));

    g_hotkey = getPref(L"hotkey", 0) != 0;
    g_sim.setExtras(getPref(L"extras", 1) != 0);
    g_sim.lights = getPref(L"lights", 0) != 0;
    g_sound = getPref(L"sound", 0) != 0;
    applySeason();
    if (getPref(L"tint", 0)) wallpaperTint(g_tint);
    g_radius = windowsVersion().dwBuildNumber >= 22000 ? 8.f : 0.f;
    applyHotkey();
    if (!buildOverlays()) {
        MessageBoxW(nullptr, L"Quiet Snow could not create its overlay.", L"Quiet Snow", MB_OK | MB_ICONERROR);
        return 1;
    }
    pollSurfaces();
    addTray();
    WTSRegisterSessionNotification(g_host, NOTIFY_FOR_THIS_SESSION);
    g_powerNote = RegisterPowerSettingNotification(g_host, &kConsoleDisplayState, DEVICE_NOTIFY_WINDOW_HANDLE);
    SetTimer(g_host, kSlowTimer, 2000, nullptr);
    slowChecks();
    g_lastStep = nowSeconds();

    // The loop. While running, frames are paced by the desktop compositor (DwmFlush waits for its next pass), and a frame
    // is skipped when the last one was under 13 ms ago, so a 144 Hz screen draws about 72 a second and a 60 Hz one draws 60.
    // While paused or hidden it blocks in GetMessage and nothing wakes up.
    MSG msg;
    bool quit = false;
    while (!quit) {
        if (!running()) {
            if (GetMessageW(&msg, nullptr, 0, 0) <= 0) break;
            TranslateMessage(&msg); DispatchMessageW(&msg);
            continue;
        }
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { quit = true; break; }
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
        if (quit || !running()) continue;
        const double t0 = nowSeconds();
        if (t0 - g_lastFrame >= 0.013) { g_lastFrame = t0; frame(); }
        const HRESULT hr = DwmFlush();
        if (FAILED(hr) || nowSeconds() - t0 < 0.002) Sleep(8);   // no compositor to wait on: do not spin
    }

    removeTray();
    destroyOverlays();
    if (g_powerNote) UnregisterPowerSettingNotification(g_powerNote);
    WTSUnRegisterSessionNotification(g_host);
    if (once) CloseHandle(once);
    return 0;
}
