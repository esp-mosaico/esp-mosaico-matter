# Power Source

Matter cluster `0x002F`, published as `power_source`. Manager converts `BatPercentRemaining` from half-percent units to percent. There are no actions.

```json
{
  "properties": {
    "battery_percent": {"value": 86, "unit": "percent"}
  },
  "actions": {}
}
```

The value is `null` when Matter reports null or before the first report. Other power-source diagnostics are not exposed.
