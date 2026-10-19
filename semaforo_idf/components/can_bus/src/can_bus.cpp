#include "can_bus.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

namespace can_bus {

esp_err_t iniciar()
{
    twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(TX_GPIO, RX_GPIO, TWAI_MODE_NORMAL);
    g.alerts_enabled = TWAI_ALERT_TX_SUCCESS | TWAI_ALERT_TX_FAILED | TWAI_ALERT_ERR_PASS |
                       TWAI_ALERT_BUS_OFF | TWAI_ALERT_BUS_RECOVERED | TWAI_ALERT_BUS_ERROR |
                       TWAI_ALERT_RX_QUEUE_FULL;
    twai_timing_config_t t = TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t f  = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    esp_err_t err = twai_driver_install(&g, &t, &f);
    if (err != ESP_OK) return err;
    return twai_start();
}

esp_err_t enviar(uint32_t id, const uint8_t* dados, uint8_t dlc)
{
    twai_message_t msg = {};
    msg.identifier = id;
    msg.data_length_code = dlc;
    for (int i = 0; i < dlc && i < 8; i++) msg.data[i] = dados[i];
    return twai_transmit(&msg, pdMS_TO_TICKS(100));
}

bool receber(twai_message_t* out, uint32_t timeout_ms)
{
    return twai_receive(out, pdMS_TO_TICKS(timeout_ms)) == ESP_OK;
}

void tratar_alertas(const char* tag)
{
    uint32_t alerts;
    while (twai_read_alerts(&alerts, 0) == ESP_OK) {
        if (alerts & TWAI_ALERT_BUS_OFF) {
            ESP_LOGE(tag, "[CAN] BUS-OFF! Recuperando...");
            twai_initiate_recovery();
        }
        if (alerts & TWAI_ALERT_BUS_RECOVERED) {
            ESP_LOGI(tag, "[CAN] Bus recuperado, reiniciando driver");
            twai_start();
        }
        if (alerts & TWAI_ALERT_TX_FAILED)     ESP_LOGW(tag, "[CAN] TX falhou");
        if (alerts & TWAI_ALERT_ERR_PASS)      ESP_LOGW(tag, "[CAN] Error-passive");
        if (alerts & TWAI_ALERT_RX_QUEUE_FULL) ESP_LOGW(tag, "[CAN] Fila RX cheia, frames perdidos");
    }
}

} // namespace can_bus
