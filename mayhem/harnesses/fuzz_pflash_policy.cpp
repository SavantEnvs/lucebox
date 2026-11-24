// Fuzz the PFlash request-probe policy JSON (PFLASH_REQUEST_PROBE_POLICY file) and the
// PFlash drafter IPC "compress" command line parser (parent<->drafter daemon protocol).
#include "common/pflash_drafter_ipc.h"
#include "pflash/pflash_request_probe.h"

#include <cstddef>
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
    std::string text(reinterpret_cast<const char *>(data), size);

    luce::pflash::PFlashRequestPolicy policy;
    std::string error;
    if (luce::pflash::pflash_request_policy_apply_json(text, policy, error)) {
        (void) luce::pflash::pflash_request_policy_valid(policy, error);
    }

    luce::common::PFlashDrafterIpcCompressCommand cmd;
    std::string line = text;
    size_t nl = line.find('\n');
    if (nl != std::string::npos) line.resize(nl);
    if (luce::common::parse_pflash_drafter_ipc_compress_command(line, cmd, error)) {
        std::string out;
        (void) luce::common::format_pflash_drafter_ipc_compress_command(
            cmd.keep_ratio, cmd.score_query_end, cmd.score_query_tokens,
            cmd.required_instruction_spans, cmd.path, out, error);
    }
    return 0;
}
