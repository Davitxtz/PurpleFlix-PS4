// ATENÇÃO: os nomes das funções abaixo (sceUserService*, scePad*) NÃO foram
// conferidos contra os headers instalados. Se o build falhar aqui, o log do
// workflow mostra os headers reais.
#include <orbis/Pad.h>
#include <orbis/UserService.h>
#include <string.h>

#include "pad.hpp"

namespace {
int g_handle = -1;
}

bool pad::init() {
    sceUserServiceInitialize(nullptr);  // pode já estar inicializado
    int userId = 0;
    if (sceUserServiceGetInitialUser(&userId) != 0) return false;
    if (scePadInit() != 0) return false;
    g_handle = scePadOpen(userId, 0, 0, nullptr);
    return g_handle >= 0;
}

uint32_t pad::poll() {
    if (g_handle < 0) return 0;
    OrbisPadData d;
    memset(&d, 0, sizeof(d));
    if (scePadReadState(g_handle, &d) != 0) return 0;
    return d.buttons;
}
