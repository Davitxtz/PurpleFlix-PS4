
#pragma once

namespace cfg {

constexpr const char* kAppFullName = "PurpleFlix by Davitxtz";
constexpr const char* kAppByline = "by Davitxtz";
constexpr const char* kDevName = "Davitxtz";
constexpr const char* kVersion = "0.1.0";

constexpr const char* kInstagramUrl = "https://instagram.com/SEU_USUARIO";
constexpr const char* kTiktokUrl = "https://tiktok.com/@SEU_USUARIO";
constexpr const char* kGithubUrl = "https://github.com/Davitxtz";

constexpr const char* kLogDir = "/data/purpleflix";
constexpr const char* kLogPath = "/data/purpleflix/log.txt";

constexpr const char* kBioLines[] = {
    "Desenvolvedor independente.",
    "Criando projetos e explorando tecnologia.",
    "PurpleFlix: entretenimento em uma interface roxa."
};

constexpr int kBioLineCount =
    sizeof(kBioLines) / sizeof(kBioLines[0]);

}
