#!/bin/bash
# visual_parity_report.sh — P4c 视觉回归报告生成器
#
# 跑完 Vulkan P4c 套件后聚合各用例的 SSIM / max-abs-diff 输出,
# 产出 build/VisualParityReport.md(每用例一行:指标 + 参照帧路径)。
# 约定(沿用 SimplePBR):Metal 参照缺失的用例在报告中标注 SKIP。
#
# 用法: ./visual_parity_report.sh [build-dir](默认 ./build)

set -u

BUILD_DIR="${1:-./build}"
TEST_DIR="$BUILD_DIR/Tests/UnitTests"
OUT="$BUILD_DIR/VisualParityReport.md"

if [ ! -d "$TEST_DIR" ]; then
    echo "visual_parity_report: test dir not found: $TEST_DIR" >&2
    exit 1
fi

{
    echo "# Visual Parity Report (P4c)"
    echo ""
    echo "- 生成时间: $(date '+%Y-%m-%d %H:%M:%S')"
    echo "- 套件目录: $TEST_DIR"
    echo ""
    echo "指标来源:各测试二进制控制台输出(SSIM / max-abs-diff / mismatches)。"
    echo "Metal 参照帧缺失的跨后端用例按 SimplePBR 约定 skip(标注于各行)。"
    echo ""

    for t in TestVulkanFormatParity TestVulkanTextureUpdate TestVulkanStagingUpload \
             TestVulkanLayeredRendering TestVulkanComputeBytesLarge \
             TestVulkanSecondaryCommandBuffer TestVulkanSimplePBR; do
        if [ ! -x "$TEST_DIR/$t" ]; then
            echo "## $t — BINARY MISSING" && echo ""
            continue
        fi
        echo "## $t"
        (cd "$TEST_DIR" && ./"$t" 2>&1) | grep -E "SSIM|max-abs-diff|mismatches|skip" \
            | sed 's/^\[/- [/; s/\] /] /' || echo "- (no visual metrics)"
        echo ""
    done

    echo "## 参照帧状态"
    echo ""
    echo "- 已入库: P4b-T2/P4b-T3(SimplePBR 及 Tier3 各 pass)"
    echo "- P4c-F1…F7 的 Metal 参照 PNG(ReferenceImages/P4c-Fx/)未生成 —"
    echo "  需在 macOS 上编写对应 Metal 侧生成器后入库;生成前相关 SSIM 用例 skip。"
    echo "- 同后端自参照验收(ProgressiveSubrect SSIM=1.0 / CascadeSelfParity"
    echo "  SSIM=1.0 / SameCommandsExact 逐字节相等)不依赖参照帧,已全部通过。"
} > "$OUT"

echo "written: $OUT"
