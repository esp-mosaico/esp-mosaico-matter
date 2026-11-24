#!/usr/bin/env bash
# Source this script to install and export the pinned ESP-IDF ESP32-S31 toolchain.
set -e
ci_repo_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
export IDF_PATH="$ci_repo_dir/build/esp-idf"
mkdir -p "$ci_repo_dir/build"
if [ ! -d "$IDF_PATH/.git" ]; then
    git clone --filter=blob:none --single-branch --branch master --no-checkout https://github.com/espressif/esp-idf.git "$IDF_PATH"
fi
git -C "$IDF_PATH" fetch origin d7547231a622dbc558efe14f28ece6d6249295ee
git -C "$IDF_PATH" checkout --detach d7547231a622dbc558efe14f28ece6d6249295ee
git -C "$IDF_PATH" submodule update --init --recursive --depth 1
# The bootstrap image may export a Python environment for a newer IDF.
# Let the pinned checkout select its own version-specific environment.
unset IDF_PYTHON_ENV_PATH
"$IDF_PATH/install.sh" esp32s31
. "$IDF_PATH/export.sh"
python -m pip install -r "$ci_repo_dir/third_party/esp-mosaico-claw/requirements.txt"
