-- Phase 2b.5 Test 15 fixture: on_reload throws AFTER incrementing reload_count.
-- update increments normally so the test can verify the new instance is usable
-- after the reload completes despite the on_reload error.

local M = {}
M.update_count  = 0
M.reload_count  = 0

function M.begin_play(self) end

function M.update(self, dt)
    self.update_count = self.update_count + 1
end

function M.fixed_update(self, dt) end
function M.late_update(self, dt) end

function M.on_reload(self, old_state)
    self.reload_count = self.reload_count + 1
    error("boom in on_reload")
end

function M.destroy(self) end

return M
