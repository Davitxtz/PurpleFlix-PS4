// PurpleFlix Probe v6: experimentos automaticos com o AvPlayer do sistema.
// ATENCAO: a estrutura de inicializacao abaixo NAO foi verificada no PS4.
// Layout baseado na documentacao da comunidade (Vita), ajustado para 64 bits.
// A cada execucao o Probe usa um modo diferente (contador em probe_run.txt).
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

// V = habilita video, A = habilita e consome audio, X = usa GetVideoDataEx.
const char* const kModes[] = {"", "A", "X", "VA", "VAX"};
constexpr int kModeCount = 5;

// Bloco estatico para pedidos pequenos.
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

// Callbacks: registra apenas os pedidos GRANDES (para o log nao ficar enorme).
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

// ---- alocadores sem libc (atomicos) ----
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
typedef int32_t (*FnEnableStream)(int32_t, uint32_t);
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
    const char* mode = kModes[run % kModeCount];
    bool fV = strchr(mode, 'V') != nullptr;
    bool fA = strchr(mode, 'A') != nullptr;
    bool fX = strchr(mode, 'X') != nullptr;

    char path[96];
    snprintf(path, sizeof path, "%s/video_probe_run%d.txt", kDir, run);
    g_fd = sceKernelOpen(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);

    char b[320];
    snprintf(b, sizeof b, "PurpleFlix Probe v6 - execucao %d, modo \"%s\" (V=habilita video, A=audio, X=GetVideoDataEx)",
             run, mode[0] ? mode : "nenhum");
    out(b);
    out("ATENCAO: estrutura de inicializacao NAO verificada no PS4.");

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

    int videoId = -1, audioId = -1;
    for (int i = 0; i < sc && i < 4; ++i) {
        alignas(16) uint8_t info[256];
        memset(info, 0, sizeof info);
        int32_t ri = ((FnGetStreamInfo)sceAvPlayerGetStreamInfo)(h, (uint32_t)i, info);
        uint32_t type = 0;
        memcpy(&type, info, 4);
        snprintf(b, sizeof b, "trilha %d: GetStreamInfo=0x%08X tipo=%u (0=video 1=audio)", i, (unsigned)ri,
                 (unsigned)type);
        out(b);
        char label[24];
        snprintf(label, sizeof label, "info trilha %d", i);
        dumpWords(label, info, 24);
        if (type == 0 && videoId < 0) videoId = i;
        if (type == 1 && audioId < 0) audioId = i;
    }

    if (fV && videoId >= 0) {
        int32_t re = ((FnEnableStream)sceAvPlayerEnableStream)(h, (uint32_t)videoId);
        snprintf(b, sizeof b, "EnableStream(video, trilha %d) = 0x%08X", videoId, (unsigned)re);
        out(b);
    }
    if (fA && audioId >= 0) {
        int32_t re = ((FnEnableStream)sceAvPlayerEnableStream)(h, (uint32_t)audioId);
        snprintf(b, sizeof b, "EnableStream(audio, trilha %d) = 0x%08X", audioId, (unsigned)re);
        out(b);
    }

    out("chamando sceAvPlayerStart...");
    int32_t rst = ((FnStart)sceAvPlayerStart)(h);
    snprintf(b, sizeof b, "sceAvPlayerStart = 0x%08X", (unsigned)rst);
    out(b);

    alignas(16) uint8_t fi[512];
    alignas(16) uint8_t ai[512];
    int vNon = 0, vZero = 0, aNon = 0, vDumps = 0, aDumps = 0;
    uint32_t distinct[4] = {0, 0, 0, 0};
    int nd = 0;

    for (int i = 0; i < 1000; ++i) {
        memset(fi, 0, sizeof fi);
        int32_t got = fX ? ((FnGetFrame)sceAvPlayerGetVideoDataEx)(h, fi)
                         : ((FnGetFrame)sceAvPlayerGetVideoData)(h, fi);
        if (got == 0) {
            ++vZero;
        } else {
            ++vNon;
            bool seen = false;
            for (int k = 0; k < nd; ++k) {
                if (distinct[k] == (uint32_t)got) seen = true;
            }
            if (!seen && nd < 4) distinct[nd++] = (uint32_t)got;
            if (vDumps < 4) {
                ++vDumps;
                uint64_t ptr = 0;
                memcpy(&ptr, fi, 8);
                bool inPool = g_tex.base && ptr >= (uint64_t)(uintptr_t)g_tex.base &&
                              ptr < (uint64_t)(uintptr_t)(g_tex.base + g_tex.size);
                snprintf(b, sizeof b, "video: iteracao %d retorno=0x%08X ptr=0x%llx dentro_do_pool=%s", i,
                         (unsigned)got, (unsigned long long)ptr, inPool ? "SIM" : "NAO");
                out(b);
                dumpWords("quadro", fi, 16);
                if (inPool) {
                    out("lendo 8 bytes da imagem...");
                    uint8_t px[8];
                    memcpy(px, (const void*)(uintptr_t)ptr, 8);
                    snprintf(b, sizeof b, "bytes da imagem: %02x %02x %02x %02x %02x %02x %02x %02x", px[0], px[1],
                             px[2], px[3], px[4], px[5], px[6], px[7]);
                    out(b);
                }
            }
        }
        if (fA) {
            memset(ai, 0, sizeof ai);
            int32_t ag = ((FnGetFrame)sceAvPlayerGetAudioData)(h, ai);
            if (ag != 0) {
                ++aNon;
                if (aDumps < 2) {
                    ++aDumps;
                    snprintf(b, sizeof b, "audio: iteracao %d retorno=0x%08X", i, (unsigned)ag);
                    out(b);
                    dumpWords("audio", ai, 12);
                }
            }
        }
        if (i % 100 == 0) {
            int32_t active = ((FnIsActive)sceAvPlayerIsActive)(h);
            snprintf(b, sizeof b, "iteracao %d: ativo=%d video(ret!=0)=%d video(ret==0)=%d audio(ret!=0)=%d", i,
                     (int)active, vNon, vZero, aNon);
            out(b);
        }
        sceKernelUsleep(10000);
    }

    snprintf(b, sizeof b, "RESULTADO modo \"%s\": video ret!=0 em %d chamadas, valores distintos:", mode[0] ? mode : "nenhum",
             vNon);
    out(b);
    for (int k = 0; k < nd; ++k) {
        snprintf(b, sizeof b, "  valor 0x%08X", (unsigned)distinct[k]);
        out(b);
    }

    ((FnStop)sceAvPlayerStop)(h);
    ((FnClose)sceAvPlayerClose)(h);
    out("fim.");
    return 0;
}

// FIM DO ARQUIVO (probe/main.cpp)
