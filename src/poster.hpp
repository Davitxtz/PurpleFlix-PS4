#pragma once

struct SDL_Renderer;

namespace poster {

void init(SDL_Renderer* r);

// Associa ao título 'id' o poster_path da TMDB (ex.: "/abc123.jpg").
void setPath(int id, const char* path);

// Desenha o pôster se já estiver carregado (retorna true).
// Se não estiver, registra o pedido de carregamento e retorna false.
bool tryDraw(int id, int x, int y, int w, int h);

// Baixa e decodifica no máximo UM pôster pendente. Chamar a cada quadro.
void loadOne();

}  // namespace poster
