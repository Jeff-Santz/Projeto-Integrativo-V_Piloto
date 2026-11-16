#include "esp_log.h"

#include "can_bus.hpp"
#include "protocolo.hpp"
#include "semaforo.hpp"

static const char* TAG = "MAIN";

// =====================================================================
// CONFIGURACAO DESTE NO -- e so aqui que voce muda de um semaforo pro outro.
// Encruzilhada de 2 semaforos: mesmos tempos, node_id diferente e fases
// iniciais opostas (um VERDE, o outro VERMELHO).
//
// Este e o firmware OFICIAL: 3 LEDs + 3 entradas de leitura (verificacao
// de coerencia por pino). A variante de teste com o LED RGB embutido fica
// em semaforo_idf_rgb/ (reaproveita os mesmos componentes).
// =====================================================================
namespace {

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
    static Semaforo semaforo(NODE_ID, PINOS, TEMPOS, FASE_INICIAL);
    semaforo.iniciar();
}
