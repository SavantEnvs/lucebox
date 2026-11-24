// Fuzz the server tokenizer on request text: pre_tokenize (Qwen3.5 regex classes, is_letter /
// is_mark Unicode tables), added/control-token matching, byte-level BPE, decode and token_text.
// The vocabulary is a small byte-level BPE GGUF written once in LLVMFuzzerInitialize.
#include "server/tokenizer.h"
#include "tok_vocab.h"

#include <cstdint>
#include <cstdlib>
#include <string>
#include <unistd.h>
#include <vector>

static luce::common::Tokenizer * g_tok;

extern "C" int LLVMFuzzerInitialize(int *, char ***) {
    std::string path = lucefuzz::scratch_path("vocab.gguf");
    if (!lucefuzz::write_vocab_gguf(path)) std::abort();
    g_tok = new luce::common::Tokenizer();
    if (!g_tok->load_from_gguf(path.c_str())) std::abort();
    unlink(path.c_str());
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
    std::string text(reinterpret_cast<const char *>(data), size);
    for (bool match_control : {true, false}) {
        std::vector<int32_t> ids = g_tok->encode(text, match_control);
        std::string back = g_tok->decode(ids);
        for (int32_t id : ids) (void) g_tok->token_text(id);
        (void) back;
    }
    return 0;
}
