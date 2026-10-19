# Projeto Integrativo — Semáforo Inteligente

Sistema de semáforo inteligente com nós ESP32-S3 se comunicando por
barramento CAN, e um controlador (master) rodando no PC.

```
.
├── docs/
│   └── protocolo.md        # fonte de verdade do protocolo CAN (IDs, StatusMsg)
├── legacy/                 # versão anterior: Ethernet/TCP/JSON, mantida só como histórico
│   ├── semaforo_tcp/
│   └── master_tcp/
├── semaforo_idf/            # firmware do nó — ESP-IDF, C++ OOP, barramento CAN (TWAI)
└── master_py/                # controlador PC — Python, python-can/gs_usb
```

## Arquitetura atual

- **Nó (`semaforo_idf/`)**: ESP32-S3 em ESP-IDF/FreeRTOS. Classe `Semaforo`
  (C++ OOP) controla o ciclo de fases, monitora a coerência entre o que
  manda acender e o que lê de volta nos pinos, e entra em fail-safe
  sozinho se detectar divergência. Comunica status e fail-safe pelo
  barramento CAN (TWAI, 500 kbps).
- **Master (`master_py/`)**: PC fala CAN com um adaptador USB-CAN
  (candleLight/gs_usb) via `python-can`. Escuta o status de todos os
  nós, detecta nós que pararam de responder (watchdog) e tem comandos
  manuais de reset/fail-safe.
- **`legacy/`**: a versão anterior do projeto, que usava Ethernet (W5500)
  + TCP + JSON (ArduinoJson) em vez de CAN. Mantida só como referência
  histórica — não é mais o caminho ativo.

Ver o `README.md` de cada pasta pra instruções de build/execução
específicas, e `docs/protocolo.md` pro protocolo CAN compartilhado entre
firmware e master.

## Próximos passos

Lógica de coordenação entre múltiplos semáforos numa encruzilhada
(controladora) ainda não implementada — por enquanto cada nó opera de
forma autônoma com fase inicial fixa e fail-safe local + propagado.
