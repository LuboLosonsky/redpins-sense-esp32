idf_component_register(SRCS "display_hal.cpp" "main.cpp" "storage.cpp" "ble_server.cpp" "wifi_scanner.cpp" "app_config.cpp" "sensor_core.cpp"  "gui.cpp" "weather_client.cpp" "gui_primitives.cpp" "gui_helpers.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES littlefs)