/*
 * Copyright (C) 2022 BlueKitchen GmbH
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holders nor the names of
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 * 4. Any redistribution, use, or modification is done solely for
 *    personal benefit and not for any commercial purpose or for
 *    monetary gain.
 *
 * THIS SOFTWARE IS PROVIDED BY BLUEKITCHEN GMBH AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL BLUEKITCHEN
 * GMBH OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Please inquire about commercial licensing options at
 * contact@bluekitchen-gmbh.com
 *
 */

#define BTSTACK_FILE__ "btstack_audio_pico_i2s.c"

/*
 *  btstack_audio_pico_i2s.c
 *
 *  Implementation of btstack_audio.h using pico_i2s
 *  Based on btstack_audio_pico.c from pico-examples, plus
 *  - hardware is only set up once (re-init used to claim dma and pio again and panic)
 *  - sample rate changes on re-init
 *  - volume with a dB curve, ramped per buffer to avoid zipper noise
 */

#include "btstack_config.h"

#include "btstack_debug.h"
#include "btstack_audio.h"
#include "btstack_run_loop.h"

#include <stddef.h>
#include <hardware/dma.h>
#include <hardware/pio.h>

#include "pico/audio_i2s.h"

#include "gain.h"

#ifndef VOLUME_RANGE_DB
#define VOLUME_RANGE_DB 50
#endif

#define DRIVER_POLL_INTERVAL_MS   5
#define SAMPLES_PER_BUFFER      512
#define BUFFER_COUNT              3

// client
static void (*playback_callback)(int16_t * buffer, uint16_t num_samples, const btstack_audio_context_t * timeinfo);

// timer to fill output ring buffer
static btstack_timer_source_t  driver_timer_sink;

static bool btstack_audio_pico_sink_active;

static audio_format_t        btstack_audio_pico_audio_format;
static audio_buffer_format_t btstack_audio_pico_producer_format;
static audio_buffer_pool_t * btstack_audio_pico_audio_buffer_pool;
static uint8_t               btstack_audio_pico_channel_count;

// gain in Q16, current approaches target within one buffer
static int32_t btstack_audio_pico_gain_current = GAIN_UNITY;
static int32_t btstack_audio_pico_gain_target  = GAIN_UNITY;


static audio_buffer_pool_t *init_audio(uint32_t sample_frequency) {

    // always use stereo
    btstack_audio_pico_audio_format.format = AUDIO_BUFFER_FORMAT_PCM_S16;
    btstack_audio_pico_audio_format.sample_freq = sample_frequency;
    btstack_audio_pico_audio_format.channel_count = 2;

    btstack_audio_pico_producer_format.format = &btstack_audio_pico_audio_format;
    btstack_audio_pico_producer_format.sample_stride = 2 * 2;

    audio_buffer_pool_t * producer_pool = audio_new_producer_pool(&btstack_audio_pico_producer_format, BUFFER_COUNT, SAMPLES_PER_BUFFER);

    // audio_i2s_setup claims dma channel and state machine itself https://github.com/raspberrypi/pico-extras/issues/48
    // so just look for free ones here (the cyw43 driver also uses a pio)
    audio_i2s_config_t config;
    config.data_pin       = PICO_AUDIO_I2S_DATA_PIN;
    config.clock_pin_base = PICO_AUDIO_I2S_CLOCK_PIN_BASE;  // BCK, LRCK = BCK+1
    config.dma_channel    = (uint8_t) dma_claim_unused_channel(true);
    dma_channel_unclaim(config.dma_channel);
    PIO pio = pio_get_instance(PICO_AUDIO_I2S_PIO);
    config.pio_sm         = (uint8_t) pio_claim_unused_sm(pio, true);
    pio_sm_unclaim(pio, config.pio_sm);

    const audio_format_t * output_format = audio_i2s_setup(&btstack_audio_pico_audio_format, &config);
    if (!output_format) {
        panic("PicoAudio: Unable to open audio device.\n");
    }

    bool ok = audio_i2s_connect(producer_pool);
    if (!ok) {
        panic("PicoAudio: Unable to connect audio device.\n");
    }

    return producer_pool;
}

static void btstack_audio_pico_sink_fill_buffers(void){
    while (true){
        audio_buffer_t * audio_buffer = take_audio_buffer(btstack_audio_pico_audio_buffer_pool, false);
        if (audio_buffer == NULL){
            break;
        }

        int16_t * buffer16 = (int16_t *) audio_buffer->buffer->bytes;
        uint32_t num_frames = audio_buffer->max_sample_count;
        (*playback_callback)(buffer16, (uint16_t) num_frames, NULL);

        // duplicate samples for mono
        if (btstack_audio_pico_channel_count == 1){
            for (int i = (int) num_frames - 1; i >= 0; i--){
                buffer16[2*i+1] = buffer16[i];
                buffer16[2*i  ] = buffer16[i];
            }
        }

        btstack_audio_pico_gain_current = gain_apply(buffer16, num_frames,
            btstack_audio_pico_gain_current, btstack_audio_pico_gain_target);

        audio_buffer->sample_count = num_frames;
        give_audio_buffer(btstack_audio_pico_audio_buffer_pool, audio_buffer);
    }
}

static void driver_timer_handler_sink(btstack_timer_source_t * ts){

    // refill
    btstack_audio_pico_sink_fill_buffers();

    // re-set timer
    btstack_run_loop_set_timer(ts, DRIVER_POLL_INTERVAL_MS);
    btstack_run_loop_add_timer(ts);
}

static int btstack_audio_pico_sink_init(
    uint8_t channels,
    uint32_t samplerate,
    void (*playback)(int16_t * buffer, uint16_t num_samples, const btstack_audio_context_t * timeinfo)
){
    btstack_assert(playback != NULL);
    btstack_assert(channels == 1 || channels == 2);

    playback_callback  = playback;
    btstack_audio_pico_channel_count = channels;

    if (!btstack_audio_pico_audio_buffer_pool) {
        btstack_audio_pico_audio_buffer_pool = init_audio(samplerate);
    } else {
        // i2s driver adapts its clock to the producer format with the next buffer
        btstack_audio_pico_audio_format.sample_freq = samplerate;
    }
    return 0;
}

static uint32_t btstack_audio_pico_sink_get_samplerate(void) {
    return btstack_audio_pico_audio_format.sample_freq;
}

// 0: mute, 1..127: -VOLUME_RANGE_DB..0 dB in equal dB steps
static void btstack_audio_pico_sink_set_volume(uint8_t volume){
    int32_t gain = gain_from_volume(volume, VOLUME_RANGE_DB);

    btstack_audio_pico_gain_target = gain;
    if (!btstack_audio_pico_sink_active) {
        btstack_audio_pico_gain_current = gain;  // no ramp needed while silent
    }
}

static void btstack_audio_pico_sink_start_stream(void){

    // pre-fill HAL buffers
    btstack_audio_pico_sink_fill_buffers();

    // start timer
    btstack_run_loop_set_timer_handler(&driver_timer_sink, &driver_timer_handler_sink);
    btstack_run_loop_set_timer(&driver_timer_sink, DRIVER_POLL_INTERVAL_MS);
    btstack_run_loop_add_timer(&driver_timer_sink);

    // state
    btstack_audio_pico_sink_active = true;

    audio_i2s_set_enabled(true);
}

static void btstack_audio_pico_sink_stop_stream(void){

    audio_i2s_set_enabled(false);

    // stop timer
    btstack_run_loop_remove_timer(&driver_timer_sink);
    // state
    btstack_audio_pico_sink_active = false;
}

static void btstack_audio_pico_sink_close(void){
    // stop stream if needed, hardware stays set up for the next init
    if (btstack_audio_pico_sink_active){
        btstack_audio_pico_sink_stop_stream();
    }
}

static const btstack_audio_sink_t btstack_audio_pico_sink = {
    .init = &btstack_audio_pico_sink_init,
    .get_samplerate = &btstack_audio_pico_sink_get_samplerate,
    .set_volume = &btstack_audio_pico_sink_set_volume,
    .start_stream = &btstack_audio_pico_sink_start_stream,
    .stop_stream = &btstack_audio_pico_sink_stop_stream,
    .close = &btstack_audio_pico_sink_close,
};

const btstack_audio_sink_t * btstack_audio_pico_sink_get_instance(void){
    return &btstack_audio_pico_sink;
}
