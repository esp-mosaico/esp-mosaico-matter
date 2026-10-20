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

- `mode.value` is `null` until reported. Unknown current modes display `Mode N`.
- `set_mode` is advertised after a nonempty `SupportedModes` report. Lists use
  dynamically allocated storage and support every distinct 8-bit mode ID.
- Labels retain up to the Matter maximum of 64 UTF-8 bytes. Both UIs receive
  the complete choices; the GSP picker allocates storage for the reported count.
- Identical duplicate IDs/labels are deduplicated. Conflicting duplicate IDs,
  invalid labels, and decoding/allocation failures leave prior mode metadata intact.
- Invocations validate the ID against the reported choices and call `ChangeToMode`.
