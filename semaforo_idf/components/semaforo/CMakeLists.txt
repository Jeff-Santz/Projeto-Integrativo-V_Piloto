idf_component_register(SRCS "src/semaforo.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES protocolo can_bus driver esp_driver_gpio freertos
                       PRIV_REQUIRES log)
