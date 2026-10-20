# CLAW Matter bridge

`claw_matter_bridge` is the CLAW-facing management layer for bridged devices
owned by `matter_bridge`. The component is implemented in C++ and exposes
a small C ABI from `include/claw_matter_bridge.h`.

The source layout separates lifecycle/runtime work from its adapters:

- `claw_matter_bridge.cpp` owns persistent state, driver supervision, and the
  start/stop lifecycle.
- `claw_matter_bridge_api.cpp` implements explicit device-management APIs.
- `claw_matter_bridge_lua.cpp` implements the device-scoped Lua driver API.
- `claw_matter_bridge_lua_json.cpp` adapts management operations to the
  installed `matter_bridge` Lua module.
- `claw_matter_bridge_model.cpp` and `claw_matter_bridge_hardware.cpp` contain
  definition validation, persistence helpers, and hardware ownership.

`claw_matter_bridge_stop()` stops only the CLAW management plane. It waits for
an in-flight management operation and then rejects create, remove, list,
inspect, restart, update, and factory-reset requests. Existing Matter
endpoints, reachability, drivers, listeners, and the supervisor remain active.
Calling `claw_matter_bridge_start()` again with the original configuration
re-enables management without rebuilding those endpoints.

The installed skill and Lua module deliberately keep the stable runtime name
`matter_bridge`. JSON returned through the public C API is allocated by cJSON
and must be released with `cJSON_free()`.
