# Matter bridge

`matter_bridge` owns one Matter server with an aggregator and a persistent
catalog of dynamic bridged devices. It has no CLAW, Lua or GPIO dependency.
The public API is split into [bridge_core.h](include/bridge_core.h) and
[bridge_dev_mngr.h](include/bridge_dev_mngr.h).

The C API uses the `bridge_core_*` and `bridge_dev_mngr_*` prefixes, with
`BRIDGE_DEV_MNGR_*` constants. These replace the previous API names; callers
must use the new headers and symbols. Device catalogs keep the existing
`mb_dmgr` NVS namespace so saved devices survive the rename.

Create a core, attach a device manager, then start the core. The catalog keeps
stable identity, profile and endpoint allocation; applications reactivate the
devices they can serve after each boot. Lifecycle and attribute operations are
asynchronous and complete on the Matter task with a request id. Catalog
snapshots (`get_device` and `get_devices`) are synchronous.

Built-in profiles use a persistent definition such as:

```c
bridge_dev_mngr_device_definition_t definition = {
    .id = "hall-presence-01",
    .name = "Hall Presence",
    .profile_id = BRIDGE_DEV_MNGR_PROFILE_OCCUPANCY_SENSOR,
    .device_type = 0x0107,
};
```

A custom profile uses the same persistent definition plus a runtime-only
`bridge_dev_mngr_profile_t` supplied to `activate_device`. Function pointers and
context are never persisted. Cluster-specific behavior lives under
`dev_mngr/clusters/<cluster-name>`; ordinary attributes use the generic bounded
path/value adapter.

Remote Matter writes are delivered to every registered listener. Local
`report_attribute` calls suppress that feedback. Reachability is updated
through `update_device` with `BRIDGE_DEV_MNGR_UPDATE_REACHABLE`, alongside other
patchable device metadata such as the name.
