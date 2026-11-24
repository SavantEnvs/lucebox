// Fuzz the streaming reply path: SseEmitter (think-tag splitting, stop sequences, tool-call
// buffering and parsing, UTF-8 handling, every API's SSE framing) fed with model-generated
// pieces, then ServerStatus::to_json / to_sse_event over the same text as the prompt excerpt
// and messages (the /status stream, UTF-8 fix 55af9341).
// Input: byte 0 = flags (bits 0-1 API format, bit 2 started_in_thinking, bit 3
// tool_calls_allowed, bit 4 think_tags_are_tokens, bit 5 ended_on_eos, bit 6 degenerate);
// then <tools JSON> '\0' <pieces>, the pieces separated by 0x01; a piece starting with 0x02 is
// passed as a thinking-boundary token. Without a NUL there are no tools and everything after
// byte 0 is pieces. Per-input state: a fresh emitter, ToolMemory and ServerStatus.
#include "server/sse_emitter.h"
#include "server/server_status.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <vector>

using namespace luce::common;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
    if (size < 1) return -1;
    const uint8_t ctl = data[0];
    const std::string in(reinterpret_cast<const char *>(data + 1), size - 1);
    static const ApiFormat kFormats[] = {ApiFormat::OPENAI_CHAT, ApiFormat::ANTHROPIC,
                                         ApiFormat::RESPONSES, ApiFormat::COMPLETIONS};
    const ApiFormat format = kFormats[ctl & 3];

    json tools = json::array();
    std::string stream = in;
    const size_t nul = in.find('\0');
    if (nul != std::string::npos) {
        tools = json::parse(in.substr(0, nul), nullptr, /*allow_exceptions=*/false);
        if (tools.is_discarded()) tools = json::array();
        stream = in.substr(nul + 1);
    }

    try {
        ToolMemory tool_memory;
        SseEmitter emitter(format, "req_fuzz", "lucebox-fuzz", 7, tools, &tool_memory,
                           {"</stop>", "\n\nUser:"}, (ctl & 4) != 0, (ctl & 8) != 0);
        emitter.set_think_tags_are_tokens((ctl & 16) != 0);
        size_t bytes = 0;
        for (const auto & s : emitter.emit_start()) bytes += s.size();
        int n = 0;
        size_t pos = 0;
        while (pos <= stream.size()) {
            size_t end = stream.find('\x01', pos);
            if (end == std::string::npos) end = stream.size();
            std::string piece = stream.substr(pos, end - pos);
            bool think = !piece.empty() && piece[0] == '\x02';
            if (think) piece.erase(0, 1);
            std::string text;
            if (!emitter.suppress_undeclared_tool_protocol_token(piece)) {
                for (const auto & s : emitter.emit_token(piece, think)) bytes += s.size();
            }
            ++n;
            if (emitter.stop_hit()) break;
            pos = end + 1;
        }
        GenTimings timings;
        timings.prefill_s = 0.25;
        timings.decode_s = 0.5;
        for (const auto & s : emitter.emit_finish(n, &timings, n, (ctl & 32) != 0, (ctl & 64) != 0))
            bytes += s.size();
        (void) emitter.finish_reason();
        (void) bytes;

        ServerStatus status;
        ServerStatus::RequestInfo info;
        info.model = "lucebox-fuzz";
        info.format = api_format_name(format);
        status.set_running(stream, 7, true, info);
        status.set_messages(emitter.accumulated_raw());
        status.update_completion_tokens(n);
        status.set_draft_tokens({emitter.reasoning_text(), emitter.accumulated_text()});
        (void) status.to_sse_event();
    } catch (const std::exception &) {
        // The server's job loop catches std::exception around generation and reports it.
    }
    return 0;
}
