#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent.parent.parent / "common"))

from font_paths import DEJAVU_SANS, DEJAVU_SANS_BOLD  # noqa: E402
from scene_common import (  # noqa: E402
    CONTENT_H, CONTENT_W, auto_charset, layer, scene_out_path,
    shared_charset, shared_prefix, write_scene,
)
from home_scene import add_home  # noqa: E402
from commissioning_scene import add_commissioning  # noqa: E402
from detail_scene import add_detail  # noqa: E402

FONT = DEJAVU_SANS
BOLD = DEJAVU_SANS_BOLD
ASSETS = "../../../common/assets"
CAMERA_CANVAS = "camera_canvas_fullscreen.png"
IMAGES = [
    (f"{ASSETS}/icons/matter.png", 118, 118),
    (f"{ASSETS}/icons/camera.png", 118, 118),
    (CAMERA_CANVAS, 480, 480),
]
FONT_POLICIES = {16: shared_charset(), 18: shared_charset(),
                 20: auto_charset(), 22: auto_charset(),
                 24: auto_charset(), 30: auto_charset()}



def main():
    objs, content = shared_prefix(IMAGES, FONT_POLICIES, BOLD,
                                  canvas_images=(CAMERA_CANVAS,))
    root = len(objs)
    objs.append(layer(content, 0, 0, CONTENT_W, CONTENT_H,
                      name="matter_root"))

    add_home(objs, root)
    add_commissioning(objs, root)
    add_detail(objs, root)

    # Keep one explicit catalog owner. With the retired demo Apps omitted,
    # automatic font deduplication may legitimately find no duplicate pack.
    write_scene(scene_out_path(HERE, "matter_480.json"), "matter", objs,
                font=FONT, font_link="external")


if __name__ == "__main__":
    main()
