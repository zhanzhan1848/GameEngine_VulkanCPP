-- file_read_nonexistent.lua
-- Test 32 fixture: read data://missing.txt, expect nil; exists returns false.

local M = {}
M.result = "UNSET"
M.exists_result = true
function M.begin_play(self)
    local c = file.read("data://missing.txt")
    if c == nil then
        self.result = "nil"
    else
        self.result = "not_nil"
    end
    self.exists_result = file.exists("data://missing.txt")
end
function M.update(self, dt) end
function M.fixed_update(self, dt) end
function M.late_update(self, dt) end
function M.destroy(self) end
return M
