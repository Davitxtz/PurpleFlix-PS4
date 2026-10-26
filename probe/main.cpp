// PurpleFlix Probe v7: confirma quadros do AvPlayer, mede conversao NV12->RGBA, despeja quadros.
// ATENCAO: a estrutura de inicializacao abaixo NAO foi verificada no PS4.
// Layout baseado na documentacao da comunidade (Vita), ajustado para 64 bits.
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <orbis/AvPlayer.h>
#include <orbis/Sysmodule.h>
#include <orbis/libkernel.h>

// Rotulo assembly: evita conflito com o prototype do header.
extern "C" int32_t pf_load_module(uint16_t id) __asm__("sceSysmoduleLoadModule");

namespace {

const char* const kDir = "/data/purpleflix";
const char* const kRunFile = "/data/purpleflix/probe_run.txt";
const char* const kFileApp = "/app0/teste.mp4";
const char* const kFileData = "/data/purpleflix/teste.mp4";
int32_t g_fd = -1;

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

// Buffers para medir o custo de copia e conversao (720p).
constexpr size_t kMaxW = 1280;
constexpr size_t kMaxH = 720;
alignas(64) uint8_t g_copy[kMaxW * kMaxH * 3 / 2 + 4096];
alignas(64) uint8_t g_rgba[kMaxW * kMaxH * 4];

uint64_t g_tscFreq = 0;
inline uint64_t rdtsc() { return __builtin_ia32_rdtsc(); }
uint64_t usFrom(uint64_t ticks) { return g_tscFreq ? (ticks * 1000000ull) / g_tscFreq : 0; }

// ---- escrita sem libc (segura para threads do player) ----
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

void logReq(const char* what, uint32_t align, uint32_t size, const void* ret) {
    if (size < 65536) return;
    char b[200];
    size_t n = 0;
    n = putS(b, n, "  [callback] ");
    n = putS(b, n, what);
    n = putS(b, n, " alinh=");
    n = putD(b, n, align);
    n = putS(b, n, " tam=");
    n = putD(b, n, size);
    n = putS(b, n, " ret=");
    n = putH(b, n, (uint64_t)(uintptr_t)ret);
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
    void* p = nullptr;
    if (size >= (1u << 20) && g_gen.base) p = bump(g_gen.base, g_gen.size, &g_gen.off, align, size);
    else p = bump(g_pool, kPoolSize, &g_poolOff, align, size);
    logReq("GERAL ", align, size, p);
    return p;
}

void cbFree(void*, void*) {}

void* cbAllocTex(void*, uint32_t align, uint32_t size) {
    void* p = nullptr;
    if (g_tex.base) p = bump(g_tex.base, g_tex.size, &g_tex.off, align, size);
    else p = bump(g_pool, kPoolSize, &g_poolOff, align, size);
    logReq("IMAGEM", align, size, p);
    return p;
}

void cbFreeTex(void*, void*) {}

bool initBlock(Block& blk, const char* name, size_t want, size_t minSize) {
    size_t dm = sceKernelGetDirectMemorySize();
    char b[220];
    for (size_t len = want; len >= minSize; len >>= 1) {
        off_t phys = 0;
        int32_t r = sceKernelAllocateDirectMemory(0, (off_t)dm, len, 0x200000, ORBIS_KERNEL_WB_ONION, &phys);
        if (r != 0) {
            snprintf(b, sizeof b, "bloco %s: Allocate(%llu MB) = 0x%08X", name, (unsigned long long)(len >> 20),
                     (unsigned)r);
            out(b);
            continue;
        }
        void* addr = nullptr;
        r = sceKernelMapDirectMemory(&addr, len, ORBIS_KERNEL_PROT_CPU_RW | ORBIS_KERNEL_PROT_GPU_RW, 0, phys,
                                     0x200000);
        if (r == 0 && addr) {
            blk.base = (uint8_t*)addr;
            blk.size = len;
            blk.off = 0;
            snprintf(b, sizeof b, "bloco %s: %llu MB em %p", name, (unsigned long long)(len >> 20), addr);
            out(b);
            return true;
        }
        snprintf(b, sizeof b, "bloco %s: Map = 0x%08X", name, (unsigned)r);
        out(b);
    }
    return false;
}

void dumpWords(const char* label, const uint8_t* p, int words32) {
    char b[300];
    for (int i = 0; i < words32; i += 6) {
        int cnt = (words32 - i < 6) ? (words32 - i) : 6;
        uint32_t w[6] = {0, 0, 0, 0, 0, 0};
        memcpy(w, p + i * 4, (size_t)cnt * 4);
        int n = snprintf(b, sizeof b, "%s +%03d:", label, i * 4);
        for (int k = 0; k < cnt; ++k) n += snprintf(b + n, sizeof b - (size_t)n, " %08x", (unsigned)w[k]);
        out(b);
    }
}

// Grava um bloco de memoria em arquivo (para eu analisar a imagem no servidor).
void dumpFile(const char* path, const uint8_t* data, size_t len) {
    int32_t fd = sceKernelOpen(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        out("falha ao criar arquivo de despejo");
        return;
    }
    size_t off = 0;
    while (off < len) {
        size_t n = sceKernelWrite(fd, data + off, len - off);
        if (n == 0 || n > len - off) break;
        off += n;
    }
    sceKernelClose(fd);
}

inline uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v)); }

// Conversao NV12 -> RGBA (BT.601, inteiros). Assume plano Y seguido de UV intercalado.
void nv12ToRgba(const uint8_t* y, const uint8_t* uv, int w, int h, uint8_t* dst) {
    for (int j = 0; j < h; ++j) {
        const uint8_t* yr = y + (size_t)j * w;
        const uint8_t* uvr = uv + (size_t)(j >> 1) * w;
        uint8_t* d = dst + (size_t)j * w * 4;
        for (int i = 0; i < w; ++i) {
            int Y = (int)yr[i] - 16;
            int U = (int)uvr[i & ~1] - 128;
            int V = (int)uvr[(i & ~1) + 1] - 128;
            int c = 298 * Y;
            d[4 * i + 0] = clamp8((c + 409 * V + 128) >> 8);
            d[4 * i + 1] = clamp8((c - 100 * U - 208 * V + 128) >> 8);
            d[4 * i + 2] = clamp8((c + 516 * U + 128) >> 8);
            d[4 * i + 3] = 255;
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

int readRun() {
    FILE* f = fopen(kRunFile, "r");
    int n = 0;
    if (f) {
        char buf[16];
        if (fgets(buf, sizeof buf, f)) n = atoi(buf);
        fclose(f);
    }
    return n < 0 ? 0 : n;
}

void writeRun(int n) {
    FILE* f = fopen(kRunFile, "w");
    if (f) {
        fprintf(f, "%d\n", n);
        fclose(f);
    }
}

}  // namespace

int main() {
    mkdir(kDir, 0777);
    int run = readRun();
    writeRun(run + 1);
    g_tscFreq = sceKernelGetTscFrequency();

    char path[96];
    snprintf(path, sizeof path, "%s/video_probe_run%d.txt", kDir, run);
    g_fd = sceKernelOpen(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);

    char b[360];
    snprintf(b, sizeof b, "PurpleFlix Probe v7 - execucao %d (sem EnableStream; retorno testado so no byte baixo)", run);
    out(b);
    snprintf(b, sizeof b, "frequencia do TSC = %llu Hz", (unsigned long long)g_tscFreq);
    out(b);

    const char* file = kFileApp;
    struct stat st;
    bool haveFile = (stat(kFileApp, &st) == 0);
    if (!haveFile) {
        file = kFileData;
        haveFile = (stat(kFileData, &st) == 0);
    }
    snprintf(b, sizeof b, "arquivo de teste: %s (%s)", haveFile ? file : "NENHUM", haveFile ? "encontrado" : "AUSENTE");
    out(b);

    int32_t r = pf_load_module(ORBIS_SYSMODULE_AV_PLAYER);
    snprintf(b, sizeof b, "sceSysmoduleLoadModule(AV_PLAYER) = 0x%08X", (unsigned)r);
    out(b);

    bool genOk = initBlock(g_gen, "GERAL", 160u << 20, 64u << 20);
    bool texOk = initBlock(g_tex, "IMAGEM", 256u << 20, 64u << 20);
    snprintf(b, sizeof b, "blocos: geral=%s imagem=%s", genOk ? "OK" : "FALHOU", texOk ? "OK" : "FALHOU");
    out(b);

    InitData d;
    memset(&d, 0, sizeof d);
    d.mem.obj = nullptr;
    d.mem.alloc = cbAlloc;
    d.mem.dealloc = cbFree;
    d.mem.allocTex = cbAllocTex;
    d.mem.deallocTex = cbFreeTex;
    d.debugLevel = 0;
    d.basePriority = 700;
    d.numFrameBuffers = 4;
    d.autoStart = 0;
    d.lang = "eng";

    out("chamando sceAvPlayerInit...");
    int32_t h = ((FnInit)sceAvPlayerInit)(&d);
    snprintf(b, sizeof b, "sceAvPlayerInit = 0x%08X", (unsigned)h);
    out(b);
    if (h <= 0 || !haveFile) {
        out("RESULTADO: Init falhou ou arquivo ausente. Parando.");
        return 0;
    }

    out("chamando sceAvPlayerAddSource...");
    int32_t rs = ((FnAddSource)sceAvPlayerAddSource)(h, file);
    snprintf(b, sizeof b, "sceAvPlayerAddSource = 0x%08X", (unsigned)rs);
    out(b);

    int32_t sc = 0;
    int waited = 0;
    for (int i = 0; i < 50; ++i) {
        sc = ((FnStreamCount)sceAvPlayerStreamCount)(h);
        if (sc > 0) break;
        sceKernelUsleep(100000);
        ++waited;
    }
    snprintf(b, sizeof b, "StreamCount = %d (apos esperar %d ms)", (int)sc, waited * 100);
    out(b);

    out("chamando sceAvPlayerStart...");
    int32_t rst = ((FnStart)sceAvPlayerStart)(h);
    snprintf(b, sizeof b, "sceAvPlayerStart = 0x%08X", (unsigned)rst);
    out(b);

    alignas(16) uint8_t fi[512];
    alignas(16) uint8_t ai[512];
    int vTrue = 0, aTrue = 0, aDumps = 0, tsLogged = 0, convN = 0;
    bool dumpedA = false, dumpedB = false;
    uint64_t firstTs = 0, lastTs = 0;
    uint64_t copyUs = 0, convUs = 0, convMaxUs = 0;
    int nextPrint = 1;
    uint64_t start = rdtsc();

    for (;;) {
        uint64_t nowMs = usFrom(rdtsc() - start) / 1000;
        if (nowMs > 9000) break;
        if ((int)(nowMs / 1000) >= nextPrint) {
            int32_t active = ((FnIsActive)sceAvPlayerIsActive)(h);
            snprintf(b, sizeof b, "t=%llu ms: ativo=%d quadros_reais=%d audio_real=%d", (unsigned long long)nowMs,
                     (int)active, vTrue, aTrue);
            out(b);
            ++nextPrint;
        }

        memset(fi, 0, sizeof fi);
        int32_t ret = ((FnGetFrame)sceAvPlayerGetVideoData)(h, fi);
        if ((ret & 0xFF) != 0) {
            uint64_t ptr = 0, ts = 0;
            uint32_t w = 0, hh = 0;
            memcpy(&ptr, fi, 8);
            memcpy(&ts, fi + 16, 8);
            memcpy(&w, fi + 24, 4);
            memcpy(&hh, fi + 28, 4);
            const uint8_t* p = (const uint8_t*)(uintptr_t)ptr;
            bool inPool = g_tex.base && p >= g_tex.base && p < g_tex.base + g_tex.size;
            if (inPool && w > 0 && hh > 0 && w <= kMaxW && hh <= kMaxH) {
                ++vTrue;
                if (vTrue == 1) firstTs = ts;
                lastTs = ts;
                if (tsLogged < 12) {
                    ++tsLogged;
                    snprintf(b, sizeof b, "quadro %d: ts=%llu ms, tempo_real=%llu ms, %ux%u, ptr=%p", vTrue,
                             (unsigned long long)ts, (unsigned long long)nowMs, (unsigned)w, (unsigned)hh,
                             (const void*)p);
                    out(b);
                }
                size_t ysize = (size_t)w * hh;
                if (convN < 30) {
                    uint64_t t0 = rdtsc();
                    memcpy(g_copy, p, ysize * 3 / 2);
                    uint64_t t1 = rdtsc();
                    nv12ToRgba(g_copy, g_copy + ysize, (int)w, (int)hh, g_rgba);
                    uint64_t t2 = rdtsc();
                    copyUs += usFrom(t1 - t0);
                    uint64_t cu = usFrom(t2 - t1);
                    convUs += cu;
                    if (cu > convMaxUs) convMaxUs = cu;
                    ++convN;
                }
                size_t room = (size_t)((g_tex.base + g_tex.size) - p);
                size_t dumpLen = room < 1600000u ? room : 1600000u;
                if (!dumpedA && ts >= 1000) {
                    dumpedA = true;
                    out("despejando frame_a.raw...");
                    dumpFile("/data/purpleflix/frame_a.raw", p, dumpLen);
                    snprintf(b, sizeof b, "frame_a.raw gravado (ts=%llu ms, %llu bytes)", (unsigned long long)ts,
                             (unsigned long long)dumpLen);
                    out(b);
                } else if (!dumpedB && ts >= 3000) {
                    dumpedB = true;
                    out("despejando frame_b.raw...");
                    dumpFile("/data/purpleflix/frame_b.raw", p, dumpLen);
                    snprintf(b, sizeof b, "frame_b.raw gravado (ts=%llu ms, %llu bytes)", (unsigned long long)ts,
                             (unsigned long long)dumpLen);
                    out(b);
                }
            }
        }

        memset(ai, 0, sizeof ai);
        int32_t ag = ((FnGetFrame)sceAvPlayerGetAudioData)(h, ai);
        if ((ag & 0xFF) != 0) {
            ++aTrue;
            if (aDumps < 2) {
                ++aDumps;
                snprintf(b, sizeof b, "audio: retorno=0x%08X", (unsigned)ag);
                out(b);
                dumpWords("audio", ai, 12);
            }
        }
        sceKernelUsleep(5000);
    }

    uint64_t totalMs = usFrom(rdtsc() - start) / 1000;
    snprintf(b, sizeof b, "RESULTADO: %d quadros de video REAIS em %llu ms; ts primeiro=%llu ultimo=%llu", vTrue,
             (unsigned long long)totalMs, (unsigned long long)firstTs, (unsigned long long)lastTs);
    out(b);
    if (convN > 0) {
        snprintf(b, sizeof b, "custo por quadro (%d medidos): copia=%llu us, conversao NV12->RGBA media=%llu us, maxima=%llu us",
                 convN, (unsigned long long)(copyUs / convN), (unsigned long long)(convUs / convN),
                 (unsigned long long)convMaxUs);
        out(b);
    }
    snprintf(b, sizeof b, "audio: %d chamadas com amostra real", aTrue);
    out(b);

    ((FnStop)sceAvPlayerStop)(h);
    ((FnClose)sceAvPlayerClose)(h);
    out("fim.");
    return 0;
}

// FIM DO ARQUIVO (probe/main.cpp)
