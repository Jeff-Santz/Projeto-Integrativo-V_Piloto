#pragma once
// =====================================================================
// CanBus: abre o adaptador candleLight/gs_usb via libusb e expoe uma
// interface simples (enviar/receber frame), igual o can_bus.hpp do
// firmware da ESP, so que por baixo usa USB em vez de TWAI.
//
// IMPORTANTE (debug): toda etapa de abertura (host_format, BT_CONST,
// bittiming calculado, mode START) e logada com detalhe via Log::debug.
// Se alguma coisa der errado na primeira execucao com hardware real,
// olhe esse log primeiro -- ele mostra exatamente em que control
// transfer parou e com que codigo de erro, igual fizemos pra achar o
// bug de fiacao do GPIO.
// =====================================================================
#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>

#include <libusb.h>

#include "can/gs_usb_protocol.hpp"

namespace can {

struct Frame {
    uint32_t id  = 0;
    uint8_t  dlc = 0;
    uint8_t  data[8] = {};
};

// Chamado em uma thread dedicada a cada frame genuino recebido do
// barramento (frames de eco/confirmacao de TX sao filtrados antes).
using FrameCallback = std::function<void(const Frame&)>;

class CanBus {
public:
    CanBus() = default;
    ~CanBus();

    CanBus(const CanBus&)            = delete;
    CanBus& operator=(const CanBus&) = delete;

    // Abre o primeiro adaptador gs_usb encontrado, configura 500 kbps
    // (calculado a partir do BT_CONST real do device) e inicia o canal.
    // Retorna false se qualquer etapa falhar (log explica qual).
    bool abrir(uint32_t bitrate_bps = 500000);

    // Inicia a thread de leitura (bulk IN em loop). Cada frame genuino
    // recebido dispara `callback` em uma thread dedicada -- quem
    // implementa o callback precisa ser thread-safe.
    void iniciarLeitura(FrameCallback callback);

    // Envia um frame (bloqueante, timeout curto). Retorna false se o
    // bulk transfer falhar.
    bool enviar(uint32_t id, const uint8_t* dados, uint8_t dlc);

    void fechar();

private:
    bool configurarBittiming(uint32_t bitrate_bps);
    void loopLeitura();

    libusb_context*       ctx_        = nullptr;
    libusb_device_handle* handle_     = nullptr;
    uint8_t                ep_in_     = 0;
    uint8_t                ep_out_    = 0;

    std::thread        thread_leitura_;
    std::atomic<bool>  rodando_{false};
    FrameCallback      callback_;
};

} // namespace can
