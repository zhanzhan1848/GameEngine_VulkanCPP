local M = {}
M.checks_passed = 0
function M.begin_play(self)
    local ok, _ = pcall(function()
        -- Lua closures capture `self` by reference, so incrementing
        -- self.checks_passed inside the pcall closure works correctly.
        local t = { a = 1, b = 2 }
        local sum = 0
        for _, v in pairs(t) do sum = sum + v end
        if sum == 3 then self.checks_passed = self.checks_passed + 1 end
        if tonumber("123") == 123 then self.checks_passed = self.checks_passed + 1 end
        if tostring(42) == "42" then self.checks_passed = self.checks_passed + 1 end
        local r1 = select(2, "a", "b", "c")
        if r1 == "b" then self.checks_passed = self.checks_passed + 1 end
    end)
    if not ok then self.checks_passed = -1 end  -- signal pcall failure distinctly
end
function M.update(self, dt) end
function M.fixed_update(self, dt) end
function M.late_update(self, dt) end
function M.destroy(self) end
return M
