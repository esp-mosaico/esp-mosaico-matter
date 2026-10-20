# Illuminance Measurement

Matter cluster `0x0400`, published as `illuminance_measurement`. Manager converts Matter's logarithmic `MeasuredValue` to lux. There are no actions.

```json
{
  "properties": {
    "illuminance_lux": {"value": 320.1, "unit": "lux"}
  },
  "actions": {}
}
```

The value is `null` when Matter reports null, zero, or has not reported yet.
