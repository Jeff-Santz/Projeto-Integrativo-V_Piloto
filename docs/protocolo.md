# Protocolo CAN — Projeto Integrativo

Fonte de verdade documentada do protocolo usado no barramento CAN (500
kbps) entre os nós `semaforo_idf` e o `master_py`. Os dois lados
implementam isso em código duplicado de propósito (sem build
compartilhado entre ESP-IDF e Python/C++) — **se mudar aqui, muda nos
dois**:

- `semaforo_idf/components/protocolo/include/protocolo.hpp`
- `master_py/protocolo.py`
- `master_idf/include/protocolo/protocolo.hpp` (histórico, não mantido ativamente)

## IDs (11 bits, formato CAN clássico)

Em CAN, ID menor vence a arbitragem — por isso fail-safe fica no menor
ID possível: se dois frames disputarem o barramento ao mesmo tempo, o
fail-safe sempre ganha.

| ID (hex)        | Nome            | Quem manda                     | Dados |
|------------------|-----------------|----------------------------------|-------|
| `0x000`          | `ID_FAILSAFE`   | Qualquer nó, ou a master (watchdog) | 1 byte: node_id de origem (0xFF = master) |
| `0x001`          | `ID_RESET`      | A master (ou um nó, no auto-recovery) | 1 byte: node_id de origem |
| `0x010`–`0x01F`  | status do nó    | Cada nó, 1x/segundo              | 8 bytes: `StatusMsg` |

`status_id(node_id) = 0x010 + node_id` (suporta node_id 0–15).

## `StatusMsg` (8 bytes, little-endian, sem padding)

| Campo              | Tipo     | Offset | Tamanho |
|---------------------|----------|--------|---------|
| `node_id`           | uint8    | 0      | 1       |
| `fase`               | uint8    | 1      | 1       |
| `tempo_na_fase_ms`   | uint32   | 2      | 4       |
| `flags_mismatch`     | uint8    | 6      | 1       |
| `reserved`           | uint8    | 7      | 1       |

`fase`: `0=VERMELHO 1=AMARELO 2=VERDE 3=FAIL_SAFE`

`flags_mismatch`: `bit0=verde bit1=amarelo bit2=vermelho` — 1 quando a
leitura do pino de entrada daquela cor não bate com o que a fase atual
esperava.

## Comportamento esperado

- Cada nó manda seu `StatusMsg` a cada ~1s, sempre.
- Se o nó detectar `flags_mismatch != 0` por tempo demais (checagem a
  cada 200ms no firmware atual), ele mesmo entra em fail-safe e
  broadcasta `ID_FAILSAFE`.
- Qualquer nó que receber `ID_FAILSAFE` entra em fail-safe imediatamente
  (pisca amarelo), mesmo sem erro próprio — é um broadcast que vale pra
  rede inteira.
- Auto-recovery: um nó em fail-safe confere sozinho se a fiação voltou a
  bater; depois de ~3 leituras boas seguidas, sai do fail-safe e
  broadcasta `ID_RESET`.
- A master (`master_py`) ouve tudo isso passivamente e adiciona um
  watchdog: se um nó parar de mandar status por >3s sem ter avisado
  fail-safe (morte silenciosa — crash, queda de energia), a master
  broadcasta `ID_FAILSAFE` em nome dele.
