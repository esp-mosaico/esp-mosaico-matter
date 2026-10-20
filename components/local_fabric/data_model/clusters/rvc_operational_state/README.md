# RVC Operational State

Matter cluster `0x0061`, published as `rvc_operational_state`. Manager maps standard operational-state identifiers to stable lowercase names and exposes basic operational commands.

```json
{
  "properties": {"state": {"value": "docked"}},
  "actions": {
    "pause": {"arguments": {}},
    "resume": {"arguments": {}},
    "go_home": {"arguments": {}}
  }
}
```

`state.value` is `null` until reported and `"unknown"` for an unrecognized identifier. Maps, areas, progress, errors, and vendor extensions are not exposed.
