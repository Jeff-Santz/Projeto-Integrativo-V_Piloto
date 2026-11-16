#include "semaforo.hpp"

#include <cstring>

#include "can_bus.hpp"
#include "esp_log.h"

static const char* TAG = "SEMAFORO";

namespace {

// Ordem fixa do ciclo normal. A fase inicial de cada no so escolhe por
// onde ele entra nesse ciclo.
constexpr can_proto::Fase ORDEM_CICLO[3] = {
    can_proto::Fase::VERMELHO,
    can_proto::Fase::VERDE,
    can_proto::Fase::AMARELO,
};

const char* nome_fase(can_proto::Fase fase)
{
    switch (fase) {
        case can_proto::Fase::VERMELHO:  return "VERMELHO";
        case can_proto::Fase::AMARELO:   return "AMARELO";
        case can_proto::Fase::VERDE:     return "VERDE";
        case can_proto::Fase::FAIL_SAFE: return "FAIL_SAFE";
        default:                         return "?";
    }
}

int indice_na_ordem(can_proto::Fase fase)
{
    for (int i = 0; i < 3; i++) {
        if (ORDEM_CICLO[i] == fase) return i;
    }
    return 0; // fallback: VERMELHO
}

uint32_t agora_ms()
{
    return xTaskGetTickCount() * portTICK_PERIOD_MS;
}

// Ha quantos ms passou de "t" (seguro contra t um pouco no futuro, que
// acontece quando outra task grava o instante depois de "agora" ser lido).
int32_t decorrido_ms(uint32_t agora, uint32_t t)
{
    return (int32_t)(agora - t);
}

uint32_t tempo_ms_da_fase(const TemposSemaforo& t, can_proto::Fase fase)
{
    switch (fase) {
        case can_proto::Fase::VERDE:    return t.verde_ms;
        case can_proto::Fase::AMARELO:  return t.amarelo_ms;
        case can_proto::Fase::VERMELHO: return t.vermelho_ms;
        default: return 0;
    }
}

// Dado "ha quanto tempo (ms) o ciclo oficial comecou" (pos_no_ciclo, ja
// dentro de [0, duracao_total_do_ciclo_deste_no)), descobre em que fase
// ESTE no deveria estar e ha quanto tempo nela -- andando pela mesma
// ORDEM_CICLO, mas comecando em fase_inicial (cada no entra no ciclo por
// um ponto diferente, so isso que os diferencia).
void calcular_fase_canonica(can_proto::Fase fase_inicial, const TemposSemaforo& tempos,
                             uint32_t pos_no_ciclo, can_proto::Fase& fase_out, uint32_t& tempo_out)
{
    int indice = indice_na_ordem(fase_inicial);
    uint32_t acumulado = 0;
    for (int i = 0; i < 3; i++) {
        can_proto::Fase fase = ORDEM_CICLO[(indice + i) % 3];
        uint32_t duracao = tempo_ms_da_fase(tempos, fase);
        if (pos_no_ciclo < acumulado + duracao) {
            fase_out = fase;
            tempo_out = pos_no_ciclo - acumulado;
            return;
        }
        acumulado += duracao;
    }
    // So cai aqui se duracao_total == 0 (config invalida). Fallback seguro.
    fase_out = fase_inicial;
    tempo_out = 0;
}

} // namespace

Semaforo::Semaforo(uint8_t node_id, const PinosSemaforo& pinos,
                    const TemposSemaforo& tempos, can_proto::Fase fase_inicial)
    : node_id_(node_id), pinos_(pinos), tempos_(tempos),
      fase_inicial_(fase_inicial), fase_atual_(fase_inicial),
      inicio_fase_tick_(0)
{
    mutex_tempos_       = xSemaphoreCreateMutex();
    sinal_envio_status_ = xSemaphoreCreateBinary();
}

void Semaforo::configurarGpios()
{
    gpio_config_t out_cfg = {};
    out_cfg.mode = GPIO_MODE_OUTPUT;
    out_cfg.pin_bit_mask = (1ULL << pinos_.out_verde) | (1ULL << pinos_.out_amarelo) |
                           (1ULL << pinos_.out_vermelho);
    gpio_config(&out_cfg);

    // Leitura digital (nivel alto = sinal aceso). GPIO46 do ESP32-S3 nao
    // tem ADC, entao nao ha o corte analogico de 100 mV do firmware antigo.
    gpio_config_t in_cfg = {};
    in_cfg.mode = GPIO_MODE_INPUT;
    in_cfg.pull_down_en = GPIO_PULLDOWN_ENABLE;
    in_cfg.pin_bit_mask = (1ULL << pinos_.in_verde) | (1ULL << pinos_.in_amarelo) |
                          (1ULL << pinos_.in_vermelho);
    gpio_config(&in_cfg);
}

void Semaforo::apagarTudo()
{
    gpio_set_level(pinos_.out_verde,    0);
    gpio_set_level(pinos_.out_amarelo,  0);
    gpio_set_level(pinos_.out_vermelho, 0);
}

void Semaforo::iniciar()
{
    configurarGpios();

    aplicarFase(fase_atual_.load());
    inicio_fase_tick_ = xTaskGetTickCount() * portTICK_PERIOD_MS;
    boot_ms_ = agora_ms();

    xTaskCreatePinnedToCore(taskRotinaTrampolim, "sem_rotina", 4096, this, 2, &task_rotina_handle_, 1);
    xTaskCreatePinnedToCore(taskStatusTrampolim, "sem_status", 4096, this, 1, nullptr, 1);
    xTaskCreatePinnedToCore(taskEscutaTrampolim, "sem_escuta", 4096, this, 2, nullptr, 0);

    timer_status_ = xTimerCreate("timer_status", pdMS_TO_TICKS(PERIODO_STATUS_MS), pdTRUE, this, timerStatusCallback);
    xTimerStart(timer_status_, 0);

    ESP_LOGI(TAG, "node_id=%d pronto, fase inicial=%d", node_id_, (int)fase_inicial_);
}

void Semaforo::definirTempos(const TemposSemaforo& novos)
{
    if (xSemaphoreTake(mutex_tempos_, portMAX_DELAY) == pdTRUE) {
        tempos_ = novos;
        xSemaphoreGive(mutex_tempos_);
        ESP_LOGI(TAG, "Novos tempos aplicados (verde=%lu amarelo=%lu vermelho=%lu)",
                 (unsigned long)novos.verde_ms, (unsigned long)novos.amarelo_ms,
                 (unsigned long)novos.vermelho_ms);
    }
}

void Semaforo::aplicarFase(can_proto::Fase fase)
{
    bool v = (fase == can_proto::Fase::VERDE);
    bool a = (fase == can_proto::Fase::AMARELO) || (fase == can_proto::Fase::FAIL_SAFE);
    bool r = (fase == can_proto::Fase::VERMELHO);
    gpio_set_level(pinos_.out_verde,    v ? 1 : 0);
    gpio_set_level(pinos_.out_amarelo,  a ? 1 : 0);
    gpio_set_level(pinos_.out_vermelho, r ? 1 : 0);
}

bool Semaforo::verificarCoerencia(can_proto::Fase fase, uint8_t& flags_mismatch)
{
    bool v_lido = gpio_get_level(pinos_.in_verde);
    bool a_lido = gpio_get_level(pinos_.in_amarelo);
    bool r_lido = gpio_get_level(pinos_.in_vermelho);

    bool v_esperado = (fase == can_proto::Fase::VERDE);
    bool a_esperado = (fase == can_proto::Fase::AMARELO) || (fase == can_proto::Fase::FAIL_SAFE);
    bool r_esperado = (fase == can_proto::Fase::VERMELHO);

    flags_mismatch = 0;
    if (v_lido != v_esperado) flags_mismatch |= 0x01;
    if (a_lido != a_esperado) flags_mismatch |= 0x02;
    if (r_lido != r_esperado) flags_mismatch |= 0x04;

    // DEBUG: log cru dos 3 pinos em toda checagem. Tira isso (ou troca pra
    // ESP_LOGD) depois que a fiacao estiver validada -- fica MUITO verboso
    // em operacao normal (uma linha a cada CHECK_INTERVAL_MS).
    // ESP_LOGI(TAG, "[GPIO] node_id=%d fase_checada=%s | verde(pino%d)=%d esperado=%d | "
    //               "amarelo(pino%d)=%d esperado=%d | vermelho(pino%d)=%d esperado=%d",
    //          node_id_, nome_fase(fase),
    //          (int)pinos_.in_verde,    v_lido ? 1 : 0, v_esperado ? 1 : 0,
    //          (int)pinos_.in_amarelo,  a_lido ? 1 : 0, a_esperado ? 1 : 0,
    //          (int)pinos_.in_vermelho, r_lido ? 1 : 0, r_esperado ? 1 : 0);

    return flags_mismatch == 0;
}

void Semaforo::onMensagemCAN(const twai_message_t& /*msg*/)
{
    // Base nao trata frames de outros nos por enquanto. Subclasses ou a
    // controladora futura sobrescrevem isso.
}

void Semaforo::entrarFailSafe(MotivoFailSafe motivo)
{
    can_proto::Fase anterior = fase_atual_.exchange(can_proto::Fase::FAIL_SAFE);
    if (anterior == can_proto::Fase::FAIL_SAFE) return; // evita floodar o barramento

    motivo_failsafe_.store(motivo);
    ESP_LOGW(TAG, "node_id=%d ENTRANDO EM FAIL-SAFE (motivo=%d)", node_id_, (int)motivo);

    if (motivo != MotivoFailSafe::REMOTO) {
        uint8_t dados[1] = { node_id_ };
        can_bus::enviar(can_proto::ID_FAILSAFE, dados, 1);
    }

    if (task_rotina_handle_) xTaskNotifyGive(task_rotina_handle_);
}

void Semaforo::restaurarOperacao()
{
    if (fase_atual_.load() != can_proto::Fase::FAIL_SAFE) return;

    ESP_LOGI(TAG, "node_id=%d restaurando rotina normal (fase inicial)", node_id_);
    uint32_t local_ms = agora_ms();
    // Ate chegar um SYNC de verdade, assume que "oficial = 0" e' agora
    // (equivalente a um reset local). ajuste_ms_ = -local_ms faz
    // relogioOficialMs() valer 0 neste instante.
    ajuste_ms_.store(-(int32_t)local_ms);

    // Da um tempo de graca aos vigias: senao o silencio que causou o
    // fail-safe re-dispararia na hora, antes do primeiro SYNC/status novo.
    ultimo_sync_ms_.store(local_ms);
    uint16_t vistos = peers_vistos_.load();
    for (int i = 0; i < 16; i++) {
        if (vistos & (1u << i)) ultimo_status_ms_[i].store(local_ms);
    }

    motivo_failsafe_.store(MotivoFailSafe::NENHUM);
    inicio_fase_tick_.store(local_ms);
    fase_atual_.store(fase_inicial_);
    if (task_rotina_handle_) xTaskNotifyGive(task_rotina_handle_);
}

uint16_t Semaforo::paresSilenciosos(uint32_t agora) const
{
    uint16_t vistos = peers_vistos_.load();
    uint16_t mudos = 0;
    for (int i = 0; i < 16; i++) {
        if (i == node_id_) continue;
        uint16_t bit = (uint16_t)(1u << i);
        if (vistos & bit) {
            if (decorrido_ms(agora, ultimo_status_ms_[i].load()) > (int32_t)PEER_TIMEOUT_MS) mudos |= bit;
        } else if ((nos_esperados_ & bit) &&
                   decorrido_ms(agora, boot_ms_) > (int32_t)BOOT_GRACE_MS) {
            mudos |= bit; // esperado e nunca apareceu
        }
    }
    return mudos;
}

bool Semaforo::condicaoDeSaida(bool fiacao_ok)
{
    uint32_t agora = agora_ms();
    switch (motivo_failsafe_.load()) {
        case MotivoFailSafe::REMOTO:
            return false; // so o ID_RESET de quem causou tira daqui
        case MotivoFailSafe::SYNC_PERDIDO:
            return fiacao_ok &&
                   decorrido_ms(agora, ultimo_sync_ms_.load()) < (int32_t)(SYNC_TIMEOUT_MS / 2);
        case MotivoFailSafe::PAR_SILENCIOSO:
            return fiacao_ok && paresSilenciosos(agora) == 0;
        case MotivoFailSafe::LOCAL:
        case MotivoFailSafe::NENHUM:
        default:
            return fiacao_ok;
    }
}

uint32_t Semaforo::relogioOficialMs() const
{
    int64_t local_ms = (int64_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    return (uint32_t)(local_ms + (int64_t)ajuste_ms_.load());
}

void Semaforo::aplicarSync(uint32_t epoch_ms)
{
    uint32_t local_ms = agora_ms();
    // Sempre registra que a master esta viva (inclusive em fail-safe: e' o
    // que deixa um no SYNC_PERDIDO sair quando os SYNCs voltam).
    ultimo_sync_ms_.store(local_ms);
    sync_visto_.store(true);

    // Em fail-safe nao mexe no relogio nem acorda a rotina (acordar
    // zeraria a contagem de leituras boas da recuperacao).
    if (fase_atual_.load() == can_proto::Fase::FAIL_SAFE) return;

    int32_t ajuste_anterior = ajuste_ms_.load();
    int32_t novo_ajuste = (int32_t)epoch_ms - (int32_t)local_ms;
    ajuste_ms_.store(novo_ajuste);

    // Log so informativo (pra acompanhar no monitor serial): nao muda
    // nada de estado, so mostra pra onde o ajuste foi e o que isso
    // significa em fase/tempo oficial agora.
    TemposSemaforo copia;
    if (xSemaphoreTake(mutex_tempos_, portMAX_DELAY) == pdTRUE) {
        copia = tempos_;
        xSemaphoreGive(mutex_tempos_);
    } else {
        copia = tempos_;
    }
    uint32_t duracao_total = copia.verde_ms + copia.amarelo_ms + copia.vermelho_ms;
    can_proto::Fase fase_oficial = fase_inicial_;
    uint32_t tempo_oficial = 0;
    if (duracao_total > 0) {
        calcular_fase_canonica(fase_inicial_, copia, epoch_ms % duracao_total, fase_oficial, tempo_oficial);
    }

    ESP_LOGI(TAG, "node_id=%d SYNC recebido: epoch_ms=%lu (oficial agora=%s %lums) | ajuste %ldms -> %ldms (mudou %ldms)",
             node_id_, (unsigned long)epoch_ms, nome_fase(fase_oficial), (unsigned long)tempo_oficial,
             (long)ajuste_anterior, (long)novo_ajuste, (long)(novo_ajuste - ajuste_anterior));

    if (task_rotina_handle_) xTaskNotifyGive(task_rotina_handle_); // acorda a rotina pra reavaliar logo, sem esperar o CHECK_INTERVAL_MS
}

// ---------------------------------------------------------------------
// Task: rotina (ciclo de fases + monitoramento de coerencia)
// ---------------------------------------------------------------------

void Semaforo::taskRotinaTrampolim(void* self) { static_cast<Semaforo*>(self)->taskRotina(); }

void Semaforo::taskRotina()
{
    int mismatches_seguidos = 0;
    // Depois de sair do fail-safe as saidas ainda estao no estado da
    // piscada, mesmo com fase_atual_ == fase calculada: reaplica a fase uma
    // vez antes de conferir a fiacao (senao da mismatch falso).
    bool forcar_aplicar = false;

    for (;;) {
        // --- Fail-safe: pisca amarelo; sai conforme o MOTIVO ---
        // A leitura da fiacao e' feita com o amarelo ACESO (o padrao
        // esperado em fail-safe e "so amarelo"). Cada leitura boa conta;
        // RECOVERY_CONFIRMACOES seguidas + a condicao do motivo = sai.
        // Quem nao e' REMOTO re-anuncia o fail-safe a cada piscada, pra
        // quem perdeu o frame original (ou ligou depois) entrar tambem.
        if (fase_atual_.load() == can_proto::Fase::FAIL_SAFE) {
            mismatches_seguidos = 0;
            int leituras_boas_seguidas = 0;
            while (fase_atual_.load() == can_proto::Fase::FAIL_SAFE) {
                aplicarFase(can_proto::Fase::FAIL_SAFE);
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(BLINK_FAILSAFE_MS));
                if (fase_atual_.load() != can_proto::Fase::FAIL_SAFE) break;

                uint8_t flags = 0;
                bool fiacao_ok = verificarCoerencia(can_proto::Fase::FAIL_SAFE, flags);

                apagarTudo();
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(BLINK_FAILSAFE_MS));
                if (fase_atual_.load() != can_proto::Fase::FAIL_SAFE) break;

                MotivoFailSafe motivo = motivo_failsafe_.load();
                if (motivo != MotivoFailSafe::REMOTO) {
                    uint8_t dados[1] = { node_id_ };
                    can_bus::enviar(can_proto::ID_FAILSAFE, dados, 1);
                }

                if (condicaoDeSaida(fiacao_ok)) {
                    leituras_boas_seguidas++;
                    ESP_LOGI(TAG, "node_id=%d condicao de saida OK (motivo=%d, %d/%d)",
                             node_id_, (int)motivo, leituras_boas_seguidas, RECOVERY_CONFIRMACOES);
                    if (leituras_boas_seguidas >= RECOVERY_CONFIRMACOES) {
                        ESP_LOGI(TAG, "node_id=%d AUTO-RECUPERANDO (motivo=%d)", node_id_, (int)motivo);
                        // Avisa os que obedeceram (REMOTO) que podem voltar.
                        uint8_t dados[1] = { node_id_ };
                        can_bus::enviar(can_proto::ID_RESET, dados, 1);
                        restaurarOperacao();
                        break;
                    }
                } else {
                    leituras_boas_seguidas = 0;
                }
            }
            forcar_aplicar = true;
            continue;
        }

        // --- Vigias: master e pares ---
        uint32_t agora = agora_ms();
        if (sync_visto_.load() &&
            decorrido_ms(agora, ultimo_sync_ms_.load()) > (int32_t)SYNC_TIMEOUT_MS) {
            ESP_LOGE(TAG, "node_id=%d sem SYNC ha mais de %lums - fail-safe", node_id_,
                     (unsigned long)SYNC_TIMEOUT_MS);
            entrarFailSafe(MotivoFailSafe::SYNC_PERDIDO);
            continue;
        }
        uint16_t mudos = paresSilenciosos(agora);
        if (mudos != 0) {
            ESP_LOGE(TAG, "node_id=%d par(es) silencioso(s) mascara=0x%04X - fail-safe", node_id_, mudos);
            entrarFailSafe(MotivoFailSafe::PAR_SILENCIOSO);
            continue;
        }

        // --- Operacao normal: a fase e' so uma funcao do relogio oficial ---
        // Nao tem "proxima fase" guardada em variavel nenhuma. A cada
        // volta, pergunta pro relogio (local + ajuste_ms_, atualizado
        // pelos SYNCs) onde o ciclo deveria estar AGORA, e so reage se
        // isso mudou desde a ultima volta.
        TemposSemaforo copia;
        if (xSemaphoreTake(mutex_tempos_, portMAX_DELAY) == pdTRUE) {
            copia = tempos_;
            xSemaphoreGive(mutex_tempos_);
        } else {
            copia = tempos_;
        }
        uint32_t duracao_total = copia.verde_ms + copia.amarelo_ms + copia.vermelho_ms;

        if (duracao_total > 0) {
            can_proto::Fase fase;
            uint32_t tempo_na_fase;
            calcular_fase_canonica(fase_inicial_, copia, relogioOficialMs() % duracao_total, fase, tempo_na_fase);

            can_proto::Fase atual = fase_atual_.load();
            if (atual == can_proto::Fase::FAIL_SAFE) continue; // outra task acabou de derrubar
            if (fase != atual) {
                // compare_exchange: se o fail-safe entrou entre o load e aqui,
                // NAO sobrescreve com a fase do ciclo.
                if (!fase_atual_.compare_exchange_strong(atual, fase)) continue;
                inicio_fase_tick_.store(agora_ms() - tempo_na_fase);
                aplicarFase(fase);
                ESP_LOGI(TAG, "[FASE] node_id=%d -> %s (tempo_na_fase=%lums, duracao=%lums)",
                         node_id_, nome_fase(fase), (unsigned long)tempo_na_fase,
                         (unsigned long)tempo_ms_da_fase(copia, fase));
            }

            if (forcar_aplicar) {
                aplicarFase(fase);
                forcar_aplicar = false;
            }

            uint8_t flags = 0;
            if (!verificarCoerencia(fase, flags)) {
                mismatches_seguidos++;
                ESP_LOGW(TAG, "node_id=%d MISMATCH na fase %d (flags=0x%02X) %d/%d",
                         node_id_, (int)fase, flags, mismatches_seguidos, MISMATCH_CONFIRMACOES);
                if (mismatches_seguidos >= MISMATCH_CONFIRMACOES) {
                    ESP_LOGE(TAG, "node_id=%d mismatch confirmado - entrando em fail-safe", node_id_);
                    entrarFailSafe(MotivoFailSafe::LOCAL);
                    continue;
                }
            } else {
                mismatches_seguidos = 0;
            }
        }

        // Espera um pedacinho, mas acorda na hora se um SYNC ou fail-safe
        // precisar de reacao imediata.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(CHECK_INTERVAL_MS));
    }
}

// ---------------------------------------------------------------------
// Task: status (heartbeat periodico via CAN)
// ---------------------------------------------------------------------

void Semaforo::timerStatusCallback(TimerHandle_t xTimer)
{
    Semaforo* self = static_cast<Semaforo*>(pvTimerGetTimerID(xTimer));
    xSemaphoreGive(self->sinal_envio_status_);
}

void Semaforo::taskStatusTrampolim(void* self) { static_cast<Semaforo*>(self)->taskStatus(); }

void Semaforo::taskStatus()
{
    for (;;) {
        if (xSemaphoreTake(sinal_envio_status_, portMAX_DELAY) == pdTRUE) {
            can_proto::Fase fase = fase_atual_.load();
            uint32_t agora = xTaskGetTickCount() * portTICK_PERIOD_MS;
            uint32_t tempo_na_fase = agora - inicio_fase_tick_.load();

            uint8_t flags = 0;
            verificarCoerencia(fase, flags); // so reporta; quem decide fail-safe e a rotina

            can_proto::StatusMsg msg{};
            msg.node_id          = node_id_;
            msg.fase             = (uint8_t)fase;
            msg.tempo_na_fase_ms = tempo_na_fase;
            msg.flags_mismatch   = flags;
            msg.reserved         = 0;

            esp_err_t err = can_bus::enviar(can_proto::status_id(node_id_),
                    reinterpret_cast<uint8_t*>(&msg), sizeof msg);
            ESP_LOGI(TAG, "[TX] status node_id=%d fase=%s t_na_fase=%lu ms mismatch=0x%02X (%s)",
                    node_id_, nome_fase(fase), (unsigned long)tempo_na_fase,
                    flags, esp_err_to_name(err));
        }
    }
}

// ---------------------------------------------------------------------
// Task: escuta (recebe frames de todo o barramento)
// ---------------------------------------------------------------------

void Semaforo::taskEscutaTrampolim(void* self) { static_cast<Semaforo*>(self)->taskEscuta(); }

void Semaforo::taskEscuta()
{
    for (;;) {
        twai_message_t msg;
        if (can_bus::receber(&msg, 1000)) {
            if (msg.identifier == can_proto::ID_FAILSAFE) {
                uint8_t origem = msg.data_length_code >= 1 ? msg.data[0] : 0xFF;
                ESP_LOGW(TAG, "node_id=%d recebeu FAIL-SAFE do node_id=%d pelo barramento",
                         node_id_, origem);
                entrarFailSafe(MotivoFailSafe::REMOTO);
            } else if (msg.identifier == can_proto::ID_RESET) {
                // RESET de outro no: so solta quem obedeceu um fail-safe alheio
                // (REMOTO). Quem esta em fail-safe por motivo proprio sai pela
                // propria condicao -- senao mascararia o defeito.
                // RESET da master (origem 0xFF = comando do operador): forca a
                // saida de qualquer motivo. Se o defeito persistir, o mismatch
                // confirmado traz o no de volta ao fail-safe em ~0,4 s.
                uint8_t origem = msg.data_length_code >= 1 ? msg.data[0] : 0xFF;
                if (origem == 0xFF || motivo_failsafe_.load() == MotivoFailSafe::REMOTO) restaurarOperacao();
            } else if (msg.identifier == can_proto::ID_SYNC &&
                       msg.data_length_code >= sizeof(can_proto::SyncMsg)) {
                can_proto::SyncMsg sync;
                memcpy(&sync, msg.data, sizeof sync);
                aplicarSync(sync.epoch_ms);
            } else {
                bool eh_status = msg.identifier >= can_proto::ID_STATUS_BASE &&
                                msg.identifier <  can_proto::ID_STATUS_BASE + 16 &&
                                msg.data_length_code == sizeof(can_proto::StatusMsg);
                if (eh_status) {
                    can_proto::StatusMsg st;
                    memcpy(&st, msg.data, sizeof st);
                    if (msg.identifier != can_proto::status_id(node_id_)) {
                        uint8_t id = (uint8_t)(msg.identifier - can_proto::ID_STATUS_BASE);
                        ultimo_status_ms_[id].store(agora_ms());
                        peers_vistos_.fetch_or((uint16_t)(1u << id));
                    }
                    ESP_LOGI(TAG, "[RX] status de node_id=%d: fase=%s t_na_fase=%lu ms mismatch=0x%02X",
                            st.node_id, nome_fase((can_proto::Fase)st.fase),
                            (unsigned long)st.tempo_na_fase_ms, st.flags_mismatch);
                }
                onMensagemCAN(msg);
            }
        }
        can_bus::tratar_alertas(TAG);
    }
}
