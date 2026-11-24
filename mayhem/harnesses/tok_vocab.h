// Builds a small byte-level BPE vocabulary GGUF (Qwen-style: GPT-2 byte alphabet, a few
// merges, control and user-defined tokens) with ggml's gguf writer. Shared by the tokenizer harnesses.
#pragma once
#include "gguf.h"

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace lucefuzz {

inline std::string utf8_cp(uint32_t cp) {
    std::string s;
    if (cp < 0x80) s += char(cp);
    else if (cp < 0x800) { s += char(0xC0 | (cp >> 6)); s += char(0x80 | (cp & 0x3F)); }
    else { s += char(0xE0 | (cp >> 12)); s += char(0x80 | ((cp >> 6) & 0x3F)); s += char(0x80 | (cp & 0x3F)); }
    return s;
}

// GPT-2 bytes_to_unicode.
inline std::string byte_char(int b) {
    if ((b >= 33 && b <= 126) || (b >= 161 && b <= 172) || (b >= 174 && b <= 255)) return utf8_cp(b);
    int n = 0;
    for (int x = 0; x < b; ++x)
        if (!((x >= 33 && x <= 126) || (x >= 161 && x <= 172) || (x >= 174 && x <= 255))) ++n;
    return utf8_cp(256 + n);
}

inline bool write_vocab_gguf(const std::string & path) {
    std::vector<std::string> toks, merges;
    std::vector<int32_t> types;
    for (int b = 0; b < 256; ++b) { toks.push_back(byte_char(b)); types.push_back(1); }
    const std::string G = byte_char(' ');
    const char * pairs[][2] = {{"h", "e"}, {"l", "l"}, {"he", "ll"}, {"hell", "o"}, {"w", "o"},
                               {"r", "l"}, {"wo", "rl"}, {"worl", "d"}, {"t", "h"}, {"th", "e"}};
    for (auto & p : pairs) { merges.push_back(std::string(p[0]) + " " + p[1]); toks.push_back(std::string(p[0]) + p[1]); types.push_back(1); }
    const char * gpairs[] = {"t", "w", "h"};
    for (auto * p : gpairs) { merges.push_back(G + " " + p); toks.push_back(G + p); types.push_back(1); }
    const char * control[] = {"<|endoftext|>", "<|im_start|>", "<|im_end|>", "<think>", "</think>"};
    for (auto * c : control) { toks.push_back(c); types.push_back(3); }
    const char * user[] = {"<tool_call>", "</tool_call>"};
    for (auto * u : user) { toks.push_back(u); types.push_back(4); }

    gguf_context * ctx = gguf_init_empty();
    std::vector<const char *> tp, mp;
    for (auto & t : toks) tp.push_back(t.c_str());
    for (auto & m : merges) mp.push_back(m.c_str());
    gguf_set_val_str(ctx, "general.architecture", "qwen35");
    gguf_set_val_str(ctx, "tokenizer.ggml.model", "gpt2");
    gguf_set_val_str(ctx, "tokenizer.ggml.pre", "qwen35");
    gguf_set_arr_str(ctx, "tokenizer.ggml.tokens", tp.data(), tp.size());
    gguf_set_arr_str(ctx, "tokenizer.ggml.merges", mp.data(), mp.size());
    gguf_set_arr_data(ctx, "tokenizer.ggml.token_type", GGUF_TYPE_INT32, types.data(), types.size());
    gguf_set_val_u32(ctx, "tokenizer.ggml.bos_token_id", 256 + 10 + 3);
    gguf_set_val_u32(ctx, "tokenizer.ggml.eos_token_id", 256 + 10 + 3 + 2);
    bool ok = gguf_write_to_file(ctx, path.c_str(), /*only_meta=*/false);
    gguf_free(ctx);
    return ok;
}

inline std::string scratch_path(const char * name) {
    const char * tmp = std::getenv("TMPDIR");
    std::string dir = std::string(tmp && *tmp ? tmp : "/tmp") + "/luce_tok_XXXXXX";
    if (!mkdtemp(&dir[0])) std::abort();
    return dir + "/" + name;
}

}  // namespace lucefuzz
