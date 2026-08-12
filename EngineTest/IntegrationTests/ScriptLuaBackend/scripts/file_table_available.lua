-- file_table_available.lua
-- Test 30 fixture: record which file.* functions exist as function-typed values
-- during begin_play. C++ test reads back the 8 booleans.

local M = {}
M.available = {}
function M.begin_play(self)
    M.available.read       = type(file.read)       == "function"
    M.available.write      = type(file.write)      == "function"
    M.available.append     = type(file.append)     == "function"
    M.available.exists     = type(file.exists)     == "function"
    M.available.size       = type(file.size)       == "function"
    M.available.list       = type(file.list)       == "function"
    M.available.remove     = type(file.remove)     == "function"
    M.available.read_async = type(file.read_async) == "function"
end
function M.update(self, dt) end
function M.fixed_update(self, dt) end
function M.late_update(self, dt) end
function M.destroy(self) end
return M
