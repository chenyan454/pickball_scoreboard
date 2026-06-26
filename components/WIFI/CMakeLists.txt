idf_component_register(SRCS  "my_wifi.c"
                      INCLUDE_DIRS "."
                      REQUIRES esp_wifi esp_event freertos lwip nvs_flash esp_common)
component_compile_options(-ffast-math -O3 -Wno-error=format -Wno-format -Wno-pointer-sign)
