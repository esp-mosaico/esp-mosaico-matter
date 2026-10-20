# Matter bridge example

## Overview

Run a Matter bridge on ESP-Mosaico with esp-claw device configuration and
support for locally attached devices and hot-plug accessories.

Expose these devices as Matter endpoints for external Matter controllers.
Supported accessories appear automatically when connected. To add a GPIO
device, describe its type and wiring to esp-claw; the agent creates a Matter
endpoint and a persistent Lua driver. Once configured, devices operate locally
without an LLM connection.

Supported ESP-Mosaico accessories (detected automatically when connected):

- [Interact module](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s31/esp-mosaico/user_guide_interact.html):
  three LEDs exposed as separate on/off lights.
- [Camera module](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s31/esp-mosaico/user_guide_camera.html):
  exposed as a Matter Camera endpoint.

Other supported devices (configured through esp-claw):

- GPIO on/off lights, outlets, and dimmable lights.
- Color lights using RGB PWM or WS2812 LEDs.
- Digital contact and occupancy sensors.

## How to use

### 1. Set up ESP-IDF

Set up ESP-IDF with ESP32-S31 support before building this example. The tested
ESP-IDF commit is `d7547231a` on `master`. In your ESP-IDF checkout, run:

```sh
cd /path/to/esp-idf
git checkout d7547231a
git submodule update --init --recursive
./install.sh
source ./export.sh
```

Source `/path/to/esp-idf/export.sh` in each new terminal before using `idf.py`.

### 2. Build and flash

Run from this repository's root:

```sh
git submodule update --init --recursive
python -m pip install -r third_party/esp-mosaico-claw/requirements.txt
cd examples/matter_bridge

idf.py bmgr -c "boards;../../third_party/esp-mosaico-claw/boards" -b esp_mosaico
idf.py build
idf.py flash monitor
```

Run Board Manager before the first build. If switching from another board,
remove the old `sdkconfig` and `build/`, then repeat the commands. If reusing
an existing configuration, use `idf.py menuconfig` to apply settings from
`sdkconfig.defaults` and the board overlay.
Switching from `matter_hub` requires a fresh device setup because the examples
use different storage layouts.

### 3. Connect and commission the bridge

1. On first boot, select your Wi-Fi network in the on-device setup and enter
   its password. Finish or skip the remaining esp-claw setup steps.
2. Open **Matter Bridge** from the launcher. It displays a QR code and manual
   setup code for a 180-second commissioning window.
3. In your Matter controller, add a Matter device and scan the displayed QR
   code or enter the manual code. For on-network commissioning, keep the
   controller on the same network as the bridge.
4. Wait for commissioning to complete. Add accessories or GPIO devices as
   described below, then control them from your Matter controller.

If the window expires, leave and reopen **Matter Bridge**. When the bridge is
uncommissioned and offline, the app instead offers BLE commissioning so the
controller can supply Wi-Fi credentials. After setup, saved Wi-Fi credentials
are reused on subsequent boots. Change them through Settings when needed.

### 4. Plug in an accessory

- **Interact module:** plug it into either module slot. Its three LEDs
  appear as separate on/off lights. Unplugging removes those devices;
  replugging creates them again. Both slots can host non-camera accessories.
- **Camera module:** plugging it in creates one Matter Camera endpoint with
  real video, mock Opus silence, and a fixed JPEG snapshot. Use the
  [live view instructions](#esp-mosaico-camera-live-view) below and the
  [camera component guide](components/mosaico_matter_camera/README.md) for mock
  media allocation and validation. Unplugging stops streaming; the same module
  retains its endpoint identity when plugged back in, including after a reboot.

Open **Matter Bridge** to see device status. The serial monitor also reports
the camera's endpoint ID when the module becomes available.

### 5. Add a GPIO device through esp-claw

The default GPIO allowlist (`0x21C00000003C30`) enables the side connector
pins: **left GPIO4, 12, 13, 48, 53** and **right GPIO5, 10, 11, 46, 47**.
GPIO0/1 remain reserved for shared I2C, and GPIO14/39 for module address
selection. Use only a slot without a hot-plug module. For an existing
configuration, set **CLAW Matter bridge → GPIO allowlist bit mask** in
`idf.py menuconfig`, then rebuild and flash.

Open `http://BRIDGE_IP/` using the IP address from Settings, the serial monitor,
or your router's client list. Configure your LLM provider and credentials in
esp-claw settings, then describe the device in the web UI chat. Keep esp-claw
tools and Lua modules enabled.

Include the device type, name, GPIO numbers, and signal polarity. For example:

> Add an on/off light called Desk lamp. Its relay uses GPIO4, output, active
> low. Tell me its Matter endpoint after it is ready.

> Add a dimmable light called Shelf. GPIO5 is the brightness PWM output,
> active high.

> Add a color light called Strip. It is a 30 LED WS2812 strip with its data
> line on GPIO12.

> Add a contact sensor called Door. GPIO13 reads the contact, with an internal
> pull-up. A low signal means closed. Debounce it.

These pin numbers are examples; confirm that they are available on your board.
Use suitable relay/MOSFET drivers and a common ground. GPIO and PWM pins are
control signals, not load power supplies. Analog sensors and arbitrary Matter
device types are not supported.

Wait for the agent to report that the device is ready, then find it in your
Matter controller. To remove it and release its GPIOs, ask:

> Remove Desk lamp.

Device definitions and endpoint identities persist across reboots; outputs
start off. If a driver fails, its outputs turn off and the endpoint becomes
unreachable. Ask esp-claw to inspect the device and restart or repair its driver. To change a device's type
or wiring, remove it and add it again.

### ESP-Mosaico camera live view

The camera provides H.264 video at **320×240, 10 fps**, with mock Opus silence
and a fixed JPEG test snapshot. Recording is not supported. Commission the
bridge into the camera controller's fabric first.

You can use **Samsung SmartThings** to test the camera's live view. Commission
the bridge with the SmartThings app and a Matter-capable SmartThings hub,
then plug in the camera module and open its live view in SmartThings.
See the [SmartThings Matter setup guide](https://support.smartthings.com/hc/en-us/articles/11219700390804-SmartThings-x-Matter-Integration).

Use a camera controller that supports this video profile and can select a
bridged camera endpoint. Use the endpoint ID printed as
`Camera ... available on Matter endpoint N`.

Use a 10 fps minimum; a higher minimum is rejected. For viewing across routed
networks, configure suitable STUN/TURN servers in the controller.

### Reset the bridge

Use the hold-to-reset control in on-device **Settings** to clear device
definitions, esp-claw configuration, and Matter commissioning data. If device
cleanup fails, reset stops so you can resolve the failure and retry.
