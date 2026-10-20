-- SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
-- SPDX-License-Identifier: Apache-2.0
local M = {}
local function clamp(v, low, high) return math.max(low, math.min(high, v)) end
local function hsv(h, s)
    h, s = h / 254 * 6, s / 254
    local c, x = s, s * (1 - math.abs(h % 2 - 1))
    local r, g, b
    if h < 1 then r, g, b = c, x, 0
    elseif h < 2 then r, g, b = x, c, 0
    elseif h < 3 then r, g, b = 0, c, x
    elseif h < 4 then r, g, b = 0, x, c
    elseif h < 5 then r, g, b = x, 0, c
    else r, g, b = c, 0, x end
    return r + 1 - c, g + 1 - c, b + 1 - c
end
local function xy(x, y)
    x, y = x / 65536, y / 65536
    if y <= 0 then return 0, 0, 0 end
    local X, Z = x / y, (1 - x - y) / y
    local r = math.max(0, 3.2406 * X - 1.5372 - 0.4986 * Z)
    local g = math.max(0, -0.9689 * X + 1.8758 + 0.0415 * Z)
    local b = math.max(0, 0.0557 * X - 0.2040 + 1.0570 * Z)
    local peak = math.max(1, r, g, b)
    local function gamma(v)
        v = v / peak
        return v <= 0.0031308 and 12.92 * v or 1.055 * v ^ (1 / 2.4) - 0.055
    end
    return gamma(r), gamma(g), gamma(b)
end
local function temperature(mired)
    local t = clamp(1000000 / math.max(1, mired), 2000, 6500) / 100
    -- RGB approximation of the requested white point; no separate white LED.
    local r = t <= 66 and 255 or 329.698727446 * (t - 60) ^ -0.1332047592
    local g = t <= 66 and 99.4708025861 * math.log(t) - 161.1195681661
        or 288.1221695283 * (t - 60) ^ -0.0755148492
    local b = t >= 66 and 255 or t <= 19 and 0
        or 138.5177312231 * math.log(t - 10) - 305.0447927307
    return clamp(r, 0, 255) / 255, clamp(g, 0, 255) / 255, clamp(b, 0, 255) / 255
end
function M.rgb(state)
    if not state.on then return 0, 0, 0 end
    local r, g, b
    if state.color_mode == 1 then r, g, b = xy(state.x, state.y)
    elseif state.color_mode == 2 then r, g, b = temperature(state.temperature)
    else r, g, b = hsv(clamp(state.hue, 0, 254), clamp(state.saturation, 0, 254)) end
    local level = clamp(state.level, 0, 254) / 254
    return math.floor(clamp(r, 0, 1) * level * 255 + 0.5),
           math.floor(clamp(g, 0, 1) * level * 255 + 0.5),
           math.floor(clamp(b, 0, 1) * level * 255 + 0.5)
end
return M
