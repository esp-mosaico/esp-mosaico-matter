-- SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
--
-- SPDX-License-Identifier: Apache-2.0
local matter = require("matter")
local json = require("json")
local output = require("matter_skill_output")
local query = args or {}
local value = matter.get_property(assert(query.device_id, "device_id is required"),
    assert(query.endpoint_id, "endpoint_id is required"),
    assert(query.cluster, "cluster is required"), assert(query.property, "property is required"))
-- Keep the native JSON literal so false, zero and null remain distinct.
local context = json.encode({device_id = query.device_id, endpoint_id = query.endpoint_id,
    cluster = query.cluster, property = query.property, source = "cache"})
output.print(context:sub(1, -2) .. ',"value":' .. value .. "}")
