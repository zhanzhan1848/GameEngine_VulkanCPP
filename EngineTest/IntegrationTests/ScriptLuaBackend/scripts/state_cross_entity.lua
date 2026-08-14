local M = {}
M.read_target = -1
M.write_blocked = false
function M.begin_play(self)
    local target_eid = state.game.publisher_eid
    if target_eid then
        self.read_target = state.entities[target_eid].health or -1
        -- Verify owner-write enforcement: reader must NOT be able to write
        -- to publisher's entity state.
        local ok = pcall(function()
            state.entities[target_eid].health = 999
        end)
        if not ok then self.write_blocked = true end
    end
end
function M.update(self, dt) end
function M.fixed_update(self, dt) end
function M.late_update(self, dt) end
function M.destroy(self) end
return M
