# esp-mosaico-matter

Production-ready Matter hub and bridge applications for the ESP-Mosaico platform,
with device commissioning, control, automation, and AI agent integration. The
repository provides examples for two application types:

- **Matter hub**: commission devices into a local Matter fabric and manage them
through the Web UI, the on-device UI, automation rules, or AI agents.
- **Matter bridge**: expose locally attached devices and supported ESP-Mosaico
accessories as Matter endpoints for external Matter controllers.

<a href="https://esp-mosaico.github.io/esp-mosaico-matter/"><img src="https://img.shields.io/badge/Download-Firmware-0066CC?logo=espressif" alt="Download ESP-Mosaico Matter firmware"></a>

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
- **Built-in agents**: `matter_hub` firmware can bundle esp-claw or XiaoZhi, and `matter_bridge` bundles esp-claw, see [Agents](#agents).
- **Skills and MCP**: esp-claw runs bundled Matter skills, XiaoZhi registers local Matter MCP tools, and other agents can use the hub REST API (for example the [Muse Matter Hub skill](tools/muse_skills/README.md)).
- **Thread border router**: connect Matter-over-Thread devices using a compatible
ESP-Mosaico Thread-RCP accessory.
- **Matter bridge**: expose locally attached devices and supported ESP-Mosaico
accessories as Matter endpoints.

## Agents

The **Matter hub** (`matter_hub`) is an open endpoint for smart interaction:
after devices are commissioned into the local fabric, you can manage them
through the Web UI, on-device UI, automation rules, and **AI agents** that
discover devices, read state, and run supported commands. New agents can attach
through firmware integration, MCP/skills, or REST APIs without changing the
underlying Matter stack.

| Agent | How it connects | Role |
| --- | --- | --- |
| [esp-claw](https://github.com/espressif/esp-claw) | **Firmware** — menuconfig `matter_hub_claw` (default); Web chat on `matter_hub`; natural-language configuration on `matter_bridge`. | Natural-language discovery and control of commissioned devices; bridge device setup and management. |
| [Xiaozhi](https://github.com/78/xiaozhi-esp32) | **Firmware** — menuconfig `matter_hub_xiaozhi`; on-device launcher and voice wake. | Voice discovery and control of commissioned Matter devices on the hub. |
| [Muse](https://github.com/facebookincubator/muse-gadget-sdk) | **Skill** — [Matter Hub skill](tools/muse_skills/README.md) via Muse Home Link to the hub REST API (separate LAN device; not in hub firmware). | Voice, text, or other Muse interfaces for discovery, inspection, and control of commissioned devices. |

Choose `matter_hub_no_agent` in menuconfig when you only need the UIs and automation.

## Applications

| Application | Function |
| --- | --- |
| [matter_hub](examples/matter_hub/README.md) | Hub firmware with Matter commissioning and control, Web and on-device UIs, and Thread border router support. An onboard agent is optional: menuconfig picks none (`matter_hub_no_agent`), esp-claw (`matter_hub_claw`), or XiaoZhi (`matter_hub_xiaozhi`). |
| [matter_bridge](examples/matter_bridge/README.md) | Bridge firmware with esp-claw device configuration and ESP-Mosaico accessory support. |

Follow each application's guide for build, flash, and setup instructions.

## Flash prebuilt firmware

Download prebuilt firmware and follow the flashing instructions on the
[firmware downloads page](https://esp-mosaico.github.io/esp-mosaico-matter/).
