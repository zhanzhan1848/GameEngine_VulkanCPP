-- Phase 2b.4 Test 9 + 10 fixture: complete lifecycle with order tracking.
--
-- Counters: each hook increments its own counter.
-- Step: file-scope counter, snapshot into fixed_step/update_step/late_step
--       at hook entry — used by Test 10 to verify frame_tick order.
-- dt echo: last_fixed_dt / last_late_dt read back by Test 9 to verify
--          float→double propagation.

local step = 0  -- file-scope (module-local)

local M = {}
M.fixed_count    = 0
M.update_count   = 0
M.late_count     = 0
M.last_fixed_dt  = 0
M.last_late_dt   = 0
M.fixed_step     = 0
M.update_step    = 0
M.late_step      = 0

function M.begin_play(self) end

function M.fixed_update(self, dt)
    step = step + 1
    self.fixed_count   = self.fixed_count + 1
    self.last_fixed_dt = dt
    self.fixed_step    = step
end

function M.update(self, dt)
    step = step + 1
    self.update_count  = self.update_count + 1
    self.update_step   = step
end

function M.late_update(self, dt)
    step = step + 1
    self.late_count    = self.late_count + 1
    self.last_late_dt  = dt
    self.late_step     = step
end

function M.destroy(self) end

return M
