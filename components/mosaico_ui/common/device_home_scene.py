# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Device cards and paging shared by the controller and bridge."""
from scene_common import button, container, label, layer


def add_device_grid(objs, parent, *, prefix="matter", bind_hint=False):
    for index in range(4):
        name = f"{prefix}_device_{index}"
        card = len(objs)
        objs.append(layer(parent, 12 + (index % 2) * 232,
                          138 + (index // 2) * 120, 224, 108,
                          name=f"{name}_row", bind=f"{name}_visible", hidden=True))
        objs.append(button(card, 0, 0, 224, 108, "", bg="#181819",
                           radius=24, callback=name))
        objs.append(container(card, 16, 16, 34, 34, bg="#FF4C01", radius=17,
                              callback=name))
        for x, y, w, h, text, size, color, align, binding in (
            (16, 58, 192, 24, "Matter device", 18, "#FCFCFF", "left", "name"),
            (60, 20, 148, 22, "Connecting", 16, "#91919B", "right", "status"),
            (16, 84, 192, 18, "Tap for controls", 16, "#595961", "left", "hint"),
        ):
            item = label(card, x, y, w, h, text, size=size, color=color,
                         align=align, callback=name,
                         bind=f"{name}_{binding}" if binding != "hint" or bind_hint else None)
            if binding != "hint":
                item["overflow"] = "ellipsis"
            objs.append(item)
    objs.append(button(parent, 116, 388, 76, 42, "‹", bg="#242425",
                       radius=21, size=24, callback=f"{prefix}_previous"))
    objs.append(label(parent, 194, 398, 92, 22, "1 / 1", size=16,
                      color="#91919B", align="center", bind=f"{prefix}_page"))
    objs.append(button(parent, 288, 388, 76, 42, "›", bg="#242425",
                       radius=21, size=24, callback=f"{prefix}_next"))
