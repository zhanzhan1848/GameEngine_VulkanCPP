-- Phase 2b.2 Test 4 fixture: ping/pong round-trip between instances.
--
-- Both instances subscribe to "ping" and "pong" in begin_play.
-- First instance's first update emits "ping".
-- Ping handler emits "pong" in response (exercises re-emit isolation).
-- Test reads back ping_count/pong_count to verify cross-instance delivery.

return {
    update_count = 0,
    ping_count = 0,
    pong_count = 0,

    begin_play = function(self)
        bus.on("ping", function(event)
            self.ping_count = self.ping_count + 1
            bus.emit("pong", { source = event.target })
        end)
        bus.on("pong", function(event)
            self.pong_count = self.pong_count + 1
        end)
    end,

    update = function(self, dt)
        if self.update_count == 0 then
            bus.emit("ping", { target = "self" })
        end
        self.update_count = self.update_count + 1
    end,

    destroy = function(self) end,
}
