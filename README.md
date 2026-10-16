# esp-mosaico-matter

Production-ready Matter hub and bridge applications for the esp-mosaico platform,
with device commissioning, control, automation, and AI agent integration. The
repository provides examples for two application types:

- **Matter hub**: commission devices into a local Matter fabric and manage them
  through a web UI, the on-device UI, or AI agents. Agent-specific firmware
  integrations are provided by the `matter_hub_<agent>` examples.
- **Matter bridge**: expose locally attached devices and supported esp-mosaico
  accessories as Matter endpoints for external Matter controllers.

## Platform

esp-mosaico is currently the only officially supported platform.

## Features

- **Matter commissioner**: commission devices into a local Matter fabric.
- **Matter controller**: discover, manage, and control devices in the fabric.
- **Automation**: coordinate device actions and automate interactions.
- **On-device UI**: commission and control Matter devices from the esp-mosaico
  display.
- **Web UI**: configure the hub, manage devices, and chat with agents from a
  browser.
- **MCP and skills**: expose hub capabilities to AI agents through the Model
  Context Protocol (MCP) and agent skills.
- **Thread border router**: connect Matter-over-Thread devices using a compatible
  esp-mosaico Thread-RCP accessory.
- **Matter bridge**: expose locally attached devices and supported esp-mosaico
  accessories as Matter endpoints.

## Agents

| Agent | Integration |
| --- | --- |
| esp-claw | Discover and control commissioned devices in `matter_hub_claw`; configure and manage bridged devices in `matter_bridge`. |
| XiaoZhi AI | Control Matter devices through xiaozhi AI. |
| Muse | Discover, inspect, and control commissioned devices through the `gadget-esp-matter-hub` skill and Muse Home Link. |

## Applications

| Application | Function |
| --- | --- |
| `examples/matter_hub_claw` | Matter controller and commissioner with esp-claw integration, web and on-device UIs, and a Thread border router. |
| `examples/matter_bridge` | Matter bridge with esp-claw device configuration, persistent Lua drivers, and esp-mosaico accessory integration. |

