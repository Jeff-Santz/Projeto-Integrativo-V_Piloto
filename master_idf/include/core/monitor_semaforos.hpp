#pragma once
// =====================================================================
// MonitorSemaforos: mantem o estado mais recente de cada no visto no
// barramento e detecta nos "silenciosos" -- que pararam de mandar status
// sem avisar fail-safe (crash, perda de alimentacao, controlador CAN
// travado). Isso e algo que nenhum no sozinho consegue detectar sobre
// si mesmo; so quem esta ouvindo todo mundo (a master) pode perceber o
// silencio e agir.
//
// Mirror conceitual do watchdog de "cabo desconectado" do firmware TCP
// antigo (Ethernet.linkStatus()==LinkOFF), so que adaptado pra CAN: aqui
// nao tem link fisico pra checar, entao o sinal de vida e o heartbeat de
// status de 1s que cada no manda.
// =====================================================================
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <vector>

#include "protocolo/protocolo.hpp"

namespace core {

struct EstadoNo {
    can_proto::Fase fase = can_proto::Fase::FAIL_SAFE;
    uint32_t tempo_na_fase_ms = 0;
    uint8_t  flags_mismatch   = 0;
    std::chrono::steady_clock::time_point ultimo_status;
    bool     silencioso_reportado = false; // evita logar/broadcastar repetido
};

class MonitorSemaforos {
public:
    void atualizarStatus(const can_proto::StatusMsg& msg);

    // Retorna os node_ids que nao mandam status ha mais de timeout_ms e
    // ainda nao foram reportados como silenciosos nesta ausencia.
    std::vector<uint8_t> verificarSilenciosos(uint32_t timeout_ms);

    // Chame quando o no voltar a mandar status (atualizarStatus ja faz
    // isso internamente) -- exposto separado so pra clareza de teste.
    void limparSilencioso(uint8_t node_id);

    void imprimirSnapshot() const;

private:
    mutable std::mutex mutex_;
    std::map<uint8_t, EstadoNo> nos_;
};

} // namespace core
