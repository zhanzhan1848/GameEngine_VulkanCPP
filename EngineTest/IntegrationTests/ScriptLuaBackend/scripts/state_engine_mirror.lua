local M = {}
M.frame_read_ok = false
M.time_read_ok  = false
M.dt_read_ok    = false
M.write_blocked = false
function M.begin_play(self)
    if type(state.engine.frame) == "number" and state.engine.frame >= 0 then self.frame_read_ok = true end
    if type(state.engine.time)  == "number" and state.engine.time  >= 0 then self.time_read_ok  = true end
    if type(state.engine.dt)    == "number" and state.engine.dt    >= 0 then self.dt_read_ok    = true end
    local ok, _ = pcall(function() state.engine.frame = 0 end)
    if not ok then self.write_blocked = true end
end
function M.update(self, dt) end
function M.fixed_update(self, dt) end
function M.late_update(self, dt) end
function M.destroy(self) end
return M
