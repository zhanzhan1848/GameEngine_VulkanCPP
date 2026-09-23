#!/bin/bash
# batch_capture_m2.sh — R3.1 M2 大规模采集自动化：一键挂机 ≥500 take。
#
# 设计（Docs/2026-09-22-neural-rendering-inference-training-plan.md §R3.1）：
#   循环启动 TestVulkanSponzaRenderGraph，每次 _TAKES=12，CAM_PATH 轮转机位；
#   每 12 take 落盘后 make_dataset.py 转 npz 并删除 bins（磁盘管理）；
#   每新增 50 take 自动触发 7ch 降噪器重训 + holdout 评估（--steps 摘要），
#   且可选触发 NR 微调（TRAIN_NR=1）。
#
# 磁盘预算：500 take × ~15MB npz ≈ 7.5GB（bins 临时 ~3GB/12take，每批清理）。
#
# 用法:
#   scripts/batch_capture_m2.sh [批数]         # 默认 42 批 = 504 take
#   BATCHES=6 TAKE_OFFSET=12 scripts/batch_capture_m2.sh
#   DRY_RUN=1 scripts/batch_capture_m2.sh 2    # 只打印命令不执行
#
# 关键 env：
#   TAKE_OFFSET  已有 take 数（文件名续号从 offset 开始）
#   TRAIN_NR     批末触发 NR 微调（默认只重训 7ch 降噪器）
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
# 测试二进制输出目录 = <repo>/<System>/<config>/（setup_test_target 约定），
# 两棵构建树（Debug/DebugVulkan）共用该输出位置。
TEST_BIN="$REPO/Darwin/Debug/TestVulkanSponzaRenderGraph"
PY="$REPO/.venv/bin/python"
DATA_DIR="${DATA_DIR:-/tmp/neural_capture_m2}"
TAKES_PER_BATCH="${TAKES_PER_BATCH:-12}"
BATCHES="${BATCHES:-${1:-42}}"
TAKE_OFFSET="${TAKE_OFFSET:-$(ls "$DATA_DIR"/dataset_take_*.npz 2>/dev/null | wc -l | tr -d ' ')}"
RETRAIN_EVERY="${RETRAIN_EVERY:-50}"
TRAIN_NR="${TRAIN_NR:-0}"
DRY_RUN="${DRY_RUN:-0}"

[ -x "$TEST_BIN" ] || { echo "error: $TEST_BIN 不存在（先构建 DebugVulkan 树）" >&2; exit 1; }

echo "[batch] repo=$REPO data=$DATA_DIR batches=$BATCHES x$TAKES_PER_BATCH offset=$TAKE_OFFSET"

# 机位轮转：绕 Sponza 中庭的环形机位（世界系偏移，基点 = SPGI_CAM 默认机位）。
# 12 机位/批，批间整体旋转 + 抬升，几何上铺开视角多样性。
cam_path_for_batch() {
    local batch="$1"
    python3 - "$batch" <<'PYEOF'
import math, sys
batch = int(sys.argv[1])
entries = []
for i in range(12):
    ang = (batch * 7.5 + i * 30.0) * math.pi / 180.0
    r = 3.0 + 1.5 * math.sin(batch * 0.7 + i)
    y = 2.0 + 1.2 * abs(math.sin(batch * 1.3 + i * 0.5))
    entries.append(f"{r*math.cos(ang):.2f},{y:.2f},{r*math.sin(ang):.2f}")
print(";".join(entries))
PYEOF
}

maybe_retrain() {
    local count="$1"
    if [ "$((count % RETRAIN_EVERY))" -eq 0 ] && [ "$count" -gt 0 ]; then
        echo "[batch] == $count take 达到，触发重训 + holdout 评估 =="
        local train_ids test_ids
        train_ids=$(python3 - "$count" <<'PYEOF'
import sys
n = int(sys.argv[1])
ids = [i for i in range(n) if i % 4 != 3]
print(",".join(map(str, ids[-40:])))
PYEOF
)
        test_ids=$(python3 - "$count" <<'PYEOF'
import sys
n = int(sys.argv[1])
print(",".join(str(i) for i in range(n) if i % 4 == 3)[-16:])
PYEOF
)
        if [ "$DRY_RUN" != "1" ]; then
            "$PY" "$REPO/ContentTools/neural/train_denoiser.py" "$DATA_DIR" \
                --tag sponza --channels 7 --train "$train_ids" --test "$test_ids" \
                --steps 1200 || echo "[batch] WARN: 7ch 重训失败（继续采集）"
            if [ "$TRAIN_NR" = "1" ]; then
                "$PY" "$REPO/ContentTools/neural/train_nr.py" "$DATA_DIR" \
                    --train "$train_ids" --test "$test_ids" --steps 300 \
                    || echo "[batch] WARN: NR 微调失败（继续采集）"
            fi
        else
            echo "[dry] train_denoiser --train $train_ids --test $test_ids"
        fi
    fi
}

for ((b = 0; b < BATCHES; ++b)); do
    cam_path="$(cam_path_for_batch "$b")"
    batch_idx=$((TAKE_OFFSET / TAKES_PER_BATCH + b))
    log="$DATA_DIR/batch_$batch_idx.log"
    batch_dir="$DATA_DIR/b$batch_idx"   # 独立目录：take_idx_ 每次运行从 0 起会覆写旧文件
    echo "[batch $((b + 1))/$BATCHES] takes $TAKE_OFFSET..$((TAKE_OFFSET + TAKES_PER_BATCH - 1)) → $log"
    if [ "$DRY_RUN" = "1" ]; then
        echo "[dry] PRIMAL_NEURAL_CAPTURE=1 ... $TEST_BIN (CAM_PATH=$cam_path DIR=$batch_dir)"
    else
        mkdir -p "$batch_dir"
        env PRIMAL_NEURAL_CAPTURE=1 \
            PRIMAL_NEURAL_CAPTURE_FRAMES=256 \
            PRIMAL_NEURAL_CAPTURE_TAKES=$TAKES_PER_BATCH \
            PRIMAL_NEURAL_CAPTURE_WARMUP=240 \
            PRIMAL_NEURAL_CAPTURE_DIR="$batch_dir" \
            PRIMAL_NEURAL_CAPTURE_TAG=sponza \
            PRIMAL_NEURAL_CAPTURE_CAM_PATH="$cam_path" \
            "$TEST_BIN" >"$log" 2>&1 &
        app_pid=$!
        # 挂机等待：本批 take 文件齐 → 杀掉测试窗口（模块完成即 idle）
        for _ in $(seq 1 240); do   # 上限 240×30s = 2h/批
            sleep 30
            n=$(ls "$batch_dir"/take_sponza_*.json 2>/dev/null | wc -l | tr -d ' ')
            if [ "$n" -ge "$TAKES_PER_BATCH" ]; then break; fi
            if ! kill -0 "$app_pid" 2>/dev/null; then break; fi
        done
        kill "$app_pid" 2>/dev/null || true
        wait "$app_pid" 2>/dev/null || true
        n=$(ls "$batch_dir"/take_sponza_*.json 2>/dev/null | wc -l | tr -d ' ')
        echo "[batch $((b + 1))] 落盘 $n/$TAKES_PER_BATCH take"
        if [ "$n" -lt "$TAKES_PER_BATCH" ]; then
            echo "[batch $((b + 1))] WARN: 采集不足（看 $log 尾部）"
            tail -5 "$log" || true
        fi
        # bins → npz → 按全局偏移重命名 → 清理 bins（磁盘管理）
        "$PY" "$REPO/ContentTools/neural/make_dataset.py" "$batch_dir" | tail -1
        i=$TAKE_OFFSET
        for f in $(ls "$batch_dir"/dataset_take_sponza_*.npz 2>/dev/null | sort); do
            mv "$f" "$DATA_DIR/dataset_take_sponza_$i.npz"
            i=$((i + 1))
        done
        rm -rf "$batch_dir"
        TAKE_OFFSET=$i
        maybe_retrain "$i"
    fi
done
echo "[batch] 完成：$(ls "$DATA_DIR"/dataset_take_*.npz 2>/dev/null | wc -l | tr -d ' ') npz @ $DATA_DIR"
