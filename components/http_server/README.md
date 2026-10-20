# HTTP server and web UI

This component serves the CLAW configuration pages and browser chat. In
`matter_hub`, it also provides the Matter Fabric page and REST API for
commissioning and managing devices through Local Fabric.

The SolidJS frontend bundle is prebuilt and committed. Normal firmware builds
use that bundle without requiring a frontend build.

## Frontend development

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
