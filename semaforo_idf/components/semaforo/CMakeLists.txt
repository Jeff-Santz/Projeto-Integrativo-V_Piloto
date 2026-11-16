idf_component_register(SRCS "src/semaforo.cpp" "src/semaforo_rgb.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES protocolo can_bus driver esp_driver_gpio freertos led_strip
                       PRIV_REQUIRES log)
