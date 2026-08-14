local M = {}
M.has_time = false
M.has_clock = false
M.has_date = false
M.has_difftime = false
function M.begin_play(self)
    if type(os.time)     == "function" and pcall(os.time)            then self.has_time = true end
    if type(os.clock)    == "function" and pcall(os.clock)           then self.has_clock = true end
    if type(os.date)     == "function" and pcall(os.date, "%Y")      then self.has_date = true end
    if type(os.difftime) == "function" and pcall(os.difftime, 10, 5) then self.has_difftime = true end
end
function M.update(self, dt) end
function M.fixed_update(self, dt) end
function M.late_update(self, dt) end
function M.destroy(self) end
return M
