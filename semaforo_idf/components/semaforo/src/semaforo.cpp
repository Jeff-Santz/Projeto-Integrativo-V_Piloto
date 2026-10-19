#include "semaforo.hpp"

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

uint32_t tempo_ms_da_fase(const TemposSemaforo& t, can_proto::Fase fase)
{
    switch (fase) {
        case can_proto::Fase::VERDE:    return t.verde_ms;
        case can_proto::Fase::AMARELO:  return t.amarelo_ms;
        case can_proto::Fase::VERMELHO: return t.vermelho_ms;
        default: return 0;
    }
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

void Semaforo::iniciar()
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

    aplicarFase(fase_atual_.load());
    inicio_fase_tick_ = xTaskGetTickCount() * portTICK_PERIOD_MS;

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
    bool a_esperado = (fase == can_proto::Fase::AMARELO);
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

void Semaforo::entrarFailSafe(bool propagar)
{
    can_proto::Fase anterior = fase_atual_.exchange(can_proto::Fase::FAIL_SAFE);
    if (anterior == can_proto::Fase::FAIL_SAFE) return; // evita floodar o barramento

    ESP_LOGW(TAG, "node_id=%d ENTRANDO EM FAIL-SAFE (propagar=%d)", node_id_, propagar);

    if (propagar) {
        uint8_t dados[1] = { node_id_ };
        can_bus::enviar(can_proto::ID_FAILSAFE, dados, 1);
    }

    if (task_rotina_handle_) xTaskNotifyGive(task_rotina_handle_);
}

void Semaforo::restaurarOperacao()
{
    if (fase_atual_.load() != can_proto::Fase::FAIL_SAFE) return;

    ESP_LOGI(TAG, "node_id=%d restaurando rotina normal (fase inicial)", node_id_);
    fase_atual_.store(fase_inicial_);
    if (task_rotina_handle_) xTaskNotifyGive(task_rotina_handle_);
}

// ---------------------------------------------------------------------
// Task: rotina (ciclo de fases + monitoramento de coerencia)
// ---------------------------------------------------------------------

bool Semaforo::esperarFase(can_proto::Fase fase, uint32_t total_ms)
{
    uint32_t decorrido = 0;
    while (decorrido < total_ms) {
        uint32_t pedaco = (total_ms - decorrido < CHECK_INTERVAL_MS) ? (total_ms - decorrido) : CHECK_INTERVAL_MS;

        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(pedaco)) > 0) {
            return true; // interrompida por entrarFailSafe()/restaurarOperacao()
        }
        decorrido += pedaco;

        if (fase_atual_.load() == can_proto::Fase::FAIL_SAFE) return true;

        uint8_t flags = 0;
        if (!verificarCoerencia(fase, flags)) {
            ESP_LOGE(TAG, "node_id=%d MISMATCH na fase %d (flags=0x%02X) - entrando em fail-safe",
                     node_id_, (int)fase, flags);
            entrarFailSafe(true);
            return true;
        }
    }
    return false;
}

void Semaforo::taskRotinaTrampolim(void* self) { static_cast<Semaforo*>(self)->taskRotina(); }

void Semaforo::taskRotina()
{
    int indice = indice_na_ordem(fase_inicial_);

    for (;;) {
        // --- Fail-safe: pisca amarelo e tenta se auto-recuperar ---
        // A cada ciclo de piscada, confere se a fiacao voltou a bater com a
        // fase_inicial_. Depois de RECOVERY_CONFIRMACOES leituras boas
        // seguidas (nao uma unica, pra nao sair por um pulso de ruido), sai
        // do fail-safe sozinho e avisa o barramento com ID_RESET -- assim
        // nao depende de reset manual nem de uma controladora existir ainda.
        if (fase_atual_.load() == can_proto::Fase::FAIL_SAFE) {
            int leituras_boas_seguidas = 0;
            while (fase_atual_.load() == can_proto::Fase::FAIL_SAFE) {
                aplicarFase(can_proto::Fase::FAIL_SAFE);
                if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(BLINK_FAILSAFE_MS)) > 0) break;
                gpio_set_level(pinos_.out_amarelo, 0);
                if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(BLINK_FAILSAFE_MS)) > 0) break;

                uint8_t flags = 0;
                if (verificarCoerencia(fase_inicial_, flags)) {
                    leituras_boas_seguidas++;
                    ESP_LOGI(TAG, "node_id=%d fiacao OK em fail-safe (%d/%d pra auto-recuperar)",
                             node_id_, leituras_boas_seguidas, RECOVERY_CONFIRMACOES);
                    if (leituras_boas_seguidas >= RECOVERY_CONFIRMACOES) {
                        ESP_LOGI(TAG, "node_id=%d AUTO-RECUPERANDO (fiacao estavel)", node_id_);
                        uint8_t dados[1] = { node_id_ };
                        can_bus::enviar(can_proto::ID_RESET, dados, 1);
                        restaurarOperacao();
                        break;
                    }
                } else {
                    leituras_boas_seguidas = 0;
                }
            }
            indice = indice_na_ordem(fase_inicial_);
            continue;
        }

        can_proto::Fase fase = ORDEM_CICLO[indice];
        fase_atual_.store(fase);
        inicio_fase_tick_ = xTaskGetTickCount() * portTICK_PERIOD_MS;
        aplicarFase(fase);

        uint32_t tempo_ms;
        if (xSemaphoreTake(mutex_tempos_, portMAX_DELAY) == pdTRUE) {
            tempo_ms = tempo_ms_da_fase(tempos_, fase);
            xSemaphoreGive(mutex_tempos_);
        } else {
            tempo_ms = tempo_ms_da_fase(tempos_, fase);
        }

        ESP_LOGI(TAG, "[FASE] node_id=%d -> %s (%lu ms)", node_id_, nome_fase(fase), (unsigned long)tempo_ms);

        esperarFase(fase, tempo_ms); // se interrompida por fail-safe, o topo do loop trata

        if (fase_atual_.load() != can_proto::Fase::FAIL_SAFE) {
            indice = (indice + 1) % 3;
        }
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
                entrarFailSafe(false);
            } else if (msg.identifier == can_proto::ID_RESET) {
                restaurarOperacao();
            } else {
                bool eh_status = msg.identifier >= can_proto::ID_STATUS_BASE &&
                                msg.identifier <  can_proto::ID_STATUS_BASE + 16 &&
                                msg.data_length_code == sizeof(can_proto::StatusMsg);
                if (eh_status) {
                    can_proto::StatusMsg st;
                    memcpy(&st, msg.data, sizeof st);
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
