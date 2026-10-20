# mosaico_hotplug_bridge

Hot-plug adapter between esp-mosaico module slots and `claw_matter_bridge`
virtual devices. Used only by `examples/matter_bridge` on the `esp_mosaico`
board.

`mosaico_hotplug_bridge.c` owns the module-manager subscription, event queue
and dispatch. Each Mosaico board type that should appear on the Matter fabric
is a small accessory under `accessories/`.

## Adding an accessory

1. Create `accessories/<name>/` with:
   - `<name>_accessory.c` implementing `mosaico_hotplug_accessory_t`
     (`board_type`, optional `register_lua`, `on_present` / `on_absent`)
   - a Lua binding if the driver needs a `require("<module>")` into hardware
   - a Lua driver beside the adapter under `accessories/<name>/`
2. List the new `.c` files (and any `EMBED_TXTFILES` driver) in `CMakeLists.txt`.
3. Add the BSP module path to `idf_component.yml` and `REQUIRES` if needed.
4. Append `&mosaico_hotplug_<name>_accessory` to `s_accessories[]` in
   `mosaico_hotplug_bridge.c`.

`on_present` should resume existing devices by the same deterministic name
(via `mosaico_hotplug_find_existing_device`) before calling
`mosaico_hotplug_add_virtual_device`. On failure it must leave the slot idle.
The dispatcher records slot ownership only after `on_present` returns `ESP_OK`.

Chat/skill `add_device` still rejects `virtual:true`; only this C path may
create virtual devices.

## Interaction module LEDs

`accessories/interact/` claims the module through `mosaico_module_mgr` and
drives its six WS2812 LEDs with `led_strip`, using the BSP's slot GPIO mapping.
The first three LEDs appear as independent on/off lights, with white output
at brightness 32. The `mosaico_interact.led_set` Lua interface is unchanged.

This adapter initializes only the LEDs. It does not depend on the full
`mosaico_module_interact` driver or its touch sensor FSM prebuilt libraries.

Build with `idf.py -C examples/matter_bridge build` from the repository root.
For a hardware smoke check, attach an interaction module to each slot and
toggle all three lights independently. Unplug one module while controlling
the other, then reattach it and verify that its lights work again.

## Included camera adapter

`accessories/camera/` dispatches camera presence to
`mosaico_matter_camera`. Unlike the interaction module, it does not create a
Lua virtual device: the camera component builds Camera AV Stream Management
and WebRTC Transport clusters directly, disables them on unplug, and retains
the camera endpoint identity for a same-module replug.
