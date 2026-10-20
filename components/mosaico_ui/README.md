# Mosaic UI assets

This component is shared by `examples/matter_hub` and `examples/matter_bridge`.
Each example selects its app roster and Matter backend through CMake.
Camera previews use the hub's `camera_buffer_pool` only when
`CONFIG_CAMERA_BUFFER_POOL_ENABLE` is enabled. Otherwise they allocate buffers
dynamically, so the bridge does not depend on the hub's pool component.

The hub and each app have independent intermediate `.gspb` packages. Firmware
builds generate the selected apps and a shared Font Catalog under
`<example>/build/esp-idf/mosaico_ui/mosaic_gen/`:

- `hub/` and `<app>/` contain self-contained intermediates and generated headers.
- `linked/` contains product bundles after font linking.
- `common-fonts.gspb` is the shared Catalog kept open by the Launcher.

Run `idf.py build` from the chosen example after changing a scene. CMake
regenerates the affected intermediates, links the selected product bundles,
and stages them with the Catalog in the `ui_apps` mmap partition.

For optional source-tree regeneration, run `./regenerate_font_catalog.sh` from
this component directory with `GSPC` set to the matching compiler executable.
That script regenerates all apps under `hub/generated` and `apps/*/generated`,
then writes linked bundles and `font-link-report.md` under `common/generated/`.

The Loader opens the Catalog once and passes it to each App Context. Context
creation resolves local fonts first and Catalog fonts second. Rendering retains
a direct scene-local `font_ref` table and performs no Catalog lookup per frame.
Standalone/third-party Apps keep the default `embedded` policy. Mosaic scenes
use `auto`; large numeric and App-specific fonts remain embedded unless another
App has the exact same Font Asset ID.

Scene generators describe each font size with one of three charset policies:

- `shared_charset(...)` keeps a stable common set, normally printable ASCII at
  16/18px, so different Apps produce the same Catalog Asset ID.
- `auto_charset()` collects only static label text and the charsets declared by
  dynamic labels at that font and size.
- `explicit_charset(...)` documents a deliberately small dynamic set, such as
  `:0123456789` for a clock or `-.0123456789` for a sensor value.

Dynamic labels must still carry `font_charset` on the label itself. Asset-only
scenes do not seed fonts; this prevents their build from replacing the primary
scene's per-size subsets with a broad fallback set.

## Hub navigation and Lock Screen

`hub_stack` contains only ordinary navigation: Launcher, Settings,
Notification Center, and Insert Notice. Launcher owns its nested PageFlow.
AOD and CHRG are two mutually exclusive modes of the top-level `lock_screen`
visibility group; they are not StackView pages and never participate in
PageFlow transforms. While visible, the Hub input interceptor consumes every
pointer sample, with only the demo mode button and tap/swipe-up unlock handled
locally. Hiding Lock Screen therefore resumes the exact StackView/PageFlow
state that was present before locking.
