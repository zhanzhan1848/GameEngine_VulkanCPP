-- Phase 2b.5 Test 12 fixture: begin_play throws, update records dispatch.
--
-- begin_play fires synchronously during create_instance. If pcall catches
-- the error correctly, create_instance returns a valid script_id and the
-- instance table is populated. The test then calls script::update directly
-- and verifies update_count == 1.

local M = {}
M.update_count = 0

function M.begin_play(self)
    error("boom in begin_play")
end

function M.update(self, dt)
    self.update_count = self.update_count + 1
end

function M.fixed_update(self, dt) end
function M.late_update(self, dt) end
function M.destroy(self) end

return M
