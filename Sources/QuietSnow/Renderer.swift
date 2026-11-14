// Metal side: shaders compiled at launch (so building needs no Xcode Metal toolchain) and the two draw calls.

#if canImport(AppKit) && canImport(MetalKit)
import MetalKit

// MARK: shaders (compiled at launch, so no Xcode Metal toolchain is needed to build)

let shaderSource = """
#include <metal_stdlib>
using namespace metal;

struct U { float2 origin; float2 size; float t; float pad; };
struct V { float4 pos [[position]]; float size [[point_size]]; float a; float shade; };

static float4 clip(float4 f, constant U &u) {
    float2 q = (f.xy - u.origin) / u.size;
    return float4(q.x * 2.0 - 1.0, 1.0 - q.y * 2.0, 0.0, 1.0);
}

vertex V flakeV(uint i [[vertex_id]], constant float4 *p [[buffer(0)]], constant U &u [[buffer(1)]]) {
    float4 f = p[2 * i];
    float4 m = p[2 * i + 1];
    f.xy += m.xy * u.t;                       // glide on from the last model step
    V o; o.pos = clip(f, u); o.size = f.z; o.a = f.w; o.shade = 0.0; return o;
}
fragment float4 flakeF(V in [[stage_in]], float2 pc [[point_coord]]) {
    float d = length(pc - 0.5) * 2.0;
    float a = smoothstep(1.0, 0.25, d) * in.a;
    return float4(0.94 * a, 0.97 * a, 1.0 * a, a);
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



struct Uniforms { var origin: SIMD2<Float>; var size: SIMD2<Float>; var t: Float; var pad: Float = 0 }

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

    func encode(_ enc: MTLRenderCommandEncoder, origin: SIMD2<Float>, size: SIMD2<Float>, t: Float = 0,
                piles: MTLBuffer?, pileCount: Int, flakes: MTLBuffer?, flakeCount: Int) {
        var u = Uniforms(origin: origin, size: size, t: t)
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
            enc.drawPrimitives(type: .point, vertexStart: 0, vertexCount: flakeCount)
        }
    }
}
#endif
