#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
from pathlib import Path
import sys

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent.parent.parent / "common"))
from font_paths import DEJAVU_SANS, DEJAVU_SANS_BOLD  # noqa: E402
from scene_common import (  # noqa: E402
    CONTENT_H, CONTENT_W, auto_charset, button, container, image, label,
    layer, scene_out_path, shared_charset, shared_prefix, write_scene,
)

BG = "#070a12"
TEXT = "#f8fafc"
MUTED = "#94a3b8"
ACCENT = "#38bdf8"

ICON_SIZE = 96
QR_SIZE = 200
MATTER_ICON = "../../../common/assets/icons/matter.png"
QR_PLACEHOLDER = "matter_bridge_qr_placeholder.png"

# Active devices (chat- or hot-plug-added alike) are rendered as fixed rows;
# matter_bridge_app.c filters the shared device-manager catalog, fills up to
# this count, and blanks the rest. A true scrollable list of unbounded length
# is out of scope for this first pass -- see the "Known limitations" section
# of examples/matter_bridge/README.md.
# Matches CONFIG_MATTER_BRIDGE_MAX_DEVICES on the esp_mosaico board.
DEVICE_ROWS = 8
ROW_H = 26
ROW_START_Y = 200


def ensure_qr_placeholder():
    """A plain placeholder shown until the real QR is rasterized at runtime
    (mosaico_bridge_app.c's render_qr(), same approach as apps/matter's own
    commissioning QR canvas in the retired esp-mosaico-matter-bridge repo).
    Generated here instead of checked in, so this app has no binary assets."""
    path = HERE / QR_PLACEHOLDER
    if path.exists():
        return
    from PIL import Image
    Image.new("RGB", (QR_SIZE, QR_SIZE), (240, 240, 240)).save(path)


def main():
    ensure_qr_placeholder()
    objs, content = shared_prefix(
        # Canvas must be the only element referencing its placeholder.
        [(MATTER_ICON, ICON_SIZE, ICON_SIZE)],
        {14: shared_charset(), 16: shared_charset(), 18: shared_charset(),
         20: auto_charset()},
        DEJAVU_SANS_BOLD,
    )
    root = len(objs)
    objs.append(container(content, 0, 0, CONTENT_W, CONTENT_H, bg=BG,
                          name="matter_bridge_root"))

    # Header (always visible): a way back to the hub, Matter icon + title.
    objs.append(button(root, 12, 12, 88, 36, "< Home", bg="#1e293b",
                       fg=TEXT, radius=12, size=14, callback="back_hub"))
    icon_x = (CONTENT_W - ICON_SIZE) // 2
    objs.append(image(root, MATTER_ICON, icon_x, 20, ICON_SIZE, ICON_SIZE))
    objs.append(label(root, 0, 124, CONTENT_W, 28, "Matter Bridge", size=20,
                      color=TEXT, align="center"))

    # QR group: shown until commissioning completes.
    qr_group = len(objs)
    objs.append(layer(root, 0, 0, CONTENT_W, CONTENT_H,
                      name="matter_bridge_qr_group",
                      bind="matter_bridge_qr_group_visible"))
    objs.append(label(qr_group, 0, 164, CONTENT_W, 24, "Scan to commission",
                      size=16, color=ACCENT, align="center",
                      bind="matter_bridge_pairing_hint",
                      name="matter_bridge_pairing_hint"))
    qr_x = (CONTENT_W - QR_SIZE) // 2
    objs.append(image(qr_group, QR_PLACEHOLDER, qr_x, 196, QR_SIZE, QR_SIZE,
                      name="matter_bridge_qr_canvas",
                      bind="matter_bridge_qr_canvas"))
    objs[-1]["hidden"] = True
    objs.append(label(qr_group, 20, 196 + QR_SIZE + 12, 440, 24, "",
                      size=14, color=MUTED, align="center",
                      bind="matter_bridge_manual_code",
                      name="matter_bridge_manual_code"))

    # Status group: shown after commissioning completes -- one row per active
    # bridged device (chat- or hot-plug-added), polled from
    # bridge_dev_mngr_get_devices(). Hidden by default; matter_bridge_app.c
    # flips visibility once commissioned, same pattern as the retired
    # esp-mosaico-matter-bridge repo's two-slot status view.
    status_group = len(objs)
    objs.append(layer(root, 0, 0, CONTENT_W, CONTENT_H, hidden=True,
                      name="matter_bridge_status_group",
                      bind="matter_bridge_status_group_visible"))
    objs.append(label(status_group, 0, 172, CONTENT_W, 24, "Commissioned",
                      size=16, color=ACCENT, align="center"))
    for i in range(DEVICE_ROWS):
        y = ROW_START_Y + i * ROW_H
        name = f"matter_bridge_device_{i}"
        objs.append(label(status_group, 20, y, 300, ROW_H, "", size=16,
                          color=TEXT, align="left", bind=f"{name}_name",
                          name=f"{name}_name"))
        objs.append(label(status_group, 320, y, 140, ROW_H, "", size=14,
                          color=MUTED, align="right", bind=f"{name}_status",
                          name=f"{name}_status"))

    write_scene(scene_out_path(HERE, "matter_bridge_480.json"),
                "matter_bridge", objs, font=DEJAVU_SANS, font_link="external")


if __name__ == "__main__":
    main()
