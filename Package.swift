// swift-tools-version: 5.10

import PackageDescription

let package = Package(
    name: "DropCode",
    platforms: [.macOS(.v13)],
    dependencies: [
        .package(
            url: "https://github.com/Lakr233/libghostty-spm.git",
            revision: "92a5c80c15a1f85c2326e39f115a3e2d66ae0ab7"
        )
    ],
    targets: [
        .executableTarget(
            name: "DropCode",
            dependencies: [
                .product(name: "GhosttyTerminal", package: "libghostty-spm")
            ]
        )
    ]
)
