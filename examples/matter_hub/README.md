# Matter hub example

## Overview

Run a Matter hub on ESP-Mosaico with commissioning and control, Web and
on-device UIs, and Thread border router support. An onboard agent is
optional—build with esp-claw or XiaoZhi for smart interaction, or with none for
UIs only.

Commission devices into a local Matter fabric by scanning their Matter QR
codes with an ESP-Mosaico camera module, or enter setup codes manually. Then
inspect and control them from the Web UI or on-device **Matter** app, or
through esp-claw or XiaoZhi when that agent is enabled. Use the Web UI to rename
devices, assign rooms, or remove devices from the fabric.

For Matter-over-Thread devices, attach a compatible ESP-Mosaico Thread-RCP
accessory. Open **Thread Border Router** in the on-device UI, select **Start**
if needed, and wait for **Running** before commissioning devices.

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
cd examples/matter_hub

idf.py bmgr -c "boards;../../third_party/esp-mosaico-claw/boards" -b esp_mosaico
idf.py menuconfig
idf.py build
idf.py flash monitor
```

In menuconfig, open **App Config → Matter hub agent**. An onboard agent is
optional—pick none (`matter_hub_no_agent`), esp-claw (`matter_hub_claw`,
default), or XiaoZhi (`matter_hub_xiaozhi`).

Run Board Manager before the first build. If switching from another board, remove the
old `sdkconfig` and `build/`, then repeat the commands. See the
[board configuration guide](boards/README.md) for details.

### 3. Connect to Wi-Fi

On first boot, use the on-device setup to select your Wi-Fi network and enter
its password. Finish or skip the remaining setup steps to reach the launcher.
You can change the network later in Settings.

Find the hub's IP address in Settings, the serial monitor, or your router's
client list. Connect your computer or phone to the same network.

### 4. Commission a Matter device

This hub verifies PAA trust chains using certificates fetched from the mainnet
DCL at `https://on.dcl.csa-iot.org` and validates Certification Declarations
(CD) during commissioning. `skip_paa_and_cd_checks` defaults to `false`.
The hub needs Internet access to fetch PAA certificates.

Put the target device into commissioning mode, then use either interface:

- **Camera QR scan:** attach an ESP-Mosaico camera module, open **Matter** from
  the on-device launcher, and scan the device's Matter QR code. The hub's
  commissioner uses the scanned setup information to add the device to its
  local Matter fabric.
- **Manual entry:** enter the manual setup code in the on-device **Matter**
  app, or open `http://HUB_IP/#matter` in a browser. In the Web UI, enter the
  manual setup code or full QR payload (`MT:...`) under **Commission a device**,
  then select **Commission**. Wait for the device to appear in the device list.

For a device already paired with another controller, open a new commissioning
window from that controller and use its new sharing code.

### 5. View and control devices

Open a device on the Web UI's **Matter Fabric** page to inspect its state and
use its supported controls. The on-device **Matter** app provides basic device
control. Both UIs work without an LLM connection.
Commission devices through the Web or on-device UI before controlling them.

With `matter_hub_no_agent`, use the Web UI and the on-device UI only.

With `matter_hub_claw`, open `http://HUB_IP/`, configure the LLM provider and
credentials in esp-claw settings, then ask the agent, for example:

> List my Matter devices.

> Turn on the desk lamp.

With `matter_hub_xiaozhi`, open **XiaoZhi** from the on-device launcher, then say
"Hi ESP" or use **Talk** icon. If the screen shows an activation code, enter it in
the [XiaoZhi console](https://xiaozhi.me/console/agents) and wait until the hub
leaves the activation screen before waking it. XiaoZhi can then list
commissioned devices and invoke their actions, for example:

> List my Matter devices.

> Turn on the desk lamp.

## Muse Home Link skill

Use [Muse](https://github.com/facebookincubator/muse-gadget-sdk) to discover,
inspect, and control commissioned Matter devices through voice, text chat, or
other Muse control interfaces. The
[Muse Home Link skill](../../tools/muse_skills/README.md) connects Muse to the
hub's REST API through a separate device with LAN access to the hub. Follow
the guide to supply the skill and hub address to Muse. The skill is distributed
separately from the firmware's esp-claw skills.
