-- SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
-- SPDX-License-Identifier: Apache-2.0
local bridge = require("matter_bridge")
package.path = args.library_dir .. "/lib/?.lua;" .. package.path
local device = bridge.attach(args.device_id, args.generation)
local chunk, message = loadfile(args.driver_path, "t", _ENV)
assert(chunk, message)
local driver = chunk()
assert(type(driver) == "table", "Driver must return a table")
assert(type(driver.init) == "function" and type(driver.apply) == "function",
       "Driver requires init and apply functions")
local ok, failure = xpcall(function()
    driver.init(device, args.driver_args or {})
    local initial = device:state()
    driver.apply(device, initial)
    if driver.poll then driver.poll(device, bridge.now_ms()) end
    device:ready()
    while true do
        local state, changed = device:state()
        if changed then driver.apply(device, state) end
        if driver.poll then driver.poll(device, bridge.now_ms()) end
        bridge.sleep_ms(50)
    end
end, debug.traceback)
if driver.stop then pcall(driver.stop, device) end
if not ok then error(failure, 0) end
-- Native exit cleanup runs even on an error, hang cancellation or job stop.
-- A driver cannot bypass output-off cleanup by omitting a stop callback.
