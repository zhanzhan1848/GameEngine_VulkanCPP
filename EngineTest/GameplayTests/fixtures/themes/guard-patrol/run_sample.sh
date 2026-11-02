#!/bin/bash
# guard-patrol 世代多样性取样（gameplay-rules.md S4 靶子的可复现实验）：
#   同一 NPC（Guard）两个世代（g1 固定循环 / g2 run 参数分叉），各 20 局
#   （run=0..19），逐局 PrimalPlayer --dryrun 120 帧产 trace，然后按
#   gameplay.diversity 同一公式（实体过滤 Guard，n=2，跨局 n-gram 重复率
#   = 1 - distinct/total）计算重复率。
# 预期：g2 重复率显著低于 g1（变化全在 Lua 层——L0 契约两世代同一份），
# 任务成功率不变（patrol_quality 无 fail 记录 = 未被抓到/未逃逸）。
# 用法：./run_sample.sh [PLAYER_BIN] [OUT_DIR]   （默认仓库树 Debug Player）
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../../../../.." && pwd)"
PLAYER="${1:-$REPO/Darwin/Debug/PrimalPlayer}"
OUT="${2:-$(mktemp -d /tmp/gp_sample_XXXX)}"
RUNS="${RUNS:-20}"
FRAMES="${FRAMES:-120}"

[ -x "$PLAYER" ] || { echo "player missing: $PLAYER"; exit 2; }
mkdir -p "$OUT"

for gen in g1_fixed g2_varied; do
  mkdir -p "$OUT/$gen"
  for run in $(seq 0 $((RUNS-1))); do
    pkg="$OUT/$gen/run_$run.primalrun"
    sed -e "s|__THEME_DIR__|$HERE|" -e "s|GUARD_LUA|$gen.lua|" \
        -e "s|param run \"0\"|param run \"$run\"|" \
        "$HERE/base.primalrun" > "$pkg"
    "$PLAYER" --scene "$pkg" --dryrun "$FRAMES" --trace-out "$OUT/$gen/trace_$run.csv" \
        >/dev/null 2>"$OUT/$gen/run_$run.log" &
    pid=$!
    for i in $(seq 1 60); do sleep 1; kill -0 $pid 2>/dev/null || break; done
    kill -9 $pid 2>/dev/null || true
    wait $pid 2>/dev/null || true
  done
done

# 多样性度量（与 gameplay.diversity 同公式：Guard 动作签名 = kind:detail 去帧号，
# n=2 跨局展开，repeatRate = 1 - distinct/total）
python3 - "$OUT" "$RUNS" <<'PY'
import re, sys, subprocess
out, runs = sys.argv[1], int(sys.argv[2])
ent_re = re.compile(r'entity=([^|]+)')
def actions(path):
    acts = []
    for line in open(path):
        if line.startswith('#') or not line.strip(): continue
        parts = line.rstrip('\n').split(',')
        if len(parts) < 4: continue
        m = ent_re.search(parts[3])
        if not m or m.group(1) != 'Guard': continue
        detail = '|'.join(s for s in parts[3].split('|') if not s.startswith('entity='))
        acts.append(f"{parts[2]}:{detail}")
    return acts
def repeat(gen):
    grams = []
    for r in range(runs):
        a = actions(f"{out}/{gen}/trace_{r}.csv")
        for i in range(len(a)-1):
            grams.append((a[i], a[i+1]))
    distinct = len(set(grams))
    total = len(grams)
    return 1.0 - distinct/total if total else 0.0, distinct, total
r1 = repeat('g1_fixed'); r2 = repeat('g2_varied')
print(f"runs={runs} n=2")
print(f"g1_fixed  repeatRate={r1[0]:.4f} (distinct {r1[1]}/{r1[2]})")
print(f"g2_varied repeatRate={r2[0]:.4f} (distinct {r2[1]}/{r2[2]})")
print(f"drop={r1[0]-r2[0]:+.4f}  {'TARGET-MET' if r2[0] < r1[0] - 0.05 else 'TARGET-MISSED'} (阈值 g2 < g1-0.05)")
# 任务成功率：任一世代出现 task failed（kind=5）即降档
for gen in ('g1_fixed','g2_varied'):
    fails = sum(1 for r in range(runs) for line in open(f"{out}/{gen}/trace_{r}.csv")
                if line.startswith('#') is False and len(line.split(','))>2 and line.split(',')[2]=='5')
    print(f"{gen}: task_failed_records={fails} (0=成功率不降)")
PY
echo "out=$OUT"
