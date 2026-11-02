-- 玩家实体探针：每 60 帧发一次巡逻轮次事件（任务计数的驱动源）。
local M = {}
M.n = 0
function M:begin_play()
    M.n = 0
end
function M:update(dt)
    M.n = M.n + 1
    if M.n % 60 == 0 then
        bus.emit("patrol.rounds", { round = M.n / 60 })
    end
end
return M
