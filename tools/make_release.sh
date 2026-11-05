#!/bin/sh
# Build the release files after `idf.py build`:
#   build/t7bridge.bin        -> attach to the GitHub release (Wi-Fi updates)
#   docs/firmware/t7bridge-full.bin + docs/manifest.json -> web installer
set -e
cd "$(dirname "$0")/.."
VER=$(sed -n 's/^set(PROJECT_VER "\(.*\)")/\1/p' CMakeLists.txt)
OUT="$PWD/docs/firmware/t7bridge-full.bin"
mkdir -p docs/firmware
(cd build && python -m esptool --chip esp32s3 merge-bin -o "$OUT" @flash_args)
cat > docs/manifest.json <<JSON
{
  "name": "T7 Bridge",
  "version": "$VER",
  "new_install_prompt_erase": true,
  "builds": [
    { "chipFamily": "ESP32-S3", "parts": [ { "path": "firmware/t7bridge-full.bin", "offset": 0 } ] }
  ]
}
JSON
echo "Release $VER: build/t7bridge.bin, docs/firmware/t7bridge-full.bin, docs/manifest.json"
