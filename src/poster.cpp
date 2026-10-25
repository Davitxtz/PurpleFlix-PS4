#define SDL_MAIN_HANDLED
#if __has_include(<SDL2/SDL.h>)
#include <SDL2/SDL.h>
#else
#include <SDL.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include <stb/stb_image.h>

#include "net.hpp"
#include "poster.hpp"

namespace {

constexpr int kSlots = 256;
constexpr int kMaxTextures = 36;
constexpr size_t kImgCap = 300 * 1024;
const char* const kBase = "https://image.tmdb.org/t/p/w185";

enum State { S_NONE = 0, S_LOADED = 1, S_FAILED = 2 };

struct Slot {
    char path[40];
    SDL_Texture* tex;
    uint32_t wantFrame;
    uint32_t lastUse;
    int state;
};

Slot g_slots[kSlots];
SDL_Renderer* g_ren = nullptr;
uint32_t g_frame = 0;
int g_loaded = 0;
char g_img[kImgCap];

void evictOne(uint32_t cur) {
    int best = -1;
    uint32_t bestUse = 0xFFFFFFFFu;
    for (int i = 0; i < kSlots; ++i) {
        const Slot& s = g_slots[i];
        if (s.state == S_LOADED && s.tex && s.lastUse < cur && s.lastUse < bestUse) {
            best = i;
            bestUse = s.lastUse;
        }
    }
    if (best >= 0) {
        SDL_DestroyTexture(g_slots[best].tex);
        g_slots[best].tex = nullptr;
        g_slots[best].state = S_NONE;
        --g_loaded;
    }
}

}  // namespace

void poster::init(SDL_Renderer* r) {
    g_ren = r;
    for (int i = 0; i < kSlots; ++i) {
        g_slots[i].path[0] = 0;
        g_slots[i].tex = nullptr;
        g_slots[i].wantFrame = 0xFFFFFFFFu;
        g_slots[i].lastUse = 0;
        g_slots[i].state = S_FAILED;  // sem caminho = nada a carregar
    }
    g_frame = 0;
    g_loaded = 0;
}

void poster::setPath(int id, const char* path) {
    if (id < 0 || id >= kSlots) return;
    Slot& s = g_slots[id];
    if (!path || !path[0]) {
        if (s.tex) {
            SDL_DestroyTexture(s.tex);
            s.tex = nullptr;
            --g_loaded;
        }
        s.path[0] = 0;
        s.state = S_FAILED;
        return;
    }
    if (strcmp(s.path, path) == 0) return;
    if (s.tex) {
        SDL_DestroyTexture(s.tex);
        s.tex = nullptr;
        --g_loaded;
    }
    snprintf(s.path, sizeof s.path, "%s", path);
    s.state = S_NONE;
}

bool poster::tryDraw(int id, int x, int y, int w, int h) {
    if (!g_ren || id < 0 || id >= kSlots) return false;
    Slot& s = g_slots[id];
    s.wantFrame = g_frame;
    if (s.state == S_LOADED && s.tex) {
        s.lastUse = g_frame;
        SDL_Rect d = {x, y, w, h};
        SDL_RenderCopy(g_ren, s.tex, nullptr, &d);
        return true;
    }
    return false;
}

void poster::loadOne() {
    if (!g_ren) return;
    uint32_t cur = g_frame++;

    int pick = -1;
    for (int i = 0; i < kSlots; ++i) {
        if (g_slots[i].state == S_NONE && g_slots[i].path[0] && g_slots[i].wantFrame == cur) {
            pick = i;
            break;
        }
    }
    if (pick < 0) return;
    Slot& s = g_slots[pick];

    if (g_loaded >= kMaxTextures) evictOne(cur);
    if (g_loaded >= kMaxTextures) return;  // todos estão na tela: tenta depois

    int ierr = 0;
    const char* istage = "";
    if (!net::init(&ierr, &istage)) {
        s.state = S_FAILED;
        return;
    }

    char url[160];
    snprintf(url, sizeof url, "%s%s", kBase, s.path);
    net::Result r = net::get(url, g_img, kImgCap, false);
    if (!r.ok || r.status != 200 || r.bytes == 0 || r.truncated) {
        s.state = S_FAILED;
        return;
    }

    int w = 0, h = 0, n = 0;
    unsigned char* px = stbi_load_from_memory((const unsigned char*)g_img, (int)r.bytes, &w, &h, &n, 4);
    if (!px) {
        s.state = S_FAILED;
        return;
    }
    SDL_Surface* surf = SDL_CreateRGBSurfaceFrom(px, w, h, 32, w * 4, 0x000000FF, 0x0000FF00,
                                                 0x00FF0000, 0xFF000000);
    if (surf) {
        s.tex = SDL_CreateTextureFromSurface(g_ren, surf);
        SDL_FreeSurface(surf);
    }
    stbi_image_free(px);
    if (s.tex) {
        s.state = S_LOADED;
        s.lastUse = cur;
        ++g_loaded;
    } else {
        s.state = S_FAILED;
    }
}
