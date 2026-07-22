local M = {}
M.value_read = 0
function M.begin_play(self)
    if state.types.StateTypeLevel.counter == nil then
        state.types.StateTypeLevel.counter = 7
    end
    self.value_read = state.types.StateTypeLevel.counter or 0
end
function M.update(self, dt) end
function M.fixed_update(self, dt) end
function M.late_update(self, dt) end
function M.destroy(self) end
return M
