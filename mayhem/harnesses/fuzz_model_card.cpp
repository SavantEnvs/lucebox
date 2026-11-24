// Fuzz the model-card sidecar loader: share/model_cards/<normalize(general.name)>.json
// is parsed and validated by resolve_model_card when a GGUF is loaded.
// The input is written as that card in a per-process scratch directory.
#include "server/model_card.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

static std::string g_root;
static std::string g_card;

extern "C" int LLVMFuzzerInitialize(int *, char ***) {
    const char * tmp = std::getenv("TMPDIR");
    std::string tmpl = std::string(tmp && *tmp ? tmp : "/tmp") + "/luce_card_XXXXXX";
    if (!mkdtemp(&tmpl[0])) std::abort();
    g_root = tmpl;
    mkdir((g_root + "/share").c_str(), 0700);
    mkdir((g_root + "/share/model_cards").c_str(), 0700);
    g_card = g_root + "/share/model_cards/fuzz-model-7b.json";
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
    FILE * f = std::fopen(g_card.c_str(), "wb");
    if (!f) std::abort();
    if (size) std::fwrite(data, 1, size, f);
    std::fclose(f);
    luce::common::ModelCard card = luce::common::resolve_model_card(
        g_root + "/fuzz.gguf", "Fuzz Model 7B", "qwen35", g_root);
    (void) card;
    (void) luce::common::normalize_model_card_stem(
        std::string(reinterpret_cast<const char *>(data), size < 64 ? size : 64));
    return 0;
}
