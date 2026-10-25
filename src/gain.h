#ifndef gain_h
#define gain_h

#include <stdint.h>
#include <math.h>

#define GAIN_UNITY (1L << 16)  // Q16


// 0: mute, 1..127: -range_db..0 dB in equal dB steps
static inline int32_t gain_from_volume(uint8_t volume, float range_db) {
    if (volume > 127) volume = 127;
    if (volume == 0) return 0;

    float db = -range_db * (float)(127 - volume) / 126.0f;
    return (int32_t) lroundf((float)GAIN_UNITY * powf(10.0f, db / 20.0f));
}


// apply gain to interleaved stereo samples, ramp linearly from gain to target over the buffer
// gains are 0..GAIN_UNITY and num_frames < 2^15, so products fit in 32 bit
// Returns the gain at the end of the buffer
static inline int32_t gain_apply(int16_t *samples, uint32_t num_frames, int32_t gain, int32_t target) {
    int32_t delta = target - gain;

    if (delta == 0 || num_frames == 0) {
        if (gain == GAIN_UNITY) return gain;
        for (uint32_t i = 0; i < 2 * num_frames; i++) {
            samples[i] = (int16_t)((samples[i] * gain + (1 << 15)) >> 16);
        }
        return gain;
    }

    for (uint32_t i = 0; i < num_frames; i++) {
        int32_t g = gain + (delta * (int32_t)(i + 1)) / (int32_t)num_frames;
        samples[2*i]   = (int16_t)((samples[2*i]   * g + (1 << 15)) >> 16);
        samples[2*i+1] = (int16_t)((samples[2*i+1] * g + (1 << 15)) >> 16);
    }
    return target;
}

#endif
