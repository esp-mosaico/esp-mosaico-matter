# matter_hub board configuration

The example supports only `esp_mosaico` (ESP32-S31, 16 MB flash, 480x480
Mosaic GSP UI). Its full board definition lives in the `esp-mosaico-claw`
submodule; this directory contains the Matter hub overlay:

```text
esp_mosaico/
  board_amend.yaml
  sdkconfig.defaults.board
```

From `examples/matter_hub/`, select the board and build:

```sh
idf.py bmgr -c "boards;../../third_party/esp-mosaico-claw/boards" -b esp_mosaico
idf.py build
```

Board Manager auto-discovers the overlay by its directory name and applies
`sdkconfig.defaults.board` on top of the upstream board defaults. No `-a` flag
is needed. The overlay configures the Matter controller, Mosaic UI, camera,
Thread Border Router and `partitions_16MB.csv` layout. Writable DATA lives on
SPI-NAND at `/nand`.

When migrating from a removed board, delete the old `sdkconfig` and `build/`,
then regenerate `components/gen_bmgr_codes/` with the command above.
