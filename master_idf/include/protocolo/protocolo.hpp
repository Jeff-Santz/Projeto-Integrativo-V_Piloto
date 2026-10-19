#pragma once
// =====================================================================
// Mesmo protocolo CAN do firmware (Semaforo/components/protocolo/include/
// protocolo.hpp). MANTENHA OS DOIS EM SINCRONIA -- se mudar um lado, muda
// o outro. Nao criei um jeito automatico de compartilhar o header entre
// o projeto ESP-IDF e esse aqui pra nao complicar o build; num projeto
// maior isso viraria uma lib compartilhada.
// =====================================================================
#include <cstdint>
#include <cstring>

namespace can_proto {

constexpr uint32_t ID_FAILSAFE    = 0x000;
constexpr uint32_t ID_RESET       = 0x001;
constexpr uint32_t ID_STATUS_BASE = 0x010; // + node_id -> 0x010..0x01F

enum class Fase : uint8_t {
    VERMELHO  = 0,
    AMARELO   = 1,
    VERDE     = 2,
    FAIL_SAFE = 3,
};

#pragma pack(push, 1)
struct StatusMsg {
    uint8_t  node_id;
    uint8_t  fase;
    uint32_t tempo_na_fase_ms;
    uint8_t  flags_mismatch;
    uint8_t  reserved;
};
#pragma pack(pop)
static_assert(sizeof(StatusMsg) == 8, "StatusMsg precisa caber num frame CAN classico (8 bytes)");

inline uint32_t status_id(uint8_t node_id) { return ID_STATUS_BASE + node_id; }

inline const char* nome_fase(Fase fase)
{
    switch (fase) {
        case Fase::VERMELHO:  return "VERMELHO";
        case Fase::AMARELO:   return "AMARELO";
        case Fase::VERDE:     return "VERDE";
        case Fase::FAIL_SAFE: return "FAIL_SAFE";
        default:              return "?";
    }
}

} // namespace can_proto
