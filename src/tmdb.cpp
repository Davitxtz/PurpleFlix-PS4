#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "net.hpp"
#include "tmdb.hpp"

namespace {

const char* const kKeyPath = "/data/purpleflix/tmdb_key.txt";
constexpr size_t kBodyCap = 96 * 1024;
char g_body[kBodyCap];

struct Cur {
    const char* p;
    const char* end;
};

void skipWs(Cur& c) {
    while (c.p < c.end && (*c.p == ' ' || *c.p == '\n' || *c.p == '\r' || *c.p == '\t')) ++c.p;
}

int hexVal(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

struct Out {
    char* buf;
    size_t cap;
    size_t n;
};

void putByte(Out& o, unsigned b) {
    if (o.buf && o.n + 1 < o.cap) o.buf[o.n++] = (char)b;
}

void putCodePoint(Out& o, unsigned cp) {
    if (cp < 0x80) {
        putByte(o, cp);
    } else if (cp < 0x800) {
        putByte(o, 0xC0 | (cp >> 6));
        putByte(o, 0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        putByte(o, 0xE0 | (cp >> 12));
        putByte(o, 0x80 | ((cp >> 6) & 0x3F));
        putByte(o, 0x80 | (cp & 0x3F));
    } else {
        putByte(o, 0xF0 | (cp >> 18));
        putByte(o, 0x80 | ((cp >> 12) & 0x3F));
        putByte(o, 0x80 | ((cp >> 6) & 0x3F));
        putByte(o, 0x80 | (cp & 0x3F));
    }
}

bool parseHex4(Cur& c, unsigned& v) {
    if (c.end - c.p < 4) return false;
    v = 0;
    for (int i = 0; i < 4; ++i) {
        int h = hexVal(c.p[i]);
        if (h < 0) return false;
        v = (v << 4) | (unsigned)h;
    }
    c.p += 4;
    return true;
}

// Lê uma string JSON (decodifica escapes). buf pode ser nullptr para só pular.
bool parseString(Cur& c, char* buf, size_t cap) {
    if (buf && cap > 0) buf[0] = 0;
    skipWs(c);
    if (c.p >= c.end || *c.p != '"') return false;
    ++c.p;
    Out o = {buf, cap, 0};
    while (c.p < c.end && *c.p != '"') {
        unsigned char ch = (unsigned char)*c.p++;
        if (ch != '\\') {
            putByte(o, ch);
            continue;
        }
        if (c.p >= c.end) return false;
        char e = *c.p++;
        switch (e) {
            case 'n': case 'r': case 't': case 'b': case 'f':
                putByte(o, ' ');
                break;
            case 'u': {
                unsigned cp;
                if (!parseHex4(c, cp)) return false;
                if (cp >= 0xD800 && cp < 0xDC00 && c.end - c.p >= 6 && c.p[0] == '\\' && c.p[1] == 'u') {
                    Cur save = c;
                    c.p += 2;
                    unsigned lo;
                    if (parseHex4(c, lo) && lo >= 0xDC00 && lo < 0xE000) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else {
                        c = save;
                    }
                }
                putCodePoint(o, cp);
                break;
            }
            default:
                putByte(o, (unsigned char)e);  // \" \\ \/
                break;
        }
    }
    if (c.p >= c.end) return false;
    ++c.p;
    if (buf && cap > 0) buf[o.n] = 0;
    return true;
}

bool skipValue(Cur& c, int depth) {
    skipWs(c);
    if (c.p >= c.end) return false;
    char ch = *c.p;
    if (ch == '"') return parseString(c, nullptr, 0);
    if (ch == '{' || ch == '[') {
        if (depth > 16) return false;
        char close = (ch == '{') ? '}' : ']';
        ++c.p;
        skipWs(c);
        if (c.p < c.end && *c.p == close) {
            ++c.p;
            return true;
        }
        for (;;) {
            if (ch == '{') {
                if (!parseString(c, nullptr, 0)) return false;
                skipWs(c);
                if (c.p >= c.end || *c.p != ':') return false;
                ++c.p;
            }
            if (!skipValue(c, depth + 1)) return false;
            skipWs(c);
            if (c.p >= c.end) return false;
            if (*c.p == ',') {
                ++c.p;
                continue;
            }
            if (*c.p == close) {
                ++c.p;
                return true;
            }
            return false;
        }
    }
    while (c.p < c.end && *c.p != ',' && *c.p != '}' && *c.p != ']' && *c.p != ' ' &&
           *c.p != '\n' && *c.p != '\r' && *c.p != '\t') {
        ++c.p;
    }
    return true;
}

// String ou null (null deixa o buffer vazio).
bool strOrNull(Cur& c, char* buf, size_t cap) {
    skipWs(c);
    if (c.p < c.end && *c.p == '"') return parseString(c, buf, cap);
    if (buf && cap > 0) buf[0] = 0;
    return skipValue(c, 0);
}

bool parseLong(Cur& c, long& v) {
    skipWs(c);
    bool neg = false;
    if (c.p < c.end && *c.p == '-') {
        neg = true;
        ++c.p;
    }
    if (c.p >= c.end || *c.p < '0' || *c.p > '9') return false;
    long x = 0;
    while (c.p < c.end && *c.p >= '0' && *c.p <= '9') {
        x = x * 10 + (*c.p - '0');
        ++c.p;
    }
    while (c.p < c.end && (*c.p == '.' || *c.p == 'e' || *c.p == 'E' || *c.p == '+' ||
                           *c.p == '-' || (*c.p >= '0' && *c.p <= '9'))) {
        ++c.p;
    }
    v = neg ? -x : x;
    return true;
}

bool parseItem(Cur& c, tmdb::Item& it) {
    skipWs(c);
    if (c.p >= c.end || *c.p != '{') return false;
    ++c.p;
    it.id = 0;
    it.title[0] = 0;
    it.year[0] = 0;
    it.overview[0] = 0;
    it.poster[0] = 0;
    skipWs(c);
    if (c.p < c.end && *c.p == '}') {
        ++c.p;
        return true;
    }
    for (;;) {
        char key[40];
        if (!parseString(c, key, sizeof key)) return false;
        skipWs(c);
        if (c.p >= c.end || *c.p != ':') return false;
        ++c.p;
        skipWs(c);
        if (strcmp(key, "id") == 0) {
            long v;
            if (!parseLong(c, v)) return false;
            it.id = v;
        } else if (strcmp(key, "title") == 0 || strcmp(key, "name") == 0) {
            if (!strOrNull(c, it.title, sizeof it.title)) return false;
        } else if (strcmp(key, "release_date") == 0 || strcmp(key, "first_air_date") == 0) {
            char d[16];
            if (!strOrNull(c, d, sizeof d)) return false;
            bool digits = d[0] && d[1] && d[2] && d[3];
            for (int i = 0; i < 4 && digits; ++i) {
                if (d[i] < '0' || d[i] > '9') digits = false;
            }
            if (digits) {
                memcpy(it.year, d, 4);
                it.year[4] = 0;
            }
        } else if (strcmp(key, "overview") == 0) {
            if (!strOrNull(c, it.overview, sizeof it.overview)) return false;
        } else if (strcmp(key, "poster_path") == 0) {
            if (!strOrNull(c, it.poster, sizeof it.poster)) return false;
        } else if (!skipValue(c, 0)) {
            return false;
        }
        skipWs(c);
        if (c.p >= c.end) return false;
        if (*c.p == ',') {
            ++c.p;
            continue;
        }
        if (*c.p == '}') {
            ++c.p;
            return true;
        }
        return false;
    }
}

int parseResults(const char* body, size_t len, tmdb::ListResult& out) {
    Cur c = {body, body + len};
    skipWs(c);
    if (c.p >= c.end || *c.p != '{') return 0;
    ++c.p;
    for (;;) {
        skipWs(c);
        char key[40];
        if (!parseString(c, key, sizeof key)) break;
        skipWs(c);
        if (c.p >= c.end || *c.p != ':') break;
        ++c.p;
        skipWs(c);
        if (strcmp(key, "results") == 0 && c.p < c.end && *c.p == '[') {
            ++c.p;
            skipWs(c);
            if (c.p < c.end && *c.p == ']') return out.count;
            for (;;) {
                if (out.count >= tmdb::kMaxItems) return out.count;
                if (!parseItem(c, out.items[out.count])) return out.count;
                ++out.count;
                skipWs(c);
                if (c.p < c.end && *c.p == ',') {
                    ++c.p;
                    skipWs(c);
                    continue;
                }
                break;
            }
            return out.count;
        } else if (strcmp(key, "status_message") == 0) {
            if (!strOrNull(c, out.message, sizeof out.message)) break;
        } else if (!skipValue(c, 0)) {
            break;
        }
        skipWs(c);
        if (c.p < c.end && *c.p == ',') {
            ++c.p;
            continue;
        }
        break;
    }
    return out.count;
}

}  // namespace

bool tmdb::loadKey(char* out, size_t cap) {
    if (!out || cap < 2) return false;
    out[0] = 0;
    FILE* f = fopen(kKeyPath, "r");
    if (!f) return false;
    char line[256];
    size_t n = 0;
    if (fgets(line, sizeof line, f)) {
        const char* p = line;
        auto isKeyChar = [](char ch) {
            return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') ||
                   (ch >= 'A' && ch <= 'Z') || ch == '_' || ch == '-';
        };
        while (*p && !isKeyChar(*p)) ++p;  // ignora BOM/espaços iniciais
        while (*p && isKeyChar(*p) && n + 1 < cap) out[n++] = *p++;
    }
    fclose(f);
    out[n] = 0;
    return n > 0;
}

bool tmdb::fetchPopular(const char* kind, const char* lang, int page, ListResult& out) {
    out.ok = false;
    out.status = 0;
    out.err = 0;
    out.stage = "";
    out.count = 0;
    out.message[0] = 0;

    char key[128];
    if (!loadKey(key, sizeof key)) {
        out.stage = "chave";
        snprintf(out.message, sizeof out.message,
                 "Crie /data/purpleflix/tmdb_key.txt com a sua chave (32 caracteres).");
        return false;
    }
    if (strlen(key) > 40) {
        out.stage = "chave";
        snprintf(out.message, sizeof out.message,
                 "Chave longa demais: use a 'API Key', nao o token de leitura.");
        return false;
    }

    int ierr = 0;
    const char* istage = "";
    if (!net::init(&ierr, &istage)) {
        out.err = ierr;
        out.stage = istage;
        snprintf(out.message, sizeof out.message, "Falha ao iniciar a rede.");
        return false;
    }

    char url[400];
    snprintf(url, sizeof url, "https://api.themoviedb.org/3/%s/popular?api_key=%s&language=%s&page=%d",
             kind, key, lang, page);

    net::Result r = net::get(url, g_body, kBodyCap, false);
    out.status = r.status;
    out.err = r.err;
    out.stage = r.stage;

    // A URL contém a chave: nunca é registrada nem exibida.
    memset(url, 0, sizeof url);
    memset(key, 0, sizeof key);

    if (r.status == 0 || (!r.ok && r.bytes == 0)) {
        if (out.message[0] == 0) snprintf(out.message, sizeof out.message, "Sem resposta do servidor.");
        return false;
    }

    parseResults(g_body, r.bytes, out);
    if (r.status != 200) {
        if (out.message[0] == 0) snprintf(out.message, sizeof out.message, "A TMDB respondeu com erro.");
        return false;
    }
    out.ok = out.count > 0;
    if (!out.ok && out.message[0] == 0) snprintf(out.message, sizeof out.message, "Resposta sem resultados.");
    return out.ok;
}
