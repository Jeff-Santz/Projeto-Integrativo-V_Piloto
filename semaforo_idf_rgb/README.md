# semaforo_idf_rgb

Variante **de teste** do semáforo: usa o LED RGB embutido (WS2812, GPIO48) em vez dos 3 LEDs discretos. Serve pra ter um 2º nó na bancada sem montar fiação.

Não duplica código: reaproveita `semaforo`, `can_bus` e `protocolo` de `../semaforo_idf/components` via `EXTRA_COMPONENT_DIRS`. Só `components/semaforo_rgb` e o `main/main.cpp` são daqui.

```
semaforo_idf_rgb/
├── CMakeLists.txt             # aponta pros componentes do semaforo_idf
├── sdkconfig.defaults
├── main/main.cpp              # NODE_ID / FASE_INICIAL desta placa de teste
└── components/semaforo_rgb/   # SemaforoRGB (herda de Semaforo) + dependência led_strip
```

Build: `idf.py set-target esp32s3 && idf.py build flash monitor`

**Limite:** sem pino de leitura de volta, este nó nunca detecta mismatch próprio. Ele só exercita o protocolo (SYNC, propagação de fail-safe, watchdogs), não valida fiação.
