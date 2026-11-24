// Fuzz the cluster control-plane wire decoder (TCP frames between ranks):
// decode_header on the first 12 bytes, then every message decoder on the payload,
// re-encoding whatever decodes.
#include "cluster/cluster_protocol.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

using namespace luce::cluster;

template <typename Msg>
static void try_msg(const Frame & f) {
    Frame g = f;
    g.type = Msg::type();
    Msg m;
    if (parse_frame(g, m)) {
        Frame back = make_frame(m);
        Msg m2;
        (void) parse_frame(back, m2);
    }
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
    if (size < kFrameHeaderBytes) return -1;
    FrameHeader h;
    std::string err;
    bool hdr_ok = decode_header(data, h, &err);
    Frame f;
    f.type = static_cast<MsgType>(h.type);
    f.payload.assign(data + kFrameHeaderBytes, data + size);
    if (hdr_ok) (void) msg_type_name(f.type);
    try_msg<HelloMsg>(f);
    try_msg<WelcomeMsg>(f);
    try_msg<HeartbeatMsg>(f);
    try_msg<AbortMsg>(f);
    try_msg<RequestMsg>(f);
    try_msg<DecisionMsg>(f);
    try_msg<DraftMsg>(f);
    try_msg<AcceptMsg>(f);
    try_msg<RequestReportMsg>(f);
    try_msg<BackendOpMsg>(f);
    try_msg<RequestEndMsg>(f);
    try_msg<ShutdownMsg>(f);
    (void) fnv1a64(f.payload.data(), f.payload.size());
    return 0;
}
