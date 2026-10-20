-- SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
-- SPDX-License-Identifier: Apache-2.0

-- Driver for one LED of an esp-mosaico interaction module.
-- The device is virtual and uses driver_args = {slot = 0|1, led_index = 0|1|2};
-- hardware is owned by the interact accessory rather than a generic GPIO,
-- LEDC, or RMT driver in claw_matter_bridge.

local interact = require("mosaico_interact")

local slot = 0
local index = 0

local driver = {}

function driver.init(device, driver_args)
    slot = driver_args.slot or 0
    index = driver_args.led_index or 0
    interact.led_set(slot, index, false)
end

function driver.apply(device, state)
    interact.led_set(slot, index, state.on)
end

return driver
