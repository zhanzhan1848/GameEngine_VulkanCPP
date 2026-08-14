-- Phase 2b.3 Test 6 fixture: hard reload with score migration via on_reload.
--
-- begin_play: no-op (fresh state).
-- update: increments update_count; on first update sets score=42.
-- on_reload: receives old instance table, migrates score field.
-- destroy: increments file-scope destroy_count (verification for Issue 1).

-- File-scope counter (module-local, not on M) — survives across old/new
-- instance tables so the test can verify destroy fired during reload.
local destroy_count = 0

local M = {}
M.update_count = 0
M.score = 0

function M.begin_play(self)
    -- Fresh state: update_count and score reset to 0 by table init.
end

function M.update(self, dt)
    self.update_count = self.update_count + 1
    if self.update_count == 1 then
        self.score = 42
    end
end

function M.on_reload(self, old_state)
    if old_state then
        self.score = old_state.score or 0
    end
end

function M.destroy(self)
    destroy_count = destroy_count + 1
end

function M.get_destroy_count(self)
    return destroy_count
end

return M
