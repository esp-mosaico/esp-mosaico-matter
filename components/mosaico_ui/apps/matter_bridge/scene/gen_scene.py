#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
from pathlib import Path
import sys

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent.parent.parent / "common"))
from font_paths import DEJAVU_SANS, DEJAVU_SANS_BOLD  # noqa: E402
from scene_common import (  # noqa: E402
    ASCII_PRINTABLE, CONTENT_H, CONTENT_W, auto_charset, button, container,
    image, label, layer, scene_out_path, shared_charset, shared_prefix,
    write_scene,
)
from frontend_scene import add_frontend  # noqa: E402

BG = "#000000"
MUTED = "#91919B"
ACCENT = "#FF4C01"

QR_SIZE = 191
PLATE_SIZE = 211
QR_PLACEHOLDER = "matter_bridge_qr_placeholder.png"


def ensure_qr_placeholder():
    """Ensure the placeholder matches the runtime QR canvas dimensions."""
    path = HERE / QR_PLACEHOLDER
    from PIL import Image
    if path.exists():
        with Image.open(path) as current:
            if current.size == (QR_SIZE, QR_SIZE):
                return
    Image.new("RGB", (QR_SIZE, QR_SIZE), (255, 255, 255)).save(path)


def main():
    ensure_qr_placeholder()
    objs, content = shared_prefix(
        # Canvas must be the only element referencing its placeholder.
        [],
        {size: shared_charset(ASCII_PRINTABLE + "·") if size in (16, 18)
         else (shared_charset() if size == 14 else auto_charset())
         for size in (13, 14, 16, 17, 18, 20, 22, 24, 28, 30, 32, 36, 40, 48, 72)},
        DEJAVU_SANS_BOLD,
    )
    root = len(objs)
    objs.append(container(content, 0, 0, CONTENT_W, CONTENT_H, bg=BG,
                          name="matter_bridge_root"))

    # The pairing screen renders the bridge_core onboarding payload at runtime.
    qr_group = len(objs)
    objs.append(layer(root, 0, 0, CONTENT_W, CONTENT_H,
                      name="matter_bridge_qr_group",
                      bind="matter_bridge_qr_group_visible"))
    objs.append(container(qr_group, 24, 25, 14, 14, bg=ACCENT))
    objs.append(label(qr_group, 46, 15, 120, 34, "Home", size=24,
                      color="#D6D6DE"))
    objs.append(button(qr_group, 408, 8, 48, 48, "i", bg="#000000",
                       size=24, callback="matter_bridge_board_info"))
    objs.append(label(qr_group, 24, 76, 432, 40, "Matter Setup", size=40,
                      color="#FCFCFF"))
    objs.append(label(qr_group, 24, 120, 417, 34,
                      "Scan with your home app to connect.", size=24,
                      color=MUTED))
    plate_x = (CONTENT_W - PLATE_SIZE) // 2
    plate_y = 154
    objs.append(container(qr_group, plate_x, plate_y, PLATE_SIZE, PLATE_SIZE,
                          bg="#FFFFFF", radius=31))
    qr_inset = (PLATE_SIZE - QR_SIZE) // 2
    objs.append(image(qr_group, QR_PLACEHOLDER, plate_x + qr_inset,
                      plate_y + qr_inset, QR_SIZE, QR_SIZE,
                      name="matter_bridge_qr_canvas",
                      bind="matter_bridge_qr_canvas"))
    objs[-1]["hidden"] = True
    objs.append(label(qr_group, 12, 398, 456, 24, "", size=16, align="center",
                      bind="matter_bridge_pairing_hint",
                      name="matter_bridge_pairing_hint"))
    objs.append(label(qr_group, 12, 370, 456, 24, "", size=18, align="center",
                      bind="matter_bridge_manual_code",
                      name="matter_bridge_manual_code"))
    objs.append(button(qr_group, 180, 430, 260, 40, "Reopen pairing", size=18,
                       radius=20, callback="matter_bridge_matter_setup"))

    objs.append(button(qr_group, 24, 430, 140, 40, "Home", size=18,
                       radius=20, callback="matter_bridge_frontend_home_open"))

    board = len(objs)
    objs.append(layer(root, 0, 0, CONTENT_W, CONTENT_H, hidden=True,
                      name="matter_bridge_board_info",
                      bind="matter_bridge_board_info_visible"))
    objs.append(button(board, 8, 8, 48, 48, "‹", bg="#000000", size=36,
                       callback="matter_bridge_board_back"))
    objs.append(label(board, 12, 64, 440, 70, "Home", size=48, color="#FFFFFF"))
    side_inset = 12
    column_gap = 12
    status_width = 164
    status_x = CONTENT_W - side_inset - status_width
    title_width = status_x - column_gap - side_inset
    for index, (title, status_bind, setup) in enumerate((
            ("Camera Module", "matter_bridge_camera_status", False),
            ("Interaction Module", "matter_bridge_interact_status", False),
            ("Matter Connection", None, True),
    )):
        y = 134 + index * 100
        objs.append(label(board, side_inset, y + 28, title_width, 44, title, size=28,
                          color="#FCFCFF",
                          callback="matter_bridge_matter_setup" if setup else None))
        if status_bind:
            objs.append(label(board, status_x, y + 33, status_width, 34, "Not Detected",
                              size=24, color=MUTED, align="right",
                              bind=status_bind))
        else:
            objs.append(button(board, 420, y + 30, 40, 40, "›", bg="#000000",
                               size=28, callback="matter_bridge_matter_setup"))
        objs.append(container(board, 0, y + 99, CONTENT_W, 1, bg="#3B3C3D"))

    add_frontend(objs, root)

    write_scene(scene_out_path(HERE, "matter_bridge_480.json"),
                "matter_bridge", objs, font=DEJAVU_SANS, font_link="auto")


if __name__ == "__main__":
    main()
