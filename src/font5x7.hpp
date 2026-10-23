#pragma once
#include <stdint.h>

namespace font {
// Retorna 7 linhas de 5 bits (bit 4 = coluna da esquerda).
// Caractere desconhecido => retângulo.
const uint8_t* find(char c);
}
