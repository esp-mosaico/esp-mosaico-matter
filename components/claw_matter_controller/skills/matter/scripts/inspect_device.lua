-- SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
--
-- SPDX-License-Identifier: Apache-2.0
local matter = require("matter")
local json = require("json")
local output = require("matter_skill_output")
local query = args or {}
local device = json.decode(matter.get_device(assert(query.device_id, "device_id is required")))
local context = {device_id = device.device_id, type = device.type, name = device.name,
    room = device.room, online = device.online}
local items = {}

if query.endpoint_id == nil then
    assert(query.cluster == nil, "cluster requires endpoint_id")
    local endpoints = output.keys(device.endpoints)
    table.sort(endpoints, function(a, b) return tonumber(a) < tonumber(b) end)
    for _, key in ipairs(endpoints) do
        local endpoint = device.endpoints[key]
        items[#items + 1] = {endpoint_id = tonumber(key), name = endpoint.name,
            clusters = output.keys(endpoint.clusters)}
    end
else
    assert(type(query.endpoint_id) == "number" and query.endpoint_id > 0 and
        query.endpoint_id % 1 == 0, "endpoint_id must be a positive integer")
    local endpoint = assert(device.endpoints[string.format("%d", query.endpoint_id)], "Endpoint not found")
    context.endpoint_id = query.endpoint_id
    context.endpoint_name = endpoint.name
    if query.cluster == nil then
        for _, name in ipairs(output.keys(endpoint.clusters)) do
            local cluster = endpoint.clusters[name]
            items[#items + 1] = {cluster = name, properties = output.keys(cluster.properties),
                actions = output.keys(cluster.actions)}
        end
    else
        local cluster = assert(endpoint.clusters[query.cluster], "Cluster not found")
        context.cluster = query.cluster
        for _, name in ipairs(output.keys(cluster.properties)) do
            local property = cluster.properties[name]
            -- json.decode drops null fields; distinguish unknown from false/zero.
            items[#items + 1] = {kind = "property", name = name,
                known = property.value ~= nil, value = property.value,
                unit = property.unit, label = property.label}
        end
        for _, name in ipairs(output.keys(cluster.actions)) do
            items[#items + 1] = {kind = "action", name = name, schema = cluster.actions[name]}
        end
    end
end

output.page(items, query, context)
