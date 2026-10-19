"""
Camada fina sobre python-can/gs_usb -- mesma base ja validada na bancada
pelo can_candlelight.py, so que empacotada como classe reutilizavel.
"""
from __future__ import annotations

import functools
import logging
from typing import Callable

import can

logger = logging.getLogger("can_bus")


def _patch_backend_libusb() -> None:
    """Sem isso da NoBackendError no Windows (pyusb nao acha a libusb.dll
    sozinho). Mesmo patch que usamos no can_candlelight.py original."""
    import libusb_package
    import usb.core

    usb.core.find = functools.partial(usb.core.find, backend=libusb_package.get_libusb1_backend())


class _CallbackListener(can.Listener):
    """Adapta uma funcao simples pra interface Listener do python-can."""

    def __init__(self, callback: Callable[[can.Message], None]) -> None:
        self._callback = callback

    def on_message_received(self, msg: can.Message) -> None:  # noqa: D102
        self._callback(msg)


class CanBus:
    def __init__(self, bitrate: int = 500_000) -> None:
        _patch_backend_libusb()
        self.bus = can.Bus(interface="gs_usb", channel="gs_usb", index=0, bitrate=bitrate)
        self._notifier: can.Notifier | None = None
        logger.info("Adaptador aberto, %d kbps", bitrate // 1000)

    def enviar(self, arbitration_id: int, data: bytes) -> bool:
        msg = can.Message(arbitration_id=arbitration_id, is_extended_id=False, data=data)
        try:
            self.bus.send(msg)
            logger.debug("TX id=0x%03X dados=%s", arbitration_id, data.hex(" "))
            return True
        except can.CanError as exc:
            logger.warning("Envio falhou (id=0x%03X): %s", arbitration_id, exc)
            return False

    def iniciar_leitura(self, callback: Callable[[can.Message], None]) -> None:
        """Dispara `callback` numa thread do python-can pra cada frame recebido."""
        listener = _CallbackListener(callback)
        self._notifier = can.Notifier(self.bus, [listener])
        logger.info("Notifier de leitura iniciado")

    def fechar(self) -> None:
        if self._notifier is not None:
            self._notifier.stop()
        self.bus.shutdown()
        logger.info("Canal fechado")
