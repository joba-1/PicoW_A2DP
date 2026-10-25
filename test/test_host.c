// Host unit tests: sbc frame parsing against btstack's sbc encoder, volume gain math

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "btstack_sbc.h"
#include "btstack_sbc_bluedroid.h"

#include "sbc_frame.h"
#include "gain.h"

static int failures = 0;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)


// every configuration the sink capabilities allow: frame length and sample count must match the encoder
static void test_sbc_frame_length(void) {
    static const uint16_t rates[] = { 16000, 32000, 44100, 48000 };
    static const uint8_t blocks[] = { 4, 8, 12, 16 };
    static const uint8_t subbands[] = { 4, 8 };
    static const uint8_t bitpools[] = { 2, 19, 35, 53 };

    btstack_sbc_encoder_bluedroid_t context;
    const btstack_sbc_encoder_t * encoder = btstack_sbc_encoder_bluedroid_init_instance(&context);

    int16_t pcm[16 * 8 * 2];
    uint8_t sbc[512];
    unsigned max_length = 0;
    int configs = 0;

    srand(1);
    for (size_t i = 0; i < sizeof(pcm) / sizeof(pcm[0]); i++) {
        pcm[i] = (int16_t)(rand() - RAND_MAX / 2);
    }

    for (unsigned r = 0; r < 4; r++)
    for (unsigned mode = SBC_CHANNEL_MODE_MONO; mode <= SBC_CHANNEL_MODE_JOINT_STEREO; mode++)
    for (unsigned b = 0; b < 4; b++)
    for (unsigned s = 0; s < 2; s++)
    for (unsigned alloc = SBC_ALLOCATION_METHOD_LOUDNESS; alloc <= SBC_ALLOCATION_METHOD_SNR; alloc++)
    for (unsigned p = 0; p < 4; p++) {
        uint8_t status = encoder->configure(&context, SBC_MODE_STANDARD, blocks[b], subbands[s],
            (btstack_sbc_allocation_method_t) alloc, rates[r], bitpools[p], (btstack_sbc_channel_mode_t) mode);
        if (status) continue;  // encoder rejects some combinations

        memset(sbc, 0, sizeof(sbc));
        encoder->encode_signed_16(&context, pcm, sbc);
        unsigned expected = encoder->sbc_buffer_length(&context);

        unsigned samples = 0;
        unsigned length = sbc_frame_length(sbc, sizeof(sbc), &samples);
        CHECK(length == expected, "rate %u mode %u blocks %u subbands %u bitpool %u: length %u, encoder %u",
            rates[r], mode, blocks[b], subbands[s], bitpools[p], length, expected);
        CHECK(samples == (unsigned)blocks[b] * subbands[s], "samples %u", samples);

        // truncated frame is rejected
        CHECK(sbc_frame_length(sbc, expected - 1, &samples) == 0, "truncated frame accepted");

        if (length > max_length) max_length = length;
        configs++;
    }

    CHECK(configs > 500, "only %d configurations tested", configs);
    CHECK(max_length <= MAX_SBC_FRAME_SIZE, "max frame %u > %u", max_length, MAX_SBC_FRAME_SIZE);
    printf("sbc frames: %d configurations, max frame length %u\n", configs, max_length);

    // no sync word
    uint8_t junk[8] = { 0x9D, 0xBD, 53, 0 };
    unsigned samples;
    CHECK(sbc_frame_length(junk, sizeof(junk), &samples) == 0, "junk accepted");
    CHECK(sbc_frame_length(junk, 3, &samples) == 0, "short header accepted");
}


static void test_gain_curve(void) {
    CHECK(gain_from_volume(0, 50) == 0, "mute");
    CHECK(gain_from_volume(127, 50) == GAIN_UNITY, "unity at max");
    CHECK(gain_from_volume(200, 50) == GAIN_UNITY, "clamped above max");

    double db1 = 20 * log10((double)gain_from_volume(1, 50) / GAIN_UNITY);
    CHECK(fabs(db1 + 50) < 0.1, "volume 1 is %.2f dB", db1);

    double db64 = 20 * log10((double)gain_from_volume(64, 50) / GAIN_UNITY);
    CHECK(fabs(db64 + 25) < 0.3, "volume 64 is %.2f dB", db64);

    for (int v = 1; v < 127; v++) {
        CHECK(gain_from_volume((uint8_t)v, 50) < gain_from_volume((uint8_t)(v + 1), 50), "not monotonic at %d", v);
    }
}


static void test_gain_apply(void) {
    int16_t s[2 * 512];

    // unity keeps samples, including extremes
    for (int i = 0; i < 1024; i++) s[i] = (int16_t)(i % 2 ? -32768 : 32767);
    CHECK(gain_apply(s, 512, GAIN_UNITY, GAIN_UNITY) == GAIN_UNITY, "unity gain changed");
    CHECK(s[0] == 32767 && s[1] == -32768, "unity changed samples");

    // mute
    CHECK(gain_apply(s, 512, 0, 0) == 0, "mute gain changed");
    CHECK(s[0] == 0 && s[1] == 0 && s[1023] == 0, "mute left samples");

    // half
    for (int i = 0; i < 1024; i++) s[i] = 1000;
    gain_apply(s, 512, GAIN_UNITY / 2, GAIN_UNITY / 2);
    CHECK(s[0] == 500 && s[1023] == 500, "half gain: %d %d", s[0], s[1023]);

    // ramp up from mute ends at target and rises monotonically
    for (int i = 0; i < 1024; i++) s[i] = 32767;
    CHECK(gain_apply(s, 512, 0, GAIN_UNITY) == GAIN_UNITY, "ramp end gain");
    CHECK(s[1022] == 32767 && s[1023] == 32767, "ramp end %d", s[1023]);
    CHECK(s[0] < 100, "ramp start %d", s[0]);
    for (int i = 1; i < 512; i++) {
        CHECK(s[2*i] >= s[2*(i-1)], "ramp not monotonic at %d", i);
    }

    // ramp down to mute
    for (int i = 0; i < 1024; i++) s[i] = -32768;
    CHECK(gain_apply(s, 512, GAIN_UNITY, 0) == 0, "ramp down end gain");
    CHECK(s[1023] == 0 && s[0] < -32000, "ramp down %d %d", s[0], s[1023]);
}


int main(void) {
    test_sbc_frame_length();
    test_gain_curve();
    test_gain_apply();

    if (failures) {
        printf("%d failures\n", failures);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
