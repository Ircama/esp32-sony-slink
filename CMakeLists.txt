cmake_minimum_required(VERSION 3.16)

# Matter build (ESP32-S3 only; Linux or WSL; needs esp-matter and ESP-IDF >= 6.0): environment SLINK_MATTER=1 and ESP_MATTER_PATH=<esp-matter>. See docs/MATTER.md.
# Every other build (PlatformIO, CI) leaves both unset and is unchanged.
if(DEFINED ENV{SLINK_MATTER} AND DEFINED ENV{ESP_MATTER_PATH})
    set(ESP_MATTER_PATH $ENV{ESP_MATTER_PATH})
    set(MATTER_SDK_PATH ${ESP_MATTER_PATH}/connectedhomeip/connectedhomeip)
    set(ENV{ESP_MATTER_DEVICE_PATH} ${ESP_MATTER_PATH}/device_hal/device/esp32s3_devkit_c)
    set(PROJECT_VER "1.0")
    set(PROJECT_VER_NUMBER 1)
    include($ENV{IDF_PATH}/tools/cmake/project.cmake)
    include(${ESP_MATTER_PATH}/examples/common/cmake_common/components_include.cmake)
    set(EXTRA_COMPONENT_DIRS
        "${MATTER_SDK_PATH}/config/esp32/components"
        "${ESP_MATTER_PATH}/components"
        ${extra_components_dirs_append})
    project(esp32c3_slink)
    idf_build_set_property(CXX_COMPILE_OPTIONS "-std=gnu++17;-Os;-DCHIP_HAVE_CONFIG_H;-Wno-overloaded-virtual" APPEND)
    idf_build_set_property(C_COMPILE_OPTIONS "-Os" APPEND)
    idf_build_set_property(COMPILE_OPTIONS "-Wno-format-nonliteral;-Wno-format-security" APPEND)
else()
    include($ENV{IDF_PATH}/tools/cmake/project.cmake)
    project(esp32c3_slink)
endif()
