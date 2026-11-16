idf_component_register(SRCS "src/semaforo_rgb.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES semaforo protocolo driver esp_driver_gpio led_strip
                       PRIV_REQUIRES log)
