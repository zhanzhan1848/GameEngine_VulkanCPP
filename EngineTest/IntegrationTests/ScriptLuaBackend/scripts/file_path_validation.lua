-- file_path_validation.lua
-- Test 33 fixture: 6 categories of illegal paths must all return nil from file.read.

local M = {}
M.illegal_count = 0
function M.begin_play(self)
    -- Each illegal path should return nil. Count how many do.
    local cases = {
        "foo://bar",                  -- unknown root
        "save://",                    -- empty rel path
        "save:///etc/passwd",         -- absolute path
        "data://../save/secret.json", -- traversal
        "save://C:\\foo",             -- Windows drive letter
        "noseparator"                 -- missing ://
    }
    for _, p in ipairs(cases) do
        if file.read(p) == nil then
            self.illegal_count = self.illegal_count + 1
        end
    end
end
function M.update(self, dt) end
function M.fixed_update(self, dt) end
function M.late_update(self, dt) end
function M.destroy(self) end
return M
