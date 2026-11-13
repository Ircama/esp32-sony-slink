# ESP32-H2 has no WiFi radio (BLE 5 + 802.15.4 only): it gets the USB serial port and BLE, without the web portal, TCP console and MQTT.
# net_nowifi.c replaces the whole WiFi/web/MQTT layer there.
idf_build_get_property(target IDF_TARGET)
if(target STREQUAL "esp32h2")
    idf_component_register(SRCS "main.c" "slink.c" "cli.c" "decode.c" "settings.c" "state.c" "net_nowifi.c" "ble_nus.c"
                           INCLUDE_DIRS "."
                           REQUIRES driver esp_driver_gpio esp_driver_rmt esp_driver_usb_serial_jtag esp_timer nvs_flash esp_event bt esp_hw_support esp_pm
                                    esp_app_format esp_system esp_rom heap)
else()
    idf_component_register(SRCS "main.c" "slink.c" "cli.c" "decode.c" "settings.c" "state.c"
                                "net.c" "web.c" "ota.c" "tcp_srv.c" "mqtt_bridge.c" "ble_nus.c" "stats.c" "web_page.c"
                           INCLUDE_DIRS "."
                           REQUIRES driver esp_timer nvs_flash esp_wifi esp_netif esp_event esp_http_server mqtt bt lwip esp_hw_support
                                    esp_pm esp_driver_tsens esp_app_format esp_system esp_rom heap
                                    app_update esp_https_ota esp_http_client esp-tls)
endif()
