#pragma once
#include <stdint.h>

struct SDL_Renderer;
struct SDL_Surface;

struct Color {
    uint8_t r, g, b, a;
};

namespace gfx {

void init(SDL_Renderer* r);

// Se 's' != nullptr, todo o desenho vai direto para os pixels dessa superficie de 32 bits
// (usado pelo player). Cores com transparencia sao escurecidas contra preto. nullptr volta ao renderizador.
void setSurface(SDL_Surface* s);

void rect(int x, int y, int w, int h, Color c);
void frame(int x, int y, int w, int h, int thick, Color c);
void vgradient(int x, int y, int w, int h, Color top, Color bottom);

// Texto UTF-8 (acentos do portugues). 'scale' = tamanho do pixel da fonte 5x7.
int textWidth(const char* utf8, int scale);
void text(int x, int y, const char* utf8, int scale, Color c);
void textShadow(int x, int y, const char* utf8, int scale, Color c);
void textCentered(int cx, int y, const char* utf8, int scale, Color c);

}  // namespace gfx
