set(MOSAIC_APP_NAME matter)
set(MOSAIC_APP_MODULE_SOURCE matter_app.c)
set(MOSAIC_APP_EXTRA_SOURCES
    matter_commissioning.c
    matter_detail.c
    matter_home.c
    matter_view_model.c)
set(MOSAIC_APP_MODULE_SYMBOL mosaic_matter_app)
set(MOSAIC_APP_BUNDLE generated/matter_480.gspb)
set(MOSAIC_APP_SCENE_DIR scene)
set(MOSAIC_APP_SCENE_JSON scene/matter_480.json)
set(MOSAIC_APP_GENERATOR scene/gen_scene.py)
set(MOSAIC_APP_SCENE_SOURCES
    scene/home_scene.py
    scene/commissioning_scene.py
    scene/detail_scene.py
    scene/layout.py)
set(MOSAIC_APP_GENERATED_HEADERS matter_templates.h)
set(MOSAIC_APP_LOGIC NATIVE)
set(MOSAIC_APP_TICK_MS 100)

set(MOSAIC_APP_REQUIRED 1)
