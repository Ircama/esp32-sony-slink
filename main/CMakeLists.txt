# ESP32-H2 has no WiFi radio (BLE 5 + 802.15.4 only): it gets the USB serial port and BLE, without the web portal, TCP console and MQTT.
# net_nowifi.c replaces the whole WiFi/web/MQTT layer there.
idf_build_get_property(target IDF_TARGET)
if(target STREQUAL "esp32h2")
    idf_component_register(SRCS "main.c" "slink.c" "cli.c" "decode.c" "settings.c" "state.c" "net_nowifi.c" "ble_nus.c" "svcmem.c" "sha256.c"
                           INCLUDE_DIRS "."
                           REQUIRES driver esp_driver_gpio esp_driver_rmt esp_driver_usb_serial_jtag esp_timer nvs_flash esp_event bt esp_hw_support esp_pm
                                    esp_app_format esp_system esp_rom heap esp_coex)
else()
    set(srcs "main.c" "slink.c" "cli.c" "decode.c" "settings.c" "state.c"
             "net.c" "web.c" "tls.c" "ota.c" "tcp_srv.c" "slinke.c" "hue.c" "mqtt_bridge.c" "ble_nus.c" "stats.c" "web_page.c" "svcmem.c" "sha256.c")
    set(reqs driver esp_driver_gpio esp_driver_rmt esp_driver_usb_serial_jtag spi_flash esp_partition esp_timer nvs_flash esp_wifi esp_netif esp_event esp_http_server esp_https_server mbedtls mqtt bt lwip esp_hw_support
             esp_pm esp_driver_tsens esp_app_format esp_system esp_rom heap esp_coex
             app_update esp_https_ota esp_http_client esp-tls)
    if(DEFINED ENV{SLINK_MATTER})                      # the esp32s3-matter build: matter.cpp and the esp-matter component (docs/MATTER.md)
        list(APPEND srcs "matter.cpp")
        list(APPEND reqs esp_matter)
    endif()
    idf_component_register(SRCS ${srcs} INCLUDE_DIRS "." REQUIRES ${reqs})
    if(DEFINED ENV{SLINK_MATTER})
        target_compile_definitions(${COMPONENT_LIB} PRIVATE SLINK_MATTER=1 CHIP_HAVE_CONFIG_H)
    endif()
endif()
