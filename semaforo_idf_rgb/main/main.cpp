#include "esp_log.h"

#include "can_bus.hpp"
#include "protocolo.hpp"
#include "semaforo.hpp"
#include "semaforo_rgb.hpp"

static const char* TAG = "MAIN_RGB";

// =====================================================================
// VARIANTE DE TESTE: usa o LED RGB embutido (WS2812) em vez dos 3 LEDs
// discretos, pra testar um no extra sem montar fiacao nova. Reaproveita
// os componentes de semaforo_idf (fail-safe, SYNC, protocolo, CAN).
//
// LIMITE: sem pino de leitura de volta, este no nunca detecta mismatch
// proprio -- ele so exercita o lado de protocolo (SYNC, propagacao de
// fail-safe, watchdogs). Nao serve pra validar a fiacao.
//
// Muda NODE_ID / FASE_INICIAL aqui pra cada placa de teste.
// =====================================================================
namespace {

constexpr uint8_t         NODE_ID      = 1;                          // 0..15, unico por no
constexpr can_proto::Fase FASE_INICIAL = can_proto::Fase::VERDE;     // oposto do no oficial (NODE_ID=0, VERMELHO)

// GPIO do LED RGB embutido (WS2812). ESP32-S3-DevKitC-1: GPIO48 na maioria
// das revisoes -- se nao acender, confere no silkscreen/datasheet da placa.
constexpr gpio_num_t RGB_GPIO = GPIO_NUM_48;

const TemposSemaforo TEMPOS = {
    .verde_ms    = 3000,
    .amarelo_ms  = 1000,
    .vermelho_ms = 4000,
};

} // namespace

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "=== Semaforo de TESTE (RGB onboard + CAN) ===");

    ESP_ERROR_CHECK(can_bus::iniciar());

    // static: o objeto precisa sobreviver ao fim do app_main, pois as tasks usam `this`.
    static SemaforoRGB semaforo(NODE_ID, RGB_GPIO, TEMPOS, FASE_INICIAL);
    // Nos que DEVEM existir na rede (bit = node_id). Manter igual ao oficial.
    semaforo.definirNosEsperados((1u << 0) | (1u << 1));
    semaforo.iniciar();
}
