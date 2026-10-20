# Temperature Measurement

Matter cluster `0x0402`, published as `temperature_measurement`. Manager converts `MeasuredValue` from hundredths of a degree Celsius. There are no actions.

```json
{
  "properties": {
    "temperature_celsius": {"value": 21.5, "unit": "celsius"}
  },
  "actions": {}
}
```

The value is `null` when Matter reports null or before the first report.
