#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""XiaoZhi screen: status and Talk share the top row, the emotion
glyph sits under them, and chat scrolls below.
"""

from pathlib import Path
import sys

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent.parent.parent / "common"))
from font_paths import DEJAVU_SANS, DEJAVU_SANS_BOLD  # noqa: E402
from scene_common import (  # noqa: E402
    ASCII_PRINTABLE, CONTENT_H, CONTENT_W, label, layer,
    scene_out_path, shared_charset, shared_prefix, write_scene, button,
)

BAR_Y = 16
BAR_H = 40
EMOJI_W = 128
EMOJI_H = 88
EMOJI_Y = BAR_Y + BAR_H + 8
LIST_Y = EMOJI_Y + EMOJI_H + 8
LIST_H = CONTENT_H - LIST_Y - 12
# Same code points as xiaozhi-esp32 noto_emoji.h / xiaozhi_chat's table.
EMOTION_GLYPHS = "😶🙂😆😂😔😠😭😍😳😯😱🤔😉😎😌🤤😘😏😴😜🙄"
EMOJI_FONT = str((HERE.parent / "fonts" / "NotoEmoji-Regular.ttf").resolve())


def main():
    objs, content = shared_prefix([], {
        16: shared_charset(),
        18: shared_charset(),
    }, DEJAVU_SANS_BOLD)
    root = len(objs)
    objs.append(layer(content, 0, 0, CONTENT_W, CONTENT_H, name="xiaozhi_root"))
    status = label(root, 16, BAR_Y + 6, 280, 28, "", size=18,
                   color="#FFFFFF", bind="xiaozhi_status", align="left")
    status["overflow"] = "ellipsis"
    status["font_charset"] = ASCII_PRINTABLE
    objs.append(status)
    emotion = label(root, (CONTENT_W - EMOJI_W) // 2, EMOJI_Y, EMOJI_W, EMOJI_H,
                    "", size=64, color="#FFFFFF", align="center",
                    bind="xiaozhi_emotion", name="xiaozhi_emotion",
                    font=EMOJI_FONT, font_link="embedded")
    emotion["font_charset"] = EMOTION_GLYPHS
    objs.append(emotion)
    # No baked charset: chat text is Chinese and is shaped by the hub TTF.
    objs.append({
        "type": "message_list", "parent": root,
        "x": 0, "y": LIST_Y, "w": CONTENT_W, "h": LIST_H,
        "name": "xiaozhi_messages", "font_size": 16,
        "background_color": "#000000",
        "incoming_color": "#F4F4F6", "outgoing_color": "#95EC69",
        "message_text_color": "#161616", "outgoing_text_color": "#161616",
        "bubble_radius": 16,
        "bubble_padding_x": 12, "bubble_padding_y": 10,
        "message_gap": 8, "side_margin": 20,
        "max_bubble_width": 300, "max_message_height": 160,
        "scroll_snapshot": True,
    })
    objs.append(button(root, 312, BAR_Y, 148, BAR_H, "Talk", bg="#242425",
                       fg="#FFFFFF", radius=20, callback="xiaozhi_talk"))
    write_scene(scene_out_path(HERE, "xiaozhi_480.json"), "xiaozhi", objs,
                font=DEJAVU_SANS, font_link="external")


if __name__ == "__main__":
    main()
