-- SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
--
-- SPDX-License-Identifier: Apache-2.0
local matter = require("matter")
local json = require("json")
local output = require("matter_skill_output")
local query = args or {}
local arguments = "{}"
if query.arguments ~= nil then
    assert(type(query.arguments) == "table", "arguments must be a JSON object")
    for key in pairs(query.arguments) do
        assert(type(key) == "string", "arguments must be a JSON object, not an array")
    end
    arguments = json.encode(query.arguments)
end
output.print(matter.invoke_action(assert(query.device_id, "device_id is required"),
    assert(query.endpoint_id, "endpoint_id is required"),
    assert(query.cluster, "cluster is required"), assert(query.action, "action is required"),
    arguments))
