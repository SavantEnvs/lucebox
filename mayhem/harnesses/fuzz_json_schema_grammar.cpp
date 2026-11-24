// Fuzz json_schema_to_grammar (response_format / tool parameter schemas: $ref, pattern,
// enum, min/max...) and compile the emitted GBNF with the server's matcher.
#include "common/gbnf.h"
#include "server/json_schema_grammar.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <regex>
#include <stdexcept>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
    auto schema = nlohmann::ordered_json::parse(data, data + size, nullptr, /*allow_exceptions=*/false);
    if (schema.is_discarded()) return -1;
    std::string grammar;
    try {
        grammar = luce::common::json_schema_to_grammar(schema);
    } catch (const nlohmann::json::exception &) {
        return 0;
    } catch (const std::invalid_argument &) {
        return 0;  // unsupported schema: the server reports it as a 400
    } catch (const std::runtime_error &) {
        return 0;
    }
    std::string error;
    auto m = luce::common::gbnf::Matcher::compile(grammar, "root", error);
    if (m) {
        m->accept_bytes("{\"a\": 1, \"b\": [true, null, \"x\"]}");
        (void) m->can_end();
    }
    return 0;
}
