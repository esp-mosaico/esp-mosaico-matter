# Clock font

Regenerate from the repository root with FontTools 4.64.0:

```sh
uv tool run --from fonttools==4.64.0 pyftsubset \
  third_party/esp-mosaico-claw/third-party/esp-claw/application/edge_agent/fatfs_image/system/fonts/NotoSansSC-Regular-sub.ttf \
  --unicodes=U+002D,U+0030-003A \
  --output-file=examples/matter_hub/main/fonts/clock.ttf \
  --name-IDs='*' --name-languages='*'
```
