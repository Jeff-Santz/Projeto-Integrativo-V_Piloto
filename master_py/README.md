# Master (PC) — Projeto Integrativo

Controlador em Python que fala CAN com o adaptador USB-CAN (candleLight/
gs_usb) usado no barramento com os nós `semaforo_idf`. Construído em cima
do `python-can` com backend `gs_usb` — a mesma combinação já validada na
bancada (ver histórico do `can_candlelight.py`), só reorganizada em
módulos em vez de um script único.

```
master_py/
├── requirements.txt
├── main.py        # fio condutor: abre o bus, threads, loop de comandos
├── can_bus.py      # camada fina sobre python-can/gs_usb
├── monitor.py      # estado por nó + watchdog de nó silencioso
└── protocolo.py    # IDs CAN, enum Fase, StatusMsg — espelha o firmware
```

## Por que Python aqui (e não C++)

Existe uma tentativa em C++ nativo com libusb (`master_idf/`, histórico
no repo) que reimplementa o protocolo `gs_usb` por baixo. Ficou pra trás
por enquanto: setup de toolchain (MSYS2 + libusb + cmake) travou em
instabilidade dos mirrors do MSYS2, e aquele código nunca chegou a rodar
contra hardware real. Python com `python-can` reusa uma pilha CAN madura
e testada, e é o que já validamos fiação/terminação/protocolo na bancada
— menor risco pra fechar o projeto no prazo. Nada impede portar pra C++
depois com mais tempo.

## Instalar

```
python -m venv .venv
.venv\Scripts\python.exe -m pip install -r requirements.txt
```

## Rodar

Fecha qualquer outro programa com o adaptador aberto (só um por vez pode
ter o device claimed).

```
.venv\Scripts\python.exe main.py
```

Comandos no terminal (digite e Enter):

| Comando    | Efeito                                                     |
|------------|---------------------------------------------------------------|
| `status`   | Snapshot de todos os nós já vistos (fase, mismatch, etc.)    |
| `reset`    | Broadcast `ID_RESET` (0x001) — tira os nós do fail-safe      |
| `failsafe` | Broadcast `ID_FAILSAFE` (0x000) — força fail-safe pra teste  |
| `sair`     | Encerra o programa                                            |

## O que a master faz sozinha

Escuta todo frame de status (`0x010..0x01F`) e mantém o último estado
conhecido de cada nó. **Watchdog de nó silencioso**: se um nó parar de
mandar status por mais de 3s (3x o período normal de 1s) sem ele mesmo
ter avisado fail-safe, a master assume que morreu silenciosamente
(crash, queda de energia, CAN travado) e broadcasta `ID_FAILSAFE` em
nome dele.

Ainda não é a lógica completa de controladora (coordenação de fases
entre semáforos, prioridade) — é a camada de comunicação + monitoramento.
A lógica de decisão entra depois disso estar validado com hardware.

## Protocolo CAN (tem que bater com `semaforo_idf/components/protocolo/`)

| ID              | Significado                                      |
|-----------------|---------------------------------------------------|
| `0x000`         | FAIL_SAFE — broadcast, qualquer nó pode mandar     |
| `0x001`         | RESET — broadcast, tira nós do fail-safe           |
| `0x010+node_id` | Status periódico (1x/s) de cada nó (8 bytes)       |

`StatusMsg` (8 bytes): `node_id(1) fase(1) tempo_na_fase_ms(4, LE) flags_mismatch(1) reserved(1)`.

Se o protocolo mudar no firmware, espelha a mudança em `protocolo.py`
aqui também — ver `docs/protocolo.md` na raiz do repo como fonte de
verdade entre os dois lados.
