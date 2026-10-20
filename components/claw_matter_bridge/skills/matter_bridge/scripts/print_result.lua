-- SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
-- SPDX-License-Identifier: Apache-2.0
local bridge = require("matter_bridge")
return function(result, failure)
    local text = bridge.encode_json(result or {ok = false, error = failure})
    assert(#text <= 4000, "Bridge result exceeds script output limit")
    print(text)
end
