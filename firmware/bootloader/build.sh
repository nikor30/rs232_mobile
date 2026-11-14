#!/bin/sh
# Builds bootloader.bin: the bootloader of Arduino-ESP32 2.0.17 (ESP-IDF v4.4.7,
# flash mode DIO, 80 MHz) plus the hook in bootloader_components/usb_port.
# Without the hook this gives, byte for byte, the bootloader PlatformIO flashes
# otherwise - checked with "./build.sh check" before the result is trusted.
#
# Needs git, cmake, ninja, python3 and PlatformIO's packages for this project
# (run "pio run" once). ESP-IDF is fetched to $IDF_PATH on first use (300 MB).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
PIO=${PLATFORMIO_CORE_DIR:-$HOME/.platformio}/packages
CORE=$PIO/framework-arduinoespressif32
IDF_PATH=${IDF_PATH:-$HOME/.cache/rs232-console/esp-idf-v4.4.7}
WORK=${WORK:-$HOME/.cache/rs232-console/bootloader-build}
export IDF_PATH IDF_SKIP_CHECK_SUBMODULES=1 IDF_COMPONENT_MANAGER=0
export PATH="$PIO/toolchain-xtensa-esp32s3/bin:$PATH"

grep -q 'ESP_IDF_VERSION_PATCH *7' "$CORE/tools/sdk/esp32s3/include/esp_common/include/esp_idf_version.h" ||
  { echo "the Arduino core is no longer built on ESP-IDF 4.4.7: adjust the version here" >&2; exit 1; }
if [ ! -d "$IDF_PATH" ]; then
  git -c advice.detachedHead=false clone -q --depth 1 -b v4.4.7 https://github.com/espressif/esp-idf "$IDF_PATH"
  git -C "$IDF_PATH" submodule update -q --init --depth 1 components/esptool_py/esptool \
    components/bootloader/subproject/components/micro-ecc/micro-ecc
fi
mkdir -p "$WORK"
[ -x "$WORK/py/bin/python" ] || { python3 -m venv --system-site-packages "$WORK/py"; "$WORK/py/bin/pip" -q install kconfiglib==13.7.1 future pyparsing; }

# the settings the core's own bootloader was built with; DIO as in platformio.ini
sed -e 's/^CONFIG_ESPTOOLPY_FLASHMODE_QIO=y/# CONFIG_ESPTOOLPY_FLASHMODE_QIO is not set/' \
    -e 's/^# CONFIG_ESPTOOLPY_FLASHMODE_DIO is not set/CONFIG_ESPTOOLPY_FLASHMODE_DIO=y/' \
    "$CORE/tools/sdk/esp32s3/sdkconfig" > "$WORK/sdkconfig"

build() {   # $1 = project directory whose bootloader_components go in, $2 = output
  rm -rf "$WORK/build"
  cmake -G Ninja -S "$IDF_PATH/components/bootloader/subproject" -B "$WORK/build" -DSDKCONFIG="$WORK/sdkconfig" \
    -DIDF_PATH="$IDF_PATH" -DIDF_TARGET=esp32s3 -DPYTHON_DEPS_CHECKED=1 -DPYTHON="$WORK/py/bin/python" \
    -DEXTRA_COMPONENT_DIRS="$IDF_PATH/components/bootloader" -DPROJECT_SOURCE_DIR="$1" \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 > "$WORK/cmake.log" 2>&1 || { tail -20 "$WORK/cmake.log" >&2; exit 1; }
  ninja -C "$WORK/build" bootloader.elf > "$WORK/ninja.log" 2>&1 || { tail -20 "$WORK/ninja.log" >&2; exit 1; }
  python3 "$PIO/tool-esptoolpy/esptool.py" --chip esp32s3 elf2image --flash_mode dio --flash_freq 80m \
    --flash_size 16MB -o "$2" "$WORK/build/bootloader.elf" > /dev/null
}

if [ "$1" = check ]; then   # no hook: must equal the core's bootloader
  mkdir -p "$WORK/empty"
  build "$WORK/empty" "$WORK/plain.bin"
  python3 "$PIO/tool-esptoolpy/esptool.py" --chip esp32s3 elf2image --flash_mode dio --flash_freq 80m \
    --flash_size 16MB -o "$WORK/stock.bin" "$CORE/tools/sdk/esp32s3/bin/bootloader_dio_80m.elf" > /dev/null
  cmp "$WORK/plain.bin" "$WORK/stock.bin" && echo "the build without the hook equals the core's bootloader"
else
  build "$HERE" "$HERE/bootloader.bin"
  ls -l "$HERE/bootloader.bin"
fi
