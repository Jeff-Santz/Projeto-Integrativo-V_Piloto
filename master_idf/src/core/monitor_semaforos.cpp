#include "core/monitor_semaforos.hpp"

#include <sstream>

#include "core/log.hpp"

namespace core {

namespace { constexpr const char* TAG = "MONITOR"; }

void MonitorSemaforos::atualizarStatus(const can_proto::StatusMsg& msg)
{
    std::lock_guard<std::mutex> lock(mutex_);
    EstadoNo& e = nos_[msg.node_id];
    e.fase               = static_cast<can_proto::Fase>(msg.fase);
    e.tempo_na_fase_ms    = msg.tempo_na_fase_ms;
    e.flags_mismatch      = msg.flags_mismatch;
    e.ultimo_status       = std::chrono::steady_clock::now();
    e.silencioso_reportado = false; // voltou a falar, limpa a flag
}

void MonitorSemaforos::limparSilencioso(uint8_t node_id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = nos_.find(node_id);
    if (it != nos_.end()) it->second.silencioso_reportado = false;
}

std::vector<uint8_t> MonitorSemaforos::verificarSilenciosos(uint32_t timeout_ms)
{
    std::vector<uint8_t> resultado;
    auto agora = std::chrono::steady_clock::now();

    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [node_id, estado] : nos_) {
        auto decorrido_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                agora - estado.ultimo_status).count();
        if (decorrido_ms > (int64_t)timeout_ms && !estado.silencioso_reportado) {
            estado.silencioso_reportado = true;
            resultado.push_back(node_id);
        }
    }
    return resultado;
}

void MonitorSemaforos::imprimirSnapshot() const
{
    auto agora = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(mutex_);

    if (nos_.empty()) {
        log_::info(TAG, "Nenhum no visto ainda no barramento.");
        return;
    }

    std::ostringstream oss;
    oss << "\n  node_id | fase      | t_na_fase | mismatch | ha_quanto_tempo_sem_status\n";
    oss <<   "  --------|-----------|-----------|----------|---------------------------\n";
    for (const auto& [node_id, e] : nos_) {
        auto ha_ms = std::chrono::duration_cast<std::chrono::milliseconds>(agora - e.ultimo_status).count();
        char buf[160];
        snprintf(buf, sizeof buf, "  %-7d | %-9s | %7lu ms | 0x%02X     | %ld ms\n",
                 node_id, can_proto::nome_fase(e.fase), (unsigned long)e.tempo_na_fase_ms,
                 e.flags_mismatch, (long)ha_ms);
        oss << buf;
    }
    log_::info(TAG, oss.str());
}

} // namespace core
