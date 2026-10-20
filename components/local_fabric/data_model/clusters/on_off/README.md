# On/Off

Matter cluster `0x0006`, published as `on_off`. Manager reads `OnOff` and maps `set` to the standard On or Off command. A missing report is represented by `null`.

```json
{
  "properties": {"on": {"value": true}},
  "actions": {
    "set": {"arguments": {"on": {"type": "boolean"}}}
  }
}
```
