"""
Master do Projeto Integrativo -- controlador PC via CAN (candleLight/gs_usb).

Arquitetura:
    CanBus.iniciar_leitura()  -- thread do python-can (Notifier), chama
                                 on_frame_recebido() a cada frame.
    on_frame_recebido()       -- decodifica o frame e atualiza o
                                 MonitorSemaforos. Mantido rapido de
                                 proposito, roda na thread de leitura.
    thread_watchdog()         -- verifica periodicamente se algum no
                                 parou de mandar status e broadcasta
                                 FAIL-SAFE em nome dele.
    loop de comandos (main)   -- le comandos do operador no terminal.

Pre-requisitos:  pip install "python-can[gs-usb]" libusb-package
Rodar:            python main.py
"""
from __future__ import annotations

import logging
import threading
import time

from can_bus import CanBus
from monitor import MonitorSemaforos
from protocolo import ID_FAILSAFE, ID_RESET, Fase, StatusMsg, eh_status_id

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s.%(msecs)03d [%(levelname)-5s] [%(name)s] %(message)s",
    datefmt="%H:%M:%S",
)
log = logging.getLogger("main")

WATCHDOG_INTERVALO_S = 1.0
WATCHDOG_TIMEOUT_S = 3.0  # 3x o periodo de status esperado (1s)

monitor = MonitorSemaforos()
rodando = threading.Event()
rodando.set()


def on_frame_recebido(bus: CanBus, msg) -> None:  # msg: can.Message
    if msg.arbitration_id == ID_FAILSAFE:
        origem = msg.data[0] if msg.dlc >= 1 else 0xFF
        log.warning("FAIL-SAFE visto no barramento (origem node_id=%d)", origem)
        return
    if msg.arbitration_id == ID_RESET:
        log.info("RESET visto no barramento")
        return
    if eh_status_id(msg.arbitration_id) and msg.dlc == 8:
        status = StatusMsg.decode(bytes(msg.data))
        monitor.atualizar_status(status)
        log.info(
            "[RX status] node_id=%d fase=%s t_na_fase=%dms mismatch=0x%02X",
            status.node_id, status.fase.name, status.tempo_na_fase_ms, status.flags_mismatch,
        )
        return
    log.debug("Frame nao reconhecido: id=0x%03X", msg.arbitration_id)


def thread_watchdog(bus: CanBus) -> None:
    log.info("[thread watchdog] iniciada")
    while rodando.is_set():
        time.sleep(WATCHDOG_INTERVALO_S)
        for node_id in monitor.verificar_silenciosos(WATCHDOG_TIMEOUT_S):
            log.error(
                "node_id=%d ficou mais de %.0fs sem mandar status -- "
                "broadcastando FAIL-SAFE em nome dele",
                node_id, WATCHDOG_TIMEOUT_S,
            )
            bus.enviar(ID_FAILSAFE, bytes([node_id]))
    log.info("[thread watchdog] encerrada")


def loop_comandos(bus: CanBus) -> None:
    log.info("Comandos: reset | failsafe | status | sair")
    while rodando.is_set():
        try:
            linha = input().strip().lower()
        except EOFError:
            break

        if linha == "reset":
            bus.enviar(ID_RESET, bytes([0xFF]))  # 0xFF = origem "master"
            log.info("RESET broadcastado pelo operador")
        elif linha == "failsafe":
            bus.enviar(ID_FAILSAFE, bytes([0xFF]))
            log.info("FAIL-SAFE forcado pelo operador")
        elif linha == "status":
            log.info(monitor.snapshot_texto())
        elif linha in ("sair", "quit", "exit"):
            rodando.clear()
            break
        elif linha:
            log.warning("Comando desconhecido: '%s'", linha)


def main() -> None:
    log.info("=== Master do Projeto Integrativo (CAN) ===")

    bus = CanBus(bitrate=500_000)
    bus.iniciar_leitura(lambda msg: on_frame_recebido(bus, msg))

    wd = threading.Thread(target=thread_watchdog, args=(bus,), daemon=True)
    wd.start()

    try:
        loop_comandos(bus)
    except KeyboardInterrupt:
        pass
    finally:
        rodando.clear()
        bus.fechar()
        log.info("Encerrado.")


if __name__ == "__main__":
    main()
