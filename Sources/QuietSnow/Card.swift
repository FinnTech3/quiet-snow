// The show as a video you can send, and the wallpaper's colour for tinting the snow.

#if canImport(AppKit) && canImport(MetalKit)
import AppKit
import AVFoundation
import MetalKit
import QuietSnowCore

/// The main display's wallpaper, or nil if it cannot be read.
func wallpaperImage() -> CGImage? {
    guard let screen = NSScreen.main, let url = NSWorkspace.shared.desktopImageURL(for: screen),
          let img = NSImage(contentsOf: url) else { return nil }
    return img.cgImage(forProposedRect: nil, context: nil, hints: nil)
}

/// The wallpaper's average colour, brightened so the snow keeps its light, as a tint with its strength in w.
func wallpaperTint() -> SIMD4<Float> {
    guard let cg = wallpaperImage() else { return .zero }
    var px = [UInt8](repeating: 0, count: 8 * 8 * 4)
    let ok: Bool = px.withUnsafeMutableBytes { buf in
        guard let ctx = CGContext(data: buf.baseAddress, width: 8, height: 8, bitsPerComponent: 8, bytesPerRow: 32,
                                  space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else { return false }
        ctx.interpolationQuality = .medium
        ctx.draw(cg, in: CGRect(x: 0, y: 0, width: 8, height: 8))
        return true
    }
    guard ok else { return .zero }
    var r: Float = 0, g: Float = 0, b: Float = 0
    for k in stride(from: 0, to: px.count, by: 4) { r += Float(px[k]); g += Float(px[k + 1]); b += Float(px[k + 2]) }
    let m = max(r, g, b, 1)
    return SIMD4(0.5 + 0.5 * r / m, 0.5 + 0.5 * g / m, 0.5 + 0.5 * b / m, 0.35)
}

/// Record the hidden show over your wallpaper: 20 seconds of snow settling, then the show, the fireworks and the pink
/// snow, at 30 frames a second, as an H.264 movie.
func writeCard(device: MTLDevice, to url: URL, wallpaper: CGImage?) throws {
    let scene = try Scene(device: device)
    scene.card = true
    scene.wallpaper = wallpaper
    scene.sim.extras = true
    for _ in 0..<4 { scene.updateSurfaces() }             // past the grace a vanished surface gets, so the drawn windows go
    scene.sim.weather = .steady
    for _ in 0..<(30 * 20) { scene.step(1.0 / 30) }
    try? FileManager.default.removeItem(at: url)
    let writer = try AVAssetWriter(outputURL: url, fileType: .mov)
    let input = AVAssetWriterInput(mediaType: .video, outputSettings: [AVVideoCodecKey: AVVideoCodecType.h264, AVVideoWidthKey: W, AVVideoHeightKey: H])
    input.expectsMediaDataInRealTime = false
    let adaptor = AVAssetWriterInputPixelBufferAdaptor(assetWriterInput: input, sourcePixelBufferAttributes: [
        kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA, kCVPixelBufferWidthKey as String: W, kCVPixelBufferHeightKey as String: H])
    writer.add(input)
    guard writer.startWriting() else { throw writer.error ?? NSError(domain: "QuietSnow", code: 5) }
    writer.startSession(atSourceTime: .zero)
    scene.sim.startShow()
    var frame = 0, after = 0
    while after < 30 * 6 && frame < 30 * 45 {             // the show, then six seconds of pink snow
        scene.step(1.0 / 30)
        scene.time += 1.0 / 30
        if !scene.sim.showActive { after += 1 }
        let image = try scene.render()
        while !input.isReadyForMoreMediaData { Thread.sleep(forTimeInterval: 0.005) }
        guard let pool = adaptor.pixelBufferPool else { throw NSError(domain: "QuietSnow", code: 6) }
        var pb: CVPixelBuffer?
        CVPixelBufferPoolCreatePixelBuffer(nil, pool, &pb)
        guard let buffer = pb else { throw NSError(domain: "QuietSnow", code: 7) }
        CVPixelBufferLockBaseAddress(buffer, [])
        if let ctx = CGContext(data: CVPixelBufferGetBaseAddress(buffer), width: W, height: H, bitsPerComponent: 8,
                               bytesPerRow: CVPixelBufferGetBytesPerRow(buffer), space: CGColorSpaceCreateDeviceRGB(),
                               bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue) {
            ctx.draw(image, in: CGRect(x: 0, y: 0, width: W, height: H))
        }
        CVPixelBufferUnlockBaseAddress(buffer, [])
        adaptor.append(buffer, withPresentationTime: CMTime(value: CMTimeValue(frame), timescale: 30))
        frame += 1
    }
    input.markAsFinished()
    let done = DispatchSemaphore(value: 0)
    writer.finishWriting { done.signal() }
    done.wait()
    if writer.status != .completed { throw writer.error ?? NSError(domain: "QuietSnow", code: 8) }
    print("card: \(frame) frames to \(url.path)")
}
#endif
