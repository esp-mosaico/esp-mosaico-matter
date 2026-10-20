-- SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
-- SPDX-License-Identifier: Apache-2.0
local output = dofile("/system/skills/matter_bridge/scripts/print_result.lua")
output(require("matter_bridge").list_devices(args or {}))
