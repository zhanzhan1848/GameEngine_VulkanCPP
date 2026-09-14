#!/bin/bash
# check_shader_copies.sh — T1.7 (E1 前半)：shader 同语言拷贝一致性门禁
#
# 背景（Master Plan §5.3 / 发现 E1）：GI kernel 存在四份手拷贝
#   1. Engine/Graphics/Metal/shaders/**      （Metal 权威源，运行时编译）
#   2. Engine/Graphics/Vulkan/Shaders/**     （GLSL 权威源，build_spv.sh 预编译）
#   3. Engine/Graphics/Dawn/shaders/**       （WGSL 权威源）
#   4. EngineTest/shaders/**                 （测试可执行文件的第四份拷贝）
# 同语言拷贝（1↔4）会静默漂移到算法级（DIRECT_INTENSITY 式）。
# 跨语言（1↔2↔3）是手工移植关系，无法内容 hash，走显式豁免清单跟踪。
#
# 机制：基线快照。每次运行计算全部同名拷贝对的规范化哈希
#   （规范化 = 剔除 #include 行——两棵树的 include 解析约定不同），
# 与 scripts/shader_copy_baseline.txt 对比：
#   - 基线一致、现在漂移  → FAIL（新漂移，必须同步或显式加豁免）
#   - 基线漂移、现在一致  → INFO（漂移被修复，用 --update-baseline 记账）
#   - 基线漂移、现在仍漂移 → 豁免（存量，逐个修复时从豁免清单移除）
#
# 用法：
#   scripts/check_shader_copies.sh                    # 门禁
#   scripts/check_shader_copies.sh --update-baseline  # 重生成基线
#   PRIMAL_SHADER_COPY_BASELINE=<path> 指定基线文件（默认仓库内路径）

set -u
cd "$(dirname "$0")/.."

BASELINE="scripts/shader_copy_baseline.txt"
UPDATE=0
[ "${1:-}" = "--update-baseline" ] && UPDATE=1

norm_sha() {  # 规范化哈希：剔除 #include 行
    grep -v '^[[:space:]]*#include' "$1" | shasum -a 256 | cut -d' ' -f1
}

# 测试副本 → 引擎权威源的映射：先同相对路径，再 Lumen/ 子目录
# （T3.6 教训：引擎 gather 在 Lumen/ 下，flat 映射把这对标成 TEST_ONLY，
#  绑定签名级断裂从门禁漏网）
twin_of() {
    local f="$1"
    if [ -f "Engine/Graphics/Metal/shaders/$f" ]; then
        echo "Engine/Graphics/Metal/shaders/$f"
    elif [ -f "Engine/Graphics/Metal/shaders/Lumen/$f" ]; then
        echo "Engine/Graphics/Metal/shaders/Lumen/$f"
    else
        echo ""
    fi
}

current_state() {
    # 遍历 EngineTest/shaders 下所有 .metal，与 Engine 树同名文件比对
    (cd EngineTest/shaders && find . -name '*.metal' | sed 's|^\./||' | sort) | while read -r f; do
        twin=$(twin_of "$f")
        if [ -n "$twin" ]; then
            ha=$(norm_sha "EngineTest/shaders/$f")
            hb=$(norm_sha "$twin")
            if [ "$ha" = "$hb" ]; then
                echo "SAME $twin $f"
            else
                echo "DRIFT $twin $f"
            fi
        else
            echo "TEST_ONLY - $f"
        fi
    done
}

if [ "$UPDATE" = "1" ]; then
    current_state > "$BASELINE"
    echo "[shader-copies] baseline updated: $BASELINE ($(wc -l < "$BASELINE" | tr -d ' ') pairs)"
    exit 0
fi

if [ ! -f "$BASELINE" ]; then
    echo "[shader-copies] FAIL: baseline missing — run with --update-baseline once" >&2
    exit 1
fi

fail=0
fixed=0
# 逐对比对（baseline 驱动；行格式 = <status> <twin-path> <test-path>）
while read -r exp_status twin f; do
    if [ ! -f "EngineTest/shaders/$f" ] || [ ! -f "$twin" ]; then
        continue  # 文件被移动/删除：结构变化，由 --update-baseline 记账
    fi
    ha=$(norm_sha "EngineTest/shaders/$f")
    hb=$(norm_sha "$twin")
    now_status="DRIFT"; [ "$ha" = "$hb" ] && now_status="SAME"
    if [ "$exp_status" = "SAME" ] && [ "$now_status" = "DRIFT" ]; then
        echo "[shader-copies] FAIL: NEW drift — $f"
        echo "    EngineTest/shaders/$f vs $twin diverged."
        echo "    Sync the copy, or (structural reason) update the baseline deliberately."
        fail=1
    elif [ "$exp_status" = "DRIFT" ] && [ "$now_status" = "SAME" ]; then
        echo "[shader-copies] INFO: drift FIXED — $f (run --update-baseline to record)"
        fixed=$((fixed+1))
    fi
done < "$BASELINE"

if [ "$fail" = "1" ]; then
    echo "[shader-copies] gate FAILED" >&2
    exit 1
fi
echo "[shader-copies] gate OK ($fixed pairs pending baseline update)"
exit 0
