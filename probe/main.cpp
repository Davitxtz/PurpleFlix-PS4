// PurpleFlix Probe: teste do AvPlayer do sistema.
// ATENCAO: a estrutura de inicializacao abaixo NAO foi verificada no PS4.
// Layout baseado na documentacao da comunidade (Vita), ajustado para 64 bits.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <orbis/AvPlayer.h>
#include <orbis/Sysmodule.h>

// Rotulo assembly: evita conflito com o prototype do header.
extern "C" int32_t pf_load_module(uint16_t id) __asm__("sceSysmoduleLoadModule");

namespace {

const char* const kDir = "/data/purpleflix";
const char* const kFile = "/data/purpleflix/teste.mp4";
FILE* g_f = nullptr;

void mark(const char* name) {
    char p[96];
    snprintf(p, sizeof p, "%s/m_probe_%s", kDir, name);
    FILE* f = fopen(p, "w");
    if (f) fclose(f);
}

void out(const char* s) {
    if (!g_f) return;
    fputs(s, g_f);
    fputc('\n', g_f);
    fflush(g_f);
}

void* pAlloc(void*, uint32_t align, uint32_t size) {
    uint32_t a = 8;
    while (a < align) a <<= 1;
    void* p = nullptr;
    if (posix_memalign(&p, a, size) != 0) return nullptr;
    return p;
}

void pFree(void*, void* p) { free(p); }

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
typedef int32_t (*FnIsActive)(int32_t);
typedef int32_t (*FnStreamCount)(int32_t);
typedef int32_t (*FnGetVideo)(int32_t, void*);
typedef int32_t (*FnClose)(int32_t);

}  // namespace

int main() {
    mkdir(kDir, 0777);
    g_f = fopen("/data/purpleflix/video_probe.txt", "w");
    mark("00_main");
    out("PurpleFlix Probe - teste do AvPlayer (estrutura NAO verificada no PS4)");

    struct stat st;
    bool haveFile = (stat(kFile, &st) == 0);
    char b[200];
    snprintf(b, sizeof b, "arquivo de teste %s: %s (%ld bytes)", kFile,
             haveFile ? "encontrado" : "AUSENTE", haveFile ? (long)st.st_size : 0L);
    out(b);

    mark("01_antes_modulo");
    int32_t r = pf_load_module(ORBIS_SYSMODULE_AV_PLAYER);
    mark("02_depois_modulo");
    snprintf(b, sizeof b, "sceSysmoduleLoadModule(AV_PLAYER) = 0x%08X", (unsigned)r);
    out(b);

    InitData d;
    memset(&d, 0, sizeof d);
    d.mem.obj = nullptr;
    d.mem.alloc = pAlloc;
    d.mem.dealloc = pFree;
    d.mem.allocTex = pAlloc;
    d.mem.deallocTex = pFree;
    d.debugLevel = 0;
    d.basePriority = 700;
    d.numFrameBuffers = 4;
    d.autoStart = 0;
    d.lang = "eng";
    snprintf(b, sizeof b, "sizeof(InitData) = %u", (unsigned)sizeof(InitData));
    out(b);

    mark("03_antes_init");
    int32_t h = ((FnInit)sceAvPlayerInit)(&d);
    mark("04_depois_init");
    snprintf(b, sizeof b, "sceAvPlayerInit = 0x%08X", (unsigned)h);
    out(b);

    if (h <= 0) {
        out("RESULTADO: Init falhou (retorno <= 0). Veja o codigo acima.");
        mark("99_fim_init_falhou");
        if (g_f) fclose(g_f);
        return 0;
    }
    out("Init OK: o AvPlayer do sistema inicializou em homebrew.");

    if (!haveFile) {
        out("Sem teste.mp4: parando aqui.");
        ((FnClose)sceAvPlayerClose)(h);
        mark("98_fim_sem_arquivo");
        if (g_f) fclose(g_f);
        return 0;
    }

    mark("05_antes_addsource");
    int32_t rs = ((FnAddSource)sceAvPlayerAddSource)(h, kFile);
    mark("06_depois_addsource");
    snprintf(b, sizeof b, "sceAvPlayerAddSource = 0x%08X", (unsigned)rs);
    out(b);

    mark("07_antes_streamcount");
    int32_t sc = ((FnStreamCount)sceAvPlayerStreamCount)(h);
    mark("08_depois_streamcount");
    snprintf(b, sizeof b, "sceAvPlayerStreamCount = %d", (int)sc);
    out(b);

    mark("09_antes_start");
    int32_t rst = ((FnStart)sceAvPlayerStart)(h);
    mark("10_depois_start");
    snprintf(b, sizeof b, "sceAvPlayerStart = 0x%08X", (unsigned)rst);
    out(b);

    alignas(16) uint8_t fi[256];
    int frames = 0;
    for (int i = 0; i < 30; ++i) {
        memset(fi, 0, sizeof fi);
        int32_t active = ((FnIsActive)sceAvPlayerIsActive)(h);
        int32_t got = ((FnGetVideo)sceAvPlayerGetVideoData)(h, fi);
        void* ptr = nullptr;
        memcpy(&ptr, fi, sizeof ptr);
        snprintf(b, sizeof b, "tentativa %d: ativo=%d, quadro=%d, ponteiro=%p", i, (int)active, (int)got, ptr);
        out(b);
        if (got) ++frames;
        usleep(100000);
    }
    mark("11_depois_loop");
    snprintf(b, sizeof b, "RESULTADO: %d tentativas com quadro de video", frames);
    out(b);

    ((FnClose)sceAvPlayerClose)(h);
    mark("12_fim");
    if (g_f) fclose(g_f);
    return 0;
}
