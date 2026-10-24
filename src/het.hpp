#pragma once
#include <stddef.h>

namespace net {

struct Result {
    bool ok;            // resposta lida até o fim
    int status;         // código HTTP (0 se não chegou a obter)
    int err;            // código de erro do sistema (0 se nenhum)
    const char* stage;  // etapa em que falhou
    size_t bytes;       // bytes gravados no buffer
    bool truncated;     // buffer cheio, resposta cortada
};

// Carrega os módulos de rede e inicia Net/Ssl/Http.
bool init(int* err, const char** stage);
void shutdown();

// GET simples. insecureTls = true aceita qualquer certificado (INSEGURO).
Result get(const char* url, char* buf, size_t cap, bool insecureTls);

}  // namespace net
