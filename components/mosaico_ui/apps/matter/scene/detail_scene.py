# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

from scene_common import ASCII_PRINTABLE, button, container, image, label, layer

BACK_IMAGE = "../../settings/scene/settings_html_back.png"


def add_detail(objs, root):
    detail = len(objs)
    objs.append(layer(root, 0, 0, 480, 480, name="matter_detail",
                      bind="matter_detail_visible", hidden=True))
    objs.append(button(detail, 0, 0, 96, 64, "", bg="#000000",
                       fg="#000000", name="matter_detail_back",
                       callback="matter_detail_back"))
    objs.append(image(detail, BACK_IMAGE, 20, 16, 32, 32,
                      callback="matter_detail_back"))
    device_name = label(detail, 96, 0, 288, 64, "Matter device", size=22,
                        color="#91919B", align="center",
                        bind="matter_detail_name")
    device_name["overflow"] = "ellipsis"
    device_name["font_charset"] = ASCII_PRINTABLE + "°—"
    objs.append(device_name)
    device_status = label(detail, 24, 58, 432, 24, "Connecting", size=16,
                          color="#91919B", bind="matter_detail_status")
    device_status["overflow"] = "ellipsis"
    objs.append(device_status)

    entries = len(objs)
    objs.append(layer(detail, 0, 104, 480, 376, name="matter_entries",
                      bind="matter_entries_visible"))
    objs.append({
        "type": "list", "parent": entries,
        "x": 0, "y": 0, "w": 480, "h": 376,
        "item_height": 72, "item_count": 0,
        "name": "matter_detail_rows",
        "row_template": "matter_detail_row",
        "scroll_snapshot": False,
        "bg_color": "#000000", "fg_color": "#FCFCFF",
    })
    empty = len(objs)
    objs.append(layer(entries, 0, 0, 480, 72, name="matter_empty",
                      bind="matter_empty_visible", hidden=True))
    objs.append(label(empty, 24, 32, 432, 28, "No reported properties",
                      size=18, color="#B5B5BF"))

    editor = len(objs)
    objs.append(layer(detail, 0, 104, 480, 376, name="matter_editor_page",
                      bind="matter_editor_page_visible", hidden=True))
    power = len(objs)
    objs.append(layer(editor, 0, 56, 480, 88, name="matter_power",
                      bind="matter_power_visible", hidden=True))
    objs.append(container(power, 12, 0, 456, 80, bg="#181819", radius=18))
    objs.append(label(power, 30, 12, 210, 24, "Power", size=18))
    objs.append(label(power, 30, 42, 210, 24, "Unknown", size=16,
                      color="#B5B5BF", bind="matter_power_state"))
    objs.append({
        "type": "toggle", "parent": power,
        "x": 340, "y": 16, "w": 110, "h": 48,
        "checked": False, "bg_color": "#3B3C3D",
        "fg_color": "#FF4C01", "knob_color": "#FCFCFF",
        "radius": 24, "bind": "matter_power_toggle",
        "name": "matter_power_toggle",
    })
    # GSP Toggle consumes taps without delivering a call; the transparent button
    # forwards them while the toggle displays the last reported state.
    objs.append(button(power, 340, 16, 110, 48, "", bg="#00000000",
                       fg="#00000000", radius=24,
                       name="matter_power_tap", callback="matter_power_toggle"))

    brightness = len(objs)
    objs.append(layer(editor, 0, 56, 480, 116,
                      name="matter_brightness",
                      bind="matter_brightness_visible", hidden=True))
    objs.append(container(brightness, 12, 0, 456, 108,
                          bg="#181819", radius=18))
    objs.append(label(brightness, 30, 12, 230, 24, "Brightness", size=18))
    objs.append(label(brightness, 268, 12, 182, 24, "Unknown", size=18,
                      align="right", color="#FCFCFF",
                      bind="matter_brightness_state"))
    objs.append({
        "type": "slider", "parent": brightness,
        "x": 30, "y": 52, "w": 420, "h": 44,
        "value": 50, "min": 1, "max": 100,
        "name": "matter_brightness_slider", "callback": "matter_level",
        "bg_color": "#242425", "fg_color": "#FF4C01",
        "knob_color": "#FCFCFF", "radius": 20, "track_size": 10,
    })

    temperature = len(objs)
    objs.append(layer(editor, 0, 56, 480, 136,
                      name="matter_color_temperature",
                      bind="matter_color_temperature_visible", hidden=True))
    objs.append(container(temperature, 12, 0, 456, 128,
                          bg="#181819", radius=18))
    objs.append(label(temperature, 30, 14, 270, 26,
                      "Color temperature", size=18))
    objs.append(label(temperature, 310, 14, 140, 26, "Unknown", size=18,
                      align="right", bind="matter_color_temperature_state"))
    objs.append({
        "type": "slider", "parent": temperature,
        "x": 30, "y": 52, "w": 420, "h": 44,
        "value": 50, "min": 0, "max": 100,
        "name": "matter_color_temperature_slider",
        "callback": "matter_color_temp",
        "bg_color": "#242425", "fg_color": "#FFB45C",
        "knob_color": "#FCFCFF", "radius": 20, "track_size": 10,
    })
    objs.append(label(temperature, 30, 100, 210, 22, "Warm", size=13,
                      color="#B5B5BF", bind="matter_temperature_warm"))
    objs.append(label(temperature, 240, 100, 210, 22, "Cool", size=13,
                      align="right", color="#B5B5BF",
                      bind="matter_temperature_cool"))

    color = len(objs)
    objs.append(layer(editor, 0, 56, 480, 236, name="matter_color_controls",
                      bind="matter_color_visible", hidden=True))
    objs.append(container(color, 12, 0, 456, 228,
                          bg="#181819", radius=18))
    objs.append(label(color, 30, 12, 260, 26, "Hue / saturation", size=18))
    objs.append(label(color, 30, 42, 205, 24, "Hue", size=17))
    hue_value = label(color, 240, 42, 210, 24, "Unknown", size=17,
                      align="right", bind="matter_hue_text")
    hue_value["font_charset"] = ASCII_PRINTABLE + "°"
    objs.append(hue_value)
    objs.append({
        "type": "slider", "parent": color,
        "x": 30, "y": 68, "w": 420, "h": 44,
        "value": 0, "min": 0, "max": 359,
        "name": "matter_hue_slider", "callback": "matter_hue",
        "bg_color": "#242425", "fg_color": "#C855FF",
        "knob_color": "#FCFCFF", "radius": 20, "track_size": 10,
    })
    objs.append(label(color, 30, 125, 205, 24, "Saturation", size=17))
    objs.append(label(color, 240, 125, 210, 24, "Unknown", size=17,
                      align="right", bind="matter_saturation_text"))
    objs.append({
        "type": "slider", "parent": color,
        "x": 30, "y": 151, "w": 420, "h": 44,
        "value": 0, "min": 0, "max": 100,
        "name": "matter_saturation_slider", "callback": "matter_saturation",
        "bg_color": "#242425", "fg_color": "#FF4C01",
        "knob_color": "#FCFCFF", "radius": 20, "track_size": 10,
    })

    readonly = len(objs)
    objs.append(layer(editor, 0, 56, 480, 88, name="matter_readonly",
                      bind="matter_readonly_visible", hidden=True))
    objs.append(container(readonly, 12, 0, 456, 80, bg="#181819", radius=18))
    objs.append(label(readonly, 30, 12, 420, 24, "State", size=18))
    readonly_value = label(readonly, 30, 42, 420, 24, "Unknown", size=18,
                           bind="matter_readonly_value")
    readonly_value["overflow"] = "ellipsis"
    readonly_value["font_charset"] = ASCII_PRINTABLE + "°—"
    objs.append(readonly_value)

    controls = len(objs)
    objs.append(layer(editor, 0, 56, 480, 292, name="matter_controls",
                      bind="matter_controls_visible", hidden=True))
    for group_name, y, title in (("first", 8, "Mode: —"),
                                 ("second", 84, "Clean: —")):
        group = len(objs)
        objs.append(layer(controls, 12, y, 456, 64,
                          name=f"matter_mode_{group_name}_group",
                          bind=f"matter_mode_{group_name}_visible", hidden=True))
        objs.append(container(group, 0, 0, 456, 64, bg="#181819", radius=18))
        for direction, x, symbol in (("previous", 0, "‹"),
                                     ("next", 392, "›")):
            arrow = button(group, x, 0, 64, 64, symbol, bg="#181819",
                           fg="#FF4C01", radius=18, size=30,
                           callback=f"matter_mode_{group_name}_{direction}")
            arrow["name"] = f"matter_mode_{group_name}_{direction}"
            objs.append(arrow)
        current = label(group, 72, 18, 312, 28, title, size=17,
                        align="center", bind=f"matter_mode_{group_name}_text")
        current["overflow"] = "ellipsis"
        current["font_charset"] = ASCII_PRINTABLE + "°—"
        objs.append(current)
    for name, y, callback in (("matter_pause_button", 14, "matter_pause_resume"),
                               ("matter_home_button", 94, "matter_go_home")):
        group = len(objs)
        objs.append(layer(controls, 12, y, 456, 64, name=f"{name}_group",
                          bind=f"{name}_visible", hidden=True))
        item = button(group, 0, 0, 456, 64, "Control", bg="#181819",
                      fg="#FF4C01", radius=18, callback=callback)
        item["bind"] = f"{name}_text"
        item["enabled"] = True
        item["name"] = name
        objs.append(item)
    speed = len(objs)
    objs.append(layer(controls, 0, 168, 480, 116, name="matter_speed",
                      bind="matter_speed_visible", hidden=True))
    objs.append(container(speed, 12, 0, 456, 108, bg="#181819", radius=18))
    objs.append(label(speed, 30, 12, 240, 24, "Speed", size=18))
    objs.append(label(speed, 270, 12, 180, 24, "—", size=18,
                      align="right", bind="matter_speed_value"))
    objs.append({
        "type": "slider", "parent": speed, "x": 30, "y": 52, "w": 420,
        "h": 44, "value": 0, "min": 0, "max": 100,
        "name": "matter_speed_slider", "callback": "matter_speed_set",
        "bg_color": "#242425", "fg_color": "#FF4C01",
        "knob_color": "#FCFCFF", "radius": 20, "track_size": 10,
    })
    target = len(objs)
    objs.append(layer(controls, 0, 14, 480, 116, name="matter_target",
                      bind="matter_target_visible", hidden=True))
    objs.append(container(target, 12, 0, 456, 108, bg="#181819", radius=18))
    objs.append(label(target, 30, 12, 420, 24, "—", size=18,
                      bind="matter_target_value"))
    objs.append({
        "type": "slider", "parent": target, "x": 30, "y": 52, "w": 420,
        "h": 44, "value": 0, "min": 0, "max": 100,
        "name": "matter_target_slider", "callback": "matter_target_set",
        "bg_color": "#242425", "fg_color": "#FF4C01",
        "knob_color": "#FCFCFF", "radius": 20, "track_size": 10,
    })
    title = label(editor, 24, 0, 432, 32, "Control", size=24,
                  bind="matter_editor_title")
    title["overflow"] = "ellipsis"
    title["font_charset"] = ASCII_PRINTABLE + "°—"
    objs.append(title)
    objs.append(label(editor, 24, 34, 432, 20, "", size=16,
                      color="#FF4C01", bind="matter_action_status"))

    row = len(objs)
    objs.append({
        "type": "container", "parent": -1,
        "x": 0, "y": 0, "w": 480, "h": 72,
        "template": "matter_detail_row",
        "events": [
            {"event": "click", "action": "call",
             "target_name": "matter_entry_open"},
        ],
        "bg_color": "#000000", "radius": 0,
    })
    objs.append(container(row, 12, 0, 456, 64, bg="#181819", radius=18))
    state_name = label(row, 30, 20, 192, 24, "State", size=17,
                       name="matter_detail_row_name")
    state_name["font_charset"] = ASCII_PRINTABLE + "°—"
    state_name["overflow"] = "ellipsis"
    objs.append(state_name)
    state_value = label(row, 226, 20, 198, 24, "—", size=17,
                        color="#B5B5BF", align="right",
                        name="matter_detail_row_value")
    state_value["dynamic_color"] = True
    state_value["overflow"] = "ellipsis"
    state_value["font_charset"] = ASCII_PRINTABLE + "°—"
    objs.append(state_value)
    objs.append(label(row, 430, 17, 25, 28, "›", size=22,
                      color="#B5B5BF", align="right",
                      name="matter_detail_row_arrow"))
