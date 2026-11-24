// Fuzz the tool-call parser the server runs on model-generated text
// (luce::common::parse_tool_calls and the streaming helpers the SSE emitter uses).
// Input: <tools JSON> '\0' <generated text>. Without a NUL, the whole input is text
// and the tools array is a fixed two-tool schema.
#include "server/tool_parser.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

using luce::common::json;

static const char * kDefaultTools = R"([
 {"type":"function","function":{"name":"get_weather","parameters":{"type":"object",
   "properties":{"city":{"type":"string"},"days":{"type":"integer"},"metric":{"type":"boolean"},
   "tags":{"type":"array","items":{"type":"string"}}},"required":["city"]}}},
 {"type":"function","function":{"name":"run","parameters":{"type":"object",
   "properties":{"cmd":{"type":"string"},"opts":{"type":"object"},"n":{"type":"number"}}}}}])";

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
    std::string in(reinterpret_cast<const char *>(data), size);
    json tools;
    std::string text;
    size_t nul = in.find('\0');
    if (nul == std::string::npos) {
        tools = json::parse(kDefaultTools);
        text = in;
    } else {
        tools = json::parse(in.substr(0, nul), nullptr, /*allow_exceptions=*/false);
        if (tools.is_discarded()) tools = json();
        text = in.substr(nul + 1);
    }
    try {
        size_t pos = 0;
        (void) luce::common::find_tool_syntax_start(text, tools, pos);
        (void) luce::common::tool_call_openers(tools);
        (void) luce::common::tool_syntax_holdback(tools);
        luce::common::ToolParseResult r = luce::common::parse_tool_calls(text, tools);
        for (const auto & c : r.tool_calls) {
            (void) luce::common::tool_parameter_properties(tools, c.name);
        }
        luce::common::ToolParseResult r2 = luce::common::parse_tool_calls(text);
        (void) r2;
    } catch (const json::exception &) {
        // The HTTP layer catches nlohmann exceptions and answers 400.
    }
    return 0;
}
