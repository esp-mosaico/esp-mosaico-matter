# Color Control

Matter cluster `0x0300`, published as `color_control`. Manager exposes controls only after the corresponding feature attributes have been reported. Hue/saturation reports are used directly; XY reports are adapted to HSV. Color-temperature mireds are converted to kelvin using the reported physical bounds.

```json
{
  "properties": {
    "hue_degrees": {"value": 210, "unit": "degrees"},
    "saturation_percent": {"value": 72, "unit": "percent"},
    "color_temperature_kelvin": {"value": 3200, "unit": "kelvin"}
  },
  "actions": {
    "set_color": {
      "arguments": {
        "hue_degrees": {"type": "integer", "min": 0, "max": 359, "unit": "degrees"},
        "saturation_percent": {"type": "integer", "min": 0, "max": 100, "unit": "percent"}
      }
    },
    "set_color_temperature": {
      "arguments": {
        "color_temperature_kelvin": {"type": "integer", "min": 2000, "max": 6500, "unit": "kelvin"}
      }
    }
  }
}
```

The temperature bounds above are illustrative and vary by device. Color actions use commands that turn on an off light. Raw XY, color mode, and transition time remain private.
