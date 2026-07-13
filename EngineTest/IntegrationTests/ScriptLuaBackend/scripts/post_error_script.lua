local M = {}
M.fired = false
function M.begin_play(self)
    post(function()
        error("boom in posted callback")
    end)
    post(function()
        self.fired = true
    end)
end
function M.update(self, dt) end
function M.fixed_update(self, dt) end
function M.late_update(self, dt) end
function M.destroy(self) end
return M
