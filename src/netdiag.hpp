#pragma once
#include <stdint.h>

namespace netdiag {

bool active();                // a tela de diagnóstico está aberta?
void open();                  // abre a tela
void handle(uint32_t trig);   // entrada do controle (botões recém-pressionados)
void draw();                  // desenha a tela
void runPending();            // chamar DEPOIS de apresentar o quadro: roda o teste, se pedido

}  // namespace netdiag
