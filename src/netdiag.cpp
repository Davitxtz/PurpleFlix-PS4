#define SDL_MAIN_HANDLED
#if __has_include(<SDL2/SDL.h>)
#include <SDL2/SDL.h>
#else
#include <SDL.h>
#endif

#include <stdio.h>
#include <string.h>

#include "gfx.hpp"
#include "net.hpp"
#include "netdiag.hpp"
#include "pad.hpp"

using namespace gfx;

namespace {

constexpr int W = 1920;
constexpr int H = 1080;

const Color C_TOP    = {10, 4, 22, 255};
const Color C_BOT    = {2, 0, 8, 255};
const Color C_NAV    = {8, 3, 18, 240};
const Color C_BRIGHT = {200, 140, 255, 255};
const Color C_TEXT   = {238, 230, 252, 255};
const Color C_DIM    = {150, 132, 182, 255};
const Color C_WARN   = {255, 130, 150, 255};

const char* const kUrl = "https://www.google.com/robots.txt";

constexpr int kLineLen = 112;
constexpr int kMaxLines = 8;

bool g_active = false;
bool g_pending = false;
int g_state = 0;  // 0 = parado, 1 = testando, 2 = concluído
char g_lines[kMaxLines][kLineLen];
int g_nlines = 0;
char g_dummy[kLineLen];
char g_body[1024];

char* nextLine() {
    if (g_nlines < kMaxLines) {
        char* p = g_lines[g_nlines++];
        p[0] = 0;
        return p;
    }
    g_dummy[0] = 0;
    return g_dummy;
}

void describe(const char* label, const net::Result& r, uint32_t ms) {
    snprintf(nextLine(), kLineLen, "%s: %s - HTTP %d, %u bytes, %u ms", label,
             r.ok ? "OK" : "FALHOU", r.status, (unsigned)r.bytes, (unsigned)ms);
    if (!r.ok) {
        snprintf(nextLine(), kLineLen, "   etapa: %s, código 0x%08X", r.stage, (unsigned)r.err);
    }
}

void saveLog() {
    FILE* f = fopen("/data/purpleflix/net_log.txt", "a");
    if (!f) return;
    for (int i = 0; i < g_nlines; ++i) {
        fputs(g_lines[i], f);
        fputc('\n', f);
    }
    fputs("---\n", f);
    fclose(f);
}

void runTest() {
    g_nlines = 0;
    int ierr = 0;
    const char* istage = "";
    if (!net::init(&ierr, &istage)) {
        snprintf(nextLine(), kLineLen, "Falha ao iniciar a rede. Etapa: %s", istage);
        snprintf(nextLine(), kLineLen, "   código 0x%08X", (unsigned)ierr);
        g_state = 2;
        saveLog();
        return;
    }
    snprintf(nextLine(), kLineLen, "Rede iniciada.");

    uint32_t t0 = SDL_GetTicks();
    net::Result r = net::get(kUrl, g_body, sizeof g_body, false);
    describe("TLS normal", r, SDL_GetTicks() - t0);
    bool haveBody = r.ok && r.status > 0;

    if (!r.ok && r.status == 0) {
        t0 = SDL_GetTicks();
        net::Result r2 = net::get(kUrl, g_body, sizeof g_body, true);
        describe("TLS sem verificação (INSEGURO)", r2, SDL_GetTicks() - t0);
        haveBody = r2.ok && r2.status > 0;
    }

    if (haveBody) {
        char first[56];
        int n = 0;
        for (const char* p = g_body; *p && *p != '\n' && *p != '\r' && n < (int)sizeof first - 1; ++p) {
            unsigned char c = (unsigned char)*p;
            first[n++] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
        }
        first[n] = 0;
        snprintf(nextLine(), kLineLen, "Início do conteúdo: %s", first);
    }
    g_state = 2;
    saveLog();
}

}  // namespace

bool netdiag::active() { return g_active; }

void netdiag::open() { g_active = true; }

void netdiag::handle(uint32_t trig) {
    if (trig & pad::kCircle) g_active = false;
    if ((trig & pad::kCross) && g_state != 1) {
        g_state = 1;
        g_pending = true;
    }
}

void netdiag::draw() {
    vgradient(0, 0, W, H, C_TOP, C_BOT);
    textShadow(100, 160, "Diagnóstico de rede", 6, C_TEXT);
    text(100, 240, "Teste de conexão do console. Nenhum dado pessoal é enviado.", 3, C_DIM);
    text(100, 280, "Alvo (o mesmo do sample oficial do OpenOrbis):", 3, C_DIM);
    text(100, 314, kUrl, 3, C_BRIGHT);

    if (g_state == 0) {
        text(100, 400, "Pressione X para executar o teste.", 4, C_TEXT);
    } else if (g_state == 1) {
        text(100, 400, "Testando... aguarde.", 4, C_BRIGHT);
    } else {
        for (int i = 0; i < g_nlines; ++i) {
            bool bad = strstr(g_lines[i], "FALHOU") || strstr(g_lines[i], "INSEGURO");
            text(100, 400 + i * 46, g_lines[i], 3, bad ? C_WARN : C_TEXT);
        }
    }

    rect(0, 1010, W, 70, C_NAV);
    text(60, 1034, "X: executar o teste    O: voltar", 2, C_DIM);
}

void netdiag::runPending() {
    if (!g_pending) return;
    g_pending = false;
    runTest();
}
