-- file_write_permission_denied.lua
-- Test 35 fixture: verify 5 write/append/remove operations on read-only/append-only roots are denied.

local M = {}
M.denied_count = 0
function M.begin_play(self)
    if file.write("data://x",  "...") == false then self.denied_count = self.denied_count + 1 end
    if file.write("log://x",   "...") == false then self.denied_count = self.denied_count + 1 end
    if file.append("data://x", "...") == false then self.denied_count = self.denied_count + 1 end
    if file.remove("data://x") == false then self.denied_count = self.denied_count + 1 end
    if file.remove("log://x")  == false then self.denied_count = self.denied_count + 1 end
end
function M.update(self, dt) end
function M.fixed_update(self, dt) end
function M.late_update(self, dt) end
function M.destroy(self) end
return M
