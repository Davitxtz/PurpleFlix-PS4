// PurpleFlix player v4: video/audio pelo AvPlayer do sistema + SceAudioOut.
// Desenho DIRETO na superficie da janela (confirmado como o caminho fluido neste console),
// conversao NV12 -> cores com SSE2 e barra de controle desenhada na propria superficie.
// ATENCAO: o layout da estrutura de inicializacao do AvPlayer foi confirmado apenas
// pelos testes do Probe neste console (nao ha header oficial com estruturas).
#define SDL_MAIN_HANDLED
#if __has_include(<SDL2/SDL.h>)
#include <SDL2/SDL.h>
#else
#include <SDL.h>
#endif

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__SSE2__) && __has_include(<emmintrin.h>)
#include <emmintrin.h>
#define PF_SIMD 1
#else
#define PF_SIMD 0
#endif

#include <orbis/AudioOut.h>
#include <orbis/AvPlayer.h>
#include <orbis/Sysmodule.h>
#include <orbis/libkernel.h>

#include "gfx.hpp"
#include "pad.hpp"
#include "player.hpp"

// Rotulo assembly: evita conflito com o prototype do header.
extern "C" int32_t pf_load_module(uint16_t id) __asm__("sceSysmoduleLoadModule");

namespace {

const char* const kLogPath = "/data/purpleflix/player_log.txt";
constexpr int W = 1920;
constexpr int H = 1080;
constexpr size_t kMaxW = 1920;
constexpr size_t kMaxH = 1088;

constexpr size_t kPoolSize = 24u << 20;
alignas(64) uint8_t g_pool[kPoolSize];
size_t g_poolOff = 0;

struct Block {
    uint8_t* base;
    size_t size;
    size_t off;
};
Block g_gen = {nullptr, 0, 0};
Block g_tex = {nullptr, 0, 0};
bool g_blocksReady = false;

alignas(64) uint8_t g_copy[kMaxW * kMaxH * 3 / 2 + 4096];
alignas(64) uint8_t g_rgba[kMaxW * kMaxH * 4];  // so usado no modo de reserva (textura)

uint64_t g_tscFreq = 0;
inline uint64_t rdtsc() { return __builtin_ia32_rdtsc(); }
uint64_t usFrom(uint64_t ticks) { return g_tscFreq ? (ticks * 1000000ull) / g_tscFreq : 0; }

// Estado compartilhado com a thread de audio.
int32_t g_av = 0;
int g_stop = 0;
int g_paused = 0;
int g_audioDone = 0;
int g_audioFailed = 0;
uint64_t g_audioMs = 0;
int g_audioChunks = 0;
int32_t g_fd = -1;

// ---- escrita sem libc (segura em qualquer thread) ----
size_t slen(const char* s) {
    size_t n = 0;
    while (s[n]) ++n;
    return n;
}

void rawWrite(const char* s, size_t n) {
    if (g_fd >= 0) sceKernelWrite(g_fd, s, n);
}

void out(const char* s) {
    rawWrite(s, slen(s));
    rawWrite("\n", 1);
}

size_t putS(char* b, size_t n, const char* s) {
    while (*s) b[n++] = *s++;
    return n;
}

size_t putD(char* b, size_t n, uint64_t v) {
    char t[24];
    int k = 0;
    if (v == 0) t[k++] = '0';
    while (v) {
        t[k++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (k) b[n++] = t[--k];
    return n;
}

size_t putH(char* b, size_t n, uint64_t v) {
    n = putS(b, n, "0x");
    for (int i = 15; i >= 0; --i) b[n++] = "0123456789abcdef"[(v >> (i * 4)) & 0xF];
    return n;
}

void logHex(const char* label, uint32_t v) {
    char b[200];
    size_t n = putS(b, 0, label);
    n = putS(b, n, " = ");
    n = putH(b, n, v);
    b[n++] = '\n';
    rawWrite(b, n);
}

void logDec(const char* label, uint64_t v) {
    char b[200];
    size_t n = putS(b, 0, label);
    n = putS(b, n, " = ");
    n = putD(b, n, v);
    b[n++] = '\n';
    rawWrite(b, n);
}

void logSigned(const char* label, int64_t v) {
    char b[200];
    size_t n = putS(b, 0, label);
    n = putS(b, n, " = ");
    if (v < 0) {
        b[n++] = '-';
        v = -v;
    }
    n = putD(b, n, (uint64_t)v);
    b[n++] = '\n';
    rawWrite(b, n);
}

void logTick(uint64_t tMs, int fps, int drops, uint64_t convUs, uint64_t presUs) {
    char b[240];
    size_t n = putS(b, 0, "t=");
    n = putD(b, n, tMs);
    n = putS(b, n, " ms: fps=");
    n = putD(b, n, (uint64_t)fps);
    n = putS(b, n, " descartados=");
    n = putD(b, n, (uint64_t)drops);
    n = putS(b, n, " conversao(us)=");
    n = putD(b, n, convUs);
    n = putS(b, n, " apresentar(us)=");
    n = putD(b, n, presUs);
    b[n++] = '\n';
    rawWrite(b, n);
}

void* bump(uint8_t* base, size_t total, size_t* offp, uint32_t align, uint32_t size) {
    size_t a = align < 64 ? 64 : align;
    size_t cur = __atomic_load_n(offp, __ATOMIC_SEQ_CST);
    for (;;) {
        size_t start = (cur + a - 1) & ~(a - 1);
        size_t nxt = start + size;
        if (nxt > total) return nullptr;
        if (__atomic_compare_exchange_n(offp, &cur, nxt, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
            return base + start;
        }
    }
}

void* cbAlloc(void*, uint32_t align, uint32_t size) {
    if (size >= (1u << 20) && g_gen.base) return bump(g_gen.base, g_gen.size, &g_gen.off, align, size);
    return bump(g_pool, kPoolSize, &g_poolOff, align, size);
}

void cbFree(void*, void*) {}

void* cbAllocTex(void*, uint32_t align, uint32_t size) {
    if (g_tex.base) return bump(g_tex.base, g_tex.size, &g_tex.off, align, size);
    return bump(g_pool, kPoolSize, &g_poolOff, align, size);
}

void cbFreeTex(void*, void*) {}

bool initBlock(Block& blk, size_t want, size_t minSize) {
    size_t dm = sceKernelGetDirectMemorySize();
    for (size_t len = want; len >= minSize; len >>= 1) {
        off_t phys = 0;
        int32_t r = sceKernelAllocateDirectMemory(0, (off_t)dm, len, 0x200000, ORBIS_KERNEL_WB_ONION, &phys);
        if (r != 0) continue;
        void* addr = nullptr;
        r = sceKernelMapDirectMemory(&addr, len, ORBIS_KERNEL_PROT_CPU_RW | ORBIS_KERNEL_PROT_GPU_RW, 0, phys,
                                     0x200000);
        if (r == 0 && addr) {
            blk.base = (uint8_t*)addr;
            blk.size = len;
            blk.off = 0;
            return true;
        }
    }
    return false;
}

bool ensureBlocks() {
    if (g_blocksReady) return true;
    if (!initBlock(g_gen, 160u << 20, 64u << 20)) return false;
    if (!initBlock(g_tex, 256u << 20, 64u << 20)) return false;
    g_blocksReady = true;
    return true;
}

// ---- conversao NV12 -> pixels de 32 bits (BT.601 limitado, inteiros) ----
// BGRA=false: bytes R,G,B,A (ABGR8888). BGRA=true: bytes B,G,R,A (ARGB8888).
// Plano Y seguido de UV intercalado (confirmado no PS4). A versao SSE2 foi testada contra
// a escalar em 1280x720 com um quadro real do console: diferenca maxima = 0.
inline uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v)); }

template <bool BGRA>
void nv12RowScalar(const uint8_t* yr, const uint8_t* uvr, int w, uint8_t* d) {
    for (int i = 0; i + 1 < w; i += 2) {
        int U = (int)uvr[i] - 128, V = (int)uvr[i + 1] - 128;
        int rv = 409 * V + 128, guv = -100 * U - 208 * V + 128, bu = 516 * U + 128;
        for (int k = 0; k < 2; ++k) {
            int c = 298 * ((int)yr[i + k] - 16);
            uint8_t r = clamp8((c + rv) >> 8), g = clamp8((c + guv) >> 8), b = clamp8((c + bu) >> 8);
            uint8_t* p = d + 4 * (i + k);
            if (BGRA) {
                p[0] = b; p[1] = g; p[2] = r; p[3] = 255;
            } else {
                p[0] = r; p[1] = g; p[2] = b; p[3] = 255;
            }
        }
    }
}

#if PF_SIMD
template <bool BGRA>
void nv12RowSimd(const uint8_t* yr, const uint8_t* uvr, int w, uint8_t* d) {
    const __m128i zero = _mm_setzero_si128();
    const __m128i c16 = _mm_set1_epi16(16);
    const __m128i c128 = _mm_set1_epi16(128);
    const __m128i kY = _mm_set1_epi32(298);
    const __m128i kRv = _mm_set1_epi32(409 << 16);
    const __m128i kBu = _mm_set1_epi32(516);
    const __m128i kG = _mm_set1_epi32((int)(((uint32_t)(uint16_t)(-208) << 16) | (uint16_t)(-100)));
    const __m128i round = _mm_set1_epi32(128);
    const __m128i alpha = _mm_set1_epi8((char)255);
    int i = 0;
    for (; i + 8 <= w; i += 8) {
        __m128i y8 = _mm_loadl_epi64((const __m128i*)(yr + i));
        __m128i uv8 = _mm_loadl_epi64((const __m128i*)(uvr + i));
        __m128i y16 = _mm_sub_epi16(_mm_unpacklo_epi8(y8, zero), c16);
        __m128i uvw = _mm_sub_epi16(_mm_unpacklo_epi8(uv8, zero), c128);
        __m128i uvLo = _mm_unpacklo_epi32(uvw, uvw);
        __m128i uvHi = _mm_unpackhi_epi32(uvw, uvw);
        __m128i yLo = _mm_unpacklo_epi16(y16, zero);
        __m128i yHi = _mm_unpackhi_epi16(y16, zero);
        __m128i ytLo = _mm_madd_epi16(yLo, kY), ytHi = _mm_madd_epi16(yHi, kY);
        __m128i rLo = _mm_srai_epi32(_mm_add_epi32(_mm_add_epi32(ytLo, _mm_madd_epi16(uvLo, kRv)), round), 8);
        __m128i rHi = _mm_srai_epi32(_mm_add_epi32(_mm_add_epi32(ytHi, _mm_madd_epi16(uvHi, kRv)), round), 8);
        __m128i gLo = _mm_srai_epi32(_mm_add_epi32(_mm_add_epi32(ytLo, _mm_madd_epi16(uvLo, kG)), round), 8);
        __m128i gHi = _mm_srai_epi32(_mm_add_epi32(_mm_add_epi32(ytHi, _mm_madd_epi16(uvHi, kG)), round), 8);
        __m128i bLo = _mm_srai_epi32(_mm_add_epi32(_mm_add_epi32(ytLo, _mm_madd_epi16(uvLo, kBu)), round), 8);
        __m128i bHi = _mm_srai_epi32(_mm_add_epi32(_mm_add_epi32(ytHi, _mm_madd_epi16(uvHi, kBu)), round), 8);
        __m128i r8 = _mm_packus_epi16(_mm_packs_epi32(rLo, rHi), zero);
        __m128i g8 = _mm_packus_epi16(_mm_packs_epi32(gLo, gHi), zero);
        __m128i b8 = _mm_packus_epi16(_mm_packs_epi32(bLo, bHi), zero);
        __m128i first = BGRA ? _mm_unpacklo_epi8(b8, g8) : _mm_unpacklo_epi8(r8, g8);
        __m128i second = BGRA ? _mm_unpacklo_epi8(r8, alpha) : _mm_unpacklo_epi8(b8, alpha);
        _mm_storeu_si128((__m128i*)(d + 4 * i), _mm_unpacklo_epi16(first, second));
        _mm_storeu_si128((__m128i*)(d + 4 * i + 16), _mm_unpackhi_epi16(first, second));
    }
    if (i < w) nv12RowScalar<BGRA>(yr + i, uvr + i, w - i, d + 4 * i);
}
#endif

// Converte as linhas [jStart, jStart + rows) do quadro (Y em 'y', UV em 'uv', largura w)
// para dst (linha 0 de dst = linha jStart do quadro).
template <bool BGRA>
void nv12ToDst(const uint8_t* y, const uint8_t* uv, int w, int rows, int jStart, uint8_t* dst, size_t pitch) {
    for (int k = 0; k < rows; ++k) {
        int j = jStart + k;
        const uint8_t* yr = y + (size_t)j * w;
        const uint8_t* uvr = uv + (size_t)(j >> 1) * w;
        uint8_t* d = dst + (size_t)k * pitch;
#if PF_SIMD
        nv12RowSimd<BGRA>(yr, uvr, w, d);
#else
        nv12RowScalar<BGRA>(yr, uvr, w, d);
#endif
    }
}

struct MemRepl {
    void* obj;
    void* (*alloc)(void*, uint32_t, uint32_t);
    void (*dealloc)(void*, void*);
    void* (*allocTex)(void*, uint32_t, uint32_t);
    void (*deallocTex)(void*, void*);
};
struct FileRepl {
    void* p[5];
};
struct EventRepl {
    void* obj;
    void* cb;
};
struct InitData {
    MemRepl mem;
    FileRepl file;
    EventRepl ev;
    int32_t debugLevel;
    uint32_t basePriority;
    int32_t numFrameBuffers;
    uint8_t autoStart;
    uint8_t reserved[3];
    const char* lang;
};

typedef int32_t (*FnInit)(void*);
typedef int32_t (*FnAddSource)(int32_t, const char*);
typedef int32_t (*FnStart)(int32_t);
typedef int32_t (*FnStop)(int32_t);
typedef int32_t (*FnIsActive)(int32_t);
typedef int32_t (*FnStreamCount)(int32_t);
typedef int32_t (*FnGetStreamInfo)(int32_t, uint32_t, void*);
typedef int32_t (*FnGetFrame)(int32_t, void*);
typedef int32_t (*FnClose)(int32_t);

void closeAv() {
    if (g_av > 0) {
        ((FnStop)sceAvPlayerStop)(g_av);
        ((FnClose)sceAvPlayerClose)(g_av);
        g_av = 0;
    }
}

bool failMsg(char* err, size_t cap, const char* msg) {
    if (err && cap) snprintf(err, cap, "%s", msg);
    out(msg);
    return false;
}

// ---- thread de audio: toca cada pedaco pela SceAudioOut (sem libc) ----
void* audioThread(void*) {
    int32_t r = sceAudioOutInit();
    logHex("audio: sceAudioOutInit", (uint32_t)r);
    int32_t ah = sceAudioOutOpen(ORBIS_USER_SERVICE_USER_ID_SYSTEM, ORBIS_AUDIO_OUT_PORT_TYPE_MAIN, 0, 1024, 48000,
                                 ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
    logHex("audio: sceAudioOutOpen", (uint32_t)ah);
    if (ah < 0) {
        __atomic_store_n(&g_audioFailed, 1, __ATOMIC_SEQ_CST);
        __atomic_store_n(&g_audioDone, 1, __ATOMIC_SEQ_CST);
        return nullptr;
    }
    alignas(16) uint8_t ai[512];
    int misses = 0;
    for (;;) {
        if (__atomic_load_n(&g_stop, __ATOMIC_SEQ_CST)) break;
        if (__atomic_load_n(&g_paused, __ATOMIC_SEQ_CST)) {
            sceKernelUsleep(5000);
            continue;
        }
        memset(ai, 0, sizeof ai);
        int32_t got = ((FnGetFrame)sceAvPlayerGetAudioData)(g_av, ai);
        if ((got & 0xFF) != 0) {
            misses = 0;
            uint64_t ptr = 0, ts = 0;
            uint32_t size = 0;
            memcpy(&ptr, ai, 8);
            memcpy(&ts, ai + 16, 8);
            memcpy(&size, ai + 32, 4);
            if (ptr != 0 && size == 4096) {
                sceAudioOutOutput(ah, (const void*)(uintptr_t)ptr);
                __atomic_store_n(&g_audioMs, ts, __ATOMIC_SEQ_CST);
                __atomic_add_fetch(&g_audioChunks, 1, __ATOMIC_SEQ_CST);
            }
        } else {
            ++misses;
            int32_t active = ((FnIsActive)sceAvPlayerIsActive)(g_av);
            if (active == 0 && misses > 30) break;
            sceKernelUsleep(2000);
        }
    }
    sceAudioOutClose(ah);
    __atomic_store_n(&g_audioDone, 1, __ATOMIC_SEQ_CST);
    return nullptr;
}

// ---- barra de controle e estatisticas (desenhadas via gfx, na superficie ou no renderizador) ----
struct Ov {
    const char* title;
    uint32_t durMs;
    uint64_t lastTs;
    bool paused;
    bool stats;
    bool showBars;
    int fps;
    uint64_t convUs, copyUs, presUs;
    int drops;
    bool audioFailed;
    uint32_t vw, vh;
    bool direct;
};

void usText(char* sb, size_t cap, const char* label, uint64_t us) {
    snprintf(sb, cap, "%s: %u.%u ms", label, (unsigned)(us / 1000), (unsigned)((us % 1000) / 100));
}

void drawOverlay(const Ov& o) {
    const Color kBar = {0, 0, 0, 170};
    const Color kText = {238, 230, 252, 255};
    const Color kDim = {170, 150, 200, 255};
    const Color kPurple = {168, 85, 247, 255};
    if (o.showBars) {
        gfx::rect(0, 0, W, 90, kBar);
        gfx::text(60, 28, o.title ? o.title : "", 4, kText);
        gfx::rect(0, 940, W, 140, kBar);
        int frac = o.durMs ? (int)((o.lastTs * 1800ull) / o.durMs) : 0;
        if (frac > 1800) frac = 1800;
        if (frac < 0) frac = 0;
        gfx::rect(60, 980, 1800, 10, Color{70, 55, 95, 255});
        gfx::rect(60, 980, frac, 10, kPurple);
        char tb[64];
        unsigned cs = (unsigned)(o.lastTs / 1000), ds = (unsigned)(o.durMs / 1000);
        snprintf(tb, sizeof tb, "%u:%02u / %u:%02u", cs / 60, cs % 60, ds / 60, ds % 60);
        gfx::text(60, 1004, tb, 3, kText);
        gfx::text(60, 1040, "X: pausar    O: voltar    Triângulo: estatísticas    Quadrado: mostrar barra", 2, kDim);
        if (o.paused) gfx::textCentered(W / 2, 480, "PAUSADO", 8, kText);
    }
    if (o.stats) {
        char sb[96];
        gfx::rect(W - 600, 100, 560, 330, kBar);
        int y = 116;
        snprintf(sb, sizeof sb, "modo: %s   fps: %d", o.direct ? "direto" : "textura", o.fps);
        gfx::text(W - 580, y, sb, 3, kText);
        y += 34;
        usText(sb, sizeof sb, "conversão", o.convUs);
        gfx::text(W - 580, y, sb, 3, kText);
        y += 34;
        usText(sb, sizeof sb, "cópia do quadro", o.copyUs);
        gfx::text(W - 580, y, sb, 3, kText);
        y += 34;
        usText(sb, sizeof sb, "apresentar", o.presUs);
        gfx::text(W - 580, y, sb, 3, kText);
        y += 34;
        snprintf(sb, sizeof sb, "descartados: %d", o.drops);
        gfx::text(W - 580, y, sb, 3, kText);
        y += 34;
        snprintf(sb, sizeof sb, "resolução: %ux%u", (unsigned)o.vw, (unsigned)o.vh);
        gfx::text(W - 580, y, sb, 3, kText);
        y += 34;
        snprintf(sb, sizeof sb, "som: %s", o.audioFailed ? "indisponível" : "ok");
        gfx::text(W - 580, y, sb, 3, o.audioFailed ? Color{255, 130, 150, 255} : kText);
        y += 34;
        snprintf(sb, sizeof sb, "conversão: %s", PF_SIMD ? "SSE2" : "escalar");
        gfx::text(W - 580, y, sb, 3, kDim);
    }
}

}  // namespace

bool player::run(SDL_Renderer* ren, const char* path, const char* title, char* err, size_t errCap) {
    if (err && errCap) err[0] = 0;
    g_tscFreq = sceKernelGetTscFrequency();
    g_fd = sceKernelOpen(kLogPath, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    out("PurpleFlix player v4 (desenho direto + SSE2)");
    out(path);
    logDec("conversao SIMD compilada (1=sim)", PF_SIMD);

    if (!ensureBlocks()) return failMsg(err, errCap, "Memória de vídeo indisponível.");

    SDL_Window* win = SDL_GetWindowFromID(1);  // a janela do app e a primeira criada (confirmado no log)
    SDL_Surface* ws = win ? SDL_GetWindowSurface(win) : nullptr;
    bool direct = ws && ws->pixels && ws->format && ws->format->BytesPerPixel == 4 && ws->w == W && ws->h == H &&
                  (ws->format->format == SDL_PIXELFORMAT_ABGR8888 || ws->format->format == SDL_PIXELFORMAT_ARGB8888);
    bool bgra = direct && ws->format->format == SDL_PIXELFORMAT_ARGB8888;
    {
        char b[200];
        snprintf(b, sizeof b, "superficie da janela: %s, modo %s",
                 ws && ws->format ? SDL_GetPixelFormatName(ws->format->format) : "indisponivel",
                 direct ? "DIRETO" : "TEXTURA (reserva)");
        out(b);
    }
    Uint32 texFmt = SDL_PIXELFORMAT_ABGR8888;
    if (!direct) {
        Uint32 wf = win ? SDL_GetWindowPixelFormat(win) : SDL_PIXELFORMAT_UNKNOWN;
        if (wf == SDL_PIXELFORMAT_ARGB8888) {
            texFmt = wf;
            bgra = true;
        }
    }

    // Nova sessao: reinicia os alocadores e o estado compartilhado.
    g_poolOff = 0;
    g_gen.off = 0;
    g_tex.off = 0;
    g_stop = 0;
    g_paused = 0;
    g_audioDone = 0;
    g_audioFailed = 0;
    g_audioMs = 0;
    g_audioChunks = 0;

    pf_load_module(ORBIS_SYSMODULE_AV_PLAYER);

    InitData d;
    memset(&d, 0, sizeof d);
    d.mem.alloc = cbAlloc;
    d.mem.dealloc = cbFree;
    d.mem.allocTex = cbAllocTex;
    d.mem.deallocTex = cbFreeTex;
    d.basePriority = 700;
    d.numFrameBuffers = 4;
    d.lang = "eng";

    g_av = ((FnInit)sceAvPlayerInit)(&d);
    logHex("sceAvPlayerInit", (uint32_t)g_av);
    if (g_av <= 0) return failMsg(err, errCap, "O player de vídeo não iniciou.");

    int32_t rs = ((FnAddSource)sceAvPlayerAddSource)(g_av, path);
    logHex("sceAvPlayerAddSource", (uint32_t)rs);
    if (rs != 0) {
        closeAv();
        return failMsg(err, errCap, "O player não abriu o arquivo de vídeo.");
    }

    int32_t sc = 0;
    for (int i = 0; i < 50; ++i) {
        sc = ((FnStreamCount)sceAvPlayerStreamCount)(g_av);
        if (sc > 0) break;
        sceKernelUsleep(100000);
    }
    logDec("StreamCount", (uint64_t)sc);
    if (sc <= 0) {
        closeAv();
        return failMsg(err, errCap, "Formato de vídeo não reconhecido pelo player.");
    }

    uint32_t vw = 0, vh = 0, durMs = 0;
    for (int i = 0; i < sc && i < 4; ++i) {
        alignas(16) uint8_t info[256];
        memset(info, 0, sizeof info);
        ((FnGetStreamInfo)sceAvPlayerGetStreamInfo)(g_av, (uint32_t)i, info);
        uint32_t type = 0;
        memcpy(&type, info, 4);
        if (type == 0 && vw == 0) {
            memcpy(&vw, info + 8, 4);
            memcpy(&vh, info + 12, 4);
            memcpy(&durMs, info + 24, 4);
        }
    }
    logDec("video largura", vw);
    logDec("video altura", vh);
    logDec("duracao (ms)", durMs);
    if (vw == 0 || vh == 0 || vw > kMaxW || vh > kMaxH || (vw & 1) || (vh & 1)) {
        closeAv();
        return failMsg(err, errCap, "Resolução de vídeo não suportada (máximo 1920x1088).");
    }

    // Area do video na tela (centralizada; recorta linhas se o video for mais alto que a tela).
    const int outW = (int)vw;
    const int outH = (int)vh > H ? H : (int)vh;
    const int xOff = (W - outW) / 2;
    const int yOff = (H - outH) / 2;
    const int jStart = (((int)vh - outH) / 2) & ~1;

    SDL_Texture* tex = nullptr;
    if (!direct) {
        tex = SDL_CreateTexture(ren, texFmt, SDL_TEXTUREACCESS_STREAMING, (int)vw, (int)vh);
        if (!tex) {
            closeAv();
            return failMsg(err, errCap, "Não foi possível criar a textura de vídeo.");
        }
    }
    Uint32 black = direct ? SDL_MapRGB(ws->format, 0, 0, 0) : 0;
    if (direct) gfx::setSurface(ws);

    int32_t rst = ((FnStart)sceAvPlayerStart)(g_av);
    logHex("sceAvPlayerStart", (uint32_t)rst);

    OrbisPthreadAttr attr;
    scePthreadAttrInit(&attr);
    scePthreadAttrSetstacksize(&attr, 256 * 1024);
    OrbisPthread thr;
    int32_t rt = scePthreadCreate(&thr, &attr, audioThread, nullptr, "pf_audio");
    logHex("scePthreadCreate(audio)", (uint32_t)rt);
    bool haveThread = (rt == 0);
    if (!haveThread) g_audioDone = 1;

    alignas(16) uint8_t fi[512];
    bool havePending = false, haveFrame = false;
    uint64_t pPtr = 0, pTs = 0, lastTs = 0;
    bool paused = false, showStats = false, stateChanged = true, newFrame = false;
    uint32_t prevBtn = pad::poll();
    uint64_t overlayUntilMs = 4000;
    uint64_t start = rdtsc();
    uint64_t endIdleSince = 0;

    int presented = 0, drops = 0;
    uint64_t lastConvUs = 0, lastCopyUs = 0, lastPresUs = 0;
    uint64_t convSum = 0, copySum = 0, presSum = 0;
    int presN = 0, convN = 0;
    int64_t sumLate = 0;
    int lateN = 0;
    int fps = 0, fpsCount = 0;
    uint64_t fpsStartMs = 0, tickMs = 0;
    bool overlayShownLast = true;
    bool finished = false;

    for (;;) {
        uint64_t nowMs = usFrom(rdtsc() - start) / 1000;

        uint32_t cur = pad::poll();
        uint32_t press = cur & ~prevBtn;
        prevBtn = cur;
        if (press & pad::kCircle) break;
        if (press & pad::kCross) {
            paused = !paused;
            __atomic_store_n(&g_paused, paused ? 1 : 0, __ATOMIC_SEQ_CST);
            overlayUntilMs = nowMs + 3000;
            stateChanged = true;
        }
        if (press & pad::kTriangle) {
            showStats = !showStats;
            stateChanged = true;
        }
        if (press & pad::kSquare) {
            overlayUntilMs = nowMs + 4000;
            stateChanged = true;
        }
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
        }

        bool audioDone = __atomic_load_n(&g_audioDone, __ATOMIC_SEQ_CST) != 0;
        uint64_t clockMs = audioDone ? 0 : __atomic_load_n(&g_audioMs, __ATOMIC_SEQ_CST);

        newFrame = false;
        if (!paused) {
            if (!havePending) {
                memset(fi, 0, sizeof fi);
                int32_t ret = ((FnGetFrame)sceAvPlayerGetVideoData)(g_av, fi);
                if ((ret & 0xFF) != 0) {
                    uint64_t ptr = 0, ts = 0;
                    uint32_t w = 0, hh = 0;
                    memcpy(&ptr, fi, 8);
                    memcpy(&ts, fi + 16, 8);
                    memcpy(&w, fi + 24, 4);
                    memcpy(&hh, fi + 28, 4);
                    const uint8_t* p = (const uint8_t*)(uintptr_t)ptr;
                    bool inPool = g_tex.base && p >= g_tex.base && p < g_tex.base + g_tex.size;
                    if (inPool && w == vw && hh == vh) {
                        havePending = true;
                        pPtr = ptr;
                        pTs = ts;
                        endIdleSince = 0;
                    }
                }
            }
            if (havePending) {
                bool tooLate = !audioDone && pTs + 80 < clockMs;
                bool due = audioDone || pTs <= clockMs + 10;
                if (tooLate) {
                    ++drops;
                    havePending = false;
                } else if (due) {
                    const uint8_t* p = (const uint8_t*)(uintptr_t)pPtr;
                    uint64_t t0 = rdtsc();
                    memcpy(g_copy, p, (size_t)vw * vh * 3 / 2);
                    lastCopyUs = usFrom(rdtsc() - t0);
                    copySum += lastCopyUs;
                    if (!audioDone) {
                        sumLate += (int64_t)clockMs - (int64_t)pTs;
                        ++lateN;
                    }
                    lastTs = pTs;
                    ++presented;
                    ++fpsCount;
                    havePending = false;
                    haveFrame = true;
                    newFrame = true;
                }
            } else if (audioDone) {
                if (endIdleSince == 0) endIdleSince = nowMs;
                if (nowMs - endIdleSince > 800) {
                    finished = true;
                    break;
                }
            }
        }

        if (nowMs - fpsStartMs >= 1000) {
            fps = fpsCount;
            fpsCount = 0;
            fpsStartMs = nowMs;
        }
        if (nowMs - tickMs >= 5000) {
            tickMs = nowMs;
            logTick(nowMs, fps, drops, lastConvUs, lastPresUs);
        }
        bool overlayVisible = paused || nowMs < overlayUntilMs;
        if (overlayVisible != overlayShownLast) {
            overlayShownLast = overlayVisible;
            stateChanged = true;
        }

        if (stateChanged || newFrame) {
            Ov ov = {title, durMs, lastTs, paused, showStats, overlayVisible, fps, lastConvUs, lastCopyUs, lastPresUs,
                     drops, g_audioFailed != 0, vw, vh, direct};
            if (direct) {
                if (stateChanged) SDL_FillRect(ws, nullptr, black);  // limpa barras antigas
                if (haveFrame && (newFrame || stateChanged)) {
                    uint8_t* dst = (uint8_t*)ws->pixels + (size_t)yOff * ws->pitch + (size_t)xOff * 4;
                    uint64_t t0 = rdtsc();
                    if (bgra) nv12ToDst<true>(g_copy, g_copy + (size_t)vw * vh, (int)vw, outH, jStart, dst, ws->pitch);
                    else nv12ToDst<false>(g_copy, g_copy + (size_t)vw * vh, (int)vw, outH, jStart, dst, ws->pitch);
                    lastConvUs = usFrom(rdtsc() - t0);
                    convSum += lastConvUs;
                    ++convN;
                }
                if (overlayVisible || showStats) drawOverlay(ov);
                uint64_t t1 = rdtsc();
                SDL_UpdateWindowSurface(win);
                lastPresUs = usFrom(rdtsc() - t1);
                if (newFrame) {
                    presSum += lastPresUs;
                    ++presN;
                }
            } else {
                uint64_t t0 = rdtsc();
                if (haveFrame && (newFrame || stateChanged)) {
                    if (bgra) nv12ToDst<true>(g_copy, g_copy + (size_t)vw * vh, (int)vw, (int)vh, 0, g_rgba, (size_t)vw * 4);
                    else nv12ToDst<false>(g_copy, g_copy + (size_t)vw * vh, (int)vw, (int)vh, 0, g_rgba, (size_t)vw * 4);
                    lastConvUs = usFrom(rdtsc() - t0);
                    convSum += lastConvUs;
                    ++convN;
                }
                SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
                SDL_RenderClear(ren);
                if (haveFrame) {
                    SDL_Rect dstR = {xOff, yOff, (int)vw, (int)vh};
                    SDL_UpdateTexture(tex, nullptr, g_rgba, (int)vw * 4);
                    SDL_RenderCopy(ren, tex, nullptr, &dstR);
                }
                if (overlayVisible || showStats) drawOverlay(ov);
                uint64_t t1 = rdtsc();
                SDL_RenderPresent(ren);
                lastPresUs = usFrom(rdtsc() - t1);
                if (newFrame) {
                    presSum += lastPresUs;
                    ++presN;
                }
            }
            stateChanged = false;
        }
        sceKernelUsleep(1000);
    }

    // Encerra: audio, superficie, textura e player.
    __atomic_store_n(&g_stop, 1, __ATOMIC_SEQ_CST);
    if (haveThread) scePthreadJoin(thr, nullptr);
    gfx::setSurface(nullptr);
    if (tex) SDL_DestroyTexture(tex);
    closeAv();

    out("---- RESUMO ----");
    logDec("terminou sozinho (1=sim)", finished ? 1 : 0);
    logDec("quadros apresentados", (uint64_t)presented);
    logDec("quadros descartados", (uint64_t)drops);
    logDec("audio: pedacos enviados", (uint64_t)__atomic_load_n(&g_audioChunks, __ATOMIC_SEQ_CST));
    logDec("audio falhou (1=sim)", (uint64_t)g_audioFailed);
    if (lateN > 0) logSigned("atraso medio do video vs som (ms)", sumLate / lateN);
    if (presented > 0) logDec("copia media (us)", copySum / (uint64_t)presented);
    if (convN > 0) logDec("conversao media (us)", convSum / (uint64_t)convN);
    if (presN > 0) logDec("apresentar media (us)", presSum / (uint64_t)presN);
    out("fim.");
    if (g_fd >= 0) {
        sceKernelClose(g_fd);
        g_fd = -1;
    }
    return true;
}

// FIM DO ARQUIVO (player.cpp)
