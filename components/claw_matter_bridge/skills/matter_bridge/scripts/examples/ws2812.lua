-- SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
-- SPDX-License-Identifier: Apache-2.0
local color = require("matter_bridge_color")
local driver = {}
function driver.init(device) device:rgb("color", 0, 0, 0) end
function driver.apply(device, state) device:rgb("color", color.rgb(state)) end
return driver
