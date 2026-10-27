#pragma once

// Edite aqui os textos e links do aplicativo.
namespace cfg {

constexpr const char* kAppName     = "PurpleFlix";
constexpr const char* kAppByline   = "by Davitxtz";
constexpr const char* kAppFullName = "PurpleFlix by Davitxtz";
constexpr const char* kVersion     = "0.6.0";   // versão exibida no app
constexpr const char* kDevName     = "Davitxtz";

// Apresentação do desenvolvedor (uma linha por item, editável).
constexpr const char* kBioLines[] = {
    "Desenvolvedor de homebrew para PS4.",
    "PurpleFlix é um projeto independente de catálogo",
    "e reprodução de vídeos de fontes autorizadas?",
    "Ω"
};
constexpr int kBioLineCount = 4;

// Links. Se algum contiver "SEU_", o app o trata como PLACEHOLDER.
constexpr const char* kInstagramUrl = "https://instagram.com/davitxtz";
constexpr const char* kTiktokUrl    = "https://tiktok.com/@odavivov";
constexpr const char* kGithubUrl    = "https://github.com/Davitxtz";

constexpr const char* kLogDir  = "/data/purpleflix";
constexpr const char* kLogPath = "/data/purpleflix/log.txt";

}  // namespace cfg
