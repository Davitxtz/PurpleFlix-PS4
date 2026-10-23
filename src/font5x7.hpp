
#pragma once

#include <stdint.h>

namespace font5x7 {

// Cada caractere ocupa 5 colunas e 7 linhas.
// Um bit ligado representa um pixel aceso.
const uint8_t* glyph(char c);

}
