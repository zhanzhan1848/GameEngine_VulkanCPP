// swift-tools-version:5.9
// NeuralMLXShim — 神经渲染推理的 MLX 后端（Docs/2026-09-15-neural-mlx-backend-plan.md M1-a）。
// 产物：动态库 libNeuralMLXShim.dylib，暴露 C ABI；引擎 C++ 侧 dlopen，
// 不可用时降级回裸 Metal kernel（NeuralConv）。
import PackageDescription

let package = Package(
    name: "NeuralMLXShim",
    platforms: [.macOS(.v14)],   // MLX 要求 macOS 14+
    products: [
        .library(name: "NeuralMLXShim", type: .dynamic, targets: ["NeuralMLXShim"])
    ],
    dependencies: [
        .package(url: "https://github.com/ml-explore/mlx-swift", from: "0.21.0"),
        // M3 FrameGenerator：MLX-DLSS 的 Swift MLX FG 执行器（DLSSMLX.FrameGenerator）
        .package(url: "https://github.com/iamwavecut/MLX-DLSS.git", revision: "0ca2deab092fe6f3e331bf4f616271dbc64521d0")
    ],
    targets: [
        .target(
            name: "NeuralMLXShim",
            dependencies: [
                .product(name: "MLX", package: "mlx-swift"),
                .product(name: "MLXNN", package: "mlx-swift"),
                .product(name: "DLSSMLX", package: "MLX-DLSS"),
            ],
            path: "Sources/NeuralMLXShim"
        ),
        .executableTarget(
            name: "RepeatCheck",
            dependencies: [
                .product(name: "MLX", package: "mlx-swift"),
                .product(name: "DLSSMLX", package: "MLX-DLSS"),
                .product(name: "DLSSCore", package: "MLX-DLSS"),
            ],
            path: "Sources/NeuralMLXDebug"
        ),
    ]
)
