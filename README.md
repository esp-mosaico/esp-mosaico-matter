# esp-mosaico-matter

Production-ready Matter hub and bridge applications for the ESP-Mosaico platform,
with device commissioning, control, automation, and AI agent integration. The
repository provides examples for two application types:

- **Matter hub**: commission devices into a local Matter fabric and manage them
  through a web UI, the on-device UI, automation rules, or AI agents.
- **Matter bridge**: expose locally attached devices and supported ESP-Mosaico
  accessories as Matter endpoints for external Matter controllers.

## Platform

[ESP-Mosaico](https://mosaico.espressif.com/) is currently the only officially
supported platform.

## Features

- **Commissioning**: scan devices' Matter QR codes with an ESP-Mosaico camera
  module to add them to the hub's local Matter fabric, or enter setup codes
  manually.
- **Device control**: discover, manage, and control devices in the fabric.
- **Automation**: coordinate device actions and automate interactions.
- **On-device UI**: control Matter devices from the ESP-Mosaico display.
- **Web UI**: configure the hub, manage devices, and chat with agents from a
  browser.
- **MCP and skills**: expose hub capabilities to AI agents through the Model
  Context Protocol (MCP) and agent skills.
- **Thread border router**: connect Matter-over-Thread devices using a compatible
  ESP-Mosaico Thread-RCP accessory.
- **Matter bridge**: expose locally attached devices and supported ESP-Mosaico
  accessories as Matter endpoints.

## Agents

Use AI agents to control commissioned devices or configure the bridge:

| Agent | Integration |
| --- | --- |
| [esp-claw](https://github.com/espressif/esp-claw) | Discover and control commissioned devices in `matter_hub`, or configure and manage bridged devices in `matter_bridge`, through natural-language requests. |
| [Muse](https://github.com/facebookincubator/muse-gadget-sdk) | Discover, inspect, and control commissioned Matter devices from voice, text chat, or other Muse control interfaces through Muse Home Link using the [Matter Hub skill](tools/muse_skills/README.md). |

## Applications

| Application | Function |
| --- | --- |
| [matter_hub](examples/matter_hub/README.md) | Hub firmware with Matter commissioning and control, esp-claw integration, web and on-device UIs, and Thread border router support. |
| [matter_bridge](examples/matter_bridge/README.md) | Bridge firmware with esp-claw device configuration and ESP-Mosaico accessory support. |

Follow each application's guide for build, flash, and setup instructions.
