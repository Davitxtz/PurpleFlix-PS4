#define SDL_MAIN_HANDLED
#if __has_include(<SDL2/SDL.h>)
#include <SDL2/SDL.h>
#else
#include <SDL.h>
#endif

#include "font5x7.hpp"
#include "gfx.hpp"

namespace {

SDL_Renderer* g_r = nullptr;
SDL_Surface* g_surf = nullptr;

enum Accent { A_NONE, A_ACUTE, A_GRAVE, A_CIRC, A_TILDE, A_CEDIL };
struct Ch {
    char base;
    Accent acc;
};
struct MapEntry {
    unsigned cp;
    char base;
    Accent acc;
};

const MapEntry kMap[] = {
    {0xC0, 'A', A_GRAVE}, {0xC1, 'A', A_ACUTE}, {0xC2, 'A', A_CIRC},
    {0xC3, 'A', A_TILDE}, {0xC7, 'C', A_CEDIL}, {0xC9, 'E', A_ACUTE},
    {0xCA, 'E', A_CIRC},  {0xCD, 'I', A_ACUTE}, {0xD3, 'O', A_ACUTE},
    {0xD4, 'O', A_CIRC},  {0xD5, 'O', A_TILDE}, {0xDA, 'U', A_ACUTE},
    {0xE0, 'a', A_GRAVE}, {0xE1, 'a', A_ACUTE}, {0xE2, 'a', A_CIRC},
    {0xE3, 'a', A_TILDE}, {0xE7, 'c', A_CEDIL}, {0xE9, 'e', A_ACUTE},
    {0xEA, 'e', A_CIRC},  {0xED, 'i', A_ACUTE}, {0xF3, 'o', A_ACUTE},
    {0xF4, 'o', A_CIRC},  {0xF5, 'o', A_TILDE}, {0xFA, 'u', A_ACUTE},
};

bool nextChar(const unsigned char*& p, Ch& out) {
    unsigned c = *p;
    if (!c) return false;
    if (c < 0x80) {
        out = {(char)c, A_NONE};
        ++p;
        return true;
    }
    if ((c & 0xE0) == 0xC0 && p[1]) {
        unsigned cp = ((c & 0x1F) << 6) | (p[1] & 0x3F);
        p += 2;
        out = {'?', A_NONE};
        for (const MapEntry& m : kMap) {
            if (m.cp == cp) {
                out = {m.base, m.acc};
                break;
            }
        }
        return true;
    }
    int n = ((c & 0xF0) == 0xE0) ? 3 : ((c & 0xF8) == 0xF0) ? 4 : 1;
    for (int i = 0; i < n && *p; ++i) ++p;
    out = {'?', A_NONE};
    return true;
}

inline bool isLower(char b) { return b >= 'a' && b <= 'z'; }

int pixelSize(char b, int s) {
    if (isLower(b)) {
        int p = s - s / 4;  // minuscula = versalete (75%)
        return p < 1 ? 1 : p;
    }
    return s;
}

int advance(char b, int s) { return 6 * pixelSize(b, s); }

void drawChar(int x, int y, const Ch& ch, int s, Color col) {
    char b = ch.base;
    int ps = pixelSize(b, s);
    int oy = y;
    if (isLower(b)) {
        oy = y + (7 * s - 7 * ps);  // alinha na linha de base
        b = (char)(b - 'a' + 'A');
    }
    const uint8_t* g = font::find(b);
    for (int row = 0; row < 7; ++row) {
        for (int c = 0; c < 5; ++c) {
            if (g[row] & (0x10 >> c)) gfx::rect(x + c * ps, oy + row * ps, ps, ps, col);
        }
    }
    auto dot = [&](int cx, int cy) { gfx::rect(x + cx * ps, oy + cy * ps, ps, ps, col); };
    switch (ch.acc) {
        case A_ACUTE: dot(3, -2); dot(2, -1); break;
        case A_GRAVE: dot(1, -2); dot(2, -1); break;
        case A_CIRC:  dot(2, -2); dot(1, -1); dot(3, -1); break;
        case A_TILDE: dot(1, -1); dot(2, -2); dot(3, -1); dot(4, -2); break;
        case A_CEDIL: dot(2, 7); dot(1, 8); break;
        default: break;
    }
}

// Preenche um retangulo direto na superficie (32 bits por pixel).
void fillSurface(int x, int y, int w, int h, Color c) {
    if (!g_surf || !g_surf->pixels || g_surf->format->BytesPerPixel != 4) return;
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w > g_surf->w ? g_surf->w : x + w;
    int y1 = y + h > g_surf->h ? g_surf->h : y + h;
    if (x0 >= x1 || y0 >= y1) return;
    // Transparencia: escurece contra preto (rapido, e identico sobre fundo preto).
    Uint8 r = (Uint8)((int)c.r * c.a / 255);
    Uint8 g = (Uint8)((int)c.g * c.a / 255);
    Uint8 b = (Uint8)((int)c.b * c.a / 255);
    Uint32 px = SDL_MapRGB(g_surf->format, r, g, b);
    for (int j = y0; j < y1; ++j) {
        Uint32* row = (Uint32*)((Uint8*)g_surf->pixels + (size_t)j * g_surf->pitch) + x0;
        for (int i = 0; i < x1 - x0; ++i) row[i] = px;
    }
}

}  // namespace

void gfx::init(SDL_Renderer* r) {
    g_r = r;
    SDL_SetRenderDrawBlendMode(g_r, SDL_BLENDMODE_BLEND);
}

void gfx::setSurface(SDL_Surface* s) { g_surf = s; }

void gfx::rect(int x, int y, int w, int h, Color c) {
    if (g_surf) {
        fillSurface(x, y, w, h, c);
        return;
    }
    SDL_SetRenderDrawColor(g_r, c.r, c.g, c.b, c.a);
    SDL_Rect rc = {x, y, w, h};
    SDL_RenderFillRect(g_r, &rc);
}

void gfx::frame(int x, int y, int w, int h, int t, Color c) {
    rect(x, y, w, t, c);
    rect(x, y + h - t, w, t, c);
    rect(x, y, t, h, c);
    rect(x + w - t, y, t, h, c);
}

void gfx::vgradient(int x, int y, int w, int h, Color a, Color b) {
    const int band = 8;
    for (int yy = 0; yy < h; yy += band) {
        int bh = (yy + band > h) ? (h - yy) : band;
        int t = (h > 1) ? (yy * 255) / (h - 1) : 0;
        Color c = {(uint8_t)(a.r + ((b.r - a.r) * t) / 255),
                   (uint8_t)(a.g + ((b.g - a.g) * t) / 255),
                   (uint8_t)(a.b + ((b.b - a.b) * t) / 255), 255};
        rect(x, y + yy, w, bh, c);
    }
}

int gfx::textWidth(const char* utf8, int s) {
    const unsigned char* p = (const unsigned char*)utf8;
    Ch ch;
    int w = 0;
    while (nextChar(p, ch)) w += advance(ch.base, s);
    return w > s ? w - s : w;
}

void gfx::text(int x, int y, const char* utf8, int s, Color c) {
    const unsigned char* p = (const unsigned char*)utf8;
    Ch ch;
    while (nextChar(p, ch)) {
        drawChar(x, y, ch, s, c);
        x += advance(ch.base, s);
    }
}

void gfx::textShadow(int x, int y, const char* utf8, int s, Color c) {
    int d = s / 2 < 2 ? 2 : s / 2;
    text(x + d, y + d, utf8, s, Color{0, 0, 0, 170});
    text(x, y, utf8, s, c);
}

void gfx::textCentered(int cx, int y, const char* utf8, int s, Color c) {
    text(cx - textWidth(utf8, s) / 2, y, utf8, s, c);
}
