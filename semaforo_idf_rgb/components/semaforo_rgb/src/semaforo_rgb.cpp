#include "semaforo_rgb.hpp"
#include "esp_log.h"

static const char* TAG = "SEMAFORO_RGB";

namespace {
// Brilho moderado -- WS2812 em 255 fica ofuscante de perto. Ajuste se quiser.
constexpr uint8_t BRILHO = 40;
} // namespace

SemaforoRGB::SemaforoRGB(uint8_t node_id, gpio_num_t pino_rgb,
                          const TemposSemaforo& tempos, can_proto::Fase fase_inicial)
    : Semaforo(node_id,
               // Pinos discretos nao sao usados por esta variante (todas as
               // funcoes que os leriam/escreveriam estao sobrescritas) --
               // GPIO_NUM_NC deixa isso explicito em vez de inventar um pino.
               PinosSemaforo{GPIO_NUM_NC, GPIO_NUM_NC, GPIO_NUM_NC,
                             GPIO_NUM_NC, GPIO_NUM_NC, GPIO_NUM_NC},
               tempos, fase_inicial),
      pino_rgb_(pino_rgb)
{
}

void SemaforoRGB::configurarGpios()
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = pino_rgb_,
        .max_leds = 1,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .led_model = LED_MODEL_WS2812,
    };
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000,
    };

    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &strip_));
    led_strip_clear(strip_);
    ESP_LOGI(TAG, "LED RGB embutido iniciado no GPIO%d", (int)pino_rgb_);
}

void SemaforoRGB::aplicarFase(can_proto::Fase fase)
{
    uint8_t r = 0, g = 0, b = 0;
    switch (fase) {
        case can_proto::Fase::VERDE:
            g = BRILHO;
            break;
        case can_proto::Fase::AMARELO:
        case can_proto::Fase::FAIL_SAFE: // mesma cor do firmware original: fail-safe = ambar piscando
            r = BRILHO;
            g = BRILHO / 2;
            break;
        case can_proto::Fase::VERMELHO:
            r = BRILHO;
            break;
    }
    led_strip_set_pixel(strip_, 0, r, g, b);
    led_strip_refresh(strip_);
}

void SemaforoRGB::apagarTudo()
{
    led_strip_clear(strip_);
}

bool SemaforoRGB::verificarCoerencia(can_proto::Fase /*fase*/, uint8_t& flags_mismatch)
{
    // Sem pino de leitura de volta nesta variante -- nao ha como conferir,
    // entao nunca reporta mismatch (decisao explicita, nao esquecimento).
    flags_mismatch = 0;
    return true;
}
