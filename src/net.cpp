#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <orbis/Http.h>
#include <orbis/Net.h>
#include <orbis/Ssl.h>
#include <orbis/Sysmodule.h>

#include "net.hpp"

namespace {

const char* const kUserAgent = "Mozilla/5.0 (PLAYSTATION 4; 1.00)";
constexpr size_t kNetPoolSize = 4 * 1024;  // mesmo valor do sample oficial

int g_netMemId = 0;
int g_sslCtxId = 0;
int g_httpCtxId = 0;
bool g_ready = false;

// Marcador de etapa: cria um arquivo vazio (ajuda a achar onde o app travou).
void mark(const char* name) {
    char path[96];
    snprintf(path, sizeof path, "/data/purpleflix/m_net_%s", name);
    FILE* f = fopen(path, "w");
    if (f) fclose(f);
}

// Aceita qualquer certificado. Usado SOMENTE no modo inseguro do diagnóstico.
int32_t acceptAllCallback(int32_t, uint32_t, void* const[], int32_t, void*) { return 1; }

}  // namespace

bool net::init(int* err, const char** stage) {
    int dummyErr = 0;
    const char* dummyStage = "";
    if (!err) err = &dummyErr;
    if (!stage) stage = &dummyStage;
    *err = 0;
    *stage = "";
    if (g_ready) return true;

    int ret;

    mark("01_modulo_net");
    *stage = "carregar modulo Net";
    ret = sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NET);
    if (ret < 0) { *err = ret; return false; }

    mark("02_modulo_http");
    *stage = "carregar modulo Http";
    ret = sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_HTTP);
    if (ret < 0) { *err = ret; return false; }

    mark("03_modulo_ssl");
    *stage = "carregar modulo Ssl";
    ret = sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_SSL);
    if (ret < 0) { *err = ret; return false; }

    mark("04_net_init");
    *stage = "sceNetInit";
    sceNetInit();

    mark("05_net_pool");
    *stage = "sceNetPoolCreate";
    ret = sceNetPoolCreate("netPool", kNetPoolSize, 0);
    if (ret < 0) { *err = ret; return false; }
    g_netMemId = ret;

    mark("06_ssl_init");
    *stage = "sceSslInit";
    ret = sceSslInit(SSL_POOLSIZE);
    if (ret < 0) { *err = ret; return false; }
    g_sslCtxId = ret;

    mark("07_http_init");
    *stage = "sceHttpInit";
    ret = sceHttpInit(g_netMemId, g_sslCtxId, LIBHTTP_POOLSIZE);
    if (ret < 0) { *err = ret; return false; }
    g_httpCtxId = ret;

    mark("08_rede_pronta");
    g_ready = true;
    return true;
}

void net::shutdown() {
    if (!g_ready) return;
    sceHttpTerm(g_httpCtxId);
    // O header declara sceSslTerm() sem parâmetros (prototype genérico do gerador de stubs),
    // mas o sample oficial chama com o id do contexto. Chamamos via ponteiro com o tipo correto.
    ((void (*)(int))sceSslTerm)(g_sslCtxId);
    sceNetPoolDestroy(g_netMemId);
    g_ready = false;
}

net::Result net::get(const char* url, char* buf, size_t cap, bool insecureTls) {
    Result r;
    r.ok = false;
    r.status = 0;
    r.err = 0;
    r.stage = "inicio";
    r.bytes = 0;
    r.truncated = false;

    if (!g_ready || !buf || cap < 2) {
        r.stage = "rede nao iniciada";
        return r;
    }
    buf[0] = 0;

    int tpl = 0, conn = 0, req = 0;

    do {
        mark("10_get_inicio");
        tpl = sceHttpCreateTemplate(g_httpCtxId, kUserAgent, ORBIS_HTTP_VERSION_1_1, 1);
        if (tpl < 0) { r.err = tpl; r.stage = "criar template"; break; }

        if (insecureTls) {
            sceHttpsSetSslCallback(tpl, acceptAllCallback, NULL);
        }

        mark("11_conexao");
        conn = sceHttpCreateConnectionWithURL(tpl, url, 1);
        if (conn < 0) { r.err = conn; r.stage = "conectar"; break; }

        mark("12_requisicao");
        req = sceHttpCreateRequestWithURL(conn, ORBIS_METHOD_GET, url, 0);
        if (req < 0) { r.err = req; r.stage = "criar requisicao"; break; }

        mark("13_envio");
        int ret = sceHttpSendRequest(req, NULL, 0);
        if (ret < 0) { r.err = ret; r.stage = "enviar"; break; }

        mark("14_status");
        int32_t status = 0;
        ret = sceHttpGetStatusCode(req, &status);
        if (ret < 0) { r.err = ret; r.stage = "ler status"; break; }
        r.status = (int)status;

        mark("15_leitura");
        size_t total = 0;
        for (;;) {
            size_t avail = cap - 1 - total;
            if (avail == 0) {
                r.truncated = true;
                r.ok = true;
                break;
            }
            int n = sceHttpReadData(req, buf + total, avail);
            if (n < 0) { r.err = n; r.stage = "ler dados"; break; }
            if (n == 0) { r.ok = true; break; }
            total += (size_t)n;
        }
        r.bytes = total;
        buf[total] = 0;
        mark("16_fim");
    } while (0);

    if (req > 0) sceHttpDeleteRequest(req);
    if (conn > 0) sceHttpDeleteConnection(conn);
    if (tpl > 0) sceHttpDeleteTemplate(tpl);
    return r;
}
