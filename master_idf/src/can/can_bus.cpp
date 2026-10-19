#include "can/can_bus.hpp"

#include <cstring>
#include <sstream>
#include <vector>

#include "core/log.hpp"

namespace can {

namespace {
constexpr const char* TAG = "CAN_BUS";
constexpr unsigned CTRL_TIMEOUT_MS = 1000;
constexpr unsigned BULK_TIMEOUT_MS = 50; // curto: a thread de leitura precisa reagir a rodando_=false

// Calcula prop_seg/phase_seg1/phase_seg2/sjw/brp pra bater o bitrate
// pedido, usando o clock e os limites reais que o device informou na
// BT_CONST (em vez de chutar um clock fixo). Busca o menor brp que
// resulte num numero EXATO de time-quanta (senao o bitrate sai torto),
// com ponto de amostragem perto de 87.5% (padrao de fato pra CAN).
//
// prop_seg fica sempre 0 e tudo vai pra phase_seg1, porque bxCAN (o
// periferico CAN dos STM32, incluindo o F072 do seu adaptador) nao
// distingue as duas fases de verdade -- internamente e tudo BS1/BS2.
bool calcularBittiming(uint32_t fclk_can, uint32_t bitrate_bps,
                        const gs_usb::DeviceBtConst& lim,
                        gs_usb::DeviceBittiming& out)
{
    constexpr double SAMPLE_POINT = 0.875;

    for (uint32_t brp = lim.brp_min; brp <= lim.brp_max; brp += (lim.brp_inc ? lim.brp_inc : 1)) {
        uint64_t denom = (uint64_t)bitrate_bps * brp;
        if (denom == 0 || fclk_can % denom != 0) continue; // precisa dar exato

        uint32_t total_tq = (uint32_t)(fclk_can / denom); // 1 (sync) + tseg1 + tseg2
        if (total_tq < 8 || total_tq > 25) continue;        // faixa razoavel, evita extremos

        uint32_t tseg1 = (uint32_t)((double)total_tq * SAMPLE_POINT) - 1;
        if (tseg1 < 1 || tseg1 > total_tq - 2) continue;
        uint32_t tseg2 = total_tq - 1 - tseg1;

        if (tseg1 < lim.tseg1_min || tseg1 > lim.tseg1_max) continue;
        if (tseg2 < lim.tseg2_min || tseg2 > lim.tseg2_max) continue;

        uint32_t sjw = std::min<uint32_t>({4u, tseg2, lim.sjw_max});
        if (sjw < 1) sjw = 1;

        out.prop_seg   = 0;
        out.phase_seg1 = tseg1;
        out.phase_seg2 = tseg2;
        out.sjw        = sjw;
        out.brp        = brp;

        double sample_point_real = (double)(1 + tseg1) / (double)total_tq;
        std::ostringstream oss;
        oss << "fclk_can=" << fclk_can << " brp=" << brp << " total_tq=" << total_tq
            << " tseg1=" << tseg1 << " tseg2=" << tseg2 << " sjw=" << sjw
            << " sample_point=" << sample_point_real;
        log_::debug(TAG, "Bittiming calculado: " + oss.str());
        return true;
    }
    return false;
}

std::string hexDump(const uint8_t* d, int n)
{
    std::ostringstream oss;
    for (int i = 0; i < n; i++) {
        char buf[4];
        snprintf(buf, sizeof buf, "%02X ", d[i]);
        oss << buf;
    }
    return oss.str();
}

} // namespace

CanBus::~CanBus() { fechar(); }

bool CanBus::abrir(uint32_t bitrate_bps)
{
    int rc = libusb_init(&ctx_);
    if (rc != 0) {
        log_::erro(TAG, std::string("libusb_init falhou: ") + libusb_error_name(rc));
        return false;
    }

    handle_ = libusb_open_device_with_vid_pid(ctx_, gs_usb::VID, gs_usb::PID);
    if (!handle_) {
        log_::erro(TAG, "Adaptador nao encontrado (VID 0x1d50 / PID 0x606f). "
                         "Confere no Gerenciador de Dispositivos se e esse o VID/PID, "
                         "e se nenhum outro programa (Cangaroo, outro script Python) esta com ele aberto.");
        return false;
    }
    log_::info(TAG, "Adaptador aberto (VID/PID 0x1d50/0x606f)");

    // No Windows normalmente nem existe driver de kernel pra destacar
    // (o device usa WinUSB), entao um erro aqui e esperado e inofensivo.
    libusb_set_auto_detach_kernel_driver(handle_, 1);

    rc = libusb_claim_interface(handle_, 0);
    if (rc != 0) {
        log_::erro(TAG, std::string("libusb_claim_interface falhou: ") + libusb_error_name(rc) +
                         " -- o adaptador pode estar em uso por outro programa.");
        return false;
    }

    // Descobre os endpoints bulk IN/OUT da interface 0.
    libusb_device* dev = libusb_get_device(handle_);
    libusb_config_descriptor* cfg = nullptr;
    rc = libusb_get_active_config_descriptor(dev, &cfg);
    if (rc != 0 || !cfg) {
        log_::erro(TAG, "Nao consegui ler o config descriptor do USB.");
        return false;
    }
    for (int i = 0; i < cfg->interface[0].altsetting[0].bNumEndpoints; i++) {
        const auto& ep = cfg->interface[0].altsetting[0].endpoint[i];
        bool bulk = (ep.bmAttributes & 0x03) == LIBUSB_TRANSFER_TYPE_BULK;
        bool in   = (ep.bEndpointAddress & 0x80) != 0;
        if (bulk && in)  ep_in_  = ep.bEndpointAddress;
        if (bulk && !in) ep_out_ = ep.bEndpointAddress;
    }
    libusb_free_config_descriptor(cfg);
    if (!ep_in_ || !ep_out_) {
        log_::erro(TAG, "Nao achei endpoints bulk IN/OUT -- device inesperado?");
        return false;
    }
    {
        std::ostringstream oss;
        oss << "ep_in=0x" << std::hex << (int)ep_in_ << " ep_out=0x" << (int)ep_out_;
        log_::debug(TAG, oss.str());
    }

    // 1) host_format
    gs_usb::HostConfig hc{0x0000beef};
    rc = libusb_control_transfer(handle_,
            LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_INTERFACE,
            gs_usb::BREQ_HOST_FORMAT, 0, 0,
            reinterpret_cast<uint8_t*>(&hc), sizeof hc, CTRL_TIMEOUT_MS);
    log_::debug(TAG, "HOST_FORMAT -> rc=" + std::to_string(rc));
    if (rc < 0) { log_::erro(TAG, std::string("HOST_FORMAT falhou: ") + libusb_error_name(rc)); return false; }

    // 2) BT_CONST -- pergunta o clock e limites reais do device
    gs_usb::DeviceBtConst btc{};
    rc = libusb_control_transfer(handle_,
            LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_INTERFACE,
            gs_usb::BREQ_BT_CONST, 0, 0,
            reinterpret_cast<uint8_t*>(&btc), sizeof btc, CTRL_TIMEOUT_MS);
    log_::debug(TAG, "BT_CONST -> rc=" + std::to_string(rc) + " (esperado " + std::to_string(sizeof btc) + ")");
    if (rc != (int)sizeof btc) {
        log_::erro(TAG, std::string("BT_CONST falhou ou veio truncado: ") + libusb_error_name(rc < 0 ? rc : 0));
        return false;
    }
    {
        std::ostringstream oss;
        oss << "fclk_can=" << btc.fclk_can << " tseg1=[" << btc.tseg1_min << "," << btc.tseg1_max
            << "] tseg2=[" << btc.tseg2_min << "," << btc.tseg2_max << "] sjw_max=" << btc.sjw_max
            << " brp=[" << btc.brp_min << "," << btc.brp_max << "] brp_inc=" << btc.brp_inc;
        log_::debug(TAG, "BT_CONST do device: " + oss.str());
    }

    gs_usb::DeviceBittiming bt{};
    if (!calcularBittiming(btc.fclk_can, bitrate_bps, btc, bt)) {
        log_::erro(TAG, "Nao achei combinacao de timing exata pra esse bitrate com o clock desse device.");
        return false;
    }

    // 3) aplica o bittiming calculado
    rc = libusb_control_transfer(handle_,
            LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_INTERFACE,
            gs_usb::BREQ_BITTIMING, 0, 0,
            reinterpret_cast<uint8_t*>(&bt), sizeof bt, CTRL_TIMEOUT_MS);
    log_::debug(TAG, "BITTIMING -> rc=" + std::to_string(rc));
    if (rc < 0) { log_::erro(TAG, std::string("BITTIMING falhou: ") + libusb_error_name(rc)); return false; }

    // 4) inicia o canal
    gs_usb::DeviceMode modo{gs_usb::MODE_START, 0};
    rc = libusb_control_transfer(handle_,
            LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_INTERFACE,
            gs_usb::BREQ_MODE, 0, 0,
            reinterpret_cast<uint8_t*>(&modo), sizeof modo, CTRL_TIMEOUT_MS);
    log_::debug(TAG, "MODE_START -> rc=" + std::to_string(rc));
    if (rc < 0) { log_::erro(TAG, std::string("MODE_START falhou: ") + libusb_error_name(rc)); return false; }

    log_::info(TAG, "Canal CAN iniciado a " + std::to_string(bitrate_bps / 1000) + " kbps");
    return true;
}

void CanBus::iniciarLeitura(FrameCallback callback)
{
    callback_ = std::move(callback);
    rodando_  = true;
    thread_leitura_ = std::thread(&CanBus::loopLeitura, this);
}

void CanBus::loopLeitura()
{
    log_::info(TAG, "Thread de leitura iniciada");
    gs_usb::HostFrame frame{};

    while (rodando_) {
        int transferido = 0;
        int rc = libusb_bulk_transfer(handle_, ep_in_,
                reinterpret_cast<uint8_t*>(&frame), sizeof frame,
                &transferido, BULK_TIMEOUT_MS);

        if (rc == LIBUSB_ERROR_TIMEOUT) continue; // normal, so da tempo de checar rodando_
        if (rc != 0) {
            log_::warn(TAG, std::string("bulk_transfer (leitura) erro: ") + libusb_error_name(rc));
            continue;
        }
        if (transferido != (int)sizeof frame) {
            log_::warn(TAG, "Frame recebido com tamanho inesperado: " + std::to_string(transferido) + " bytes");
            continue;
        }

        if (frame.echo_id != gs_usb::ECHO_ID_RX) {
            // Confirmacao de um TX nosso, nao e frame do barramento.
            log_::debug(TAG, "TX confirmado (echo_id=" + std::to_string(frame.echo_id) + ")");
            continue;
        }

        Frame f;
        f.id  = frame.can_id & gs_usb::CAN_SFF_MASK;
        f.dlc = frame.can_dlc;
        std::memcpy(f.data, frame.data, sizeof f.data);

        log_::debug(TAG, "RX id=0x" + [&]{ std::ostringstream o; o << std::hex << f.id; return o.str(); }()
                          + " dlc=" + std::to_string(f.dlc) + " dados=" + hexDump(f.data, f.dlc));

        if (callback_) callback_(f);
    }
    log_::info(TAG, "Thread de leitura encerrada");
}

bool CanBus::enviar(uint32_t id, const uint8_t* dados, uint8_t dlc)
{
    gs_usb::HostFrame frame{};
    frame.echo_id = 0; // != ECHO_ID_RX -> o device sabe que e um TX nosso
    frame.can_id  = id & gs_usb::CAN_SFF_MASK;
    frame.can_dlc = dlc > 8 ? 8 : dlc;
    frame.channel = 0;
    std::memcpy(frame.data, dados, frame.can_dlc);

    int transferido = 0;
    int rc = libusb_bulk_transfer(handle_, ep_out_,
            reinterpret_cast<uint8_t*>(&frame), sizeof frame,
            &transferido, CTRL_TIMEOUT_MS);

    log_::debug(TAG, "TX id=0x" + [&]{ std::ostringstream o; o << std::hex << id; return o.str(); }()
                      + " dlc=" + std::to_string(frame.can_dlc) + " dados=" + hexDump(dados, frame.can_dlc)
                      + " -> rc=" + std::to_string(rc));

    if (rc != 0) {
        log_::warn(TAG, std::string("Envio falhou: ") + libusb_error_name(rc));
        return false;
    }
    return true;
}

void CanBus::fechar()
{
    rodando_ = false;
    if (thread_leitura_.joinable()) thread_leitura_.join();

    if (handle_) {
        gs_usb::DeviceMode modo{gs_usb::MODE_RESET, 0};
        libusb_control_transfer(handle_,
                LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_INTERFACE,
                gs_usb::BREQ_MODE, 0, 0,
                reinterpret_cast<uint8_t*>(&modo), sizeof modo, CTRL_TIMEOUT_MS);
        libusb_release_interface(handle_, 0);
        libusb_close(handle_);
        handle_ = nullptr;
    }
    if (ctx_) {
        libusb_exit(ctx_);
        ctx_ = nullptr;
    }
}

} // namespace can
