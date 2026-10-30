#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
from pathlib import Path
import sys

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent.parent.parent / "common"))
from font_paths import DEJAVU_SANS, DEJAVU_SANS_BOLD  # noqa: E402
from scene_common import (  # noqa: E402
    CONTENT_H, CONTENT_W, auto_charset, container, image, label,
    layer, scene_out_path, shared_charset, shared_prefix, write_scene,
)
from device_home_scene import add_device_grid  # noqa: E402
from device_detail_scene import add_detail  # noqa: E402

BG = "#000000"
MUTED = "#91919B"
ACCENT = "#FF4C01"

QR_SIZE = 200
QR_PLACEHOLDER = "matter_bridge_qr_placeholder.png"


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
        [],
        {size: shared_charset() if size in (14, 16, 18) else auto_charset()
         for size in (13, 14, 16, 17, 18, 20, 22, 24, 30)},
        DEJAVU_SANS_BOLD,
    )
    root = len(objs)
    objs.append(container(content, 0, 0, CONTENT_W, CONTENT_H, bg=BG,
                          name="matter_bridge_root"))

    # QR group: shown until commissioning completes.
    qr_group = len(objs)
    objs.append(layer(root, 0, 0, CONTENT_W, CONTENT_H,
                      name="matter_bridge_qr_group",
                      bind="matter_bridge_qr_group_visible"))
    objs.append(label(qr_group, 0, 84, CONTENT_W, 24, "Scan to commission",
                      size=16, color=ACCENT, align="center",
                      bind="matter_bridge_pairing_hint",
                      name="matter_bridge_pairing_hint"))
    qr_x = (CONTENT_W - QR_SIZE) // 2
    objs.append(image(qr_group, QR_PLACEHOLDER, qr_x, 132, QR_SIZE, QR_SIZE,
                      name="matter_bridge_qr_canvas",
                      bind="matter_bridge_qr_canvas"))
    objs[-1]["hidden"] = True
    objs.append(label(qr_group, 20, 348, 440, 24, "",
                      size=14, color=MUTED, align="center",
                      bind="matter_bridge_manual_code",
                      name="matter_bridge_manual_code"))

    # The catalog includes both CLAW devices and native hot-plug accessories.
    status_group = len(objs)
    objs.append(layer(root, 0, 0, CONTENT_W, CONTENT_H, hidden=True,
                      name="matter_bridge_status_group",
                      bind="matter_bridge_status_group_visible"))
    objs.append(label(status_group, 20, 16, 350, 40, "Home", size=30,
                      color="#D6D6DE"))
    objs.append(label(status_group, 22, 56, 432, 24, "0 devices", size=16,
                      color=MUTED, bind="matter_bridge_device_count"))
    add_device_grid(objs, status_group, prefix="matter_bridge", bind_hint=True)
    add_detail(objs, root, prefix="matter_bridge")

    write_scene(scene_out_path(HERE, "matter_bridge_480.json"),
                "matter_bridge", objs, font=DEJAVU_SANS, font_link="external")


if __name__ == "__main__":
    main()
