-- Phase 2b.5 Test 13 + 14 fixture: update throws AFTER incrementing counter,
-- late_update increments normally.
--
-- The increment-before-error pattern lets the test verify "update was dispatched"
-- not just "no crash". Lua's pcall preserves side effects committed before error().

local M = {}
M.update_count = 0
M.late_count    = 0

function M.begin_play(self) end

function M.update(self, dt)
    self.update_count = self.update_count + 1
    error("boom in update")
end

function M.fixed_update(self, dt) end

function M.late_update(self, dt)
    self.late_count = self.late_count + 1
end

function M.destroy(self) end

return M
