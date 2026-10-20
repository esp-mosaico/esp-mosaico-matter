# CLAW Matter controller

`claw_matter_controller` is a small agent-facing adapter over the authoritative
Local Fabric device manager (DMGR). It exposes the same numeric-device →
endpoint → cluster schema to C and Lua, plus bounded transient delivery of
committed property changes. It owns no Matter state cache and no durable
automation configuration.

## Lifecycle and events

Start it after DMGR. Filters independently match any explicit coordinate; null
fields are wildcards:

```c
ESP_ERROR_CHECK(claw_matter_controller_start(dmgr));

const fabric_manager_device_id_t device_id = 7;
const uint16_t endpoint_id = 2;
const claw_matter_event_filter_t filter = {
    .device_id = &device_id,
    .endpoint_id = &endpoint_id,
    .cluster = "on_off",
    .property = "on",
};
claw_matter_subscription_id_t subscription_id;
ESP_ERROR_CHECK(claw_matter_subscribe(
    &filter, on_matter_changes, application, &subscription_id));
```

A null filter matches every committed property change. Filters are copied when
registered. Callback data is callback-scoped. `claw_matter_unsubscribe()` waits
for an active callback and also supports self-unsubscribe. A callback may
unsubscribe a later subscription in the same batch; that subscription is
skipped immediately. Subscriptions registered during a callback begin with
the next batch. Subscription IDs are not reused during one controller run.

Callbacks run on the controller worker. Calling
`claw_matter_controller_stop()` from a callback returns
`ESP_ERR_INVALID_STATE` before changing controller state. Request a stop from
another task if an event should shut down the adapter.

DMGR callbacks copy complete batches into one bounded queue. A batch is dropped
in full if it exceeds configured bounds, JSON does not fit, allocation fails,
or the queue is full. `claw_matter_dropped_batch_count()` reports the cumulative
count. There is no replay, coalescing, or pending-state store.

## Shared operations

- `claw_matter_list_devices_json()` returns DMGR's `{ "devices": [...] }` list;
- `claw_matter_get_device_json(device_id)` returns endpoint/cluster detail;
- `claw_matter_get_property_json(device_id, endpoint, cluster, property)`
  returns one property's JSON value;
- `claw_matter_invoke_action(device_id, endpoint, cluster, action, arguments)`
  submits an advertised action.

Returned text uses cJSON allocation. Successful action completion never
synthesizes state; a later Matter report remains authoritative.

## Lua

Call `claw_matter_lua_register_module()` before `app_claw_start()`. This example
assumes the first listed device has an `on_off` cluster on endpoint 2:

```lua
local matter = require("matter")
local json = require("json")

local list = json.decode(matter.list_devices())
assert(list.devices[1], "Commission a device first")
local device = json.decode(matter.get_device(list.devices[1].device_id))
local value = json.decode(matter.get_property(
    device.device_id, 2, "on_off", "on"
))
assert(type(value) == "boolean", "Wait for an OnOff report before toggling")
local submission = matter.invoke_action(
    device.device_id, 2, "on_off", "set", json.encode({ on = not value })
)
```

Action argument contracts are inline under
`endpoints[endpoint].clusters[cluster].actions[action].arguments`; consumers do
not need a separate schema registry. The Lua module deliberately has no
subscription or automation CRUD API. An upper application owns durable rules
and passes matched batches through its normal job-input mechanism.

## Agent skill

The bundled [Matter skill](skills/matter/SKILL.md) teaches discovery and control
through the semantic model, including endpoint selection, argument schemas,
cached-state interpretation, and asynchronous submission semantics. Its Lua
scripts page device/capability queries within the tool output budget and retain
null, false, and zero in targeted property reads.

The hub's existing skill-builder integration stages this component's `skills/`
directory into the system filesystem. The skill activates the `cap_lua` group;
its scripts require the registered `matter` and `json` modules.
