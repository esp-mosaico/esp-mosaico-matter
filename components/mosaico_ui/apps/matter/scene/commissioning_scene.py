# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

from scene_common import button, container, image, label, layer
from layout import header

ASSETS = "../../../common/assets"
CAMERA_CANVAS = "camera_canvas_fullscreen.png"

def add_commissioning(objs, root):
    scan = len(objs)
    objs.append(layer(root, 0, 0, 480, 480, name="matter_scan",
                      bind="matter_scan_visible", hidden=True))
    objs.append(image(scan, CAMERA_CANVAS, 0, 0, 480, 480,
                      name="matter_scan_preview", bind="matter_scan_canvas"))
    objs.append(container(scan, 80, 92, 320, 320, bg="#000000", radius=28,
                          border="#FF4C01", border_w=4, opacity=20))
    objs.append(button(scan, 12, 12, 72, 48, "‹", bg="#242425", radius=24,
                       size=32, callback="matter_scan_cancel"))
    objs.append(label(scan, 96, 18, 288, 34, "Scan Matter QR code", size=24,
                      align="center"))
    objs.append(button(scan, 120, 420, 240, 48, "Enter code manually",
                       bg="#242425", radius=24, callback="matter_manual"))
    missing = len(objs)
    objs.append(layer(scan, 20, 170, 440, 100, name="matter_camera_missing",
                      bind="matter_camera_missing_visible", hidden=True))
    objs.append(container(missing, 0, 0, 440, 100, bg="#181819", radius=24))
    objs.append(label(missing, 10, 25, 420, 52, "Camera not detected\nUse manual pairing code",
                      size=20, align="center"))

    manual = len(objs)
    objs.append(layer(root, 0, 0, 480, 480, name="matter_manual",
                      bind="matter_manual_visible", hidden=True))
    header(objs, manual, "Pairing code", "matter_manual_cancel")
    objs.append(container(manual, 24, 72, 432, 62, bg="#181819", radius=20))
    objs.append(label(manual, 38, 88, 404, 32, "", size=24,
                      align="center", bind="matter_manual_code"))
    positions = [(80, 154), (190, 154), (300, 154),
                 (80, 218), (190, 218), (300, 218),
                 (80, 282), (190, 282), (300, 282)]
    for digit, (x, y) in enumerate(positions, 1):
        objs.append(button(manual, x, y, 100, 54, str(digit),
                           bg="#242425", radius=22, size=24,
                           callback=f"matter_digit_{digit}"))
    objs.append(button(manual, 80, 346, 100, 54, "Delete", bg="#242425",
                       radius=22, callback="matter_delete"))
    objs.append(button(manual, 190, 346, 100, 54, "0", bg="#242425",
                       radius=22, size=24, callback="matter_digit_0"))
    objs.append(button(manual, 300, 346, 100, 54, "Pair", bg="#FF4C01",
                       radius=22, callback="matter_pair"))
    objs.append(label(manual, 34, 414, 412, 42,
                      "Enter the 11 or 21 digit code", size=18,
                      color="#91919B", align="center",
                      bind="matter_manual_hint"))

    progress = len(objs)
    objs.append(layer(root, 0, 0, 480, 480, name="matter_progress",
                      bind="matter_progress_visible", hidden=True))
    objs.append(image(progress, f"{ASSETS}/icons/matter.png",
                      181, 60, 118, 118))
    objs.append(label(progress, 30, 198, 420, 44, "Adding device", size=30,
                      align="center"))
    objs.append(label(progress, 44, 252, 392, 70,
                      "Connecting over Bluetooth…", size=20,
                      color="#91919B", align="center",
                      bind="matter_progress_text"))
    objs.append(label(progress, 30, 326, 420, 26,
                      "Uses available network credentials", size=16,
                      color="#B5B5BF", align="center"))
    objs.append(button(progress, 140, 360, 200, 52, "Cancel",
                       bg="#242425", radius=26,
                       callback="matter_commission_cancel"))
