# Level Control

Matter cluster `0x0008`, published as `level_control`. Manager converts `CurrentLevel` from Matter's `1..254` range to `1..100` percent. `set_level` uses `MoveToLevelWithOnOff`, so changing the level also turns on an off device. Transition time is fixed internally and not exposed.

```json
{
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
```

`level_percent.value` is `null` until a usable report arrives.
