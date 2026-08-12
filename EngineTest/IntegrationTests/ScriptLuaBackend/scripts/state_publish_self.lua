local M = {}
M.published = false
function M.begin_play(self)
    state.entities[entity_id].health = 100
    if state.entities[entity_id].health == 100 then self.published = true end
    -- Publish own entity_id for the reader fixture to find
    state.game.publisher_eid = entity_id
end
function M.update(self, dt) end
function M.fixed_update(self, dt) end
function M.late_update(self, dt) end
function M.destroy(self) end
return M
