#pragma once
#include <stdint.h>

namespace pad {

// Máscaras de botão do DualShock 4 (padrão ScePad).
constexpr uint32_t kL3       = 0x00000002;
constexpr uint32_t kR3       = 0x00000004;
constexpr uint32_t kOptions  = 0x00000008;
constexpr uint32_t kUp       = 0x00000010;
constexpr uint32_t kRight    = 0x00000020;
constexpr uint32_t kDown     = 0x00000040;
constexpr uint32_t kLeft     = 0x00000080;
constexpr uint32_t kL2       = 0x00000100;
constexpr uint32_t kR2       = 0x00000200;
constexpr uint32_t kL1       = 0x00000400;
constexpr uint32_t kR1       = 0x00000800;
constexpr uint32_t kTriangle = 0x00001000;
constexpr uint32_t kCircle   = 0x00002000;
constexpr uint32_t kCross    = 0x00004000;
constexpr uint32_t kSquare   = 0x00008000;

bool init();        // true se o controle foi aberto
uint32_t poll();    // botões pressionados agora (0 se sem controle)

}  // namespace pad
