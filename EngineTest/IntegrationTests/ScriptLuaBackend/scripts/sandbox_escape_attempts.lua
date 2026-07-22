local M = {}
M.escaped = false
M.escaped_via = ""
function M.begin_play(self)
    local escaped = false
    local via = ""

    -- 1. Direct access to io
    local ok1, r1 = pcall(function() return io.open end)
    if ok1 and r1 ~= nil then escaped = true; via = "io.open" end

    -- 2. Via _G
    local ok2, r2 = pcall(function() return _G.io.open end)
    if ok2 and r2 ~= nil then escaped = true; via = "_G.io.open" end

    -- 3. rawget(_G, "io") — does not throw, but should return nil
    local r3 = rawget(_G, "io")
    if r3 ~= nil then escaped = true; via = "rawget(_G,'io')" end

    -- 4. debug library
    local ok4, r4 = pcall(function() return debug.getregistry end)
    if ok4 and r4 ~= nil then escaped = true; via = "debug.getregistry" end

    -- 5. load / dofile
    local ok5 = pcall(function() return load("return 1") end)
    if ok5 then escaped = true; via = "load" end
    local ok6 = pcall(function() return dofile("/tmp/lua_sandbox_probe_nonexistent") end)
    if ok6 then escaped = true; via = "dofile" end

    -- 6. string metatable reflection
    local mt = getmetatable("")
    if mt and mt.__index and mt.__index.io then escaped = true; via = "string-mt.__index.io" end

    self.escaped = escaped
    self.escaped_via = via
end
function M.update(self, dt) end
function M.fixed_update(self, dt) end
function M.late_update(self, dt) end
function M.destroy(self) end
return M
