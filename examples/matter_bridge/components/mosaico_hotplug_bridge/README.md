# mosaico_hotplug_bridge

Hot-plug adapter between ESP-Mosaico module slots and the `matter_bridge`
device catalog. Used only by `examples/matter_bridge` on the `esp_mosaico`
board. Accessories use native drivers and consume no Lua jobs.

`mosaico_hotplug_bridge.c` owns the module-manager subscription, event queue
and dispatch. Each Mosaico board type that should appear on the Matter fabric
is a small accessory under `accessories/`.

## Adding an accessory

1. Create `accessories/<name>/` with a `.c` or `.cpp` adapter implementing
   `mosaico_hotplug_accessory_t`: `board_type`, `on_present` / `on_absent`,
   and optional `poll` / `attribute_changed` hooks.
2. List the new source files in `CMakeLists.txt`.
3. Add the hardware dependencies to `idf_component.yml` and `REQUIRES` if needed.
4. Append `&mosaico_hotplug_<name>_accessory` to `s_accessories[]` in
   `mosaico_hotplug_bridge.c`.

Use the catalog helpers in `mosaico_hotplug_accessory.h`. On setup failure,
release hardware and roll back new devices. The dispatcher records slot
ownership only after `on_present` returns `ESP_OK`. `attribute_changed` runs
on the Matter task and must not wait for Matter or access hardware.

## Interaction module

`accessories/interact/` claims the module through `mosaico_module_mgr` and
drives its six WS2812 LEDs with `led_strip`, using the BSP's slot GPIO mapping.
The first three LEDs appear as independent color lights, supporting brightness,
hue/saturation, XY color, and RGB-emulated color temperature. Full-brightness
white is `(255, 255, 255)`; the lights start off after insertion or reboot.
The remaining three LEDs stay off. Each module also creates a PIR occupancy
sensor and an illuminance sensor.

PIR is sampled every 100 ms and illuminance once per second. Illuminance is
estimated and capped at about 179 lux; ADC failures report an unknown value.

The adapter uses GPIO/ADC and `led_strip` without the full
`mosaico_module_interact` driver or its touch sensor FSM prebuilt libraries.

Build with `idf.py -C examples/matter_bridge build` from the repository root.
For a hardware smoke check, attach an interaction module to each slot and
control all three lights independently, then check PIR and illuminance readings.
Unplug one module while using the other, then reattach it and verify recovery.

## Included camera adapter

`accessories/camera/` dispatches camera presence to
`mosaico_matter_camera`. The camera component builds Camera AV Stream Management
and WebRTC Transport clusters directly, disables them on unplug, and retains
the camera endpoint identity for a same-module replug.
