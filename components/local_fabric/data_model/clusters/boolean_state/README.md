# Boolean State

Matter cluster `0x0045`, published as `boolean_state`. Manager reads `StateValue`; there are no actions. The generic `state` key intentionally avoids assigning contact/open semantics that are not present in the cluster.

```json
{
  "properties": {"state": {"value": true}},
  "actions": {}
}
```

`state.value` is `null` until reported.
