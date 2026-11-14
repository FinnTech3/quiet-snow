// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "QuietSnow",
    platforms: [.macOS(.v13)],
    targets: [
        .target(name: "QuietSnowCore", swiftSettings: [.unsafeFlags(["-Ounchecked"], .when(configuration: .release))]),
        .executableTarget(name: "QuietSnow", dependencies: ["QuietSnowCore"]),
        .testTarget(name: "QuietSnowCoreTests", dependencies: ["QuietSnowCore"]),
    ]
)
