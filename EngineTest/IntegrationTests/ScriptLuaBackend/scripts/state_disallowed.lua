local M = {}
M.fn_blocked     = false
M.thread_blocked = false
M.deep_blocked   = false
function M.begin_play(self)
    local ok1, _ = pcall(function() state.game.fn = function() end end)
    if not ok1 then self.fn_blocked = true end

    -- coroutine library is whitelisted (Phase 2b.7); coroutine.create returns a thread
    local ok2, _ = pcall(function() state.game.th = coroutine.create(function() end) end)
    if not ok2 then self.thread_blocked = true end

    -- Deep circular table
    local t = {}
    t.self = t
    local ok3, _ = pcall(function() state.game.deep = t end)
    if not ok3 then self.deep_blocked = true end
end
function M.update(self, dt) end
function M.fixed_update(self, dt) end
function M.late_update(self, dt) end
function M.destroy(self) end
return M
