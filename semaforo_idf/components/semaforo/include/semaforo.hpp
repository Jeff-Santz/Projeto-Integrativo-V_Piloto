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
    // Configura os GPIOs (ou outro periferico de saida) usados pela fase.
    // Chamado uma vez em iniciar(). Subclasses que nao usam os pinos de
    // pinos_ (ex.: variante com LED RGB enderecavel) sobrescrevem isso.
    virtual void configurarGpios();
    virtual void aplicarFase(can_proto::Fase fase);
    virtual bool verificarCoerencia(can_proto::Fase fase, uint8_t& flags_mismatch);
    virtual void onMensagemCAN(const twai_message_t& msg);

    // Apaga todas as saidas (usado no "off" da piscada do fail-safe).
    // Default: desliga os 3 GPIOs de pinos_. Subclasses sobrescrevem se
    // o jeito de "desligar" nao for 3 gpio_set_level().
    virtual void apagarTudo();

    // propagar=true: este no detectou o problema e avisa o barramento.
    // propagar=false: o fail-safe veio de outro no, so obedece.
    void entrarFailSafe(bool propagar);

    // Sai do fail-safe e retoma a rotina a partir da fase_inicial_.
    void restaurarOperacao();

    // Recebeu um SyncMsg (relogio oficial) da master. Nao muda fase nem
    // estado nenhum diretamente -- so atualiza ajuste_ms_ (ver abaixo).
    // Nao faz nada se estiver em fail-safe (isso e decidido so pela
    // fiacao).
    void aplicarSync(uint32_t epoch_ms);

private:
    static void taskRotinaTrampolim(void* self);
    static void taskStatusTrampolim(void* self);
    static void taskEscutaTrampolim(void* self);
    static void timerStatusCallback(TimerHandle_t xTimer);

    void taskRotina();
    void taskStatus();
    void taskEscuta();

    // "Hora oficial" deste no agora, em ms: o tick local (livre, nunca
    // para) + ajuste_ms_ (atualizado a cada SyncMsg). A fase atual e so
    // uma funcao disso -- nao tem "proxima fase" guardada em lugar
    // nenhum, a rotina so reflete o que esse relogio diz a cada volta.
    uint32_t relogioOficialMs() const;

    uint8_t node_id_;
    PinosSemaforo pinos_;
    TemposSemaforo tempos_;
    can_proto::Fase fase_inicial_;
    std::atomic<can_proto::Fase> fase_atual_;
    std::atomic<uint32_t> inicio_fase_tick_;
    // Quanto somar ao tick local (xTaskGetTickCount*portTICK_PERIOD_MS)
    // pra chegar na "hora oficial" do ciclo. Cada SyncMsg recalcula isso
    // (nunca "pula" a fase na mao, so ajusta o relogio que a rotina
    // consulta). restaurarOperacao() zera pra "oficial = 0 agora" ate
    // chegar um sync de verdade.
    std::atomic<int32_t> ajuste_ms_{0};

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
