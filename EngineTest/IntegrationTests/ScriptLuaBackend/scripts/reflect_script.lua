-- reflect_script.lua
-- Test fixture for Test 2 (lua_reflect_declares_properties).
-- Uses LuaBackend's Lua-side visitor API (added in Task 6) to declare 2 props.

return {
    speed = 1.0,
    count = 0,

    reflect = function(self, visitor)
        visitor:property("speed", "float32")
        visitor:property("count", "int32")
    end,
}
