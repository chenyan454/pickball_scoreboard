idf_component_register(SRCS  "onenet.c"
                      INCLUDE_DIRS "."
                      REQUIRES my_MQTT mqtt json)

target_add_binary_data(${COMPONENT_TARGET} "certs/certificate.pem" TEXT)
component_compile_options(-ffast-math -O3 -Wno-error=format -Wno-format -Wno-pointer-sign)
