"""
Mesmo protocolo CAN do firmware (semaforo_idf/components/protocolo/include/
protocolo.hpp). MANTENHA OS DOIS EM SINCRONIA -- se mudar um lado, muda o
outro. Ver docs/protocolo.md na raiz do repo pra fonte de verdade comentada.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass
from enum import IntEnum

ID_FAILSAFE = 0x000
ID_RESET = 0x001
ID_STATUS_BASE = 0x010  # + node_id -> 0x010..0x01F (nodes 0-15)


class Fase(IntEnum):
    VERMELHO = 0
    AMARELO = 1
    VERDE = 2
    FAIL_SAFE = 3


# node_id(B) fase(B) tempo_na_fase_ms(I, little-endian) flags_mismatch(B) reserved(B) = 8 bytes,
# igual ao StatusMsg com #pragma pack(1) no firmware.
_STATUS_FMT = "<BBIBB"
STATUS_SIZE = struct.calcsize(_STATUS_FMT)
assert STATUS_SIZE == 8, "StatusMsg precisa ter exatamente 8 bytes (frame CAN classico)"


@dataclass
class StatusMsg:
    node_id: int
    fase: Fase
    tempo_na_fase_ms: int
    flags_mismatch: int

    @classmethod
    def decode(cls, data: bytes) -> "StatusMsg":
        node_id, fase, tempo_na_fase_ms, flags_mismatch, _reserved = struct.unpack(_STATUS_FMT, data)
        return cls(node_id, Fase(fase), tempo_na_fase_ms, flags_mismatch)


def status_id(node_id: int) -> int:
    return ID_STATUS_BASE + node_id


def eh_status_id(arbitration_id: int) -> bool:
    return ID_STATUS_BASE <= arbitration_id < ID_STATUS_BASE + 16
