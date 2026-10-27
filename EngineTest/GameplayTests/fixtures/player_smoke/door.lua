local M = {}
function M:begin_play()
    bus.on("button.near", function(payload)
        if game.state.get() ~= "open" then
            game.state.transition("open")
        end
    end)
    bus.on("door.opened", function(payload)
        entity.set_position(0, 3, -2)
        file.write("save://door_opened.txt", "opened")
    end)
end
function M:update(dt) end
return M
