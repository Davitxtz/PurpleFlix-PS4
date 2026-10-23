
#pragma once

#include <stdint.h>

namespace pad {

constexpr uint32_t kUp      = 1u << 0;
constexpr uint32_t kDown    = 1u << 1;
constexpr uint32_t kLeft    = 1u << 2;
constexpr uint32_t kRight   = 1u << 3;
constexpr uint32_t kCross   = 1u << 4;
constexpr uint32_t kCircle  = 1u << 5;
constexpr uint32_t kL1      = 1u << 6;
constexpr uint32_t kR1      = 1u << 7;
constexpr uint32_t kOptions = 1u << 8;

bool init();
uint32_t poll();
void shutdown();

}
