local M = {}
M.read_target = -1
function M.begin_play(self)
    local target_eid = state.game.publisher_eid
    if target_eid then
        self.read_target = state.entities[target_eid].health or -1
    end
end
function M.update(self, dt) end
function M.fixed_update(self, dt) end
function M.late_update(self, dt) end
function M.destroy(self) end
return M
