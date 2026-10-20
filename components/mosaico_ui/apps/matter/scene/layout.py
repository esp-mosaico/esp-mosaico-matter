# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

from scene_common import button, label


def header(objs, parent, title, back=None):
    if back:
        objs.append(button(parent, 8, 8, 72, 48, "‹", bg="#000000",
                           radius=20, size=36, callback=back))
    objs.append(label(parent, 84 if back else 20, 16,
                      300 if back else 350, 40, title, size=30,
                      color="#D6D6DE"))
