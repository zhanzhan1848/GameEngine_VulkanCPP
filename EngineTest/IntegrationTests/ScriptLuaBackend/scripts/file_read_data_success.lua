-- file_read_data_success.lua
-- Test 31 fixture: read data://hello.txt during begin_play, verify content + size.

local M = {}
M.content = nil
M.size_ok = false
function M.begin_play(self)
    self.content = file.read("data://hello.txt")
    local sz = file.size("data://hello.txt")
    if type(sz) == "number" and sz == 16 then  -- "Hello, File API!" is 16 bytes
        self.size_ok = true
    end
end
function M.update(self, dt) end
function M.fixed_update(self, dt) end
function M.late_update(self, dt) end
function M.destroy(self) end
return M
