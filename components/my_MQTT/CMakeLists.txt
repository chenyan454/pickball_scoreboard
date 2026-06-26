idf_component_register(SRCS  "my_mqtt.c"
                      INCLUDE_DIRS "."
                      REQUIRES mqtt json freertos)


component_compile_options(-ffast-math -O3 -Wno-error=format -Wno-format -Wno-pointer-sign)
