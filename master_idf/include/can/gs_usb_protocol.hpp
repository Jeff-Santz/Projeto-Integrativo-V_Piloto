#pragma once
// =====================================================================
// Protocolo USB do firmware "gs_usb" (candleLight e clones). Nao e
// invencao nossa -- e o protocolo vendor-specific que o driver gs_usb do
// Linux e o backend gs_usb do python-can usam. Aqui reimplementamos so a
// parte que precisamos, direto sobre libusb, porque nao existe driver
// SocketCAN nativo no Windows.
//
// Fonte de referencia: driver gs_usb do kernel Linux
// (drivers/net/can/usb/gs_usb.c) e firmware candleLight
// (https://github.com/candle-usb/candleLight_fw). Os nomes das
// constantes e estruturas abaixo seguem esses projetos de proposito,
// pra quem for conferir/depurar saber o que procurar.
// =====================================================================
#include <cstdint>

namespace gs_usb {

// VID/PID padrao do candleLight e da maioria dos clones gs_usb
// (inclusive o STM32F072 que voce tem -- confirme no painel do
// Windows/lsusb se o seu bater com isso; se nao bater, o
// CanBus::abrir() avisa no log).
constexpr uint16_t VID = 0x1d50;
constexpr uint16_t PID = 0x606f;

// --- bRequest das transferencias de controle (vendor, recipient=interface) ---
constexpr uint8_t BREQ_HOST_FORMAT   = 0;
constexpr uint8_t BREQ_BITTIMING     = 1;
constexpr uint8_t BREQ_MODE          = 2;
constexpr uint8_t BREQ_BERR          = 3;
constexpr uint8_t BREQ_BT_CONST      = 4;
constexpr uint8_t BREQ_DEVICE_CONFIG = 5;
constexpr uint8_t BREQ_TIMESTAMP     = 6;
constexpr uint8_t BREQ_IDENTIFY      = 7;

constexpr uint32_t MODE_RESET = 0;
constexpr uint32_t MODE_START = 1;

// Flags no campo can_id do gs_host_frame (mesma convencao do SocketCAN).
constexpr uint32_t CAN_EFF_FLAG = 0x80000000u; // ID estendido (29 bits)
constexpr uint32_t CAN_RTR_FLAG = 0x40000000u;
constexpr uint32_t CAN_ERR_FLAG = 0x20000000u;
constexpr uint32_t CAN_SFF_MASK = 0x000007FFu; // mascara do ID padrao (11 bits)

// Sentinela: o device usa echo_id=0xFFFFFFFF pra frames genuinos recebidos
// do barramento. Qualquer outro valor e eco de confirmacao de um TX nosso.
constexpr uint32_t ECHO_ID_RX = 0xFFFFFFFFu;

// Primeiro control transfer apos abrir: informa o byte-order do host.
// 0x0000beef = little-endian (o que todo PC x86/x64 usa).
#pragma pack(push, 1)
struct HostConfig {
    uint32_t byte_order;
};

struct DeviceBittiming {
    uint32_t prop_seg;
    uint32_t phase_seg1;
    uint32_t phase_seg2;
    uint32_t sjw;
    uint32_t brp;
};

struct DeviceMode {
    uint32_t mode;
    uint32_t flags;
};

// Resposta da BT_CONST: limites de timing que ESSE device aceita, e o
// clock interno dele (fclk_can). Perguntamos isso em vez de chutar um
// clock fixo, porque cada placa candleLight pode ter um cristal
// diferente -- chutar errado gera um bitrate levemente torto que nem
// sempre da erro na hora, só comunicacao instavel depois.
struct DeviceBtConst {
    uint32_t feature;
    uint32_t fclk_can;
    uint32_t tseg1_min;
    uint32_t tseg1_max;
    uint32_t tseg2_min;
    uint32_t tseg2_max;
    uint32_t sjw_max;
    uint32_t brp_min;
    uint32_t brp_max;
    uint32_t brp_inc;
};

// Frame trocado via bulk transfer (TX e RX usam o mesmo formato).
// 20 bytes fixos -- sem timestamp de hardware (nao pedimos esse feature).
struct HostFrame {
    uint32_t echo_id;
    uint32_t can_id;
    uint8_t  can_dlc;
    uint8_t  channel;
    uint8_t  flags;
    uint8_t  reserved;
    uint8_t  data[8];
};
#pragma pack(pop)
static_assert(sizeof(HostFrame) == 20, "HostFrame tem que ter exatamente 20 bytes");

} // namespace gs_usb
