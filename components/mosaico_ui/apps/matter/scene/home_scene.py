# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

from scene_common import button, container, label, layer
from layout import header
from device_home_scene import add_device_grid

def add_home(objs, root):
    dashboard = len(objs)
    objs.append(layer(root, 0, 0, 480, 480, name="matter_dashboard",
                      bind="matter_dashboard_visible"))
    header(objs, dashboard, "Home")
    objs.append(label(dashboard, 22, 56, 214, 24, "0 devices", size=16,
                      color="#91919B", bind="matter_device_count"))
    objs.append(button(dashboard, 374, 26, 86, 44, "+ Add",
                       bg="#FF4C01", radius=22, size=18,
                       callback="matter_add"))

    # A fixed, horizontally paged room strip keeps touch targets large while
    # supporting every discovered room on the 480 px display.
    objs.append(button(dashboard, 10, 88, 32, 38, "‹", bg="#101011",
                       radius=19, size=24, callback="matter_room_previous"))
    all_chip = container(dashboard, 48, 88, 58, 38, bg="#FF4C01",
                         radius=19, bind="matter_room_all_color",
                         callback="matter_room_all")
    objs.append(all_chip)
    objs.append(label(dashboard, 48, 96, 58, 22, "All", size=16,
                      align="center", callback="matter_room_all"))
    for index, x in enumerate((112, 208, 304)):
        chip = len(objs)
        objs.append(layer(dashboard, x, 88, 90, 38,
                          name=f"matter_room_{index}",
                          bind=f"matter_room_{index}_visible", hidden=True))
        objs.append(container(chip, 0, 0, 90, 38, bg="#242425", radius=19,
                              bind=f"matter_room_{index}_color",
                              callback=f"matter_room_{index}"))
        room_label = label(chip, 6, 8, 78, 22, "Room", size=16,
                           color="#D6D6DE", align="center",
                           bind=f"matter_room_{index}_text",
                           callback=f"matter_room_{index}")
        room_label["overflow"] = "ellipsis"
        objs.append(room_label)
    objs.append(button(dashboard, 438, 88, 32, 38, "›", bg="#101011",
                       radius=19, size=24, callback="matter_room_next"))

    add_device_grid(objs, dashboard)
