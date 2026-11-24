// Fuzz luce::vision::decode_image, which decodes the JPEG/PNG bytes a client sends as an image
// (an image_url data URL or an Anthropic base64 image source, after parse_image_data_url) with
// libjpeg-turbo and lodepng. Input: the encoded image bytes. Default DecodeLimits, as the server
// uses. No state is kept between inputs.
#include "common/vision/image_decode.h"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
    luce::vision::EncodedImageView view{data, size};
    luce::vision::DecodeResult r = luce::vision::decode_image(view);
    if (r) {
        // Touch every decoded byte so ASan sees an undersized pixel buffer.
        unsigned sum = 0;
        for (uint8_t b : r.image.pixels) sum += b;
        (void) sum;
    }
    return 0;
}
