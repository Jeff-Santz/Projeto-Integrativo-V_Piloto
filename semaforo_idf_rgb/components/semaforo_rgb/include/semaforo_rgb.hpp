#pragma once

#include "semaforo.hpp"
#include "led_strip.h"

// Variante de teste: usa o LED RGB enderecavel embutido na placa (WS2812,
// controlado via RMT) em vez de 3 LEDs discretos ligados por fio. Serve
// pra colocar um segundo no na rede CAN sem montar fiacao nova.
//
// Sem pino de leitura de volta -> verificarCoerencia() sempre retorna "ok"
// (nunca entra em fail-safe por conta propria, por decisao explicita, nao
// por limitacao escondida). Esse no so entra em fail-safe se outro no do
// barramento avisar, ou por comando manual da master.
class SemaforoRGB : public Semaforo {
public:
    SemaforoRGB(uint8_t node_id, gpio_num_t pino_rgb,
                const TemposSemaforo& tempos, can_proto::Fase fase_inicial);

protected:
    void configurarGpios() override;
    void aplicarFase(can_proto::Fase fase) override;
    void apagarTudo() override;
    bool verificarCoerencia(can_proto::Fase fase, uint8_t& flags_mismatch) override;

private:
    gpio_num_t pino_rgb_;
    led_strip_handle_t strip_ = nullptr;
};
