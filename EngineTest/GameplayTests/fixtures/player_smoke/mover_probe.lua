local M = {}
M.n = 0
function M:update(dt)
    M.n = M.n + 1
    if M.n % 60 == 0 then
        local p = entity.position()
        print("[mover] f=" .. M.n .. " x=" .. tostring(p))
    end
end
return M
