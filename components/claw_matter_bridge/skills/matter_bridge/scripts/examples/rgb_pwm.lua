-- SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
-- SPDX-License-Identifier: Apache-2.0
local color = require("matter_bridge_color")
local driver = {}
function driver.init(device)
    for _, role in ipairs({"red", "green", "blue"}) do device:pwm(role, 0) end
end
function driver.apply(device, state)
    local r, g, b = color.rgb(state)
    device:pwm("red", r / 255 * 100)
    device:pwm("green", g / 255 * 100)
    device:pwm("blue", b / 255 * 100)
end
return driver
