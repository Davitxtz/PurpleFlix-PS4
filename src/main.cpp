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
#include "netdiag.hpp"

using namespace gfx;

namespace {

constexpr int W = 1920;
constexpr int H = 1080;

const Color C_TOP    = {10, 4, 22, 255};
const Color C_BOT    = {2, 0, 8, 255};
const Color C_NAV    = {8, 3, 18, 240};
const Color C_PURPLE = {168, 85, 247, 255};
const Color C_BRIGHT = {200, 140, 255, 255};
const Color C_DEEP   = {88, 28, 135, 255};
const Color C_TEXT   = {238, 230, 252, 255};
const Color C_DIM    = {150, 132, 182, 255};
const Color C_WARN   = {255, 130, 150, 255};
const Color C_PANEL  = {22, 10, 42, 255};

// ---------- log ----------
FILE* g_log = nullptr;

void logInit() {
    mkdir(cfg::kLogDir, 0777);
    g_log = fopen(cfg::kLogPath, "w");
}

void logMsg(const char* s) {
    if (!g_log) return;
    fputs(s, g_log);
    fputc('\n', g_log);
    fflush(g_log);
}

// ---------- catálogo de EXEMPLO (dados locais, não são reais) ----------
constexpr int kMovieCount = 14;
constexpr int kSeriesCount = 14;
constexpr int kTitleCount = kMovieCount + kSeriesCount;

struct Title {
    char name[40];
    const char* kind;
    const char* genre;
    char year[8];
};
Title g_titles[kTitleCount];

const char* const kGenres[7] = {"Ação", "Drama", "Comédia", "Ficção", "Terror", "Aventura", "Animação"};
const char* const kSynopsis =
    "Sinopse de exemplo. O catálogo real, com capas e sinopses da internet, "
    "será carregado em uma próxima etapa.";

void initCatalog() {
    for (int i = 0; i < kTitleCount; ++i) {
        Title& t = g_titles[i];
        bool series = i >= kMovieCount;
        int n = series ? (i - kMovieCount + 1) : (i + 1);
        t.kind = series ? "Série" : "Filme";
        snprintf(t.name, sizeof t.name, "%s de exemplo %02d", t.kind, n);
        t.genre = kGenres[i % 7];
        snprintf(t.year, sizeof t.year, "%d", 2012 + (i * 3) % 13);
    }
}

Color posterTop(int id) {
    return Color{(uint8_t)(70 + (id * 37) % 110), (uint8_t)(24 + (id * 23) % 40),
                 (uint8_t)(120 + (id * 53) % 110), 255};
}

Color posterBottom(int id) {
    Color c = posterTop(id);
    return Color{(uint8_t)(c.r / 4), (uint8_t)(c.g / 4), (uint8_t)(c.b / 4), 255};
}

// Escreve texto com quebra de linha por palavras. Retorna o número de linhas.
int drawWrapped(int x, int y, int maxW, const char* s, int scale, Color c, int maxLines) {
    const int lh = 9 * scale;
    const char* p = s;
    int lines = 0;
    char line[128];
    char trial[128];
    while (*p && lines < maxLines) {
        int len = 0;
        line[0] = 0;
        while (*p) {
            const char* q = p;
            while (*q && *q != ' ') ++q;
            int wlen = (int)(q - p);
            int tl = len ? len + 1 : 0;
            if (tl + wlen >= (int)sizeof trial - 1) break;
            memcpy(trial, line, len);
            if (len) trial[len] = ' ';
            memcpy(trial + tl, p, wlen);
            trial[tl + wlen] = 0;
            if (len && textWidth(trial, scale) > maxW) break;
            memcpy(line, trial, tl + wlen + 1);
            len = tl + wlen;
            p = q;
            while (*p == ' ') ++p;
        }
        if (len == 0) {  // palavra enorme: corta
            int n = 0;
            while (p[n] && n < 20) ++n;
            memcpy(line, p, n);
            line[n] = 0;
            p += n;
        }
        text(x, y + lines * lh, line, scale, c);
        ++lines;
    }
    return lines;
}

// ---------- busca ----------
char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

bool containsCI(const char* hay, const char* needle) {
    if (!*needle) return true;
    for (const char* h = hay; *h; ++h) {
        const char* a = h;
        const char* b = needle;
        while (*a && *b && lower(*a) == lower(*b)) {
            ++a;
            ++b;
        }
        if (!*b) return true;
    }
    return false;
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

constexpr int kKeyRows = 4;
const char* const kKeyLabels[kKeyRows][10] = {
    {"A", "B", "C", "D", "E", "F", "G", "H", "I", "J"},
    {"K", "L", "M", "N", "O", "P", "Q", "R", "S", "T"},
    {"U", "V", "W", "X", "Y", "Z", "0", "1", "2", "3"},
    {"4", "5", "6", "7", "8", "9", "ESPAÇO", "APAGAR", nullptr, nullptr},
};
const int kKeyCols[kKeyRows] = {10, 10, 10, 8};
constexpr int kMaxResults = 7;

struct App {
    Tab tab = TAB_HOME;
    bool inContent = false;
    int row = 0, col = 0;
    int detailId = -1;
    int detailBtn = 0;
    char query[28] = {0};
    int qlen = 0;
    int res[kMaxResults] = {0};
    int nres = 0;
    int totalRes = 0;
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

bool isPlaceholder(const char* url) { return strstr(url, "SEU_") != nullptr; }

void updateSearch(App& a) {
    a.nres = 0;
    a.totalRes = 0;
    if (a.qlen == 0) return;
    for (int i = 0; i < kTitleCount; ++i) {
        if (containsCI(g_titles[i].name, a.query)) {
            if (a.nres < kMaxResults) a.res[a.nres++] = i;
            ++a.totalRes;
        }
    }
}

int homeId(int i) {
    static const int ids[7] = {0, 14, 1, 15, 2, 16, 3};
    return ids[i];
}

int rowCount(const App& a) {
    switch (a.tab) {
        case TAB_HOME:   return 1;
        case TAB_MOVIES:
        case TAB_SERIES: return 2;
        case TAB_SEARCH: return kKeyRows + (a.nres > 0 ? 1 : 0);
        case TAB_INFO:   return kLinkCount;
        default:         return 0;
    }
}

int colCount(const App& a, int row) {
    switch (a.tab) {
        case TAB_HOME:
        case TAB_MOVIES:
        case TAB_SERIES: return 7;
        case TAB_SEARCH: return row < kKeyRows ? kKeyCols[row] : a.nres;
        case TAB_INFO:   return 1;
        default:         return 0;
    }
}

int posterId(const App& a, int row, int col) {
    if (a.tab == TAB_HOME) return homeId(col);
    int base = (a.tab == TAB_SERIES) ? kMovieCount : 0;
    return base + row * 7 + col;
}

void clampFocus(App& a) {
    int rc = rowCount(a);
    if (a.row > rc - 1) a.row = rc - 1;
    if (a.row < 0) a.row = 0;
    int cc = colCount(a, a.row);
    if (a.col > cc - 1) a.col = cc - 1;
    if (a.col < 0) a.col = 0;
}

void switchTab(App& a, int delta) {
    a.tab = (Tab)((a.tab + delta + TAB_COUNT) % TAB_COUNT);
    a.inContent = false;
    a.row = 0;
    a.col = 0;
}

void toast(App& a, const char* l1, const char* l2, uint32_t now) {
    snprintf(a.toast1, sizeof a.toast1, "%s", l1);
    snprintf(a.toast2, sizeof a.toast2, "%s", l2);
    a.toastUntil = now + 5000;
}

void appendChar(App& a, char c) {
    if (a.qlen < (int)sizeof a.query - 1) {
        a.query[a.qlen++] = c;
        a.query[a.qlen] = 0;
        updateSearch(a);
        clampFocus(a);
    }
}

void backspace(App& a) {
    if (a.qlen > 0) {
        a.query[--a.qlen] = 0;
        updateSearch(a);
        clampFocus(a);
    }
}

void pressKey(App& a) {
    const char* k = kKeyLabels[a.row][a.col];
    if (!k) return;
    if (k[1] == 0) appendChar(a, k[0]);
    else if (strcmp(k, "APAGAR") == 0) backspace(a);
    else appendChar(a, ' ');
}

void activate(App& a, uint32_t now) {
    switch (a.tab) {
        case TAB_INFO: {
            const Link& l = kLinks[a.row];
            if (isPlaceholder(l.url)) {
                char msg[96];
                snprintf(msg, sizeof msg, "Edite %s em src/config.hpp", l.label);
                toast(a, "Link ainda não configurado (placeholder).", msg, now);
            } else {
                toast(a, "Abertura externa indisponível nesta versão.", l.url, now);
            }
            break;
        }
        case TAB_HOME:
        case TAB_MOVIES:
        case TAB_SERIES:
            a.detailId = posterId(a, a.row, a.col);
            a.detailBtn = 0;
            break;
        case TAB_SEARCH:
            if (a.row < kKeyRows) {
                pressKey(a);
            } else if (a.col < a.nres) {
                a.detailId = a.res[a.col];
                a.detailBtn = 0;
            }
            break;
        default:
            break;
    }
}

void handleInput(App& a, uint32_t trig, uint32_t now) {
    if (a.detailId >= 0) {
        if (trig & pad::kLeft) a.detailBtn = 0;
        if (trig & pad::kRight) a.detailBtn = 1;
        if (trig & pad::kCircle) a.detailId = -1;
        if (trig & pad::kCross) {
            if (a.detailBtn == 0) {
                toast(a, "Reprodução de vídeo ainda não implementada.",
                      "Será desenvolvida em uma etapa futura.", now);
            } else {
                a.detailId = -1;
            }
        }
        return;
    }

    if (trig & pad::kR1) switchTab(a, +1);
    if (trig & pad::kL1) switchTab(a, -1);

    if (!a.inContent) {
        if (trig & pad::kRight) switchTab(a, +1);
        if (trig & pad::kLeft) switchTab(a, -1);
        if ((trig & pad::kDown) && rowCount(a) > 0) {
            a.inContent = true;
            a.row = 0;
            a.col = 0;
        }
    } else {
        int rc = rowCount(a);
        if (trig & pad::kUp) {
            if (a.row > 0) {
                --a.row;
                clampFocus(a);
            } else {
                a.inContent = false;
            }
        }
        if ((trig & pad::kDown) && a.row < rc - 1) {
            ++a.row;
            clampFocus(a);
        }
        if ((trig & pad::kLeft) && a.col > 0) --a.col;
        if ((trig & pad::kRight) && a.col < colCount(a, a.row) - 1) ++a.col;
        if (trig & pad::kCircle) a.inContent = false;
        if (trig & pad::kCross) activate(a, now);
        if (a.tab == TAB_SEARCH) {
            if (trig & pad::kSquare) backspace(a);
            if (trig & pad::kTriangle) appendChar(a, ' ');
        }
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

void drawPoster(int x, int y, int w, int h, int id, bool focused, int ts) {
    vgradient(x, y, w, h, posterTop(id), posterBottom(id));
    if (focused) frame(x - 8, y - 8, w + 16, h + 16, 6, C_BRIGHT);
    else frame(x, y, w, h, 2, C_DEEP);
    text(x + 12, y + 12, "EXEMPLO", 2, Color{255, 255, 255, 150});
    drawWrapped(x + 12, y + h / 2, w - 24, g_titles[id].name, ts, C_TEXT, 3);
    text(x + 12, y + h - 26, g_titles[id].year, 2, C_TEXT);
}

void drawHome(const App& a) {
    int hid = (a.inContent && a.row == 0) ? homeId(a.col) : homeId(0);
    const Title& t = g_titles[hid];
    vgradient(100, 160, 1720, 280, posterTop(hid), C_BOT);
    frame(100, 160, 1720, 280, 3, C_DEEP);
    text(150, 190, "Destaques", 3, C_BRIGHT);
    textShadow(150, 235, t.name, 8, C_TEXT);
    char meta[96];
    snprintf(meta, sizeof meta, "%s - %s - %s", t.kind, t.genre, t.year);
    text(150, 320, meta, 3, C_BRIGHT);
    drawWrapped(150, 365, 1500, kSynopsis, 3, C_TEXT, 2);

    text(100, 480, "Recomendados (exemplos)", 3, C_PURPLE);
    for (int i = 0; i < 7; ++i) {
        drawPoster(100 + i * 250, 520, 220, 330, homeId(i), a.inContent && a.row == 0 && a.col == i, 3);
    }
}

void drawGrid(const App& a, const char* title) {
    textShadow(100, 160, title, 6, C_TEXT);
    for (int r = 0; r < 2; ++r) {
        for (int c = 0; c < 7; ++c) {
            drawPoster(100 + c * 250, 240 + r * 370, 220, 330, posterId(a, r, c),
                       a.inContent && a.row == r && a.col == c, 3);
        }
    }
}

void drawSearch(const App& a) {
    textShadow(100, 160, "Busca", 6, C_TEXT);
    rect(100, 215, 1720, 60, C_PANEL);
    frame(100, 215, 1720, 60, 2, C_PURPLE);
    if (a.qlen > 0) text(124, 229, a.query, 4, C_TEXT);
    else text(124, 229, "Digite um título...", 4, C_DIM);

    for (int r = 0; r < kKeyRows; ++r) {
        for (int c = 0; c < kKeyCols[r]; ++c) {
            const char* k = kKeyLabels[r][c];
            int kx = 160 + c * 160;
            int ky = 295 + r * 74;
            bool f = a.inContent && a.row == r && a.col == c;
            rect(kx, ky, 150, 64, f ? Color{168, 85, 247, 110} : C_PANEL);
            frame(kx, ky, 150, 64, f ? 4 : 1, f ? C_BRIGHT : C_DEEP);
            int s = (k[1] == 0) ? 4 : 3;
            textCentered(kx + 75, ky + (64 - 7 * s) / 2, k, s, f ? C_TEXT : C_DIM);
        }
    }

    if (a.qlen == 0) {
        text(100, 615, "Digite para buscar no catálogo de exemplo.", 3, C_DIM);
    } else {
        char buf[64];
        snprintf(buf, sizeof buf, "Resultados: %d", a.totalRes);
        text(100, 615, buf, 3, C_PURPLE);
        if (a.nres == 0) text(100, 655, "Nenhum título encontrado.", 3, C_DIM);
    }
    for (int i = 0; i < a.nres; ++i) {
        drawPoster(100 + i * 185, 655, 160, 240, a.res[i],
                   a.inContent && a.row == kKeyRows && a.col == i, 2);
    }
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
        bool f = a.inContent && a.row == i;
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
    text(1040, 702, "Licenças completas: ver README", 3, C_DIM);
}

void drawDetail(const App& a) {
    int id = a.detailId;
    const Title& t = g_titles[id];
    vgradient(0, 0, W, H, posterTop(id), C_BOT);
    rect(0, 0, W, H, Color{0, 0, 0, 150});
    drawPoster(140, 170, 380, 570, id, false, 4);
    textShadow(600, 190, t.name, 7, C_TEXT);
    char meta[96];
    snprintf(meta, sizeof meta, "%s - %s - %s", t.kind, t.genre, t.year);
    text(600, 280, meta, 3, C_BRIGHT);
    drawWrapped(600, 340, 1100, kSynopsis, 3, C_TEXT, 4);

    const char* labels[2] = {"Assistir", "Voltar"};
    for (int i = 0; i < 2; ++i) {
        int bx = 600 + i * 340;
        int by = 600;
        bool f = (a.detailBtn == i);
        rect(bx, by, 300, 80, f ? Color{168, 85, 247, 140} : C_PANEL);
        frame(bx, by, 300, 80, f ? 4 : 2, f ? C_BRIGHT : C_DEEP);
        textCentered(bx + 150, by + 26, labels[i], 4, C_TEXT);
    }
    text(600, 710, "Reprodução de vídeo: ainda não implementada.", 3, C_WARN);
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
    const char* hint = "D-pad: navegar    X: selecionar    O: voltar    L1/R1: trocar aba";
    if (a.detailId >= 0) {
        hint = "Esquerda/direita: escolher    X: confirmar    O: voltar";
    } else if (a.tab == TAB_SEARCH && a.inContent) {
        hint = "D-pad: mover    X: tecla    Quadrado: apagar    Triângulo: espaço    O: menu";
    }
    text(60, 1034, hint, 2, C_DIM);
    char v[48];
    snprintf(v, sizeof v, "v%s", cfg::kVersion);
    text(W - 60 - textWidth(v, 2), 1034, v, 2, C_DIM);
}

void drawMain(const App& a, uint32_t now) {
    vgradient(0, 0, W, H, C_TOP, C_BOT);
    if (a.detailId >= 0) {
        drawDetail(a);
        drawFooter(a, now);
        return;
    }
    drawNav(a);
    switch (a.tab) {
        case TAB_HOME:   drawHome(a); break;
        case TAB_MOVIES: drawGrid(a, "Filmes"); break;
        case TAB_SERIES: drawGrid(a, "Séries"); break;
        case TAB_SEARCH: drawSearch(a); break;
        case TAB_INFO:   drawInfo(a); break;
        default: break;
    }
    drawFooter(a, now);
}

}  // namespace

static void touchFile(const char* path) {
    FILE* f = fopen(path, "w");
    if (f) fclose(f);
}
#define MARK(tag) touchFile("/data/purpleflix/m_" tag)

int main(int, char**) {
    mkdir(cfg::kLogDir, 0777);
    MARK("00_main_iniciou");

    MARK("01_antes_set_main_ready");
    SDL_SetMainReady();
    MARK("02_set_main_ready_ok");

    App app;
    MARK("03_antes_pad_init");
    app.padOk = pad::init();
    if (app.padOk) MARK("04_pad_ok"); else MARK("04_pad_falhou");

    MARK("05_antes_sdl_init");
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        MARK("ERRO_sdl_init");
        return 1;
    }
    MARK("06_sdl_init_ok");

    MARK("07_antes_janela");
    SDL_Window* win = SDL_CreateWindow(cfg::kAppFullName, SDL_WINDOWPOS_UNDEFINED,
                                       SDL_WINDOWPOS_UNDEFINED, W, H, SDL_WINDOW_SHOWN);
    if (!win) {
        MARK("ERRO_janela");
        return 1;
    }
    MARK("08_janela_ok");

    MARK("09_antes_renderer");
    SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (!ren) {
        MARK("ERRO_renderer");
        return 1;
    }
    MARK("10_renderer_ok");
    gfx::init(ren);
    MARK("11_gfx_init_ok");

    initCatalog();
    MARK("12_catalogo_ok");

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
            if (!menuMarked) MARK("14_antes_desenhar_menu");
            handleInput(app, in.trig, now);
            drawMain(app, now);
            if (!menuMarked) {
                MARK("15_menu_desenhado");
                menuMarked = true;
            }
        }
        SDL_RenderPresent(ren);

        if (firstFrame) {
            firstFrame = false;
            MARK("13_primeiro_quadro");
            logInit();
            logMsg("PurpleFlix iniciando");
            logMsg(app.padOk ? "pad: ok" : "pad: falhou");
            MARK("16_log_escrito");
        }
        SDL_Delay(16);
    }

    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
