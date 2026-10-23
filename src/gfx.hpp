
#pragma once

#include <SDL.h>
#include <stdint.h>

namespace gfx {

struct Color {
    uint8_t r, g, b, a;
};

void init(SDL_Renderer* renderer);

void rect(int x, int y, int w, int h, Color color);
void frame(int x, int y, int w, int h, int thickness, Color color);

void vgradient(int x, int y, int w, int h,
               Color top, Color bottom);

void text(int x, int y, const char* value,
          int scale, Color color);

void textCentered(int centerX, int y, const char* value,
                  int scale, Color color);

void textShadow(int x, int y, const char* value,
                int scale, Color color);

int textWidth(const char* value, int scale);

}
