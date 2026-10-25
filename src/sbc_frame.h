#ifndef sbc_frame_h
#define sbc_frame_h

#include <stdint.h>

#define MAX_SBC_FRAME_SIZE 256  // our capabilities allow up to 224 (dual channel, bitpool 53)

// Length of the sbc frame at h, or 0 if it is not a valid frame within avail bytes
// Sets samples to the number of audio frames it decodes to
static inline unsigned sbc_frame_length(const uint8_t *h, unsigned avail, unsigned *samples) {
    if (avail < 4 || h[0] != 0x9C) return 0;  // sync word

    unsigned blocks   = 4 * (1 + ((h[1] >> 4) & 0x03));
    unsigned mode     = (h[1] >> 2) & 0x03;  // 0: mono, 1: dual channel, 2: stereo, 3: joint stereo
    unsigned subbands = (h[1] & 0x01) ? 8 : 4;
    unsigned bitpool  = h[2];
    unsigned channels = (mode == 0) ? 1 : 2;

    unsigned length = 4 + (4 * subbands * channels) / 8;
    if (mode < 2) {
        length += (blocks * channels * bitpool + 7) / 8;
    } else {
        length += ((mode == 3 ? subbands : 0) + blocks * bitpool + 7) / 8;
    }

    if (length > avail || length > MAX_SBC_FRAME_SIZE) return 0;

    *samples = blocks * subbands;
    return length;
}

#endif
