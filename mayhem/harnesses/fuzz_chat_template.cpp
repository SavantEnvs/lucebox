// Fuzz the Jinja chat-template renderer (render_chat_template_jinja over the vendored
// llama.cpp common/jinja lexer/parser/runtime). The template comes from the GGUF
// `tokenizer.chat_template` key or --chat-template-file, i.e. from the model file.
// Input: <template source> ['\0' <tools JSON>].
#include "server/chat_template.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <vector>

using luce::common::ChatMessage;

static std::vector<ChatMessage> messages() {
    std::vector<ChatMessage> m(5);
    m[0].role = "system";    m[0].content = "You are helpful.";
    m[1].role = "user";      m[1].content = "Weather in Paris?";
    m[2].role = "assistant"; m[2].content = "";
    m[2].reasoning_content = "Need the tool.";
    luce::common::ChatToolCall tc;
    tc.id = "call_1"; tc.name = "get_weather";
    tc.arguments = nlohmann::ordered_json{{"city", "Paris"}, {"days", 2}};
    m[2].tool_calls.push_back(tc);
    m[3].role = "tool";      m[3].content = "{\"temp\": 21}";
    m[3].tool_call_id = "call_1"; m[3].tool_name = "get_weather";
    m[4].role = "user";      m[4].content = "Thanks \xc3\xa9\xe2\x82\xac";
    return m;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
    std::string in(reinterpret_cast<const char *>(data), size);
    size_t nul = in.find('\0');
    std::string tmpl = nul == std::string::npos ? in : in.substr(0, nul);
    std::string tools = nul == std::string::npos ? std::string() : in.substr(nul + 1);
    if (tmpl.empty()) return -1;  // the server rejects an empty template before rendering
    static const std::vector<ChatMessage> msgs = messages();
    for (int agp = 0; agp < 2; ++agp) {
        try {
            (void) luce::common::render_chat_template_jinja(
                tmpl, msgs, "<s>", "</s>", agp == 1, agp == 0, tools, agp ? "high" : "", -1);
        } catch (const std::exception &) {
            // The loader catches std::exception from the renderer and reports the template error.
        }
    }
    return 0;
}
