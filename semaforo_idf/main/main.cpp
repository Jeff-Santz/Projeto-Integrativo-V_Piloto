#include "esp_log.h"

#include "can_bus.hpp"
#include "protocolo.hpp"
#include "semaforo.hpp"
#include "semaforo_rgb.hpp"

static const char* TAG = "MAIN";

// =====================================================================
// CONFIGURACAO DESTE NO -- e so aqui que voce muda de um semaforo pro outro.
// Encruzilhada de 2 semaforos: mesmos tempos, node_id diferente e fases
// iniciais opostas (um VERDE, o outro VERMELHO).
// =====================================================================
namespace {

// true  = este no usa o LED RGB embutido na placa (WS2812) em vez dos 3
//         LEDs discretos -- pra testar um 2o no sem montar fiacao nova.
//         Nao faz verificacao de coerencia (sem pino de leitura de volta).
// false = no "de verdade", com os 3 LEDs + 3 entradas de leitura (PINOS).
constexpr bool USAR_RGB_ONBOARD = true;

constexpr uint8_t         NODE_ID      = 0;                          // 0..15, unico por no
constexpr can_proto::Fase FASE_INICIAL = can_proto::Fase::VERMELHO;  // oposto do outro no (NODE_ID=1, VERDE)

const PinosSemaforo PINOS = {
    .out_verde    = GPIO_NUM_13,
    .out_amarelo  = GPIO_NUM_16,
    .out_vermelho = GPIO_NUM_11,
    .in_verde     = GPIO_NUM_10,
    .in_amarelo   = GPIO_NUM_9,
    .in_vermelho  = GPIO_NUM_3,
};

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
    ESP_LOGI(TAG, "=== Semaforo inteligente (ESP-IDF + CAN) ===");

    ESP_ERROR_CHECK(can_bus::iniciar());

    // static: o objeto precisa sobreviver ao fim do app_main, pois as tasks usam `this`.
    // So uma das duas branches constroi o objeto (inicializacao de static
    // local so roda quando o fluxo passa por ela), a outra nunca e tocada.
    static Semaforo* semaforo = nullptr;
    if (USAR_RGB_ONBOARD) {
        static SemaforoRGB instancia(NODE_ID, RGB_GPIO, TEMPOS, FASE_INICIAL);
        semaforo = &instancia;
    } else {
        static Semaforo instancia(NODE_ID, PINOS, TEMPOS, FASE_INICIAL);
        semaforo = &instancia;
    }
    semaforo->iniciar();
}
