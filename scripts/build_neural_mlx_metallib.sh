#!/bin/bash
# build_neural_mlx_metallib.sh — 为 SwiftPM 源码构建的 mlx-swift 编译 mlx.metallib。
#
# 背景（M1-a 落地时的坑）：mlx-swift 的 SwiftPM 源码构建**不产出 metallib**
# （Cmlx 目标 exclude 了 kernels 目录，见其 Package.swift "PrepareMetalShaders"
# 注释），运行时首个 MLX GPU 算子直接抛 "Failed to load the default metallib"。
# 官方发行版走 xcframework 预编译路径；源码路径需自行按其 CMake 配方
# （Source/Cmlx/mlx/mlx/backend/metal/kernels/CMakeLists.txt）编译。
#
# 产物放置：libNeuralMLXShim.dylib 同目录（mlx 的 load_colocated_library
# 以"包含本代码的二进制目录"为锚点首选 mlx.metallib）。
#
# 用法: scripts/build_neural_mlx_metallib.sh [输出目录]
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
SHIM="$REPO/Tools/NeuralMLXShim"
CHECKOUT="$SHIM/.build/checkouts/mlx-swift"
[ -d "$CHECKOUT" ] || { echo "error: mlx-swift 未解析（先 cd $SHIM && swift package resolve）" >&2; exit 1; }

KERNELS_DIR="$CHECKOUT/Source/Cmlx/mlx/mlx/backend/metal/kernels"
INCLUDE_ROOT="$CHECKOUT/Source/Cmlx/mlx"
OUT_DIR="${1:-$SHIM/.build/release}"

# kernel 清单 = CMakeLists 的非 JIT 全集 + NAX 集（SDK ≥ 26.2 / Metal ≥ 4.0，
# C++ 侧 is_nax_available() 运行时按 OS 版本选择；macOS 26 运行时需要它们）。
# 路径相对 kernels/ 根，与 CMakeLists 的 build_kernel 参数一致。
KERNELS=(
  arg_reduce.metal conv.metal gemv.metal layer_norm.metal random.metal
  rms_norm.metal rope.metal scaled_dot_product_attention.metal fence.metal
  arange.metal binary.metal binary_two.metal copy.metal fft.metal reduce.metal
  quantized.metal fp_quantized.metal scan.metal softmax.metal logsumexp.metal
  sort.metal ternary.metal unary.metal gemv_masked.metal
  steel/conv/kernels/steel_conv.metal
  steel/conv/kernels/steel_conv_3d.metal
  steel/conv/kernels/steel_conv_general.metal
  steel/gemm/kernels/steel_gemm_fused.metal
  steel/gemm/kernels/steel_gemm_gather.metal
  steel/gemm/kernels/steel_gemm_masked.metal
  steel/gemm/kernels/steel_gemm_splitk.metal
  steel/gemm/kernels/steel_gemm_segmented.metal
  steel/attn/kernels/steel_attention.metal
  steel/gemm/kernels/steel_gemm_fused_nax.metal
  steel/gemm/kernels/steel_gemm_gather_nax.metal
  steel/gemm/kernels/steel_gemm_splitk_nax.metal
  quantized_nax.metal fp_quantized_nax.metal
  steel/attn/kernels/steel_attention_nax.metal
)

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

FLAGS=(-x metal -Wall -Wextra -fno-fast-math -Wno-c++17-extensions -Wno-c++20-extensions
       -I"$INCLUDE_ROOT" -I"$KERNELS_DIR")

for k in "${KERNELS[@]}"; do
  mkdir -p "$TMP/$(dirname "$k")"
  xcrun -sdk macosx metal "${FLAGS[@]}" -c "$KERNELS_DIR/$k" -o "$TMP/${k%.metal}.air"
done

mkdir -p "$OUT_DIR"
xcrun -sdk macosx metallib "$TMP"/*.air -o "$OUT_DIR/mlx.metallib"
echo "built $OUT_DIR/mlx.metallib ($(du -h "$OUT_DIR/mlx.metallib" | cut -f1))"
