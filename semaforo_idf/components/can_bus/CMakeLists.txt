idf_component_register(SRCS "src/can_bus.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES driver esp_driver_gpio
                       PRIV_REQUIRES log)
