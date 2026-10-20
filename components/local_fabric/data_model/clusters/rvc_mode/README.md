# RVC Mode

Matter clusters `0x0054` and `0x0055` are published as `rvc_run_mode` and `rvc_clean_mode`. Both use the same JSON shape. The device's `SupportedModes` supplies numeric IDs and display labels; these IDs are not standardized across manufacturers.

```json
{
  "properties": {
    "mode": {"value": 1, "label": "Cleaning"}
  },
  "actions": {
    "set_mode": {
      "arguments": {
        "mode": {"type": "integer", "min": 0, "max": 255,
                 "enum": [{"value": 0, "label": "Idle"},
                          {"value": 1, "label": "Cleaning"}]}
      }
    }
  }
}
```

`mode.value` is `null` until reported. If `SupportedModes` has not been reported, or exceeds the bounded mode table, the current numeric mode remains visible but `set_mode` is not advertised. Labels are device-provided and bounded to the UI's available storage; unknown current modes display `Mode N`. Invocations validate the ID against the reported choices and call `ChangeToMode`.
