# Matter device control with Muse

Use [Muse](https://github.com/facebookincubator/muse-gadget-sdk) to discover,
inspect, and control Matter devices commissioned to the
[ESP-Mosaico Matter Hub](../../examples/matter_hub/README.md). Requests can
come from voice, text chat, or any other Muse control interface that can invoke
the skill. All interfaces use the same device discovery and control workflow
through Muse Home Link and the hub's REST API.

The Matter Hub runs on ESP-Mosaico and owns the Matter fabric. Muse Home Link
runs on a separate device with LAN access to the hub and forwards requests to
it. The hub executes Matter commands; the Muse device does not need Matter
credentials or additional Matter commissioning.

To connect Muse to the hub:

1. Commission your Matter devices using the hub's web or on-device UI.
2. Enable and connect Muse Home Link's LAN transport.
3. Give Muse the contents of
   [`gadget-esp-matter-hub/SKILL.md`](gadget-esp-matter-hub/SKILL.md)
   in chat, together with the hub's current LAN base URL, including its HTTP
   port. For example: "Use this skill with my Matter Hub at
   http://192.168.1.50:80. List my devices." Replace the example address with
   your hub's actual address.

The skill finds devices by name and room, reads their state, checks their
advertised capabilities, sends supported commands, and verifies completion
and reported state. Try "List my devices", "Turn on the bedroom light", or
"Is the bedroom light on?" through your preferred Muse control interface.
