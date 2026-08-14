local M = {}
M.nil_count = 0
function M.begin_play(self)
    -- Each check is individual (do NOT use { io, package, ... } + ipairs:
    -- ipairs stops at the first nil, so a fully-nil table has length 0).
    if io             == nil then self.nil_count = self.nil_count + 1 end
    if package        == nil then self.nil_count = self.nil_count + 1 end
    if debug          == nil then self.nil_count = self.nil_count + 1 end
    if load           == nil then self.nil_count = self.nil_count + 1 end
    if loadfile       == nil then self.nil_count = self.nil_count + 1 end
    if dofile         == nil then self.nil_count = self.nil_count + 1 end
    if collectgarbage == nil then self.nil_count = self.nil_count + 1 end
    if os.execute     == nil then self.nil_count = self.nil_count + 1 end
    if os.exit        == nil then self.nil_count = self.nil_count + 1 end
    if os.getenv      == nil then self.nil_count = self.nil_count + 1 end
    if os.remove      == nil then self.nil_count = self.nil_count + 1 end
    if os.rename      == nil then self.nil_count = self.nil_count + 1 end
    if os.setlocale   == nil then self.nil_count = self.nil_count + 1 end
    if os.tmpname     == nil then self.nil_count = self.nil_count + 1 end
end
function M.update(self, dt) end
function M.fixed_update(self, dt) end
function M.late_update(self, dt) end
function M.destroy(self) end
return M
