-- 世代 2（消除重复）：同一契约（同一状态机/全 manual 转移表），行为变化
-- 全在 Lua 层——
-- 1) 效用替代固定遍历：按 run 参数的步幅取下一路点（转移不在 L0 写死顺序）；
-- 2) 记忆驱动：记住上一路点避免立即折返；
-- 3) 节拍随 run 变化（dwell 20..40 帧）。确定性：同 (世代, run) 同行为。
local M = {}
local POS = {
    w1 = { -4, 0, -4 }, w2 = { 4, 0, -4 }, w3 = { 4, 0, 4 }, w4 = { -4, 0, 4 },
}
local NAMES = { "w1", "w2", "w3", "w4" }
local run, cur, prev, n, dwell = 0, 1, 0, 0, 30
function M:begin_play()
    run = tonumber(params and params.run or "0") or 0   -- params 全局表（begin_play 内读——契约）
    dwell = 20 + (run * 7) % 21                          -- 帧节拍 20..40
    cur = 1 + (run % 4)
    prev = 0
    n = 0
    local w = NAMES[cur]
    entity.set_position(POS[w][1], 0, POS[w][3])
    state.entities[entity_id].waypoint = w
end
function M:update(dt)
    n = n + 1
    if n % dwell == 0 then
        local step = 1 + (run % 3)                  -- 步幅 1..3：遍历顺序随 run 分叉
        local nxt = (cur + step - 1) % 4 + 1
        if nxt == prev then nxt = nxt % 4 + 1 end   -- 记忆：不立即折返
        prev, cur = cur, nxt
        local w = NAMES[cur]
        if game.state.get() ~= w then game.state.transition(w) end
        entity.set_position(POS[w][1], 0, POS[w][3])
        state.entities[entity_id].waypoint = w
    end
end
return M
