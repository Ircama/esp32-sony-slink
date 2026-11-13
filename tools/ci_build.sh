#!/usr/bin/env bash
# Builds the firmware for one chip with plain ESP-IDF (used by .github/workflows/firmware.yml, usable by hand in an IDF shell):
#   tools/ci_build.sh esp32s3     ->  out/slink-esp32s3.bin          application image: what OTA (pull / push) installs
#                                     out/slink-esp32s3-factory.bin  bootloader + partition table + application: flash it at 0x0 over USB
set -euo pipefail
T="${1:?usage: tools/ci_build.sh <esp32c3|esp32c5|esp32c6|esp32h2|esp32s3>}"
cd "$(dirname "$0")/.."
git config --global --add safe.directory "$PWD" 2>/dev/null || true        # the version string comes from "git describe"
rm -rf build sdkconfig dependencies.lock                                    # one clean build per chip; the component versions are resolved afresh
python tools/gen_web.py                                                     # main/web_page.c from main/web/
idf.py set-target "$T"
idf.py build
idf.py merge-bin -o "$PWD/build/factory.bin"     # idf.py runs it inside build/, so the path must be absolute
mkdir -p out
cp build/esp32c3_slink.bin "out/slink-$T.bin"
cp build/factory.bin "out/slink-$T-factory.bin"
ls -l out
