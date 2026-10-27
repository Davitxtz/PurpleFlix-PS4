#pragma once
#include <stddef.h>

struct SDL_Renderer;

namespace player {

// Toca 'path' em tela cheia. Bloqueia até o usuário voltar (O) ou o vídeo terminar.
// Retorna false se não conseguiu tocar (mensagem em err).
bool run(SDL_Renderer* ren, const char* path, const char* title, char* err, size_t errCap);

}  // namespace player
