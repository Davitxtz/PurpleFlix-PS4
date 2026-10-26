// PurpleFlix Probe v5: teste do AvPlayer do sistema.
// ATENCAO: a estrutura de inicializacao abaixo NAO foi verificada no PS4.
// Layout baseado na documentacao da comunidade (Vita), ajustado para 64 bits.
// Callbacks de memoria SEM funcoes da libc (o player os chama de threads proprias).
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
const char* const kFileApp = "/app0/teste.mp4";
const char* const kFileData = "/data/purpleflix/teste.mp4";
int32_t g_fd = -1;

// Bloco estatico para pedidos pequenos.
constexpr size_t kPoolSize = 24u << 20;
alignas(64) uint8_t g_pool[kPoolSize];
size_t g_poolOff = 0;

// Blocos de memoria direta (grandes).
struct Block {
    uint8_t* base;
    size_t size;
    size_t off;
};
Block g_gen = {nullptr, 0, 0};
Block g_tex = {nullptr, 0, 0};

// ---- escrita sem libc ----
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

void logReq(const char* what, uint32_t align, uint32_t size, const void* ret, bool withRet) {
    char b[200];
    size_t n = 0;
    n = putS(b, n, "  [callback] ");
    n = putS(b, n, what);
    n = putS(b, n, " alinh=");
    n = putD(b, n, align);
    n = putS(b, n, " tam=");
    n = putD(b, n, size);
    if (withRet) {
        n = putS(b, n, " ret=");
        n = putH(b, n, (uint64_t)(uintptr_t)ret);
    }
    n = putS(b, n, " thread=");
    n = putH(b, n, (uint64_t)(uintptr_t)scePthreadSelf());
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
    logReq("GERAL pedido ", align, size, nullptr, false);
    void* p = nullptr;
    if (size >= (1u << 20) && g_gen.base) p = bump(g_gen.base, g_gen.size, &g_gen.off, align, size);
    else p = bump(g_pool, kPoolSize, &g_poolOff, align, size);
    logReq("GERAL entregue", align, size, p, true);
    return p;
}

void cbFree(void*, void*) {}

void* cbAllocTex(void*, uint32_t align, uint32_t size) {
    logReq("IMAGEM pedido ", align, size, nullptr, false);
    void* p = nullptr;
    if (g_tex.base) p = bump(g_tex.base, g_tex.size, &g_tex.off, align, size);
    else p = bump(g_pool, kPoolSize, &g_poolOff, align, size);
    logReq("IMAGEM entregue", align, size, p, true);
    return p;
}

void cbFreeTex(void*, void*) {}

void mark(const char* name) {
    char p[96];
    snprintf(p, sizeof p, "%s/m_probe_%s", kDir, name);
    FILE* f = fopen(p, "w");
    if (f) fclose(f);
}

// Reserva e mapeia um bloco de memoria direta; tenta tamanhos menores se falhar.
bool initBlock(Block& blk, const char* name, size_t want, size_t minSize) {
    size_t dm = sceKernelGetDirectMemorySize();
    char b[220];
    for (size_t len = want; len >= minSize; len >>= 1) {
        off_t phys = 0;
        int32_t r = sceKernelAllocateDirectMemory(0, (off_t)dm, len, 0x200000, ORBIS_KERNEL_WB_ONION, &phys);
        snprintf(b, sizeof b, "bloco %s: AllocateDirectMemory(%llu MB) = 0x%08X fisico=0x%llx", name,
                 (unsigned long long)(len >> 20), (unsigned)r, (unsigned long long)phys);
        out(b);
        if (r != 0) continue;
        void* addr = nullptr;
        r = sceKernelMapDirectMemory(&addr, len, ORBIS_KERNEL_PROT_CPU_RW | ORBIS_KERNEL_PROT_GPU_RW, 0, phys,
                                     0x200000);
        snprintf(b, sizeof b, "bloco %s: MapDirectMemory = 0x%08X endereco=%p", name, (unsigned)r, addr);
        out(b);
        if (r == 0 && addr) {
            blk.base = (uint8_t*)addr;
            blk.size = len;
            blk.off = 0;
            return true;
        }
    }
    return false;
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
typedef int32_t (*FnGetVideo)(int32_t, void*);
typedef int32_t (*FnClose)(int32_t);

}  // namespace

int main() {
    mkdir(kDir, 0777);
    g_fd = sceKernelOpen("/data/purpleflix/video_probe.txt", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    mark("00_main");
    out("PurpleFlix Probe v5 - teste do AvPlayer (estrutura NAO verificada no PS4)");

    char b[300];
    snprintf(b, sizeof b, "thread principal=0x%llx", (unsigned long long)(uintptr_t)scePthreadSelf());
    out(b);

    // Escolhe o arquivo (so verifica se existe: o tamanho de stat() nao e confiavel aqui).
    const char* file = kFileApp;
    struct stat st;
    bool haveFile = (stat(kFileApp, &st) == 0);
    if (!haveFile) {
        file = kFileData;
        haveFile = (stat(kFileData, &st) == 0);
    }
    snprintf(b, sizeof b, "arquivo de teste: %s (%s)", haveFile ? file : "NENHUM",
             haveFile ? "encontrado" : "AUSENTE");
    out(b);

    mark("01_antes_modulo");
    int32_t r = pf_load_module(ORBIS_SYSMODULE_AV_PLAYER);
    mark("02_depois_modulo");
    snprintf(b, sizeof b, "sceSysmoduleLoadModule(AV_PLAYER) = 0x%08X", (unsigned)r);
    out(b);

    mark("02b_antes_blocos");
    bool genOk = initBlock(g_gen, "GERAL", 160u << 20, 64u << 20);
    bool texOk = initBlock(g_tex, "IMAGEM", 256u << 20, 64u << 20);
    mark("02c_depois_blocos");
    snprintf(b, sizeof b, "blocos: geral=%s (%llu MB) imagem=%s (%llu MB)", genOk ? "OK" : "FALHOU",
             (unsigned long long)(g_gen.size >> 20), texOk ? "OK" : "FALHOU",
             (unsigned long long)(g_tex.size >> 20));
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

    mark("03_antes_init");
    out("chamando sceAvPlayerInit...");
    int32_t h = ((FnInit)sceAvPlayerInit)(&d);
    mark("04_depois_init");
    snprintf(b, sizeof b, "sceAvPlayerInit = 0x%08X", (unsigned)h);
    out(b);

    if (h <= 0) {
        out("RESULTADO: Init falhou (retorno <= 0). Veja o codigo acima.");
        mark("99_fim_init_falhou");
        return 0;
    }
    out("Init OK: o AvPlayer do sistema inicializou em homebrew.");

    if (!haveFile) {
        out("Sem arquivo de teste: parando aqui.");
        ((FnClose)sceAvPlayerClose)(h);
        mark("98_fim_sem_arquivo");
        return 0;
    }

    mark("05_antes_addsource");
    out("chamando sceAvPlayerAddSource...");
    int32_t rs = ((FnAddSource)sceAvPlayerAddSource)(h, file);
    mark("06_depois_addsource");
    snprintf(b, sizeof b, "sceAvPlayerAddSource = 0x%08X", (unsigned)rs);
    out(b);

    // O AddSource e assincrono: espera as trilhas aparecerem.
    mark("07_antes_espera_trilhas");
    int32_t sc = 0;
    int waited = 0;
    for (int i = 0; i < 50; ++i) {
        sc = ((FnStreamCount)sceAvPlayerStreamCount)(h);
        if (sc > 0) break;
        sceKernelUsleep(100000);
        ++waited;
    }
    mark("08_depois_espera_trilhas");
    snprintf(b, sizeof b, "StreamCount = %d (apos esperar %d ms)", (int)sc, waited * 100);
    out(b);
    if (sc <= 0) out("O player nao reconheceu nenhuma trilha: arquivo invalido ou formato nao suportado.");

    for (int i = 0; i < sc && i < 4; ++i) {
        alignas(16) uint8_t info[256];
        memset(info, 0, sizeof info);
        int32_t ri = ((FnGetStreamInfo)sceAvPlayerGetStreamInfo)(h, (uint32_t)i, info);
        uint32_t type = 0;
        memcpy(&type, info, 4);
        snprintf(b, sizeof b, "trilha %d: GetStreamInfo=0x%08X tipo=%u (0=video 1=audio 2=texto)", i,
                 (unsigned)ri, (unsigned)type);
        out(b);
        if (type == 0) {
            mark("09_antes_enable_video");
            int32_t re = ((FnEnableStream)sceAvPlayerEnableStream)(h, (uint32_t)i);
            mark("10_depois_enable_video");
            snprintf(b, sizeof b, "trilha %d: EnableStream(video)=0x%08X", i, (unsigned)re);
            out(b);
        } else {
            snprintf(b, sizeof b, "trilha %d: nao habilitada neste teste (so video)", i);
            out(b);
        }
    }

    mark("11_antes_start");
    out("chamando sceAvPlayerStart...");
    int32_t rst = ((FnStart)sceAvPlayerStart)(h);
    mark("12_depois_start");
    snprintf(b, sizeof b, "sceAvPlayerStart = 0x%08X", (unsigned)rst);
    out(b);

    alignas(16) uint8_t fi[256];
    int frames = 0;
    bool firstDump = true;
    for (int i = 0; i < 1000; ++i) {
        memset(fi, 0, sizeof fi);
        int32_t got = ((FnGetVideo)sceAvPlayerGetVideoData)(h, fi);
        if (got) {
            ++frames;
            uint64_t ptr = 0, ts = 0;
            uint32_t w = 0, hh = 0;
            memcpy(&ptr, fi, 8);
            memcpy(&ts, fi + 16, 8);
            memcpy(&w, fi + 24, 4);
            memcpy(&hh, fi + 28, 4);
            bool inPool = g_tex.base && ptr >= (uint64_t)(uintptr_t)g_tex.base &&
                          ptr < (uint64_t)(uintptr_t)(g_tex.base + g_tex.size);
            if (frames <= 5 || frames % 30 == 0) {
                snprintf(b, sizeof b,
                         "quadro %d (iteracao %d): ptr=0x%llx ts=%llu largura=%u altura=%u dentro_do_pool=%s",
                         frames, i, (unsigned long long)ptr, (unsigned long long)ts, (unsigned)w, (unsigned)hh,
                         inPool ? "SIM" : "NAO");
                out(b);
            }
            if (firstDump) {
                firstDump = false;
                uint64_t words[8];
                memcpy(words, fi, sizeof words);
                snprintf(b, sizeof b, "estrutura do quadro (8 palavras): %llx %llx %llx %llx %llx %llx %llx %llx",
                         (unsigned long long)words[0], (unsigned long long)words[1],
                         (unsigned long long)words[2], (unsigned long long)words[3],
                         (unsigned long long)words[4], (unsigned long long)words[5],
                         (unsigned long long)words[6], (unsigned long long)words[7]);
                out(b);
                if (inPool) {
                    mark("13_lendo_pixels");
                    uint8_t px[8];
                    memcpy(px, (const void*)(uintptr_t)ptr, 8);
                    mark("14_pixels_lidos");
                    snprintf(b, sizeof b, "primeiros 8 bytes da imagem: %02x %02x %02x %02x %02x %02x %02x %02x",
                             px[0], px[1], px[2], px[3], px[4], px[5], px[6], px[7]);
                    out(b);
                }
            }
        }
        if (i % 100 == 0) {
            int32_t active = ((FnIsActive)sceAvPlayerIsActive)(h);
            snprintf(b, sizeof b, "iteracao %d: ativo=%d quadros ate agora=%d", i, (int)active, frames);
            out(b);
        }
        sceKernelUsleep(10000);
    }
    mark("15_depois_loop");
    snprintf(b, sizeof b, "RESULTADO: %d quadros de video recebidos", frames);
    out(b);

    ((FnStop)sceAvPlayerStop)(h);
    ((FnClose)sceAvPlayerClose)(h);
    mark("16_fim");
    return 0;
}

// FIM DO ARQUIVO (probe/main.cpp)
