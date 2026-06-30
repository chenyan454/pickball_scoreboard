idf_component_register(SRCS "bsp_adc.c" "bsp_uart.c" "tjc_usart_hmi.c"
                       INCLUDE_DIRS "."
                       REQUIRES driver esp_common freertos esp_adc)
component_compile_options(-ffast-math -O3 -Wno-error=format -Wno-format)