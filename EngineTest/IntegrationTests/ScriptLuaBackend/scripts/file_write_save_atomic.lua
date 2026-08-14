-- file_write_save_atomic.lua
-- Test 34 fixture: write save://test.json then read it back to verify atomic write semantics.

local M = {}
M.write_ok = false
M.readback_ok = false
function M.begin_play(self)
    local ok = file.write("save://test.json", "{\"a\":1}")
    self.write_ok = ok
    if ok then
        local c = file.read("save://test.json")
        if c == "{\"a\":1}" then
            self.readback_ok = true
        end
    end
end
function M.update(self, dt) end
function M.fixed_update(self, dt) end
function M.late_update(self, dt) end
function M.destroy(self) end
return M
