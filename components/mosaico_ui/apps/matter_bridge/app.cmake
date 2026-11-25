set(MOSAIC_APP_NAME matter_bridge)
set(MOSAIC_APP_MODULE_SOURCE matter_bridge_app.c)
set(MOSAIC_APP_EXTRA_SOURCES frontend_backend.c frontend_model.c frontend_ui.c frontend_camera.c frontend_jobs.c frontend_marquee.c)
set(MOSAIC_APP_MODULE_SYMBOL mosaic_matter_bridge_app)
set(MOSAIC_APP_BUNDLE generated/matter_bridge_480.gspb)
set(MOSAIC_APP_SCENE_DIR scene)
set(MOSAIC_APP_SCENE_JSON scene/matter_bridge_480.json)
set(MOSAIC_APP_GENERATOR scene/gen_scene.py)
set(MOSAIC_APP_SCENE_SOURCES
    scene/frontend_scene.py
    scene/frontend_bindings.json
)
set(MOSAIC_APP_GENERATED_HEADERS
    matter_bridge_binds.h
    matter_bridge_actions.h
    matter_bridge_objects.h
    matter_bridge_templates.h)
set(MOSAIC_APP_LOGIC NATIVE)
set(MOSAIC_APP_TICK_MS 40)

set(MOSAIC_APP_REQUIRED 1)
