-- SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
--
-- SPDX-License-Identifier: Apache-2.0
local matter = require("matter")
local json = require("json")
local output = require("matter_skill_output")
local devices = json.decode(matter.list_devices()).devices
table.sort(devices, function(a, b) return a.device_id < b.device_id end)
output.page(devices, args or {})
