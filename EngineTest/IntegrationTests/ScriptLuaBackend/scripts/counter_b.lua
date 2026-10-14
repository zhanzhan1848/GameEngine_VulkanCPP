-- 多类型回归夹具 B（2026-10-15）：与 minimal_script 同构的计数器。
local M = {}
function M:begin_play() M.begins = 0 M.updates = 0 end
function M:update(dt) M.updates = (M.updates or 0) + 1 end
function M:get_updates() return M.updates or 0 end
return M
