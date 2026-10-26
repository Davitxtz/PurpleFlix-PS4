// PurpleFlix Probe v8: audio real (SceAudioOut em thread propria) + video no ritmo do audio.
// ATENCAO: a estrutura de inicializacao abaixo NAO foi verificada no PS4.
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <orbis/AudioOut.h>
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

constexpr size_t kMaxW = 1280;
constexpr size_t kMaxH = 720;
alignas(64) uint8_t g_copy[kMaxW * kMaxH * 3 / 2 + 4096];
alignas(64) uint8_t g_rgba[kMaxW * kMaxH * 4];

uint64_t g_tscFreq = 0;
inline uint64_t rdtsc() { return __builtin_ia32_rdtsc(); }
uint64_t usFrom(uint64_t ticks) { return g_tscFreq ? (ticks * 1000000ull) / g_tscFreq : 0; }

// Estado compartilhado com a thread de audio.
int32_t g_av = 0;
int g_stop = 0;
int g_audioDone = 0;
int g_audioFailed = 0;
uint64_t g_audioMs = 0;
uint64_t g_audioWallMs = 0;
int g_audioChunks = 0;

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

inline uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v)); }

// Conversao NV12 -> RGBA (BT.601, inteiros). Plano Y seguido de UV intercalado (confirmado).
void nv12ToRgba(const uint8_t* y, const uint8_t* uv, int w, int h, uint8_t* dst) {
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
            d[4 * i + 0] = clamp8((c0 + rv) >> 8);
            d[4 * i + 1] = clamp8((c0 + guv) >> 8);
            d[4 * i + 2] = clamp8((c0 + bu) >> 8);
            d[4 * i + 3] = 255;
            d[4 * i + 4] = clamp8((c1 + rv) >> 8);
            d[4 * i + 5] = clamp8((c1 + guv) >> 8);
            d[4 * i + 6] = clamp8((c1 + bu) >> 8);
            d[4 * i + 7] = 255;
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
typedef int32_t (*FnGetFrame)(int32_t, void*);
typedef int32_t (*FnClose)(int32_t);

// ---- thread de audio: toca cada pedaco pela SceAudioOut (sem libc) ----
void* audioThread(void*) {
    uint64_t t0 = rdtsc();
    int32_t r = sceAudioOutInit();
    logHex("audio: sceAudioOutInit", (uint32_t)r);
    int32_t ah = sceAudioOutOpen(ORBIS_USER_SERVICE_USER_ID_SYSTEM, ORBIS_AUDIO_OUT_PORT_TYPE_MAIN, 0, 1024, 48000,
                                 ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
    logHex("audio: sceAudioOutOpen (handle)", (uint32_t)ah);
    if (ah < 0) {
        __atomic_store_n(&g_audioFailed, 1, __ATOMIC_SEQ_CST);
        __atomic_store_n(&g_audioDone, 1, __ATOMIC_SEQ_CST);
        return nullptr;
    }
    alignas(16) uint8_t ai[512];
    int misses = 0;
    int logged = 0;
    for (;;) {
        if (__atomic_load_n(&g_stop, __ATOMIC_SEQ_CST)) break;
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
                int32_t o = sceAudioOutOutput(ah, (const void*)(uintptr_t)ptr);
                __atomic_store_n(&g_audioMs, ts, __ATOMIC_SEQ_CST);
                int c = __atomic_add_fetch(&g_audioChunks, 1, __ATOMIC_SEQ_CST);
                if (logged < 3 || o < 0) {
                    ++logged;
                    logHex("audio: sceAudioOutOutput", (uint32_t)o);
                }
                if (c == 1) logDec("audio: primeiro pedaco enviado em ms (tempo real)", usFrom(rdtsc() - t0) / 1000);
            }
        } else {
            ++misses;
            int32_t active = ((FnIsActive)sceAvPlayerIsActive)(g_av);
            if (active == 0 && misses > 30) break;
            sceKernelUsleep(2000);
        }
    }
    sceAudioOutClose(ah);
    __atomic_store_n(&g_audioWallMs, usFrom(rdtsc() - t0) / 1000, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_audioDone, 1, __ATOMIC_SEQ_CST);
    return nullptr;
}

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

    char b[400];
    snprintf(b, sizeof b, "PurpleFlix Probe v8 - execucao %d (audio real + video no ritmo do audio)", run);
    out(b);
    logDec("frequencia do TSC (Hz)", g_tscFreq);

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
    logHex("sceSysmoduleLoadModule(AV_PLAYER)", (uint32_t)r);

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
    g_av = ((FnInit)sceAvPlayerInit)(&d);
    logHex("sceAvPlayerInit", (uint32_t)g_av);
    if (g_av <= 0 || !haveFile) {
        out("RESULTADO: Init falhou ou arquivo ausente. Parando.");
        return 0;
    }

    out("chamando sceAvPlayerAddSource...");
    int32_t rs = ((FnAddSource)sceAvPlayerAddSource)(g_av, file);
    logHex("sceAvPlayerAddSource", (uint32_t)rs);

    int32_t sc = 0;
    for (int i = 0; i < 50; ++i) {
        sc = ((FnStreamCount)sceAvPlayerStreamCount)(g_av);
        if (sc > 0) break;
        sceKernelUsleep(100000);
    }
    logDec("StreamCount", (uint64_t)sc);

    out("chamando sceAvPlayerStart...");
    int32_t rst = ((FnStart)sceAvPlayerStart)(g_av);
    logHex("sceAvPlayerStart", (uint32_t)rst);

    // Thread de audio (pilha de 256 KB).
    OrbisPthreadAttr attr;
    scePthreadAttrInit(&attr);
    scePthreadAttrSetstacksize(&attr, 256 * 1024);
    OrbisPthread thr;
    int32_t rt = scePthreadCreate(&thr, &attr, audioThread, nullptr, "pf_audio");
    logHex("scePthreadCreate(audio)", (uint32_t)rt);
    bool haveThread = (rt == 0);

    // ---- laco de video: segura cada quadro ate chegar a hora dele (relogio = audio enviado) ----
    alignas(16) uint8_t fi[512];
    bool havePending = false;
    uint64_t pPtr = 0, pTs = 0;
    uint32_t pW = 0, pH = 0;
    uint64_t lastTs = 0;
    int presented = 0, gaps = 0, lateOver40 = 0, tsLogged = 0;
    int64_t sumLate = 0, maxLate = -1000000, minLate = 1000000;
    uint64_t convTotalUs = 0, convMaxUs = 0, copyTotalUs = 0;
    uint32_t sink = 0;
    uint64_t start = rdtsc();
    uint64_t idleSince = 0;

    for (;;) {
        uint64_t nowMs = usFrom(rdtsc() - start) / 1000;
        if (nowMs > 14000) {
            out("tempo limite (14 s) atingido");
            break;
        }
        bool audioDone = __atomic_load_n(&g_audioDone, __ATOMIC_SEQ_CST) != 0;

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
                if (inPool && w > 0 && hh > 0 && w <= kMaxW && hh <= kMaxH) {
                    havePending = true;
                    pPtr = ptr;
                    pTs = ts;
                    pW = w;
                    pH = hh;
                    idleSince = 0;
                }
            }
        }

        if (havePending) {
            uint64_t am = __atomic_load_n(&g_audioMs, __ATOMIC_SEQ_CST);
            if (pTs <= am + 10 || audioDone) {
                const uint8_t* p = (const uint8_t*)(uintptr_t)pPtr;
                size_t ysize = (size_t)pW * pH;
                uint64_t t0 = rdtsc();
                memcpy(g_copy, p, ysize * 3 / 2);
                uint64_t t1 = rdtsc();
                nv12ToRgba(g_copy, g_copy + ysize, (int)pW, (int)pH, g_rgba);
                uint64_t t2 = rdtsc();
                // Le o resultado FORA do cronometro, para o compilador nao poder remover a conversao.
                const uint32_t* q = (const uint32_t*)g_rgba;
                uint32_t cs = 0;
                for (size_t k = 0; k < ysize; ++k) cs ^= q[k] + (uint32_t)k;
                sink ^= cs;

                copyTotalUs += usFrom(t1 - t0);
                uint64_t cu = usFrom(t2 - t1);
                convTotalUs += cu;
                if (cu > convMaxUs) convMaxUs = cu;

                int64_t late = (int64_t)am - (int64_t)pTs;
                sumLate += late;
                if (late > maxLate) maxLate = late;
                if (late < minLate) minLate = late;
                if (late > 40) ++lateOver40;
                if (presented > 0 && pTs > lastTs + 50) {
                    ++gaps;
                    if (gaps <= 5) {
                        char gb[160];
                        size_t n = putS(gb, 0, "lacuna: ts anterior=");
                        n = putD(gb, n, lastTs);
                        n = putS(gb, n, " ts atual=");
                        n = putD(gb, n, pTs);
                        gb[n++] = '\n';
                        rawWrite(gb, n);
                    }
                }
                lastTs = pTs;
                ++presented;
                if (tsLogged < 12 || presented % 30 == 0) {
                    ++tsLogged;
                    snprintf(b, sizeof b, "quadro %d: ts=%llu ms, audio=%llu ms, atraso=%lld ms, tempo_real=%llu ms, conv=%llu us",
                             presented, (unsigned long long)pTs, (unsigned long long)am, (long long)late,
                             (unsigned long long)nowMs, (unsigned long long)cu);
                    out(b);
                }
                havePending = false;
            }
        } else if (audioDone) {
            if (idleSince == 0) idleSince = nowMs;
            if (nowMs - idleSince > 1500) break;
        }
        sceKernelUsleep(1000);
    }

    uint64_t wallMs = usFrom(rdtsc() - start) / 1000;
    __atomic_store_n(&g_stop, 1, __ATOMIC_SEQ_CST);
    if (haveThread) scePthreadJoin(thr, nullptr);

    out("---- RESUMO ----");
    logDec("quadros apresentados (esperado ~177)", (uint64_t)presented);
    logDec("lacunas > 50 ms entre quadros", (uint64_t)gaps);
    logDec("tempo real total do video (ms)", wallMs);
    logDec("audio: pedacos enviados (esperado ~281)", (uint64_t)__atomic_load_n(&g_audioChunks, __ATOMIC_SEQ_CST));
    logDec("audio: tempo real da thread (ms, esperado ~6000 se bloqueia em tempo real)",
           __atomic_load_n(&g_audioWallMs, __ATOMIC_SEQ_CST));
    logDec("audio falhou ao abrir (1=sim)", (uint64_t)__atomic_load_n(&g_audioFailed, __ATOMIC_SEQ_CST));
    if (presented > 0) {
        logDec("conversao NV12->RGBA media (us)", convTotalUs / (uint64_t)presented);
        logDec("conversao NV12->RGBA maxima (us)", convMaxUs);
        logDec("copia media (us)", copyTotalUs / (uint64_t)presented);
        snprintf(b, sizeof b, "atraso do video vs audio: minimo=%lld ms, medio=%lld ms, maximo=%lld ms, acima de 40 ms em %d quadros",
                 (long long)minLate, (long long)(sumLate / presented), (long long)maxLate, lateOver40);
        out(b);
    }
    logHex("soma de verificacao (evita remover a conversao)", sink);

    ((FnStop)sceAvPlayerStop)(g_av);
    ((FnClose)sceAvPlayerClose)(g_av);
    out("fim.");
    return 0;
}

// FIM DO ARQUIVO (probe/main.cpp)
