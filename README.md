# Projeto Integrativo — Semáforo Inteligente com CAN Bus

Sistema de semáforo inteligente para controle de tráfego, com nós embarcados (ESP32-S3) comunicando por barramento CAN e um controlador supervisório rodando no PC.

## Visão geral

Cada semáforo físico é controlado por um **nó** (ESP32-S3 rodando ESP-IDF/FreeRTOS), que:
- executa o ciclo de fases (verde → amarelo → vermelho) com temporização própria;
- **verifica a si mesmo**: lê de volta, via GPIO, qual lâmpada está realmente acesa, e compara com a fase que ele mesmo mandou acender;
- se detectar uma divergência (ex: mandou acender o verde, mas o pino de entrada não confirma), entra em **modo de falha segura (fail-safe)** e avisa todo o barramento;
- reporta seu status (fase atual, tempo na fase, flags de erro) periodicamente pela rede.

Um **controlador (master)** no PC escuta esse barramento, monitora todos os nós, detecta falhas silenciosas (um nó que parou de responder sem avisar) e pode emitir comandos manuais de reset ou fail-safe.

## Por que CAN bus?

CAN (Controller Area Network) é o protocolo usado em automação industrial e automotiva para conectar vários dispositivos num único par de fios, com garantias que fazem sentido pra um sistema de segurança como um semáforo:

- **Broadcast nativo**: toda mensagem enviada é recebida por todos os nós no barramento. Não existe "endereço de destino" — é perfeito para um alerta de falha que precisa chegar em todo mundo ao mesmo tempo.
- **Arbitragem por prioridade, sem colisão**: se dois nós transmitem ao mesmo tempo, o frame com o **ID numericamente menor** vence automaticamente (CAN usa a lógica de barramento dominante/recessivo bit a bit), sem precisar de um nó mestre decidindo quem fala. Por isso o ID de fail-safe (`0x000`) é o menor ID possível do protocolo: ele sempre ganha a disputa do barramento.
- **Detecção de erro embutida**: cada frame exige confirmação (ACK) de pelo menos um outro nó; se não houver, o transmissor assume que está isolado e entra em modo de recuperação (bus-off) — o próprio hardware já ajuda a detectar problema de fiação.
- **Robustez elétrica**: usa um par diferencial (CAN-H / CAN-L) terminado por resistores de 120Ω em cada ponta física do barramento, o que o torna resistente a ruído — importante em um ambiente com motores, relés, etc., como seria um cruzamento real.

Nesse projeto o barramento roda a **500 kbps**, usando o controlador TWAI (Two-Wire Automotive Interface) embutido no ESP32-S3.

## Arquitetura

```
.
├── docs/
│   └── protocolo.md        # fonte de verdade do protocolo CAN (IDs, StatusMsg)
├── legacy/                  # versão anterior do projeto (Ethernet/TCP/JSON) — histórico
│   ├── semaforo_tcp/
│   └── master_tcp/
├── semaforo_idf/             # firmware do nó — ESP-IDF, C++ OOP, barramento CAN (TWAI)
├── master_py/                 # controlador PC — Python, python-can + adaptador USB-CAN
└── master_idf/                # tentativa de controlador em C++/libusb — histórica, não finalizada
```

### Nó (`semaforo_idf/`)

Implementado em C++ orientado a objetos sobre ESP-IDF/FreeRTOS. A classe `Semaforo` recebe, no construtor, o `node_id`, o mapeamento de pinos e a **fase inicial** — o que permite que dois semáforos num mesmo cruzamento, com timers idênticos, comecem em fases opostas (um verde enquanto o outro está vermelho), sem precisar de nenhuma coordenação externa.

Cada nó roda três tarefas (FreeRTOS tasks) em paralelo:
- **rotina**: avança o ciclo de fases e aciona os LEDs de saída;
- **status**: transmite seu `StatusMsg` pelo CAN a cada ~1s;
- **escuta**: recebe frames do barramento (fail-safe de outros nós, comandos de reset).

A cada checagem, o nó compara a fase que mandou acender com o que lê de volta nos pinos de entrada (loopback de verificação). Se a leitura não bater, ele mesmo entra em fail-safe e propaga isso pro barramento — e, com a fiação correta, consegue **se recuperar automaticamente** depois de algumas leituras consecutivas confirmando que voltou ao normal.

### Controlador (`master_py/`)

Script Python que fala com o barramento CAN através de um adaptador USB-CAN (protocolo candleLight/gs_usb), usando `python-can`. Ele:
- escuta passivamente o status de todos os nós;
- mantém um **watchdog**: se um nó parar de mandar status por mais de 3 segundos sem ter avisado fail-safe (ex: crash, queda de energia, CAN travado), a master assume a falha em nome dele e broadcasta fail-safe pra rede inteira;
- permite comandos manuais via terminal (`status`, `reset`, `failsafe`, `sair`).

Essa é a camada de comunicação e monitoramento — a lógica de **coordenação entre múltiplos semáforos** de um cruzamento (quem deve estar verde quando, prioridades) ainda é o próximo passo do projeto.

### `legacy/`

Versão anterior do sistema, que usava Ethernet (módulo W5500) + TCP + JSON (ArduinoJson) em vez de CAN. Mantida só como histórico — não é mais o caminho ativo, mas documenta a evolução do projeto.

## Protocolo CAN (resumo — detalhes em `docs/protocolo.md`)

| ID (hex)        | Significado                          | Quem manda |
|------------------|----------------------------------------|-------------|
| `0x000`          | `FAIL_SAFE` — menor ID, sempre vence a arbitragem | qualquer nó, ou a master |
| `0x001`          | `RESET` — tira a rede do fail-safe     | a master, ou um nó se recuperando |
| `0x010`–`0x01F`  | status periódico de cada nó (8 bytes)  | cada nó, 1x/s |

## Status atual

- Firmware do nó validado em bancada: ciclo de fases, verificação de coerência, fail-safe por broadcast e auto-recuperação funcionando com hardware real.
- Master em Python validado em bancada: recebe status em tempo real, comandos de reset/fail-safe testados contra o nó físico, com o semáforo reagindo corretamente nos dois sentidos.
- Watchdog de nó silencioso implementado, ainda não testado em bancada (próximo item).
- Lógica de coordenação entre semáforos (a "controladora" de verdade) ainda não implementada.

Ver o `README.md` de cada subpasta para instruções específicas de build/execução.