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
| `0x000`          | `ID_FAILSAFE`   | Qualquer nó, ou a master (watchdog/conflito) | 1 byte: node_id de origem (0xFF = master) |
| `0x001`          | `ID_RESET`      | A master (ou um nó, no auto-recovery) | 1 byte: node_id de origem |
| `0x002`          | `ID_SYNC`       | A master, periodicamente (a cada ~2s) | 4 bytes: `SyncMsg` |
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

## `SyncMsg` (4 bytes, little-endian, sem padding)

| Campo      | Tipo   | Offset | Tamanho |
|------------|--------|--------|---------|
| `epoch_ms` | uint32 | 0      | 4       |

`epoch_ms`: quantos ms já passaram desde o último `ID_RESET` (ou desde
que a master subiu, se ainda não houve reset), segundo o relógio da
master. É o "tempo oficial" do ciclo.

Cada nó não guarda "em que fase estou" como estado — ele mantém só um
**ajuste** (ms, pode ser negativo) que soma ao próprio tick livre pra
formar o relógio oficial dele: `relogio_oficial = tick_local + ajuste`. A
cada `SyncMsg` recebido, o nó recalcula esse ajuste pra que
`relogio_oficial` bata com o `epoch_ms` da master naquele instante — não
"pula" fase nenhuma, só corrige o relógio que a rotina consulta. A
rotina, por sua vez, a cada ~200ms, calcula (a partir do relógio oficial
+ da própria `fase_inicial`) em que fase e com quanto tempo decorrido ELA
deveria estar agora, e só troca o LED quando essa resposta muda. Existe
porque cada ESP roda seu próprio clock livre (sem isso, um pequeno desvio
de instante de boot — ou deriva de cristal entre as placas — se acumula
e, depois de rodar um tempo, as fases dos dois semáforos saem de
sincronia).

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
- A master (`master_py`) ouve tudo isso passivamente e adiciona dois
  cuidados que nenhum nó sozinho consegue ter, porque exigem ver a rede
  inteira ao mesmo tempo:
  - **Watchdog**: se um nó parar de mandar status por >3s sem ter
    avisado fail-safe (morte silenciosa — crash, queda de energia), a
    master broadcasta `ID_FAILSAFE` em nome dele.
  - **Conflito de fase**: entre os nós com status recente (<2s) que
    estiverem reportando `VERDE` agora, a master confere par a par se
    aquela combinação está marcada como perigosa em
    `master_py/config_cruzamento.py` (`CONFLITOS`, um conjunto de pares
    `(node_id_a, node_id_b)` — é o único lugar que precisa mudar quando
    o cruzamento tiver mais vias). Se achar um par perigoso com os dois
    verdes ao mesmo tempo, a master broadcasta `ID_FAILSAFE`
    imediatamente (com um cooldown de 3s pra não floodar o barramento
    enquanto o conflito persistir).
  - A master também broadcasta `ID_SYNC` a cada ~2s (e logo depois de um
    `ID_RESET`, já com `epoch_ms=0`) pra manter os nós alinhados entre
    si — ver seção `SyncMsg` acima.
