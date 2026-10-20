# Occupancy Sensing

Matter cluster `0x0406`, published as `occupancy_sensing`. Manager maps the occupied bit from `Occupancy` to a boolean. There are no actions.

```json
{
  "properties": {"occupied": {"value": true}},
  "actions": {}
}
```

`occupied.value` is `null` until reported.
