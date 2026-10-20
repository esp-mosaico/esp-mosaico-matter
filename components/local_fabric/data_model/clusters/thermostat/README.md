# Thermostat

Matter cluster `0x0201`, published as `thermostat`. Manager reads local temperature, system mode, control sequence, FeatureMap, occupied heating and cooling setpoints, and their reported min/max limits. Setpoint actions appear only after the mode and relevant setpoint/limits are reported. In Auto mode they also require the opposite setpoint and deadband to keep the allowed ranges safe. The example below is for a device whose limits are 18–25 °C heat and 24–30 °C cool.

```json
{
  "properties": {
    "local_temperature_c": {"value": 20.5, "unit": "celsius"},
    "system_mode": {"value": "heat"},
    "heating_setpoint_c": {"value": 22, "unit": "celsius"},
    "cooling_setpoint_c": {"value": 25, "unit": "celsius"}
  },
  "actions": {
    "set_mode": {
      "arguments": {
        "mode": {"type": "string", "enum": ["off", "auto", "cool", "heat"]}
      }
    },
    "set_heating_setpoint": {
      "arguments": {
        "temperature_c": {"type": "number", "min": 18, "max": 25, "unit": "celsius"}
      }
    },
    "set_cooling_setpoint": {
      "arguments": {
        "temperature_c": {"type": "number", "min": 24, "max": 30, "unit": "celsius"}
      }
    }
  }
}
```

Values are `null` until reported. Schedules, presets, holds, and setback programming are intentionally omitted.
