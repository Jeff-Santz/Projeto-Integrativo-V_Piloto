"""
MonitorSemaforos: guarda o ultimo status de cada no visto no barramento e
detecta nos "silenciosos" -- que pararam de mandar status sem avisar
fail-safe (crash, queda de energia, controlador CAN travado). Isso e algo
que nenhum no sozinho consegue detectar sobre si mesmo; so quem ouve todo
mundo (a master) pode perceber o silencio e agir.
"""
from __future__ import annotations

import threading
import time
from dataclasses import dataclass, field

from config_cruzamento import em_conflito
from protocolo import Fase, StatusMsg


WATCHDOG_BOOT_GRACE_S = 10.0  # tempo pra um no esperado aparecer pela 1a vez


@dataclass
class EstadoNo:
    fase: Fase = Fase.FAIL_SAFE
    tempo_na_fase_ms: int = 0
    flags_mismatch: int = 0
    ultimo_status: float = field(default_factory=time.monotonic)
    silencioso_reportado: bool = False


class MonitorSemaforos:
    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._nos: dict[int, EstadoNo] = {}

    def atualizar_status(self, msg: StatusMsg) -> None:
        with self._lock:
            e = self._nos.setdefault(msg.node_id, EstadoNo())
            e.fase = msg.fase
            e.tempo_na_fase_ms = msg.tempo_na_fase_ms
            e.flags_mismatch = msg.flags_mismatch
            e.ultimo_status = time.monotonic()
            e.silencioso_reportado = False  # voltou a falar, limpa a flag

    def verificar_conflito(self, max_idade_s: float = 2.0) -> list[tuple[int, int]]:
        """Pega todos os nos com status fresco (recebido ha menos de
        max_idade_s) que estao reportando VERDE agora, e confere par a
        par se aquela combinacao esta marcada como perigosa em
        config_cruzamento.CONFLITOS. Generaliza pra qualquer numero de
        vias -- quem decide o que e perigoso e a config, nao este
        metodo. Nenhum no sozinho consegue perceber isso (cada um so
        sabe da propria fiacao); so quem ouve todo mundo consegue.
        Retorna a lista de pares (node_id_a, node_id_b) em conflito."""
        agora = time.monotonic()
        with self._lock:
            verdes = [
                node_id for node_id, e in self._nos.items()
                if e.fase == Fase.VERDE and (agora - e.ultimo_status) <= max_idade_s
            ]

        conflitos: list[tuple[int, int]] = []
        for i in range(len(verdes)):
            for j in range(i + 1, len(verdes)):
                if em_conflito(verdes[i], verdes[j]):
                    conflitos.append((verdes[i], verdes[j]))
        return conflitos

    def verificar_silenciosos(self, timeout_s: float) -> list[int]:
        """Retorna node_ids sem status ha mais de timeout_s, ainda nao
        reportados nesta ausencia (evita reportar repetido a cada tick)."""
        agora = time.monotonic()
        resultado: list[int] = []
        with self._lock:
            for node_id, e in self._nos.items():
                if (agora - e.ultimo_status) > timeout_s and not e.silencioso_reportado:
                    e.silencioso_reportado = True
                    resultado.append(node_id)
        return resultado

    def silenciosos_agora(self, timeout_s: float, esperados: set[int], desde_boot_s: float) -> set[int]:
        """Estado (nao evento): todos os nos que estao mudos AGORA -- vistos
        e parados ha mais de timeout_s, ou esperados e nunca vistos depois
        de desde_boot_s. Sem a flag silencioso_reportado, entao da pra
        re-broadcastar o fail-safe enquanto durar."""
        agora = time.monotonic()
        with self._lock:
            mudos = {n for n, e in self._nos.items() if (agora - e.ultimo_status) > timeout_s}
            if desde_boot_s > WATCHDOG_BOOT_GRACE_S:
                mudos |= {n for n in esperados if n not in self._nos}
        return mudos

    def snapshot_texto(self) -> str:
        agora = time.monotonic()
        with self._lock:
            if not self._nos:
                return "Nenhum no visto ainda no barramento."
            linhas = [
                f"  {'node_id':>7} | {'fase':<9} | {'t_na_fase':>9} | {'mismatch':<8} | ha_quanto_tempo_sem_status",
                f"  {'-'*7}-|-{'-'*9}-|-{'-'*9}-|-{'-'*8}-|-{'-'*27}",
            ]
            for node_id, e in sorted(self._nos.items()):
                ha_s = agora - e.ultimo_status
                linhas.append(
                    f"  {node_id:>7} | {e.fase.name:<9} | {e.tempo_na_fase_ms:>7}ms | "
                    f"0x{e.flags_mismatch:02X}     | {ha_s:.1f}s"
                )
            return "\n" + "\n".join(linhas)
