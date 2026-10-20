# Local Fabric data-model schema

## Identity and topology

- `device_id`: stable, persisted, nonzero `uint32_t`; independent of topology.
- Matter node IDs, root endpoints, parent links, aggregators, numeric cluster IDs,
  and raw device-type IDs are private.
- Endpoint 0 is administrative. Public endpoint keys are decimal endpoint IDs.
- Each direct aggregator child starts a device. Non-aggregator descendants belong
  to that device. Standalone roots form devices directly.
- Nested composition is supported. Cycles and ambiguous multiple non-root
  parents are rejected.
- `type` is presentation metadata derived from Matter DeviceType IDs. Cluster
  availability comes from each endpoint's ServerList, independently of `type`.

## Device JSON

List: `{"devices": [<summary>, ...]}`. Summary fields:

```json
{
  "device_id": 7,
  "type": "light",
  "name": "Desk light",
  "room": "Office",
  "online": true
}
```

Detail contains the summary fields plus `endpoints`:

```json
{
  "device_id": 7,
  "type": "light",
  "name": "Desk light",
  "room": "Office",
  "online": true,
  "endpoints": {
    "2": {
      "name": "Endpoint 2",
      "clusters": {
        "level_control": {
          "properties": {
            "level_percent": {"value": 65, "unit": "percent"}
          },
          "actions": {
            "set_level": {
              "arguments": {
                "level_percent": {
                  "type": "integer", "min": 1, "max": 100, "unit": "percent"
                }
              }
            }
          }
        }
      }
    }
  }
}
```

- Device `name` and `room` are user-selected.
- Endpoint names come from Descriptor TagList labels, then recognized standard
  location/position tags, with `Endpoint N` as fallback. They may change with
  reports and do not determine identity or validity.
- Each cluster contains `properties` and `actions` maps.
- Each property contains `value`, with optional machine `unit` or device-provided
  display `label` for numeric modes.
- `value: null` means no usable authoritative report. An absent property or
  cluster is unsupported by that endpoint.

## Actions

- Address: `(device_id, endpoint_id, cluster, action)`.
- `arguments_json` contains exactly the advertised arguments: all are required;
  unknown arguments are rejected. No-input actions advertise `arguments: {}`.
- Argument `type`: `boolean`, `integer`, `number`, or `string`.
- Optional metadata: numeric `min`/`max`, machine `unit`, and `enum`.
- String enums contain strings. Numeric mode enums contain
  `{"value": <integer>, "label": <string>}` entries.
- Action availability, bounds, and enum choices depend on reported capabilities.
- Property state updates come from committed reports, not action completion.

## Notifications

- `properties_changed` delivers an atomically committed batch of
  `fabric_manager_property_change_t` records.
- Record fields: `device_id`, `endpoint_id`, `cluster`, `property`,
  `previous_value_json`, `value_json`. JSON value pointers are callback-scoped.
- `device_set_changed` covers identity, membership, presentation, and reachability
  changes. List/detail JSON provides the updated device set.
- Callback declarations: [`fabric_manager.h`](../include/fabric_manager.h).

## Cluster contracts

| Cluster key | Properties, actions, and limits |
| --- | --- |
| `on_off` | [On/Off](clusters/on_off/README.md) |
| `level_control` | [Level Control](clusters/level_control/README.md) |
| `color_control` | [Color Control](clusters/color_control/README.md) |
| `temperature_measurement` | [Temperature](clusters/temperature_measurement/README.md) |
| `relative_humidity_measurement` | [Humidity](clusters/relative_humidity_measurement/README.md) |
| `illuminance_measurement` | [Illuminance](clusters/illuminance_measurement/README.md) |
| `occupancy_sensing` | [Occupancy](clusters/occupancy_sensing/README.md) |
| `boolean_state` | [Boolean State](clusters/boolean_state/README.md) |
| `power_source` | [Power Source](clusters/power_source/README.md) |
| `thermostat` | [Thermostat](clusters/thermostat/README.md) |
| `fan_control` | [Fan Control](clusters/fan_control/README.md) |
| `rvc_run_mode`, `rvc_clean_mode` | [RVC Mode](clusters/rvc_mode/README.md) |
| `rvc_operational_state` | [RVC Operational State](clusters/rvc_operational_state/README.md) |
