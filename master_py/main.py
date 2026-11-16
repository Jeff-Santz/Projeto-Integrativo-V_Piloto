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
from protocolo import ID_FAILSAFE, ID_RESET, ID_SYNC, Fase, StatusMsg, SyncMsg, eh_status_id

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s.%(msecs)03d [%(levelname)-5s] [%(name)s] %(message)s",
    datefmt="%H:%M:%S",
)
log = logging.getLogger("main")

WATCHDOG_INTERVALO_S = 1.0
WATCHDOG_TIMEOUT_S = 3.0  # 3x o periodo de status esperado (1s)
SYNC_INTERVALO_S = 2.0
CONFLITO_COOLDOWN_S = 3.0  # nao refloodar FAIL-SAFE a cada status enquanto o conflito persistir

monitor = MonitorSemaforos()
rodando = threading.Event()
rodando.set()
_ultimo_conflito_ts = 0.0
_epoch_inicio = time.monotonic()  # reiniciado a cada ID_RESET visto no barramento (de qualquer origem)


def on_frame_recebido(bus: CanBus, msg) -> None:  # msg: can.Message
    if msg.arbitration_id == ID_FAILSAFE:
        origem = msg.data[0] if msg.dlc >= 1 else 0xFF
        log.warning("FAIL-SAFE visto no barramento (origem node_id=%d)", origem)
        return
    if msg.arbitration_id == ID_RESET:
        # Qualquer ID_RESET -- mandado pelo operador OU auto-recovery de um
        # no sozinho -- so realinha quem estava em FAIL_SAFE (ver
        # Semaforo::restaurarOperacao no firmware). Um no que ja estava
        # rodando normal continua de onde estava, sem saber que o ciclo
        # "oficial" zerou -- e pode calhar de estar numa fase que colide
        # com a do no que acabou de voltar. Por isso, ao ver QUALQUER
        # reset no barramento, a master tambem reinicia o proprio epoch e
        # manda um SYNC(0) na hora, pra realinhar todo mundo de verdade
        # (nao so quem causou o reset).
        global _epoch_inicio
        _epoch_inicio = time.monotonic()
        bus.enviar(ID_SYNC, SyncMsg(0).encode())
        log.info("RESET visto no barramento (epoch reiniciado, SYNC(0) mandado)")
        return
    if eh_status_id(msg.arbitration_id) and msg.dlc == 8:
        status = StatusMsg.decode(bytes(msg.data))
        monitor.atualizar_status(status)
        log.info(
            "[RX status] node_id=%d fase=%s t_na_fase=%dms mismatch=0x%02X",
            status.node_id, status.fase.name, status.tempo_na_fase_ms, status.flags_mismatch,
        )
        _verificar_conflito(bus)
        return
    log.debug("Frame nao reconhecido: id=0x%03X", msg.arbitration_id)


def _verificar_conflito(bus: CanBus) -> None:
    """So a master ve todos os nos ao mesmo tempo -- nenhum no sozinho
    consegue perceber o estado de outro no. Quais pares de vias verdes
    simultaneas sao perigosas esta em config_cruzamento.py (edita la
    quando o cruzamento tiver mais vias), nao aqui. Forca FAIL-SAFE na
    rede (com cooldown pra nao floodar o barramento/log enquanto o
    conflito persistir)."""
    global _ultimo_conflito_ts
    pares_em_conflito = monitor.verificar_conflito()
    if not pares_em_conflito:
        return
    agora = time.monotonic()
    if agora - _ultimo_conflito_ts < CONFLITO_COOLDOWN_S:
        return
    _ultimo_conflito_ts = agora
    log.error("CONFLITO (regra de paridade): pares %s -- forcando FAIL-SAFE na rede", pares_em_conflito)
    bus.enviar(ID_FAILSAFE, bytes([0xFF]))


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


def thread_sync(bus: CanBus) -> None:
    """Broadcasta o relogio oficial do ciclo (ms desde o ultimo reset) a
    cada SYNC_INTERVALO_S. E assim que os nos ficam alinhados entre si
    mesmo rodando cada um com seu proprio clock livre -- sem isso, um
    pequeno desvio de inicio (ou de cristal) entre as placas acumula e,
    depois de um tempo, as fases dos dois semaforos deixam de bater."""
    log.info("[thread sync] iniciada (a cada %.0fs)", SYNC_INTERVALO_S)
    while rodando.is_set():
        time.sleep(SYNC_INTERVALO_S)
        epoch_ms = int((time.monotonic() - _epoch_inicio) * 1000) & 0xFFFFFFFF
        bus.enviar(ID_SYNC, SyncMsg(epoch_ms).encode())
        log.info("[TX] SYNC epoch_ms=%d", epoch_ms)
    log.info("[thread sync] encerrada")


def loop_comandos(bus: CanBus) -> None:
    global _epoch_inicio
    log.info("Comandos: reset | failsafe | status | sair")
    while rodando.is_set():
        try:
            linha = input().strip().lower()
        except EOFError:
            break

        if linha == "reset":
            bus.enviar(ID_RESET, bytes([0xFF]))  # 0xFF = origem "master"
            _epoch_inicio = time.monotonic()  # ciclo oficial reinicia do zero junto
            bus.enviar(ID_SYNC, SyncMsg(0).encode())
            log.info("RESET broadcastado pelo operador (epoch reiniciado)")
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
    sync = threading.Thread(target=thread_sync, args=(bus,), daemon=True)
    sync.start()

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
