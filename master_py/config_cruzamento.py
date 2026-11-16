"""
Configuracao do cruzamento: quais vias (node_id) NAO PODEM ficar verdes
ao mesmo tempo sem ser perigoso.

E o UNICO lugar que precisa mudar quando o numero de vias mudar (2 pra
3, 3 pra 4...) -- monitor.py e main.py nao sabem nada de geometria de
cruzamento, so perguntam "esse par conflita?" pra este arquivo.

Formato: um conjunto de pares (node_id_a, node_id_b). Cada par presente
aqui = "essas duas vias verdes ao mesmo tempo e perigoso, forca
fail-safe". Um par que NAO esta na lista e considerado compativel (pode
ficar verde junto sem problema -- ex.: duas vias de mao unica andando no
mesmo sentido, sem cruzamento real entre elas).

A ordem dentro do par nao importa -- (0, 1) e (1, 0) sao a mesma coisa.

--------------------------------------------------------------------
Exemplo: interseccao de 2 vias perpendiculares (config atual do projeto)
    CONFLITOS = {(0, 1)}

Exemplo: interseccao de 3 vias onde TODAS se cruzam entre si (rotatoria
pequena, ou cruzamento em T com as 3 pernas conflitando)
    CONFLITOS = {(0, 1), (0, 2), (1, 2)}

Exemplo: 4 vias, onde 0 e 2 sao paralelas e NAO conflitam entre si (duas
pistas de mao unica no mesmo sentido), mas as duas cruzam com 1 e 3
    CONFLITOS = {(0, 1), (0, 3), (1, 2), (2, 3)}
--------------------------------------------------------------------
"""
from __future__ import annotations

CONFLITOS: set[tuple[int, int]] = {
    (0, 1),
}


def em_conflito(node_a: int, node_b: int) -> bool:
    """True se esse par de node_ids esta marcado como perigoso em CONFLITOS."""
    par = (node_a, node_b) if node_a <= node_b else (node_b, node_a)
    return par in CONFLITOS
