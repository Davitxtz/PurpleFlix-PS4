#pragma once

// Edite aqui os textos e links do aplicativo.
namespace cfg {

constexpr const char* kAppName     = "PurpleFlix";
constexpr const char* kAppByline   = "by Davitxtz";
constexpr const char* kAppFullName = "PurpleFlix by Davitxtz";
constexpr const char* kVersion     = "0.1.0";   // versão exibida no app
constexpr const char* kDevName     = "Davitxtz";

// Apresentação do desenvolvedor (uma linha por item, editável).
constexpr const char* kBioLines[] = {
    "Desenvolvedor de homebrew para PS4.",
    "PurpleFlix é um projeto independente de catálogo",
    "e reprodução de vídeos de fontes autorizadas.",
    "Edite este texto em src/config.hpp.",
};
constexpr int kBioLineCount = 4;

// LINKS. Os que contêm "SEU_" são PLACEHOLDERS até você informar os reais.
constexpr const char* kInstagramUrl = "https://instagram.com/SEU_INSTAGRAM_AQUI";
constexpr const char* kTiktokUrl    = "https://tiktok.com/@SEU_TIKTOK_AQUI";
constexpr const char* kGithubUrl    = "https://github.com/Davitxtz";

constexpr const char* kLogDir  = "/data/purpleflix";
constexpr const char* kLogPath = "/data/purpleflix/log.txt";

}  // namespace cfg
