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

// Por que este no esta em fail-safe. Decide COMO ele sai:
//   LOCAL          -- este no detectou mismatch na propria fiacao. Sai sozinho
//                     quando a fiacao voltar a bater (e avisa todos com ID_RESET).
//   REMOTO         -- obedeceu um ID_FAILSAFE de outro no/master. Nao sai por
//                     conta propria: espera o ID_RESET de quem causou o problema.
//   SYNC_PERDIDO   -- parou de receber SYNC da master. Sai quando os SYNCs voltam.
//   PAR_SILENCIOSO -- um no que devia estar na rede parou de mandar status.
//                     Sai quando todos os pares voltam a falar.
enum class MotivoFailSafe : uint8_t {
    NENHUM,
    LOCAL,
    REMOTO,
    SYNC_PERDIDO,
    PAR_SILENCIOSO,
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

    // Mascara dos node_ids que DEVEM existir na rede (bit i = node_id i;
    // inclua este no, ele e ignorado). Um par esperado que nunca apareca
    // em BOOT_GRACE_MS derruba a rede pra fail-safe. 0 = so vigia pares que
    // ja foram vistos. Chamar antes de iniciar().
    void definirNosEsperados(uint16_t mascara) { nos_esperados_ = mascara; }

    uint8_t node_id() const { return node_id_; }

protected:
    // Configura os GPIOs (ou outro periferico de saida) usados pela fase.
    // Chamado uma vez em iniciar(). Subclasses que nao usam os pinos de
    // pinos_ (ex.: variante com LED RGB enderecavel) sobrescrevem isso.
    virtual void configurarGpios();
    virtual void aplicarFase(can_proto::Fase fase);
    // Em FAIL_SAFE o padrao esperado e "so amarelo aceso": chamar com o
    // amarelo ligado (se chamar com tudo apagado vai dar mismatch).
    virtual bool verificarCoerencia(can_proto::Fase fase, uint8_t& flags_mismatch);
    virtual void onMensagemCAN(const twai_message_t& msg);

    // Apaga todas as saidas (usado no "off" da piscada do fail-safe).
    // Default: desliga os 3 GPIOs de pinos_. Subclasses sobrescrevem se
    // o jeito de "desligar" nao for 3 gpio_set_level().
    virtual void apagarTudo();

    // Entra em fail-safe. So avisa o barramento (ID_FAILSAFE) quando o
    // motivo e deste no (qualquer um exceto REMOTO).
    void entrarFailSafe(MotivoFailSafe motivo);

    // Sai do fail-safe e retoma a rotina a partir da fase_inicial_.
    void restaurarOperacao();

    // Recebeu um SyncMsg (relogio oficial) da master. Nao muda fase nem
    // estado nenhum diretamente -- so atualiza ajuste_ms_ (ver abaixo) e
    // marca que a master esta viva. Em fail-safe so registra o instante
    // (o ajuste e refeito no ID_RESET).
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

    // Mascara dos pares (exceto este no) que estao mudos agora: ja foram
    // vistos e passaram de PEER_TIMEOUT_MS sem status, ou eram esperados e
    // nunca apareceram depois do BOOT_GRACE_MS.
    uint16_t paresSilenciosos(uint32_t agora_ms) const;

    // Uma volta do loop de fail-safe: diz se a condicao de saida do MOTIVO
    // atual esta satisfeita agora. (REMOTO nunca esta: espera ID_RESET.)
    bool condicaoDeSaida(bool fiacao_ok);

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

    std::atomic<MotivoFailSafe> motivo_failsafe_{MotivoFailSafe::NENHUM};

    // Vigia da master: instante do ultimo SYNC. So arma depois do 1o SYNC.
    std::atomic<uint32_t> ultimo_sync_ms_{0};
    std::atomic<bool> sync_visto_{false};

    // Vigia dos pares: instante do ultimo status de cada node_id (0..15).
    std::atomic<uint32_t> ultimo_status_ms_[16]{};
    std::atomic<uint16_t> peers_vistos_{0};
    uint16_t nos_esperados_ = 0;
    uint32_t boot_ms_ = 0;

    SemaphoreHandle_t mutex_tempos_;
    SemaphoreHandle_t sinal_envio_status_;
    TaskHandle_t task_rotina_handle_ = nullptr;
    TimerHandle_t timer_status_ = nullptr;

    static constexpr uint32_t PERIODO_STATUS_MS = 1000;
    static constexpr uint32_t CHECK_INTERVAL_MS = 200;
    static constexpr uint32_t BLINK_FAILSAFE_MS = 500;
    // Leituras corretas seguidas (cada uma a cada 2*BLINK_FAILSAFE_MS = 1s)
    // exigidas antes de sair do fail-safe sozinho. 3 = ~3s estaveis.
    static constexpr int RECOVERY_CONFIRMACOES = 3;
    // Mismatches seguidos (1 por CHECK_INTERVAL_MS) exigidos pra entrar em
    // fail-safe: filtra uma leitura ruim isolada. 1 = comportamento antigo.
    static constexpr int MISMATCH_CONFIRMACOES = 2;
    // Sem SYNC por tanto tempo (3 periodos de 2 s) = master morta/desconectada.
    static constexpr uint32_t SYNC_TIMEOUT_MS = 6000;
    // Sem status de um par por tanto tempo (3 periodos de 1 s) = par morto.
    static constexpr uint32_t PEER_TIMEOUT_MS = 3000;
    // Tempo pra um par esperado aparecer pela 1a vez depois do boot.
    static constexpr uint32_t BOOT_GRACE_MS = 10000;
};
