local M = {}
-- Note: state.game.* writes persist in the C++ ScriptState singleton (Meyers singleton).
-- Test 25 calls script::shutdown() at the end, which clears game_store_.
-- Copying this fixture pattern without shutdown will leak state across tests.
M.read_after_write = false
M.read_missing = false
M.overwrite = false
function M.begin_play(self)
    state.game.counter = nil            -- ensure clean slate
    state.game.counter = 42
    if state.game.counter == 42 then self.read_after_write = true end
    if state.game.missing_key == nil then self.read_missing = true end
    state.game.counter = 100
    if state.game.counter == 100 then self.overwrite = true end
end
function M.update(self, dt) end
function M.fixed_update(self, dt) end
function M.late_update(self, dt) end
function M.destroy(self) end
return M
