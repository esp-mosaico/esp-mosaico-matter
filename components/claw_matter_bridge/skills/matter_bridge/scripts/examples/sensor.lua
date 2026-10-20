-- SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
-- SPDX-License-Identifier: Apache-2.0
-- Contact: true means closed. Occupancy: true means occupied.
local driver = {}
local previous, candidate, changed_at, debounce_ms
function driver.init(device, args)
    debounce_ms = args.debounce_ms or 50
    previous = device:read("sensor")
    candidate, changed_at = previous, 0
    device:report(previous)
end
function driver.apply() end
function driver.poll(device, now_ms)
    local value = device:read("sensor")
    if value ~= candidate then candidate, changed_at = value, now_ms end
    if value ~= previous and ((now_ms - changed_at) & 0x7fffffff) >= debounce_ms then
        previous = value
        device:report(value)
    end
end
return driver
