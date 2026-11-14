// `QuietSnow --snapshot out.png`: runs the real model and the real shaders offscreen, over a mock desktop.
// Used for the README picture and as a smoke test of the Metal path on machines without a screen.

#if canImport(AppKit) && canImport(MetalKit)
import AppKit
import ImageIO
import MetalKit
import QuietSnowCore
import UniformTypeIdentifiers

let W = 1280, H = 800
let dockTop: Float = 742

/// A drawn desktop with the real model and real shaders on top. Used for the README picture and clip.
final class Scene {
    let device: MTLDevice
    let renderer: Renderer
    let sim = Sim(seed: 42)
    var back = Rect(x: 120, y: 150, w: 620, h: 420)
    var front = Rect(x: 560, y: 300, w: 560, h: 340)
    var cursor: (x: Float, y: Float)?
    let cs = CGColorSpaceCreateDeviceRGB()
    let info = CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue
    var flakes: [Float] = [], piles: [Float] = []

    init(device: MTLDevice) throws {
        self.device = device
        renderer = try Renderer(device: device)
        sim.weather = .steady
        sim.wind = 30
        sim.setWorld([Rect(x: 0, y: 0, w: Float(W), h: Float(H))])
        updateSurfaces()
    }

    func updateSurfaces() {
        sim.setSurfaces([
            SurfaceInput(id: 1, rect: Rect(x: front.x, y: front.y, w: front.w, h: 0)),
            SurfaceInput(id: 2, rect: Rect(x: back.x, y: back.y, w: back.w, h: 0), occluders: [front]),
            SurfaceInput(id: -1, rect: Rect(x: 0, y: dockTop, w: Float(W), h: 0), isFloor: true),
        ])
    }

    func step(_ dt: Float) { sim.step(dt: dt, pointer: cursor) }

    /// Let piles build up (a flurry, so it takes seconds not minutes), then go back to ordinary snowfall.
    func settle() {
        sim.weather = .flurry
        for _ in 0..<(30 * 300) { step(1.0 / 30) }
        sim.weather = .steady
        for _ in 0..<(30 * 10) { step(1.0 / 30) }
    }

    func buffer(_ a: [Float]) -> MTLBuffer? {
        a.isEmpty ? nil : a.withUnsafeBytes { device.makeBuffer(bytes: $0.baseAddress!, length: $0.count, options: .storageModeShared) }
    }

    func render() throws -> CGImage {
        let fn = sim.fillFlakes(&flakes), pn = sim.fillPiles(&piles)
        let td = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .bgra8Unorm, width: W, height: H, mipmapped: false)
        td.usage = [.renderTarget]
        td.storageMode = .shared
        guard let tex = device.makeTexture(descriptor: td), let cb = renderer.queue.makeCommandBuffer() else {
            throw NSError(domain: "QuietSnow", code: 2, userInfo: [NSLocalizedDescriptionKey: "no texture"])
        }
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = tex
        pass.colorAttachments[0].loadAction = .clear
        pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 0)
        pass.colorAttachments[0].storeAction = .store
        let enc = cb.makeRenderCommandEncoder(descriptor: pass)!
        renderer.encode(enc, origin: SIMD2(0, 0), size: SIMD2(Float(W), Float(H)),
                        piles: buffer(piles), pileCount: pn, flakes: buffer(flakes), flakeCount: fn)
        enc.endEncoding()
        cb.commit()
        cb.waitUntilCompleted()
        var bytes = [UInt8](repeating: 0, count: W * H * 4)
        tex.getBytes(&bytes, bytesPerRow: W * 4, from: MTLRegionMake2D(0, 0, W, H), mipmapLevel: 0)

        guard let ctx = CGContext(data: nil, width: W, height: H, bitsPerComponent: 8, bytesPerRow: 0, space: cs, bitmapInfo: info) else {
            throw NSError(domain: "QuietSnow", code: 3, userInfo: [NSLocalizedDescriptionKey: "no context"])
        }
        drawDesktop(ctx)
        let provider = CGDataProvider(data: Data(bytes) as CFData)!
        let snow = CGImage(width: W, height: H, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: W * 4, space: cs,
                           bitmapInfo: CGBitmapInfo(rawValue: info), provider: provider, decode: nil, shouldInterpolate: false, intent: .defaultIntent)!
        ctx.draw(snow, in: CGRect(x: 0, y: 0, width: W, height: H))
        if let c = cursor { drawCursor(ctx, c) }
        return ctx.makeImage()!
    }

    func drawDesktop(_ ctx: CGContext) {
        ctx.saveGState()
        ctx.translateBy(x: 0, y: CGFloat(H)); ctx.scaleBy(x: 1, y: -1)        // top-left coordinates like the sim
        let bg = CGGradient(colorsSpace: cs, colors: [NSColor(red: 0.10, green: 0.14, blue: 0.24, alpha: 1).cgColor,
                                                        NSColor(red: 0.26, green: 0.22, blue: 0.40, alpha: 1).cgColor] as CFArray, locations: [0, 1])!
        ctx.drawLinearGradient(bg, start: .zero, end: CGPoint(x: CGFloat(W), y: CGFloat(H)), options: [])
        let light = NSColor(white: 0.96, alpha: 1), lightBar = NSColor(white: 0.88, alpha: 1)
        let dark = NSColor(red: 0.11, green: 0.12, blue: 0.15, alpha: 1), darkBar = NSColor(white: 0.2, alpha: 1)
        for (r, body, bar) in [(back, light, lightBar), (front, dark, darkBar)] {
            let rect = CGRect(x: CGFloat(r.x), y: CGFloat(r.y), width: CGFloat(r.w), height: CGFloat(r.h))
            let path = CGPath(roundedRect: rect, cornerWidth: 10, cornerHeight: 10, transform: nil)
            ctx.saveGState(); ctx.setShadow(offset: CGSize(width: 0, height: -10), blur: 30, color: NSColor.black.withAlphaComponent(0.45).cgColor)
            ctx.addPath(path); ctx.setFillColor(body.cgColor); ctx.fillPath(); ctx.restoreGState()
            ctx.saveGState(); ctx.addPath(path); ctx.clip()
            ctx.setFillColor(bar.cgColor); ctx.fill(CGRect(x: rect.minX, y: rect.minY, width: rect.width, height: 34))
            ctx.restoreGState()
            for (i, c) in [NSColor.systemRed, .systemYellow, .systemGreen].enumerated() {
                ctx.setFillColor(c.cgColor); ctx.fillEllipse(in: CGRect(x: rect.minX + 14 + CGFloat(i) * 20, y: rect.minY + 11, width: 12, height: 12))
            }
            ctx.setFillColor(NSColor.gray.withAlphaComponent(0.35).cgColor)
            for line in 0..<7 {
                ctx.fill(CGRect(x: rect.minX + 28, y: rect.minY + 70 + CGFloat(line) * 28, width: rect.width * (line % 3 == 2 ? 0.45 : 0.72), height: 9))
            }
        }
        ctx.setFillColor(NSColor.white.withAlphaComponent(0.16).cgColor)
        ctx.addPath(CGPath(roundedRect: CGRect(x: 340, y: CGFloat(dockTop) + 8, width: 600, height: 50), cornerWidth: 16, cornerHeight: 16, transform: nil))
        ctx.fillPath()
        ctx.restoreGState()
    }

    func drawCursor(_ ctx: CGContext, _ c: (x: Float, y: Float)) {
        let x = CGFloat(c.x), y = CGFloat(H) - CGFloat(c.y)
        ctx.saveGState()
        ctx.beginPath()
        ctx.move(to: CGPoint(x: x, y: y)); ctx.addLine(to: CGPoint(x: x, y: y - 20))
        ctx.addLine(to: CGPoint(x: x + 5, y: y - 15)); ctx.addLine(to: CGPoint(x: x + 9, y: y - 23))
        ctx.addLine(to: CGPoint(x: x + 12, y: y - 21)); ctx.addLine(to: CGPoint(x: x + 8, y: y - 14))
        ctx.addLine(to: CGPoint(x: x + 14, y: y - 14)); ctx.closePath()
        ctx.setFillColor(NSColor.black.cgColor); ctx.setStrokeColor(NSColor.white.cgColor); ctx.setLineWidth(1.5)
        ctx.drawPath(using: .fillStroke)
        ctx.restoreGState()
    }
}

func save(_ image: CGImage, to path: String) throws {
    guard let dest = CGImageDestinationCreateWithURL(URL(fileURLWithPath: path) as CFURL, UTType.png.identifier as CFString, 1, nil) else {
        throw NSError(domain: "QuietSnow", code: 4, userInfo: [NSLocalizedDescriptionKey: "cannot write \(path)"])
    }
    CGImageDestinationAddImage(dest, image, nil)
    guard CGImageDestinationFinalize(dest) else { throw NSError(domain: "QuietSnow", code: 5, userInfo: [NSLocalizedDescriptionKey: "png failed"]) }
}

func writeSnapshot(device: MTLDevice, to path: String) throws {
    let scene = try Scene(device: device)
    scene.settle()
    try save(try scene.render(), to: path)
    print("snapshot: wrote \(path)")
}

/// `--frames dir`: 12 seconds at 60 fps as numbered PNGs. The cursor sweeps a pile, then the front window is dragged away.
func writeFrames(device: MTLDevice, to dir: String) throws {
    try FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
    let scene = try Scene(device: device)
    scene.settle()                                          // before the camera rolls
    let frames = 60 * 12
    for i in 0..<frames {
        let t = Float(i) / 60
        scene.cursor = nil
        if t >= 2 && t < 5 {                                // sweep along the top of the back window
            let u = (t - 2) / 3
            scene.cursor = (x: 140 + u * 560, y: 150 + 6 * sin(u * 12))
        }
        if t >= 7 && t < 9 {                                // drag the front window to the right
            let u = (t - 7) / 2
            scene.front.x = 560 + 90 * u * u * (3 - 2 * u)
            scene.front.y = 300 + 40 * u * u * (3 - 2 * u)
        }
        if i % 8 == 0 { scene.updateSurfaces() }
        scene.step(1.0 / 60)
        try save(try scene.render(), to: String(format: "%@/f%04d.png", dir, i))
    }
    print("frames: wrote \(frames) to \(dir)")
}
#endif
