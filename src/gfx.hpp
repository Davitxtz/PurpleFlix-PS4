#pragma once
#include <stdint.h>

struct SDL_Renderer;

struct Color {
    uint8_t r, g, b, a;
};

namespace gfx {

void init(SDL_Renderer* r);
void rect(int x, int y, int w, int h, Color c);
void frame(int x, int y, int w, int h, int thick, Color c);
void vgradient(int x, int y, int w, int h, Color top, Color bottom);

// Texto UTF-8 (acentos do português). 'scale' = tamanho do pixel da fonte 5x7.
int textWidth(const char* utf8, int scale);
void text(int x, int y, const char* utf8, int scale, Color c);
void textShadow(int x, int y, const char* utf8, int scale, Color c);
void textCentered(int cx, int y, const char* utf8, int scale, Color c);

}  // namespace gfx
