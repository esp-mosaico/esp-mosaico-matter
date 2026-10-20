-- SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
-- SPDX-License-Identifier: Apache-2.0
local driver = {}
function driver.init(device) device:write("power", false) end
function driver.apply(device, state) device:write("power", state.on) end
return driver
