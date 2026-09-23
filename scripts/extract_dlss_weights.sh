#!/bin/bash
# extract_dlss_weights.sh — 用户放置 NVIDIA 库后的一键提取(M2 NR / M3 FG 权重)。
#
# 前置:按 third_party/dlss/README.md 下载并放置:
#   third_party/dlss/nvngx_dlssnr.dll           (RTX Feature Suite 310.8.0)
#   third_party/dlss/libnvidia-ngx-dlssg.so.310.7.0  (DLSS FG SDK 310.7.0)
set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$REPO/third_party/dlss"
OUT="$REPO/weights/nr"
OUTFG="$REPO/weights/fg"
mkdir -p "$OUT" "$OUTFG"

[ -f "$SRC/nvngx_dlssnr.dll" ] || { echo "缺 $SRC/nvngx_dlssnr.dll (RTX Feature Suite 310.8.0)" >&2; exit 1; }
[ -f "$SRC/libnvidia-ngx-dlssg.so.310.7.0" ] || { echo "缺 $SRC/libnvidia-ngx-dlssg.so.310.7.0 (DLSS FG SDK 310.7.0)" >&2; exit 1; }

CLI=( "$REPO/.venv/bin/mlxdlss-weights" )
[ -x "${CLI[0]}" ] || CLI=( python3 -m mlxdlss.weights.cli )

echo "== NR (71 块 transformer → packed/logical safetensors + MLX 包) =="
"${CLI[@]}" all "$SRC/nvngx_dlssnr.dll" "$OUT"

echo "== FG (FrameGenerator conv 权重 → shim neural_fg_* ABI 可加载) =="
"${CLI[@]}" extract-fg "$SRC/libnvidia-ngx-dlssg.so.310.7.0" "$OUTFG/framegen.safetensors"

echo "== 完成。验证: =="
echo "  .venv/bin/python ContentTools/neural/safetensors_crosscheck.py  # (NR 走 weights/nr/)"
echo "  ctest --test-dir Darwin/Debug -R Neural"
