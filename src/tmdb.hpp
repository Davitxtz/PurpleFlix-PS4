#pragma once
#include <stddef.h>

namespace tmdb {

constexpr int kMaxItems = 20;

struct Item {
    long id;
    char title[64];
    char year[8];
    char overview[256];
    char poster[40];
};

struct ListResult {
    bool ok;            // HTTP 200 e pelo menos 1 item lido
    int status;         // código HTTP (0 se não obtido)
    int err;            // código de erro do sistema
    const char* stage;  // etapa da falha
    int count;
    char message[120];  // mensagem amigável ou status_message da TMDB
    Item items[kMaxItems];
};

// Lê a chave de /data/purpleflix/tmdb_key.txt. false se ausente ou vazia.
bool loadKey(char* out, size_t cap);

// kind: "movie" ou "tv". lang: "pt-BR", "en-US"...
bool fetchPopular(const char* kind, const char* lang, int page, ListResult& out);

}  // namespace tmdb
