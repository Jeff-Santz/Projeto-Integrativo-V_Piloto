#pragma once
#include <cstdint>

// Protocolo CAN do projeto. Ids de 11 bits. Em CAN, ID menor = prioridade
// maior na arbitragem, por isso o fail-safe fica no menor ID possível.
namespace can_proto {

constexpr uint32_t ID_FAILSAFE    = 0x000; // broadcast, qualquer nó pode mandar
constexpr uint32_t ID_RESET       = 0x001; // broadcast, reservado pra controladora
constexpr uint32_t ID_STATUS_BASE = 0x010; // + node_id -> 0x010..0x01F (nodes 0-15)

enum class Fase : uint8_t {
    VERMELHO  = 0,
    AMARELO   = 1,
    VERDE     = 2,
    FAIL_SAFE = 3,
};

#pragma pack(push, 1)
struct StatusMsg {
    uint8_t  node_id;
    uint8_t  fase;             // valores de Fase
    uint32_t tempo_na_fase_ms; // há quanto tempo está nessa fase
    uint8_t  flags_mismatch;   // bit0=verde bit1=amarelo bit2=vermelho (saida != leitura)
    uint8_t  reserved;
};
#pragma pack(pop)
static_assert(sizeof(StatusMsg) == 8, "StatusMsg precisa caber num frame CAN classico (8 bytes)");

inline uint32_t status_id(uint8_t node_id) { return ID_STATUS_BASE + node_id; }

} // namespace can_proto
