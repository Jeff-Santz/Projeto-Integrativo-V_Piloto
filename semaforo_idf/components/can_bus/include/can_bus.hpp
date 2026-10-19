#pragma once
#include <cstdint>
#include "driver/gpio.h"
#include "driver/twai.h"
#include "esp_err.h"

// Camada fina sobre o driver TWAI legado (500 kbps, GPIO5/GPIO4).
namespace can_bus {

constexpr gpio_num_t TX_GPIO = GPIO_NUM_5;
constexpr gpio_num_t RX_GPIO = GPIO_NUM_4;

esp_err_t iniciar();
esp_err_t enviar(uint32_t id, const uint8_t* dados, uint8_t dlc);
bool receber(twai_message_t* out, uint32_t timeout_ms);
void tratar_alertas(const char* tag);

} // namespace can_bus
