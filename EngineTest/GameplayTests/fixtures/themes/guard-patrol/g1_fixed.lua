-- 世代 1（基线）：固定循环 w1→w2→w3→w4，固定节拍（每 30 帧一步）——
-- 重复感的靶子。世代内确定（同 run 同行为）；跨 run 无变化 → 高 n-gram 重复率。
local M = {}
local DWELL = 30   -- 帧
local ORDER = { "w1", "w2", "w3", "w4" }
local POS = {
    w1 = { -4, 0, -4 }, w2 = { 4, 0, -4 }, w3 = { 4, 0, 4 }, w4 = { -4, 0, 4 },
}
local idx, n = 1, 0
function M:begin_play()
    idx, n = 1, 0
    entity.set_position(POS.w1[1], 0, POS.w1[3])
    state.entities[entity_id].waypoint = "w1"
end
function M:update(dt)
    n = n + 1
    if n % DWELL == 0 then
        idx = idx % #ORDER + 1
        local w = ORDER[idx]
        if game.state.get() ~= w then game.state.transition(w) end
        entity.set_position(POS[w][1], 0, POS[w][3])
        state.entities[entity_id].waypoint = w
    end
end
return M
