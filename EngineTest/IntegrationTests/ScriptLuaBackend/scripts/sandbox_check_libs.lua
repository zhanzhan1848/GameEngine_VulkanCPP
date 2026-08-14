local M = {}
M.checks_passed = 0
function M.begin_play(self)
    if math.abs(-5)                  == 5    then self.checks_passed = self.checks_passed + 1 end
    if string.upper("hi")            == "HI" then self.checks_passed = self.checks_passed + 1 end
    if table.concat({"x","y"}, "+")  == "x+y" then self.checks_passed = self.checks_passed + 1 end
    if utf8.codepoint("A")           == 65   then self.checks_passed = self.checks_passed + 1 end
end
function M.update(self, dt) end
function M.fixed_update(self, dt) end
function M.late_update(self, dt) end
function M.destroy(self) end
return M
