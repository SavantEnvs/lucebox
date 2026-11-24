// Fuzz the HTTP request-body path the server runs before rendering a prompt
// (http_server.cpp parse_endpoint_request / handle_model_request):
//   normalize_anthropic_system, keep_anthropic_input_order, prepare_request_images
//   (-> extract_chat_images -> parse_image_data_url on every image_url / image source),
//   redact_image_urls and normalize_chat_messages.
// Input: byte 0 selects the API format (bits 0-1), whether a ToolMemory replays call ids
// (bit 2) and the text-part separator (bit 3); the rest is the request body as JSON. A body
// that is a JSON array is taken as the `messages` array itself. Every data-URL string found
// under an `image_url` is also handed to parse_image_data_url directly.
// Per-input state: a fresh ToolMemory (seeded with one fixed entry so its replay path is
// reachable); nothing else is kept between inputs.
#include "server/api_adapters.h"
#include "server/image_input.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <vector>

using namespace luce::common;

static void feed_image_urls(const json & v, int depth) {
    if (depth > 64) return;
    if (v.is_object()) {
        auto it = v.find("image_url");
        if (it != v.end()) {
            const json * url = &*it;
            if (url->is_object() && url->contains("url")) url = &(*url)["url"];
            if (url->is_string()) {
                EncodedImage img;
                std::string err;
                (void) parse_image_data_url(url->get_ref<const std::string &>(), img, err);
            }
        }
        for (const auto & [k, child] : v.items()) feed_image_urls(child, depth + 1);
    } else if (v.is_array()) {
        for (const auto & child : v) feed_image_urls(child, depth + 1);
    }
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
    if (size < 1) return -1;
    const uint8_t ctl = data[0];
    const std::string body_text(reinterpret_cast<const char *>(data + 1), size - 1);
    static const ApiFormat kFormats[] = {ApiFormat::OPENAI_CHAT, ApiFormat::ANTHROPIC,
                                         ApiFormat::RESPONSES, ApiFormat::COMPLETIONS};
    const ApiFormat format = kFormats[ctl & 3];
    const bool replay = (ctl & 4) != 0;
    const char * separator = (ctl & 8) ? "\n\n" : "";

    json body = json::parse(body_text, nullptr, /*allow_exceptions=*/false);
    if (body.is_discarded()) return -1;   // the server answers 400 before any of this runs

    try {
        json messages;
        if (body.is_array()) {
            messages = body;
        } else if (body.is_object()) {
            if (format == ApiFormat::RESPONSES && body.contains("input")) messages = body["input"];
            else messages = body.value("messages", json::array());
            if (format == ApiFormat::ANTHROPIC) {
                normalize_anthropic_system(body, messages);
                if (body_text.find("\"tool_use\"") != std::string::npos) {
                    const auto ordered = nlohmann::ordered_json::parse(body_text, nullptr, false);
                    if (ordered.is_object() && ordered.contains("messages"))
                        keep_anthropic_input_order(ordered["messages"], messages);
                }
            }
        } else {
            return -1;
        }

        feed_image_urls(messages, 0);

        const ImageRequestPolicy policy{format == ApiFormat::OPENAI_CHAT, true, "<|image|>",
                                        (ctl & 8) ? "\n\n" : ""};
        json normalized;
        std::vector<EncodedImage> images;
        std::string error;
        if (prepare_request_images(messages, policy, normalized, images, error)) {
            messages = std::move(normalized);
            redact_image_urls(body);
            redact_image_urls(messages);
        }

        ToolMemory tool_memory;
        tool_memory.remember({"call_1", "call_2"}, "<tool_call>{\"name\":\"f\"}</tool_call>");
        std::vector<ChatMessage> out =
            normalize_chat_messages(messages, format, tool_memory, replay, separator);
        size_t total = 0;
        for (const auto & m : out) total += m.content.size();
        (void) total;
    } catch (const std::exception &) {
        // http_server.cpp catches std::exception around this path and answers 400.
    }
    return 0;
}
