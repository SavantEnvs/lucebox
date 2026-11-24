// Fuzz the GBNF grammar compiler + matcher behind the request `grammar` field.
// Input: <grammar text> '\0' <bytes to feed the matcher>.
#include "common/gbnf.h"

#include <cstddef>
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
    std::string in(reinterpret_cast<const char *>(data), size);
    size_t nul = in.find('\0');
    std::string grammar = nul == std::string::npos ? in : in.substr(0, nul);
    std::string bytes = nul == std::string::npos ? std::string() : in.substr(nul + 1);
    std::string error;
    auto m = luce::common::gbnf::Matcher::compile(grammar, "root", error);
    if (!m) return 0;
    (void) m->can_end();
    // Feed the bytes in small chunks so partial UTF-8 sequences cross calls.
    for (size_t i = 0; i < bytes.size() && !m->dead(); i += 3) {
        m->accept_bytes(bytes.substr(i, 3));
        (void) m->can_end();
    }
    return 0;
}
