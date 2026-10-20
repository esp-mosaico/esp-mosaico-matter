#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
from pathlib import Path
import sys

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent.parent.parent / "common"))
from font_paths import DEJAVU_SANS, DEJAVU_SANS_BOLD  # noqa: E402
from scene_common import (  # noqa: E402
    CONTENT_H, CONTENT_W, auto_charset, button, container, label, layer,
    scene_out_path, shared_charset, shared_prefix, write_scene,
)


def main():
    objs, content = shared_prefix([], {
        16: shared_charset(), 18: shared_charset(), 20: auto_charset(),
        26: auto_charset(),
    }, DEJAVU_SANS_BOLD)
    root = len(objs)
    objs.append(layer(content, 0, 0, CONTENT_W, CONTENT_H, name="thread_br_root"))
    objs.append(label(root, 20, 16, 440, 40, "Thread Border Router", size=26))
    for y, text, bind in (
        (72, "Thread-RCP not detected", "thread_board"),
        (112, "Unavailable", "thread_state"),
        (152, "Network: --", "thread_network"),
        (190, "Channel: -- / Role: disabled", "thread_role"),
        (228, "Wi-Fi backbone disconnected", "thread_backbone"),
        (270, "Plug Thread-RCP into the right slot", "thread_hint"),
    ):
        objs.append(label(root, 22, y, 436, 36, text, size=18,
                          color="#B5B5BF", bind=bind))
    for x, text, action in ((22, "Start", "thread_start"), (248, "Stop", "thread_stop")):
        objs.append(container(root, x, 328, 210, 54, bg="#242425", radius=24))
        objs.append(label(root, x, 341, 210, 28, text, size=16, align="center"))
        active = len(objs)
        objs.append(layer(root, x, 328, 210, 54, name=action,
                          bind=action + "_visible"))
        objs.append(button(active, 0, 0, 210, 54, text, bg="#FF4C01",
                           radius=24, callback=action))
    objs.append(button(root, 72, 405, 336, 50, "Open Matter commissioner",
                       bg="#242425", radius=24, callback="thread_matter"))
    write_scene(scene_out_path(HERE, "thread_br_480.json"), "thread_br", objs,
                font=DEJAVU_SANS, font_link="external")


if __name__ == "__main__":
    main()
