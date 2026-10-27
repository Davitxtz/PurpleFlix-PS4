// PurpleFlix player: video/audio pelo AvPlayer do sistema + SceAudioOut.
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
alignas(64) uint8_t g_rgba[kMaxW * kMaxH * 4];

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

inline uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v)); }

// NV12 -> pixels de 32 bits. BGRA=false: bytes R,G,B,A (ABGR8888). BGRA=true: bytes B,G,R,A (ARGB8888).
// Plano Y seguido de UV intercalado (confirmado no PS4, 720p).
template <bool BGRA>
void nv12Convert(const uint8_t* y, const uint8_t* uv, int w, int h, uint8_t* dst) {
    for (int j = 0; j < h; ++j) {
        const uint8_t* yr = y + (size_t)j * w;
        const uint8_t* uvr = uv + (size_t)(j >> 1) * w;
        uint8_t* d = dst + (size_t)j * w * 4;
        for (int i = 0; i < w; i += 2) {
            int U = (int)uvr[i] - 128;
            int V = (int)uvr[i + 1] - 128;
            int rv = 409 * V + 128;
            int guv = -100 * U - 208 * V + 128;
            int bu = 516 * U + 128;
            int c0 = 298 * ((int)yr[i] - 16);
            int c1 = 298 * ((int)yr[i + 1] - 16);
            uint8_t r0 = clamp8((c0 + rv) >> 8), g0 = clamp8((c0 + guv) >> 8), b0 = clamp8((c0 + bu) >> 8);
            uint8_t r1 = clamp8((c1 + rv) >> 8), g1 = clamp8((c1 + guv) >> 8), b1 = clamp8((c1 + bu) >> 8);
            if (BGRA) {
                d[4 * i + 0] = b0; d[4 * i + 1] = g0; d[4 * i + 2] = r0; d[4 * i + 3] = 255;
                d[4 * i + 4] = b1; d[4 * i + 5] = g1; d[4 * i + 6] = r1; d[4 * i + 7] = 255;
            } else {
                d[4 * i + 0] = r0; d[4 * i + 1] = g0; d[4 * i + 2] = b0; d[4 * i + 3] = 255;
                d[4 * i + 4] = r1; d[4 * i + 5] = g1; d[4 * i + 6] = b1; d[4 * i + 7] = 255;
            }
        }
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

// Acumuladores por modo (0 = esticado, 1 = nativo), em microssegundos.
struct Acc {
    uint64_t n, conv, clr, upd, copy, ovl, pres;
};

void logAcc(const char* name, const Acc& a) {
    char b[160];
    size_t n = putS(b, 0, "modo ");
    n = putS(b, n, name);
    n = putS(b, n, ": quadros medidos=");
    n = putD(b, n, a.n);
    b[n++] = '\n';
    rawWrite(b, n);
    if (a.n == 0) return;
    char l[80];
    size_t m = putS(l, 0, "  media conv (us)");
    logDec(l, a.conv / a.n);
    (void)m;
    logDec("  media limpar (us)", a.clr / a.n);
    logDec("  media atualizar textura (us)", a.upd / a.n);
    logDec("  media copiar/esticar (us)", a.copy / a.n);
    logDec("  media barra/estatisticas (us)", a.ovl / a.n);
    logDec("  media apresentar tela (us)", a.pres / a.n);
}

}  // namespace

bool player::run(SDL_Renderer* ren, const char* path, const char* title, char* err, size_t errCap) {
    if (err && errCap) err[0] = 0;
    g_tscFreq = sceKernelGetTscFrequency();
    g_fd = sceKernelOpen(kLogPath, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    out("PurpleFlix player v2 (medicao por etapa)");
    out(path);

    if (!ensureBlocks()) return failMsg(err, errCap, "Memória de vídeo indisponível.");

    // Informacoes do renderizador e da janela (para escolher o formato de pixel certo).
    SDL_RendererInfo ri;
    memset(&ri, 0, sizeof ri);
    SDL_GetRendererInfo(ren, &ri);
    {
        char b[200];
        snprintf(b, sizeof b, "renderizador: %s, flags=0x%x, formatos de textura=%d", ri.name ? ri.name : "?",
                 (unsigned)ri.flags, (int)ri.num_texture_formats);
        out(b);
        for (Uint32 i = 0; i < ri.num_texture_formats && i < 8; ++i) {
            snprintf(b, sizeof b, "  formato %u: %s", (unsigned)i, SDL_GetPixelFormatName(ri.texture_formats[i]));
            out(b);
        }
    }
    SDL_Window* win = SDL_GetWindowFromID(1);  // a janela do app é a primeira criada (id 1)
    Uint32 wf = win ? SDL_GetWindowPixelFormat(win) : SDL_PIXELFORMAT_UNKNOWN;
    {
        char b[160];
        snprintf(b, sizeof b, "formato de pixel da janela: %s", SDL_GetPixelFormatName(wf));
        out(b);
    }
    Uint32 texFmt = SDL_PIXELFORMAT_ABGR8888;
    bool bgra = false;
    if (wf == SDL_PIXELFORMAT_ARGB8888 || wf == SDL_PIXELFORMAT_RGB888) {
        texFmt = wf;
        bgra = true;
    } else if (wf == SDL_PIXELFORMAT_ABGR8888 || wf == SDL_PIXELFORMAT_BGR888) {
        texFmt = wf;
        bgra = false;
    }
    {
        char b[160];
        snprintf(b, sizeof b, "formato da textura de video: %s (ordem dos bytes: %s)", SDL_GetPixelFormatName(texFmt),
                 bgra ? "B,G,R,A" : "R,G,B,A");
        out(b);
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

    SDL_Texture* tex = SDL_CreateTexture(ren, texFmt, SDL_TEXTUREACCESS_STREAMING, (int)vw, (int)vh);
    if (!tex) {
        closeAv();
        return failMsg(err, errCap, "Não foi possível criar a textura de vídeo.");
    }

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

    // Destinos do video: 0 = esticado (mantem a proporcao), 1 = nativo centralizado.
    SDL_Rect dstMode[2];
    {
        int dw, dh;
        if ((long)vw * H >= (long)vh * W) {
            dw = W;
            dh = (int)((long)vh * W / (long)vw);
        } else {
            dh = H;
            dw = (int)((long)vw * H / (long)vh);
        }
        dstMode[0] = {(W - dw) / 2, (H - dh) / 2, dw, dh};
        dstMode[1] = {((int)W - (int)vw) / 2, ((int)H - (int)vh) / 2, (int)vw, (int)vh};
    }
    int mode = 0;
    const char* const modeName[2] = {"esticado", "nativo"};

    const Color kBar = {0, 0, 0, 170};
    const Color kText = {238, 230, 252, 255};
    const Color kDim = {170, 150, 200, 255};
    const Color kPurple = {168, 85, 247, 255};

    alignas(16) uint8_t fi[512];
    bool havePending = false;
    uint64_t pPtr = 0, pTs = 0, lastTs = 0;
    bool paused = false, showStats = false, dirty = true, renderedFrame = false;
    uint32_t prevBtn = pad::poll();
    uint64_t overlayUntilMs = 4000;
    uint64_t start = rdtsc();
    uint64_t endIdleSince = 0;

    int presented = 0, drops = 0;
    uint64_t lastConvUs = 0, lastClr = 0, lastUpd = 0, lastCopy = 0, lastOvl = 0, lastPres = 0;
    int64_t sumLate = 0;
    int lateN = 0;
    Acc acc[2] = {{0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0}};
    int fps = 0, fpsCount = 0;
    uint64_t fpsStartMs = 0;
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
            dirty = true;
        }
        if (press & pad::kTriangle) {
            showStats = !showStats;
            dirty = true;
        }
        if (press & pad::kSquare) {
            mode = 1 - mode;
            dirty = true;
        }
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
        }

        bool audioDone = __atomic_load_n(&g_audioDone, __ATOMIC_SEQ_CST) != 0;
        uint64_t clockMs = audioDone ? 0 : __atomic_load_n(&g_audioMs, __ATOMIC_SEQ_CST);

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
                    size_t ysize = (size_t)vw * vh;
                    uint64_t t0 = rdtsc();
                    memcpy(g_copy, p, ysize * 3 / 2);
                    if (bgra) nv12Convert<true>(g_copy, g_copy + ysize, (int)vw, (int)vh, g_rgba);
                    else nv12Convert<false>(g_copy, g_copy + ysize, (int)vw, (int)vh, g_rgba);
                    lastConvUs = usFrom(rdtsc() - t0);
                    if (!audioDone) {
                        sumLate += (int64_t)clockMs - (int64_t)pTs;
                        ++lateN;
                    }
                    lastTs = pTs;
                    ++presented;
                    ++fpsCount;
                    havePending = false;
                    renderedFrame = true;
                    dirty = true;
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
            if (showStats) dirty = true;
        }
        bool overlayVisible = paused || nowMs < overlayUntilMs;
        if (overlayVisible != overlayShownLast) {
            overlayShownLast = overlayVisible;
            dirty = true;
        }

        if (dirty) {
            dirty = false;
            const SDL_Rect& dst = dstMode[mode];
            bool needClear = !(dst.w == W && dst.h == H);

            uint64_t a0 = rdtsc();
            if (needClear) {
                SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
                SDL_RenderClear(ren);
            }
            uint64_t a1 = rdtsc();
            if (presented > 0) SDL_UpdateTexture(tex, nullptr, g_rgba, (int)vw * 4);
            uint64_t a2 = rdtsc();
            if (presented > 0) SDL_RenderCopy(ren, tex, nullptr, &dst);
            uint64_t a3 = rdtsc();
            if (overlayVisible) {
                gfx::rect(0, 0, W, 90, kBar);
                gfx::text(60, 28, title ? title : "", 4, kText);
                gfx::rect(0, 940, W, 140, kBar);
                int frac = durMs ? (int)((lastTs * 1800ull) / durMs) : 0;
                if (frac > 1800) frac = 1800;
                gfx::rect(60, 980, 1800, 10, Color{70, 55, 95, 255});
                gfx::rect(60, 980, frac, 10, kPurple);
                char tb[64];
                unsigned cs = (unsigned)(lastTs / 1000), ds = (unsigned)(durMs / 1000);
                snprintf(tb, sizeof tb, "%u:%02u / %u:%02u", cs / 60, cs % 60, ds / 60, ds % 60);
                gfx::text(60, 1004, tb, 3, kText);
                gfx::text(60, 1040, "X: pausar    O: voltar    Triângulo: estatísticas    Quadrado: modo", 2, kDim);
                if (paused) gfx::textCentered(W / 2, 480, "PAUSADO", 8, kText);
            }
            if (showStats) {
                char sb[96];
                gfx::rect(W - 600, 100, 560, 420, kBar);
                int y = 116;
                snprintf(sb, sizeof sb, "modo: %s   fps: %d", modeName[mode], fps);
                gfx::text(W - 580, y, sb, 3, kText);
                y += 34;
                snprintf(sb, sizeof sb, "conversão: %u.%u ms", (unsigned)(lastConvUs / 1000), (unsigned)((lastConvUs % 1000) / 100));
                gfx::text(W - 580, y, sb, 3, kText);
                y += 34;
                snprintf(sb, sizeof sb, "limpar: %u.%u ms", (unsigned)(lastClr / 1000), (unsigned)((lastClr % 1000) / 100));
                gfx::text(W - 580, y, sb, 3, kText);
                y += 34;
                snprintf(sb, sizeof sb, "textura: %u.%u ms", (unsigned)(lastUpd / 1000), (unsigned)((lastUpd % 1000) / 100));
                gfx::text(W - 580, y, sb, 3, kText);
                y += 34;
                snprintf(sb, sizeof sb, "copiar/esticar: %u.%u ms", (unsigned)(lastCopy / 1000), (unsigned)((lastCopy % 1000) / 100));
                gfx::text(W - 580, y, sb, 3, kText);
                y += 34;
                snprintf(sb, sizeof sb, "barra: %u.%u ms", (unsigned)(lastOvl / 1000), (unsigned)((lastOvl % 1000) / 100));
                gfx::text(W - 580, y, sb, 3, kText);
                y += 34;
                snprintf(sb, sizeof sb, "apresentar: %u.%u ms", (unsigned)(lastPres / 1000), (unsigned)((lastPres % 1000) / 100));
                gfx::text(W - 580, y, sb, 3, kText);
                y += 34;
                snprintf(sb, sizeof sb, "descartados: %d", drops);
                gfx::text(W - 580, y, sb, 3, kText);
                y += 34;
                snprintf(sb, sizeof sb, "som: %s", g_audioFailed ? "indisponível" : "ok");
                gfx::text(W - 580, y, sb, 3, g_audioFailed ? Color{255, 130, 150, 255} : kText);
            }
            uint64_t a4 = rdtsc();
            SDL_RenderPresent(ren);
            uint64_t a5 = rdtsc();

            if (renderedFrame) {
                renderedFrame = false;
                lastClr = usFrom(a1 - a0);
                lastUpd = usFrom(a2 - a1);
                lastCopy = usFrom(a3 - a2);
                lastOvl = usFrom(a4 - a3);
                lastPres = usFrom(a5 - a4);
                Acc& ac = acc[mode];
                ++ac.n;
                ac.conv += lastConvUs;
                ac.clr += lastClr;
                ac.upd += lastUpd;
                ac.copy += lastCopy;
                ac.ovl += lastOvl;
                ac.pres += lastPres;
            }
        }
        sceKernelUsleep(1000);
    }

    // Encerra: audio, textura e player.
    __atomic_store_n(&g_stop, 1, __ATOMIC_SEQ_CST);
    if (haveThread) scePthreadJoin(thr, nullptr);
    SDL_DestroyTexture(tex);
    closeAv();

    out("---- RESUMO ----");
    logDec("terminou sozinho (1=sim)", finished ? 1 : 0);
    logDec("quadros apresentados", (uint64_t)presented);
    logDec("quadros descartados", (uint64_t)drops);
    logDec("audio: pedacos enviados", (uint64_t)__atomic_load_n(&g_audioChunks, __ATOMIC_SEQ_CST));
    logDec("audio falhou (1=sim)", (uint64_t)g_audioFailed);
    if (lateN > 0) logSigned("atraso medio do video vs som (ms, so enquanto o som tocava)", sumLate / lateN);
    logAcc(modeName[0], acc[0]);
    logAcc(modeName[1], acc[1]);
    out("fim.");
    if (g_fd >= 0) {
        sceKernelClose(g_fd);
        g_fd = -1;
    }
    return true;
}

// FIM DO ARQUIVO (player.cpp)
