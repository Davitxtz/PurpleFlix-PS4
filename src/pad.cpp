
#include "pad.hpp"

#include <orbis/Pad.h>
#include <orbis/UserService.h>

namespace pad {

namespace {
int g_handle = -1;
bool g_padInitialized = false;
}

bool init() {
    int32_t userId = -1;

    // Inicializa o serviço de usuários.
    sceUserServiceInitialize(nullptr);

    if (sceUserServiceGetInitialUser(&userId) < 0)
        return false;

    if (scePadInit() < 0)
        return false;

    g_padInitialized = true;

    g_handle = scePadOpen(userId, 0, 0, nullptr);

    if (g_handle < 0)
        return false;

    return true;
}

uint32_t poll() {
    if (g_handle < 0)
        return 0;

    OrbisPadData data{};

    if (scePadReadState(g_handle, &data) < 0)
        return 0;

    uint32_t result = 0;
    const uint32_t b = data.buttons;

    if (b & 0x0010) result |= kUp;
    if (b & 0x0040) result |= kDown;
    if (b & 0x0080) result |= kLeft;
    if (b & 0x0020) result |= kRight;
    if (b & 0x4000) result |= kCross;
    if (b & 0x2000) result |= kCircle;
    if (b & 0x0400) result |= kL1;
    if (b & 0x0800) result |= kR1;
    if (b & 0x0008) result |= kOptions;

    return result;
}

void shutdown() {
    if (g_handle >= 0) {
        scePadClose(g_handle);
        g_handle = -1;
    }

    g_padInitialized = false;
}

}
