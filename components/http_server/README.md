# HTTP server and web UI

This product-owned component serves system status, network/time settings,
files and Matter management for both `matter_hub` and `matter_bridge`.
It does not use ESP-Claw's HTTP server. The base API in `http_server.h` is
independent of Claw.

The examples select their Matter backend before `project()` using the
`HTTP_SERVER_MATTER_BACKEND` CMake cache variable: `controller` for the hub,
`bridge` for the bridge, or `none` for a standalone server. Only the selected
backend is compiled. The hub keeps its Local Fabric REST API and SSE updates.
The bridge reads `bridge_core` and `bridge_dev_mngr` directly, including
devices created by native accessories and Claw.

## Claw web features

In `idf.py menuconfig`, select **Component config → HTTP management server →
Enable Claw web features** (`CONFIG_HTTP_SERVER_CLAW_ENABLE`, default `y`).
The optional `claw/` sources in this component provide Claw web integration:
chat, capability/module lists, WeChat login and Lua web routes. Configuration
and memory pages are also controlled by the same option.
Existing capability and Lua module switches still apply.

Disabling this option excludes the `claw/` sources and their dependencies.
System status, Wi-Fi/time settings, Matter and file management stay available.
Claw's firmware runtime, AI capabilities, device drivers and stored settings
continue to operate. The shared frontend bundle contains both sets of pages;
the device's status response determines which pages can be opened.

Hub supports STA and AP settings. Bridge retains its existing STA networking
and does not advertise AP configuration in the web UI.

## Application integration and APIs

`http_server.h` accepts JSON configuration query/patch callbacks, network
status and restart callbacks. Query results and successful patch responses
are owned by the server; the patch input is borrowed. Return
`ESP_ERR_INVALID_ARG` and a static error message for validation failures.
Other callback errors become HTTP 500 responses.

The examples share an `examples/common/http_app_config.c` adapter for their
existing `app_config_t` storage. It preserves configuration keys, partial
writes, GET redaction, group/field filters and metadata. A request containing
an unavailable Claw or AP field is rejected before saving any changes.

Extensions register routes during start and receive session-close and stop
callbacks. WeChat and WebIM declarations live in
`claw/include/http_server_claw.h`, exported when the feature is enabled.
Both examples depend only on the single `http_server` component.

### Base APIs

| Endpoint | Purpose |
| --- | --- |
| `GET /api/status` | Network, memory, storage and feature flags (`claw_enabled`, `*_supported`, `matter_role`, `matter_enabled`) |
| `GET/POST /api/config` | Read configuration or save a partial patch |
| `POST /api/restart` | Schedule a restart after replying |
| `/api/files`, `/files/*` | File management, always available |

The frontend uses `*_supported` flags to hide unavailable features. WebIM and
WeChat login flags reflect route support, not connection or login status.

IM platform settings and the separate search/HTTP request sections follow the
compiled groups returned by `/api/capabilities`. A group disabled in the saved
runtime selection can still be configured and enabled again. Configuration
GET responses and `_meta` omit unavailable fields; POST rejects them before
saving any changes. Existing stored values are retained. Lua module management
distinguishes unavailable Lua support, a disabled Lua group and an empty list.

Both examples supply WeChat QR login callbacks when Claw web features and the
WeChat capability are enabled. Bridge uses the existing `cap_im_wechat` login
flow; completed credentials populate the web form and are saved only when the
user submits it.

### Matter Bridge APIs

These APIs are available with `HTTP_SERVER_MATTER_BACKEND=bridge`.

| Endpoint | Purpose |
| --- | --- |
| `GET /api/matter/bridge` | Commissioned state and onboarding codes |
| `GET /api/matter/bridge/devices` | Read-only catalog with endpoint, type, active and reachable state |
| `POST /api/matter/bridge/commissioning-window` | Open a 180-second pairing window |

### Matter Controller REST API

These APIs are available with `HTTP_SERVER_MATTER_BACKEND=controller`.

| Endpoint | Request | Response |
| --- | --- | --- |
| `GET /api/matter/devices` | No body | `{"devices":[...]}` containing device summaries |
| `GET /api/matter/devices/{device_id}` | No body | Summary fields plus endpoint/cluster capabilities and cached state |
| `POST /api/matter/devices/{device_id}/identity` | `{"name":"Desk light","room":"Office"}` | `{"ok":true}` |
| `POST /api/matter/devices/{device_id}/actions` | `endpoint`, `cluster`, `action`, `arguments` (see below) | `{"ok":true,"request_id":12}` |
| `DELETE /api/matter/devices/{device_id}` | No body | `{"ok":true,"request_id":12}` |
| `GET /api/matter/operations/{request_id}` | No body | `active`, `request_id`, `result` for an action or removal |
| `POST /api/matter/commission` | `{"setup_code":"MT:..."}` with a full QR payload or manual setup code | `{"request_id":13}` |
| `GET /api/matter/commission` | No body | `active`, `request_id`, `result` for the latest commissioning attempt |
| `POST /api/matter/commission/cancel` | No body | `{"ok":true}` |
| `GET /api/matter/events` | No body | SSE stream (`text/event-stream`) |

#### Usage

Use `GET /api/matter/devices` to list managed devices, then
`GET /api/matter/devices/{device_id}` for their state and supported actions.
Use the returned device IDs, endpoints and action arguments. See the
[JSON schema](../local_fabric/data_model/SCHEMA.md) for field details.

For example, if device 7 supports `on_off.set` on endpoint 2, send
`POST /api/matter/devices/7/actions` with:

```json
{
  "endpoint": 2,
  "cluster": "on_off",
  "action": "set",
  "arguments": {"on": true}
}
```

- **Results:** use the returned `request_id` to poll
  `/api/matter/operations/{request_id}`. An operation succeeds when `active` is
  `false` and `result` is `"ESP_OK"`. Refresh device detail to confirm state.
- **Identity:** submit both `name` and `room` as strings to update a device's
  display name and room.
- **Commissioning:** put the target in commissioning mode and submit its manual
  code or full QR payload as `setup_code`. Poll `/api/matter/commission` for
  completion, using the same `active`/`result` checks. Only one attempt can run
  at a time.
- **Removal:** DELETE removes the entire Matter node, including any other
  devices it exposes.
- **Live updates:** `/api/matter/events` sends SSE JSON with `type` set to
  `properties_changed`, `devices_changed` or `resync`. Refresh the list and
  details after `devices_changed`, `resync` or reconnection.

JSON error responses contain an `error` string. Common HTTP statuses are 400 (invalid
request), 404 (not found), 409 (state conflict), 501 (unsupported or Controller
unavailable), and 500 (server failure).

## Frontend development

The SolidJS frontend bundle is prebuilt and committed. Normal firmware builds
use that bundle without requiring a frontend build.

From the repository root:

```sh
cd components/http_server/frontend_source
corepack pnpm install --frozen-lockfile
corepack pnpm run typecheck
corepack pnpm run build
```

Rebuild after editing `frontend_source/src`; this refreshes
`frontend_source/dist/index.html.gz`. Then rebuild and flash the firmware to
serve the updated bundle.

The Matter page uses the device manager's advertised profiles, properties,
and actions.
