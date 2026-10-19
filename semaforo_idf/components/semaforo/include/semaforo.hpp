#pragma once

#include <atomic>
#include <cstdint>

#include "driver/gpio.h"
#include "driver/twai.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#include "protocolo.hpp"

struct PinosSemaforo {
    gpio_num_t out_verde;
    gpio_num_t out_amarelo;
    gpio_num_t out_vermelho;
    gpio_num_t in_verde;
    gpio_num_t in_amarelo;
    gpio_num_t in_vermelho;
};

struct TemposSemaforo {
    uint32_t verde_ms;
    uint32_t amarelo_ms;
    uint32_t vermelho_ms;
};

// Classe base. Pedestre (ou outra variante futura) herda daqui e
// sobrescreve aplicarFase()/onMensagemCAN() sem duplicar tasks, CAN e
// maquina de estados.
class Semaforo {
public:
    Semaforo(uint8_t node_id, const PinosSemaforo& pinos,
             const TemposSemaforo& tempos, can_proto::Fase fase_inicial);
    virtual ~Semaforo() = default;

    // Configura GPIOs e cria as 3 tasks (rotina, status, escuta). Nao bloqueia.
    // O CAN (can_bus::iniciar) deve ter sido iniciado antes.
    void iniciar();

    // Troca os tempos em tempo real, protegido por mutex.
    void definirTempos(const TemposSemaforo& novos);

    uint8_t node_id() const { return node_id_; }

protected:
    virtual void aplicarFase(can_proto::Fase fase);
    virtual bool verificarCoerencia(can_proto::Fase fase, uint8_t& flags_mismatch);
    virtual void onMensagemCAN(const twai_message_t& msg);

    // propagar=true: este no detectou o problema e avisa o barramento.
    // propagar=false: o fail-safe veio de outro no, so obedece.
    void entrarFailSafe(bool propagar);

    // Sai do fail-safe e retoma a rotina a partir da fase_inicial_.
    void restaurarOperacao();

private:
    static void taskRotinaTrampolim(void* self);
    static void taskStatusTrampolim(void* self);
    static void taskEscutaTrampolim(void* self);
    static void timerStatusCallback(TimerHandle_t xTimer);

    void taskRotina();
    void taskStatus();
    void taskEscuta();

    // Espera total_ms conferindo a coerencia a cada CHECK_INTERVAL_MS.
    // Retorna true se interrompida (fail-safe/notify), false se completou.
    bool esperarFase(can_proto::Fase fase, uint32_t total_ms);

    uint8_t node_id_;
    PinosSemaforo pinos_;
    TemposSemaforo tempos_;
    can_proto::Fase fase_inicial_;
    std::atomic<can_proto::Fase> fase_atual_;
    std::atomic<uint32_t> inicio_fase_tick_;

    SemaphoreHandle_t mutex_tempos_;
    SemaphoreHandle_t sinal_envio_status_;
    TaskHandle_t task_rotina_handle_ = nullptr;
    TimerHandle_t timer_status_ = nullptr;

    static constexpr uint32_t PERIODO_STATUS_MS = 1000;
    static constexpr uint32_t CHECK_INTERVAL_MS = 200;
    static constexpr uint32_t BLINK_FAILSAFE_MS = 500;
    // Leituras corretas seguidas (cada uma a cada 2*BLINK_FAILSAFE_MS = 1s)
    // exigidas antes de sair do fail-safe sozinho. 3 = ~3s de fiacao estavel.
    static constexpr int RECOVERY_CONFIRMACOES = 3;
};
