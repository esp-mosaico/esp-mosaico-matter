# Fan Control

Matter cluster `0x0202`, published as `fan_control`. Manager reads `FanMode`, `FanModeSequence`, `PercentSetting`, and `PercentCurrent`. The published `percent` is the requested setting, not the measured speed. Mode choices appear only after `FanModeSequence` is reported; the percentage action requires a reported, non-null `PercentSetting`.

```json
{
  "properties": {
    "mode": {"value": "auto"},
    "percent": {"value": 40, "unit": "percent"}
  },
  "actions": {
    "set_mode": {
      "arguments": {
        "mode": {"type": "string", "enum": ["off", "low", "medium", "high", "auto"]}
      }
    },
    "set_percent": {
      "arguments": {
        "percent": {"type": "integer", "min": 0, "max": 100, "unit": "percent"}
      }
    }
  }
}
```

Unreported or null values are emitted as `null`. Rocking, wind, and airflow-direction extensions are not exposed.
