-- SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
--
-- SPDX-License-Identifier: Apache-2.0
local json = require("json")
local output = {}
-- Leave room for the tool's newline/error framing inside its 4 KiB buffer.
local max_bytes = 3500

function output.print(value)
    local text = type(value) == "string" and value or json.encode(value)
    assert(#text <= max_bytes, "Output too large; narrow the query")
    print(text)
end

function output.keys(object)
    local keys = {}
    for key in pairs(object or {}) do keys[#keys + 1] = key end
    table.sort(keys)
    return keys
end

-- Pages shrink to the byte budget as well as the requested item count.
-- Assemble the array explicitly: the JSON module encodes an empty table as {}.
function output.page(items, args, context)
    local offset, limit = args.offset or 0, args.limit or 8
    assert(type(offset) == "number" and offset >= 0 and offset % 1 == 0,
        "offset must be a nonnegative integer")
    assert(type(limit) == "number" and limit >= 1 and limit <= 16 and limit % 1 == 0,
        "limit must be an integer from 1 to 16")
    context = context or {}
    context.total, context.offset = #items, offset
    local encoded = {}
    local function encode_page()
        context.next_offset = offset + #encoded < #items and offset + #encoded or nil
        local metadata = json.encode(context)
        return metadata:sub(1, -2) .. ',"items":[' .. table.concat(encoded, ",") .. "]}"
    end
    for index = offset + 1, math.min(#items, offset + limit) do
        encoded[#encoded + 1] = json.encode(items[index])
        if #encode_page() > max_bytes then
            encoded[#encoded] = nil
            assert(#encoded > 0, "Item too large; narrow the query")
            break
        end
    end
    output.print(encode_page())
end

return output
