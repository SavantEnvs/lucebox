// Fuzz Tokenizer::load_from_gguf on an untrusted GGUF model file (vendored ggml gguf reader +
// the vocab/merges/token_type/special-id loading), then encode and decode with what it loaded.
#include "server/tokenizer.h"
#include "tok_vocab.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static std::string g_path;

extern "C" int LLVMFuzzerInitialize(int *, char ***) {
    g_path = lucefuzz::scratch_path("model.gguf");
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
    FILE * f = std::fopen(g_path.c_str(), "wb");
    if (!f) std::abort();
    if (size) std::fwrite(data, 1, size, f);
    std::fclose(f);
    luce::common::Tokenizer tok;
    if (!tok.load_from_gguf(g_path.c_str())) return 0;
    std::vector<int32_t> ids = tok.encode("<|im_start|>user\nHello world, the \xc3\xa9t\xc3\xa9 123!<|im_end|>");
    (void) tok.decode(ids);
    for (int32_t id = 0; id < tok.vocab_size() && id < 512; ++id) (void) tok.token_text(id);
    return 0;
}
