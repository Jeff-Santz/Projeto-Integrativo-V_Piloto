// =====================================================================
// Master do Projeto Integrativo -- versao CAN.
//
// Arquitetura (mesmo espirito do master TCP antigo, adaptado pro
// barramento CAN em vez de sockets):
//
//   CanBus::loopLeitura()  -- thread propria dentro de CanBus, le os
//                             frames do adaptador e chama o callback
//                             abaixo pra cada frame genuino recebido.
//   onFrameRecebido()      -- roda NA thread de leitura: decodifica o
//                             frame e atualiza o MonitorSemaforos.
//                             Mantido rapido de proposito (so grava
//                             estado), pra nao atrasar a leitura USB.
//   threadWatchdog()       -- verifica periodicamente se algum no parou
//                             de mandar status (crash, queda de energia,
//                             controlador CAN travado) e broadcasta
//                             FAIL-SAFE em nome dele.
//   threadComandos()       -- le comandos do operador no terminal
//                             (reset / failsafe / status / sair).
//
// Nao tem fila entre threads como no master TCP antigo porque aqui o
// volume de frames e baixo (status 1x/s por no) e o trabalho de
// decodificar um StatusMsg de 8 bytes e trivial -- uma fila dedicada
// so adicionaria complexidade sem necessidade reaL nesta escala.
// =====================================================================
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#include "can/can_bus.hpp"
#include "core/log.hpp"
#include "core/monitor_semaforos.hpp"
#include "protocolo/protocolo.hpp"

namespace {

constexpr const char* TAG = "MAIN";
constexpr uint32_t WATCHDOG_INTERVALO_MS = 1000;
constexpr uint32_t WATCHDOG_TIMEOUT_MS   = 3000; // 3x o periodo de status esperado (1s)

std::atomic<bool> g_rodando{true};
can::CanBus        g_bus;
core::MonitorSemaforos g_monitor;

void onFrameRecebido(const can::Frame& f)
{
    if (f.id == can_proto::ID_FAILSAFE) {
        uint8_t origem = f.dlc >= 1 ? f.data[0] : 0xFF;
        log_::warn(TAG, "FAIL-SAFE visto no barramento (origem node_id=" + std::to_string(origem) + ")");
        return;
    }
    if (f.id == can_proto::ID_RESET) {
        log_::info(TAG, "RESET visto no barramento");
        return;
    }
    if (f.id >= can_proto::ID_STATUS_BASE && f.id < can_proto::ID_STATUS_BASE + 16 && f.dlc == sizeof(can_proto::StatusMsg)) {
        can_proto::StatusMsg msg{};
        std::memcpy(&msg, f.data, sizeof msg);
        g_monitor.atualizarStatus(msg);
        log_::info(TAG, "[RX status] node_id=" + std::to_string(msg.node_id) +
                         " fase=" + can_proto::nome_fase((can_proto::Fase)msg.fase) +
                         " t_na_fase=" + std::to_string(msg.tempo_na_fase_ms) + "ms" +
                         " mismatch=0x" + [&]{ char b[4]; snprintf(b,sizeof b,"%02X", msg.flags_mismatch); return std::string(b); }());
        return;
    }
    log_::debug(TAG, "Frame nao reconhecido: id=0x" + [&]{ char b[8]; snprintf(b,sizeof b,"%03lX",(unsigned long)f.id); return std::string(b); }());
}

void threadWatchdog()
{
    log_::info(TAG, "[THREAD watchdog] iniciada");
    while (g_rodando) {
        std::this_thread::sleep_for(std::chrono::milliseconds(WATCHDOG_INTERVALO_MS));

        auto silenciosos = g_monitor.verificarSilenciosos(WATCHDOG_TIMEOUT_MS);
        for (uint8_t node_id : silenciosos) {
            log_::erro(TAG, "node_id=" + std::to_string(node_id) +
                             " ficou mais de " + std::to_string(WATCHDOG_TIMEOUT_MS) +
                             "ms sem mandar status -- broadcastando FAIL-SAFE em nome dele");
            uint8_t dados[1] = { node_id };
            g_bus.enviar(can_proto::ID_FAILSAFE, dados, 1);
        }
    }
    log_::info(TAG, "[THREAD watchdog] encerrada");
}

void threadComandos()
{
    log_::info(TAG, "[THREAD comandos] iniciada. Digite: reset | failsafe | status | sair");
    std::string linha;
    while (g_rodando && std::getline(std::cin, linha)) {
        if (linha == "reset") {
            uint8_t dados[1] = { 0xFF }; // 0xFF = origem "master", nao e um node_id de semaforo
            g_bus.enviar(can_proto::ID_RESET, dados, 1);
            log_::info(TAG, "RESET broadcastado pelo operador");
        } else if (linha == "failsafe") {
            uint8_t dados[1] = { 0xFF };
            g_bus.enviar(can_proto::ID_FAILSAFE, dados, 1);
            log_::info(TAG, "FAIL-SAFE forcado pelo operador");
        } else if (linha == "status") {
            g_monitor.imprimirSnapshot();
        } else if (linha == "sair" || linha == "quit" || linha == "exit") {
            g_rodando = false;
            break;
        } else if (!linha.empty()) {
            log_::warn(TAG, "Comando desconhecido: '" + linha + "' (use reset | failsafe | status | sair)");
        }
    }
    log_::info(TAG, "[THREAD comandos] encerrada");
}

} // namespace

int main()
{
    log_::info(TAG, "=== Master do Projeto Integrativo (CAN) ===");

    if (!g_bus.abrir(500000)) {
        log_::erro(TAG, "Nao consegui abrir o adaptador CAN. Veja os logs [DEBUG]/[ERRO] acima pra saber em que etapa parou.");
        return 1;
    }

    g_bus.iniciarLeitura(onFrameRecebido);

    std::thread tWatchdog(threadWatchdog);
    std::thread tComandos(threadComandos);

    tComandos.join(); // o operador digitando "sair" e o jeito normal de encerrar
    g_rodando = false;
    if (tWatchdog.joinable()) tWatchdog.join();

    g_bus.fechar();
    log_::info(TAG, "Encerrado.");
    return 0;
}
