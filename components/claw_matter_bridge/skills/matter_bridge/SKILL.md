---
{
  "name": "matter_bridge",
  "description": "Create, repair, or remove GPIO-backed bridged Matter devices using persistent Lua drivers.",
  "metadata": {
    "cap_groups": ["cap_lua", "cap_files"],
    "manage_mode": "readonly"
  }
}
---

# GPIO Matter bridge

The native management component is `claw_matter_bridge`; the installed skill
and Lua module intentionally retain the runtime name `matter_bridge`.

Use the installed Lua management scripts for devices hosted by this ESP, rather than controller
tools for remote Matter nodes. A supported Matter profile plus a Lua driver
turns the user's GPIO description into a device. This does not create arbitrary
Matter device types or clusters.

1. Run `get_info.lua` and `list_devices.lua` first using `lua_run_script`.
2. Obtain a unique name, device type, GPIO roles, hardware interface, polarity,
   and input wiring/pulls. Ask about missing hardware details. Never guess a
   reserved pin or reuse another device's GPIO.
3. Read the relevant example below and adapt its source. Submit the full Lua
   source, pin declarations, and optional `driver_args` with
   `add_device.lua`. Do not set `virtual: true`; that flag is reserved for the
   on-device hot-plug path (`add_virtual_device`) and `add_device` rejects it.
   The script writes the driver and starts it;
   do not start it separately through `lua_run` or `thread.start`.
4. Say the device was added only when the script prints `ok: true` and ready.
   Report its stable `device_id`, name and endpoint ID. Inspect errors on failure.
5. For removal, list devices, resolve the user's name to its `device_id`, and
   run `remove_device.lua` with that ID. Clarify an ambiguous description. A
   failed removal can be pending cleanup; do not claim completion.

Three definitions may exist, including failed devices. Outputs start off after
reboot. Driver failure turns outputs off and retains an unreachable endpoint.
Use `update_driver.lua` to repair code or `restart_device.lua`
to explicitly retry. Updating source preserves pins, type and Matter identity;
change hardware/type by removing and recreating the device.

## Management scripts

Call `lua_run_script` with an installed path, argument object, and a timeout:

```json
{"path":"/system/skills/matter_bridge/scripts/get_info.lua","args":{"open_commissioning_window":false},"timeout_ms":15000}
```

All scripts live under `/system/skills/matter_bridge/scripts/`:

| Script | Arguments |
|---|---|
| `get_info.lua` | Optional `open_commissioning_window` boolean |
| `list_devices.lua` | `{}` |
| `add_device.lua` | `name`, `type`, `pins`, full `driver_source`, optional object `driver_args` |
| `remove_device.lua` | `device_id` |
| `restart_device.lua` | `device_id` |
| `update_driver.lua` | `device_id`, full `driver_source`, optional object `driver_args` |

Use a 30000 ms timeout for creation, removal, or driver repair. Scripts print
JSON results; failures print `ok: false` with an error code and message.
A timeout may leave an operation pending. List devices before retrying creation
or removal. Never start these management scripts as permanent background jobs.

Custom management scripts use `require("matter_bridge")` and the matching
function names (without `.lua`) with the same argument tables. Functions return
`result` or `nil, {code=..., message=...}`. Invalid argument shapes raise errors.
Management calls are rejected from bridge-owned device driver jobs.
`bridge.encode_json()` preserves integer values and empty result arrays.
`bridge.null` represents JSON null.

## Pin declarations and examples

Each pin has `role`, `gpio`, and `mode` (`input`, `output`, `pwm`, `strip`).
`active_low` defaults false and reverses logical digital/PWM levels. Inputs may
specify `pull: "none"`, `"up"`, or `"down"`; default none. A WS2812 strip needs
`led_count: 1..256` and `active_low: false`. PWM is fixed at 2000 Hz, 12 bits.

| Matter `type` | Required roles | Driver example |
|---|---|---|
| `on_off_light`, `outlet` | `power` / output | `{CUR_SKILL_DIR}/scripts/examples/on_off.lua` |
| `dimmable_light` | `level` / pwm | `{CUR_SKILL_DIR}/scripts/examples/dimmable.lua` |
| `color_light` / RGB PWM | `red`, `green`, `blue` / pwm | `{CUR_SKILL_DIR}/scripts/examples/rgb_pwm.lua` |
| `color_light` / WS2812 | `color` / strip | `{CUR_SKILL_DIR}/scripts/examples/ws2812.lua` |
| `contact_sensor`, `occupancy_sensor` | `sensor` / input | `{CUR_SKILL_DIR}/scripts/examples/sensor.lua` |

Use three PWM roles OR one strip role for a color light. Every optional hardware
pin, such as a light's physical button, must also be declared. Sensor scripts
report true for closed contacts or occupied spaces. Set active_low to match the
electrical signal and debounce digital inputs.

## Driver API

Return a table containing `init(device, driver_args)` and `apply(device, state)`;
optionally add `poll(device, now_ms)`. The bundled runner initializes hardware,
applies the off state, samples inputs, marks readiness, and polls every 50 ms.
Callbacks should finish within 200 ms, and must not contain permanent loops or
start additional jobs. Native cleanup always stops hardware on exit. An optional
`stop` callback is best-effort only; correctness must not depend on it.

- `device:read(role)` returns logical input boolean.
- `device:write(role, boolean)` writes a logical digital output.
- `device:pwm(role, duty_percent)` accepts 0..100.
- `device:rgb(role, red, green, blue)` sets the entire strip; channels 0..255.
- `device:report(boolean)` updates the sensor's state, or a light/outlet's OnOff
  state after a physical control. Apply physical output changes before reporting.
- `device:state()` returns state and a changed flag; the runner normally owns it.

State contains `on`, `level` (0..254), `hue`, `saturation` (0..254), `color_mode`
(0 hue/saturation, 1 XY, 2 color temperature), `x`, `y` (Matter uint16 XY),
`temperature` (mireds), and `sensor`. `require("matter_bridge_color").rgb(state)`
converts color and brightness to RGB, including an RGB approximation of white.
The bridge's scoped API handles GPIO polarity and ownership; use it rather than
raw GPIO, LEDC, or strip modules.

`bridge.now_ms()` wraps at 2^31 milliseconds. For elapsed times use
`(now_ms - previous_ms) & 0x7fffffff`, as in the sensor template.
