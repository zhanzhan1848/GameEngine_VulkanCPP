-- minimal_script.lua
-- Test fixture for Test 1 (lua_lifecycle_hooks_called) and Test 3
-- (lua_multi_instance_per_type). All state lives in instance fields so
-- multiple instances of this type don't share/clobber each other.

return {
    begin_play_called = false,
    update_count = 0,
    destroy_called = false,

    begin_play = function(self)
        self.begin_play_called = true
    end,

    update = function(self, dt)
        self.update_count = self.update_count + 1
    end,

    destroy = function(self)
        self.destroy_called = true
    end,
}
