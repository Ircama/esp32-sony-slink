#!/usr/bin/env bash
# Builds the ESP32-S3 firmware WITH Matter (docs/MATTER.md): plain ESP-IDF 6.1 + esp-matter, on Linux or WSL (esp-matter does not build on Windows).
#
# One time (about 8 GB, shallow clones):
#   git clone --depth 1 --branch v6.1 --recursive --shallow-submodules https://github.com/espressif/esp-idf.git && (cd esp-idf && ./install.sh esp32s3)
#   git clone --depth 1 --branch release/v1.6.1 https://github.com/espressif/esp-matter.git
#   (cd esp-matter && git submodule update --init --depth 1 && cd connectedhomeip/connectedhomeip && ./scripts/checkout_submodules.py --shallow --platform esp32 linux)
#   (cd esp-matter && ./install.sh --no-host-tool)
# Every time, in a shell where both are exported:
#   . esp-idf/export.sh && export ESP_MATTER_PATH=$PWD/esp-matter && . esp-matter/export.sh
#   tools/build_matter.sh            ->  out/slink-esp32s3-matter.bin           application image (OTA: tools/ota_push.py <host> <file>)
#                                        out/slink-esp32s3-matter-factory.bin   bootloader + partition table + application (USB, 0x0)
# The first build compiles the Matter SDK (about 30-40 minutes); the next ones only what changed.
set -euo pipefail
: "${IDF_PATH:?run ESP-IDF's export.sh first}"
: "${ESP_MATTER_PATH:?set ESP_MATTER_PATH to the esp-matter checkout (release/v1.6.1) and run its export.sh}"
cd "$(dirname "$0")/.."
python3 tools/gen_web.py                                                      # main/web_page.c from main/web/
export SLINK_MATTER=1                                                         # CMakeLists.txt / main/CMakeLists.txt: add esp_matter and matter.cpp
D="sdkconfig.defaults;sdkconfig.defaults.esp32s3;sdkconfig.defaults.matter"
if [ ! -f sdkconfig ] || ! grep -q '^CONFIG_CUSTOM_COMMISSIONABLE_DATA_PROVIDER=y' sdkconfig; then
    rm -f sdkconfig
    idf.py -DSDKCONFIG_DEFAULTS="$D" set-target esp32s3
fi
idf.py -DSDKCONFIG_DEFAULTS="$D" build
idf.py merge-bin -o "$PWD/build/factory.bin"
mkdir -p out
cp build/esp32c3_slink.bin out/slink-esp32s3-matter.bin
cp build/factory.bin out/slink-esp32s3-matter-factory.bin
ls -l out
