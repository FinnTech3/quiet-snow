// Metal side: shaders compiled at launch (so building needs no Xcode Metal toolchain) and the two draw calls.

#if canImport(AppKit) && canImport(MetalKit)
import MetalKit

// MARK: shaders (compiled at launch, so no Xcode Metal toolchain is needed to build)

let shaderSource = """
#include <metal_stdlib>
using namespace metal;

struct U { float2 origin; float2 size; float t; float time; float scale; };
struct V { float4 pos [[position]]; float size [[point_size]]; float a; float shade; float kind [[flat]]; float ang [[flat]]; float2 dir [[flat]]; };

static float4 clip(float4 f, constant U &u) {
    float2 q = (f.xy - u.origin) / u.size;
    return float4(q.x * 2.0 - 1.0, 1.0 - q.y * 2.0, 0.0, 1.0);
}

// kind: 0 ball, 1 snowflake, 2 sleet, 3 sparkle, 4 hail (the SnowType values in the model).
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

fragment float4 flakeF(V in [[stage_in]], float2 pc [[point_coord]], constant U &u [[buffer(1)]]) {
    float2 q0 = (pc - 0.5) * 2.0;
    float m = 0.0, alpha = in.a;
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
    float a = m * alpha;
    return float4(tint * a, a);
}

vertex V pileV(uint i [[vertex_id]], constant float4 *p [[buffer(0)]], constant U &u [[buffer(1)]]) {
    V o; o.pos = clip(p[i], u); o.size = 1.0; o.a = p[i].z; o.shade = p[i].w; return o;
}
fragment float4 pileF(V in [[stage_in]]) {
    float a = in.a;
    float3 lit = float3(0.97, 0.985, 1.0);
    float3 shadow = float3(0.66, 0.76, 0.93);
    return float4(mix(lit, shadow, in.shade) * a, a);
}
"""



struct Uniforms { var origin: SIMD2<Float>; var size: SIMD2<Float>; var t: Float; var time: Float = 0; var scale: Float = 1 }

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

    func encode(_ enc: MTLRenderCommandEncoder, origin: SIMD2<Float>, size: SIMD2<Float>, t: Float = 0, time: Float = 0, scale: Float = 1,
                piles: MTLBuffer?, pileCount: Int, flakes: MTLBuffer?, flakeCount: Int) {
        var u = Uniforms(origin: origin, size: size, t: t, time: time, scale: scale)
        if pileCount > 0, let buf = piles {
            enc.setRenderPipelineState(trianglePSO)
            enc.setVertexBuffer(buf, offset: 0, index: 0)
            enc.setVertexBytes(&u, length: MemoryLayout<Uniforms>.stride, index: 1)
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
