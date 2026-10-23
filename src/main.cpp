#define SDL_MAIN_HANDLED
#if __has_include(<SDL2/SDL.h>)
#include <SDL2/SDL.h>
#else
#include <SDL.h>
#endif

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "config.hpp"
#include "gfx.hpp"
#include "pad.hpp"

using namespace gfx;

namespace {

constexpr int W = 1920;
constexpr int H = 1080;

const Color C_TOP    = {10, 4, 22, 255};
const Color C_BOT    = {2, 0, 8, 255};
const Color C_NAV    = {8, 3, 18, 240};
const Color C_PURPLE = {168, 85, 247, 255};
const Color C_DEEP   = {88, 28, 135, 255};
const Color C_TEXT   = {238, 230, 252, 255};
const Color C_DIM    = {150, 132, 182, 255};
const Color C_WARN   = {255, 130, 150, 255};
const Color C_PANEL  = {22, 10, 42, 255};

// ---------- marcadores e log (SEM funções de formatação) ----------
FILE* g_log = nullptr;

void touch(const char* path) {
    FILE* f = fopen(path, "w");
    if (f) fclose(f);
}

// "tag" precisa ser literal: o caminho é montado em tempo de compilação.
#define MARK(tag) touch("/data/purpleflix/m_" tag)

void logMsg(const char* s) {
    if (!g_log) return;
    fputs(s, g_log);
    fputc('\n', g_log);
    fflush(g_log);
}

// Junta até 3 textos em dst (sem printf).
void cat(char* dst, size_t n, const char* a, const char* b = "", const char* c = "") {
    if (n == 0) return;
    size_t len = 0;
    const char* parts[3] = {a, b, c};
    for (int i = 0; i < 3; ++i) {
        size_t pl = strlen(parts[i]);
        if (len + pl >= n) pl = n - 1 - len;
        memcpy(dst + len, parts[i], pl);
        len += pl;
    }
    dst[len] = 0;
}

// ---------- estado ----------
enum Tab { TAB_HOME, TAB_MOVIES, TAB_SERIES, TAB_SEARCH, TAB_INFO, TAB_COUNT };
const char* const kTabNames[TAB_COUNT] = {"Início", "Filmes", "Séries", "Busca", "Informações"};

struct Link {
    const char* label;
    const char* url;
};
const Link kLinks[3] = {
    {"Instagram", cfg::kInstagramUrl},
    {"TikTok", cfg::kTiktokUrl},
    {"GitHub", cfg::kGithubUrl},
};
constexpr int kLinkCount = 3;

struct App {
    Tab tab = TAB_HOME;
    bool inContent = false;
    int sel = 0;
    bool padOk = false;
    char toast1[160] = {0};
    char toast2[160] = {0};
    uint32_t toastUntil = 0;
};

struct Input {
    uint32_t held = 0, prev = 0, trig = 0;
    uint32_t nextRepeat[4] = {0, 0, 0, 0};
};

const uint32_t kDirs[4] = {pad::kUp, pad::kDown, pad::kLeft, pad::kRight};

void updateInput(Input& in, uint32_t now) {
    in.prev = in.held;
    in.held = pad::poll();
    uint32_t pressed = in.held & ~in.prev;
    in.trig = pressed & ~(pad::kUp | pad::kDown | pad::kLeft | pad::kRight);
    for (int i = 0; i < 4; ++i) {
        uint32_t b = kDirs[i];
        if (pressed & b) {
            in.trig |= b;
            in.nextRepeat[i] = now + 350;
        } else if ((in.held & b) && now >= in.nextRepeat[i]) {
            in.trig |= b;
            in.nextRepeat[i] = now + 110;
        }
    }
}

int itemCount(Tab t) { return t == TAB_INFO ? kLinkCount : 0; }

bool isPlaceholder(const char* url) { return strstr(url, "SEU_") != nullptr; }

void switchTab(App& a, int delta) {
    a.tab = (Tab)((a.tab + delta + TAB_COUNT) % TAB_COUNT);
    a.inContent = false;
    a.sel = 0;
}

void activate(App& a, uint32_t now) {
    if (a.tab != TAB_INFO) return;
    const Link& l = kLinks[a.sel];
    if (isPlaceholder(l.url)) {
        cat(a.toast1, sizeof a.toast1, "Link ainda não configurado (placeholder).");
        cat(a.toast2, sizeof a.toast2, "Edite ", l.label, " em src/config.hpp");
    } else {
        cat(a.toast1, sizeof a.toast1, "Abertura externa indisponível nesta versão.");
        cat(a.toast2, sizeof a.toast2, l.url);
    }
    a.toastUntil = now + 6000;
}

void handleInput(App& a, uint32_t trig, uint32_t now) {
    if (trig & pad::kR1) switchTab(a, +1);
    if (trig & pad::kL1) switchTab(a, -1);
    if (!a.inContent) {
        if (trig & pad::kRight) switchTab(a, +1);
        if (trig & pad::kLeft) switchTab(a, -1);
        if ((trig & pad::kDown) && itemCount(a.tab) > 0) {
            a.inContent = true;
            a.sel = 0;
        }
    } else {
        int n = itemCount(a.tab);
        if (trig & pad::kUp) {
            if (a.sel > 0) --a.sel;
            else a.inContent = false;
        }
        if ((trig & pad::kDown) && a.sel < n - 1) ++a.sel;
        if (trig & pad::kCircle) a.inContent = false;
        if (trig & pad::kCross) activate(a, now);
    }
}

// ---------- desenho ----------
void drawSplash(uint32_t t) {
    vgradient(0, 0, W, H, C_TOP, C_BOT);
    for (int i = 0; i < 9; ++i) {
        rect(360 + i * 50, 280 + i * 22, 1200 - i * 100, 520 - i * 44, Color{120, 40, 200, 10});
    }
    int a = t < 900 ? (int)(t * 255 / 900) : 255;
    const int s = 14;
    int w1 = textWidth("PURPLE", s);
    int w2 = textWidth("FLIX", s);
    int x0 = W / 2 - (w1 + s + w2) / 2;
    Color cp = C_PURPLE; cp.a = (uint8_t)a;
    Color ct = C_TEXT;   ct.a = (uint8_t)a;
    Color cd = C_DIM;    cd.a = (uint8_t)a;
    textShadow(x0, 380, "PURPLE", s, cp);
    textShadow(x0 + w1 + s, 380, "FLIX", s, ct);
    textCentered(W / 2, 530, cfg::kAppByline, 6, cd);
    rect(W / 2 - 300, 700, 600, 6, Color{40, 20, 70, 255});
    int prog = (int)(600 * (t > 3000 ? 3000 : t) / 3000);
    rect(W / 2 - 300, 700, prog, 6, C_PURPLE);
    textCentered(W / 2, 980, "X: pular", 2, C_DIM);
}

void drawNav(const App& a) {
    rect(0, 0, W, 130, C_NAV);
    rect(0, 130, W, 2, C_DEEP);
    int w1 = textWidth("PURPLE", 5);
    textShadow(80, 48, "PURPLE", 5, C_PURPLE);
    textShadow(80 + w1 + 5, 48, "FLIX", 5, C_TEXT);

    int x = 560;
    for (int i = 0; i < TAB_COUNT; ++i) {
        int tw = textWidth(kTabNames[i], 4);
        int pw = tw + 72;
        bool active = (a.tab == i);
        bool focused = active && !a.inContent;
        if (focused) {
            rect(x, 34, pw, 62, Color{168, 85, 247, 60});
            frame(x, 34, pw, 62, 3, C_PURPLE);
        }
        text(x + 36, 51, kTabNames[i], 4, active ? C_TEXT : C_DIM);
        if (active) rect(x + 20, 104, pw - 40, 5, C_PURPLE);
        x += pw + 16;
    }
}

void posterRow(int x, int y) {
    for (int i = 0; i < 7; ++i) {
        int px = x + i * 250;
        rect(px, y, 220, 330, C_PANEL);
        frame(px, y, 220, 330, 2, C_DEEP);
        textCentered(px + 110, y + 150, "Em breve", 2, C_DIM);
    }
}

void drawHome() {
    vgradient(100, 170, 1720, 330, Color{60, 18, 110, 255}, Color{14, 6, 30, 255});
    frame(100, 170, 1720, 330, 3, C_DEEP);
    text(150, 230, "Destaques", 3, C_PURPLE);
    textShadow(150, 280, "Seu catálogo aparece aqui", 7, C_TEXT);
    text(150, 370, "Etapa 1: interface e navegação. Sem dados reais ainda.", 3, C_DIM);
    text(100, 540, "Recomendados", 3, C_PURPLE);
    posterRow(100, 590);
}

void drawGrid(const char* title) {
    textShadow(100, 165, title, 6, C_TEXT);
    posterRow(100, 250);
    posterRow(100, 610);
}

void drawSearch() {
    textShadow(100, 165, "Busca", 6, C_TEXT);
    rect(100, 260, 1720, 90, C_PANEL);
    frame(100, 260, 1720, 90, 2, C_PURPLE);
    text(140, 285, "Digite um título...", 4, C_DIM);
    text(100, 400, "O teclado do sistema será integrado na próxima etapa.", 3, C_DIM);
}

void drawInfo(const App& a) {
    textShadow(100, 165, "Informações", 6, C_TEXT);
    rect(100, 222, 160, 6, C_PURPLE);

    rect(100, 250, 860, 630, C_PANEL);
    frame(100, 250, 860, 630, 2, C_DEEP);
    text(140, 275, "Desenvolvedor", 3, C_PURPLE);
    textShadow(140, 315, cfg::kDevName, 8, C_TEXT);
    for (int i = 0; i < cfg::kBioLineCount; ++i) {
        text(140, 395 + i * 34, cfg::kBioLines[i], 3, C_DIM);
    }
    text(140, 600, "Links", 3, C_PURPLE);
    for (int i = 0; i < kLinkCount; ++i) {
        int ry = 640 + i * 68;
        bool f = a.inContent && a.sel == i;
        if (f) {
            rect(140, ry, 780, 56, Color{168, 85, 247, 70});
            frame(140, ry, 780, 56, 3, C_PURPLE);
        } else {
            frame(140, ry, 780, 56, 1, C_DEEP);
        }
        text(164, ry + 14, kLinks[i].label, 4, f ? C_TEXT : C_DIM);
        text(420, ry + 21, kLinks[i].url, 2, isPlaceholder(kLinks[i].url) ? C_WARN : C_DIM);
    }

    rect(1000, 250, 820, 630, C_PANEL);
    frame(1000, 250, 820, 630, 2, C_DEEP);
    text(1040, 275, "Créditos", 3, C_PURPLE);
    text(1040, 315, cfg::kAppFullName, 3, C_TEXT);
    text(1040, 349, "Interface e código: Davitxtz", 3, C_DIM);
    text(1040, 383, "Catálogo online: em desenvolvimento", 3, C_DIM);
    text(1040, 440, "Versão instalada", 3, C_PURPLE);
    textShadow(1040, 478, cfg::kVersion, 6, C_TEXT);
    text(1040, 560, "Bibliotecas e projetos de terceiros", 3, C_PURPLE);
    text(1040, 600, "OpenOrbis PS4 Toolchain (GPL-3.0)", 3, C_DIM);
    text(1040, 634, "musl libc (MIT)", 3, C_DIM);
    text(1040, 668, "SDL2 (zlib)", 3, C_DIM);
    text(1040, 702, "PacBrew portlibs", 3, C_DIM);
    text(1040, 736, "Licenças completas: ver README", 3, C_DIM);
}

void drawFooter(const App& a, uint32_t now) {
    if (now < a.toastUntil) {
        rect(100, 900, 1720, 90, C_PANEL);
        frame(100, 900, 1720, 90, 2, C_PURPLE);
        text(130, 917, a.toast1, 3, C_TEXT);
        text(130, 953, a.toast2, 3, C_DIM);
    } else if (!a.padOk) {
        text(100, 930, "Controle não detectado. Veja /data/purpleflix/", 3, C_WARN);
    }
    rect(0, 1010, W, 70, C_NAV);
    text(60, 1034, "D-pad: navegar    X: selecionar    O: voltar    L1/R1: trocar aba", 2, C_DIM);
    char v[48];
    cat(v, sizeof v, "v", cfg::kVersion);
    text(W - 60 - textWidth(v, 2), 1034, v, 2, C_DIM);
}

void drawMain(const App& a, uint32_t now) {
    vgradient(0, 0, W, H, C_TOP, C_BOT);
    drawNav(a);
    switch (a.tab) {
        case TAB_HOME:   drawHome(); break;
        case TAB_MOVIES: drawGrid("Filmes"); break;
        case TAB_SERIES: drawGrid("Séries"); break;
        case TAB_SEARCH: drawSearch(); break;
        case TAB_INFO:   drawInfo(a); break;
        default: break;
    }
    drawFooter(a, now);
}

}  // namespace

int main(int, char**) {
    mkdir(cfg::kLogDir, 0777);
    MARK("00_main_iniciou");
    g_log = fopen(cfg::kLogPath, "w");
    logMsg("PurpleFlix iniciando");
    SDL_SetMainReady();
    MARK("01_set_main_ready");

    App app;
    MARK("02_antes_pad_init");
    app.padOk = pad::init();
    if (app.padOk) MARK("03_pad_ok"); else MARK("03_pad_falhou");

    MARK("04_antes_sdl_init");
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        MARK("ERRO_sdl_init");
        logMsg(SDL_GetError());
        return 1;
    }
    MARK("05_sdl_init_ok");

    MARK("06_antes_janela");
    SDL_Window* win = SDL_CreateWindow(cfg::kAppFullName, SDL_WINDOWPOS_UNDEFINED,
                                       SDL_WINDOWPOS_UNDEFINED, W, H, SDL_WINDOW_SHOWN);
    if (!win) {
        MARK("ERRO_janela");
        logMsg(SDL_GetError());
        return 1;
    }
    MARK("07_janela_ok");

    MARK("08_antes_renderer");
    SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (!ren) {
        MARK("ERRO_renderer");
        logMsg(SDL_GetError());
        return 1;
    }
    MARK("09_renderer_ok");
    gfx::init(ren);

    Input in;
    const uint32_t start = SDL_GetTicks();
    bool splash = true;
    bool running = true;
    bool firstFrame = true;
    bool menuMarked = false;

    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = false;
        }
        uint32_t now = SDL_GetTicks();
        updateInput(in, now);

        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);

        if (splash) {
            uint32_t t = now - start;
            if (t >= 3000 || (in.trig & (pad::kCross | pad::kOptions))) {
                splash = false;
                in.trig = 0;
            } else {
                drawSplash(t);
            }
        }
        if (!splash) {
            if (!menuMarked) {
                MARK("11_menu_principal");
                menuMarked = true;
            }
            handleInput(app, in.trig, now);
            drawMain(app, now);
        }
        SDL_RenderPresent(ren);
        if (firstFrame) {
            MARK("10_primeiro_quadro");
            // Teste isolado de snprintf (hipótese): só depois do 1º quadro.
            MARK("12_antes_snprintf");
            char tmp[16];
            snprintf(tmp, sizeof tmp, "%d", 42);
            MARK("13_snprintf_ok");
            firstFrame = false;
        }
        SDL_Delay(16);
    }

    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
