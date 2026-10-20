# Relative Humidity Measurement

Matter cluster `0x0405`, published as `relative_humidity_measurement`. Manager converts `MeasuredValue` from hundredths of a percent. There are no actions.

```json
{
  "properties": {
    "humidity_percent": {"value": 47.25, "unit": "percent"}
  },
  "actions": {}
}
```

The value is `null` when Matter reports null or before the first report.
