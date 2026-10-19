# semaforo_idf

Semáforo inteligente — ESP32-S3, ESP-IDF, barramento CAN (TWAI, 500 kbps, TX=GPIO5 RX=GPIO4).

```
semaforo_idf/
├── CMakeLists.txt
├── sdkconfig.defaults
├── main/                      # só configuração do nó + app_main
│   └── main.cpp
└── components/
    ├── protocolo/include/     # IDs CAN, enum Fase, StatusMsg
    ├── can_bus/{include,src}  # camada fina sobre o driver TWAI
    └── semaforo/{include,src} # classe Semaforo (base p/ Pedestre)
```

Build: `idf.py set-target esp32s3 && idf.py build flash monitor`
Para o 2º semáforo: mude `NODE_ID` e `FASE_INICIAL` em `main/main.cpp`.
