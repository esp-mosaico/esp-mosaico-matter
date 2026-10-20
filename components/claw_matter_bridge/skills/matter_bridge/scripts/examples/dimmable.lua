-- SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
-- SPDX-License-Identifier: Apache-2.0
local driver = {}
function driver.init(device) device:pwm("level", 0) end
function driver.apply(device, state)
    device:pwm("level", state.on and state.level / 254 * 100 or 0)
end
return driver
