# guard-patrol 题材样例（gameplay S3/S4）

与 `../player_smoke`（按钮开门）复用**同一套 L0 契约与解释器**——本题材零引擎改动，
语义全部在项目 Lua 层（题材玩法语义不上引擎的边界示范）。

- 场景：Guard（4 路点全 manual 转移表 + patrol_quality 任务）、Player（seq 巡逻路线）、
  pickup 事件驱动任务计数。
- `g1_fixed.lua`：基线世代——固定循环/固定节拍（重复感的靶子）。
- `g2_varied.lua`：改进世代——同一契约，行为分叉全在 Lua：run 参数步幅遍历 +
  记忆防折返 + 节拍变化。同 (世代, run) 逐位确定。
- `run_sample.sh`：S4 靶子实验——两世代各 20 局 dry-run，Guard 动作流 n=2
  跨局重复率（与 gameplay.diversity 同公式）。2026-10-28 实测：
  g1 0.9500 → g2 0.6984（distinct 3/60 → 19/63），两世代任务失败记录均为 0。

已知噪声：Player 干净退出路径的 RHI 泄漏 abort（既有移交项，trace 在 abort 前已落盘）；
mode 3 Metal 黑帧（既有移交项，与行为回路无关）。
