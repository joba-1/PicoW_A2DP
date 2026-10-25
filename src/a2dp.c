#include "a2dp.h"

#include <btstack.h>
#include <btstack_resample.h>
#include <classic/a2dp_sink.h>
#include <classic/btstack_sbc_bluedroid.h>

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "volume.h"
#include "sbc_frame.h"

// from btstack_audio_pico_i2s.c
const btstack_audio_sink_t * btstack_audio_pico_sink_get_instance(void);


#ifndef A2DP_LATENCY_MS
#define A2DP_LATENCY_MS 200
#endif

// buffer is static RAM, about 180 bytes per ms
_Static_assert(A2DP_LATENCY_MS >= 20 && A2DP_LATENCY_MS <= 1000, "A2DP_LATENCY_MS must be 20..1000");

#define NUM_CHANNELS          2
#define BYTES_PER_FRAME       (2*NUM_CHANNELS)  // audio frame: one int16_t sample per channel
#define SBC_MAX_AUDIO_FRAMES  128               // 16 blocks * 8 subbands
#define RESAMPLE_OUTPUT_FRAMES (SBC_MAX_AUDIO_FRAMES + 16)

// sbc frames are buffered with a 2 byte length prefix
// 90 bytes/ms covers 16 block, 8 subband, dual channel, bitpool 53 frames at 48kHz
#define SBC_STORAGE_MS        (2 * A2DP_LATENCY_MS + 100)
#define SBC_STORAGE_SIZE      (SBC_STORAGE_MS * 90)

// played but not yet audible samples in the i2s driver (3*512 + 2*256 frames at 44.1kHz)
#define I2S_LATENCY_MS        46

// resampling factor is Q16, max correction is 0x80/0x10000 ~ 0.2% (3.4 cent)
#define RESAMPLE_NOMINAL      0x10000
#define RESAMPLE_MAX_COMP     0x80

#define STATS_INTERVAL_MS     10000


// all configurations with bitpool 2-53 are supported
static const uint8_t _sbc_capabilities[] = {
    0xFF,  // all sampling frequencies, all channel modes
    0xFF,  // all block lengths, all subbands, both allocation methods
    2, 53
};

// btstack stores the configuration the source chose here, so it must stay valid
static uint8_t _sbc_configuration[4];

static a2dp_connection_cb_t _cb = 0;

// connection
static uint16_t _cid = 0;
static uint8_t  _seid = 0;
static bd_addr_t _addr;
static bool     _delay_reported = false;
static bool     _streaming = false;

// stream configuration
static uint16_t _sampling_frequency = 0;
static uint8_t  _num_channels = 0;

// media processing
static bool     _media_initialized = false;
static uint16_t _media_sampling_frequency = 0;
static bool     _audio_started = false;   // i2s output running
static bool     _buffering = true;        // play silence until enough sbc frames are buffered

static btstack_sbc_decoder_bluedroid_t _decoder_context;
static const btstack_sbc_decoder_t *   _decoder = 0;
static btstack_resample_t _resample;

static btstack_ring_buffer_t _sbc_ring;
static uint8_t  _sbc_storage[SBC_STORAGE_SIZE];
static uint32_t _sbc_samples = 0;      // audio frames buffered in _sbc_ring
static uint32_t _target_samples = 0;   // aim for this many audio frames in _sbc_ring
static int32_t  _level_avg = 0;        // low pass filtered _sbc_samples, Q4

// pcm left over from a decoded sbc frame that did not fit into the i2s buffer
static btstack_ring_buffer_t _pcm_ring;
static uint8_t  _pcm_storage[RESAMPLE_OUTPUT_FRAMES * BYTES_PER_FRAME];

// where handle_pcm_data puts decoded audio
static int16_t * _request_buffer = 0;
static int       _request_frames = 0;

// statistics
static uint32_t _underruns = 0;    // ran out of sbc frames
static uint32_t _overruns = 0;     // dropped old sbc frames to make room
static uint32_t _bad_frames = 0;   // dropped invalid sbc frames
static int32_t  _compensation = 0;


// Read next buffered sbc frame into frame, return its length or 0 if none
static unsigned sbc_frame_read(uint8_t *frame) {
    uint8_t prefix[2];
    uint32_t bytes_read;

    if (btstack_ring_buffer_bytes_available(&_sbc_ring) < sizeof(prefix)) return 0;
    btstack_ring_buffer_read(&_sbc_ring, prefix, sizeof(prefix), &bytes_read);
    unsigned length = little_endian_read_16(prefix, 0);
    btstack_ring_buffer_read(&_sbc_ring, frame, length, &bytes_read);

    unsigned samples = 0;
    sbc_frame_length(frame, length, &samples);  // was validated on write
    _sbc_samples -= samples;
    return length;
}


static void sbc_frame_write(const uint8_t *frame, unsigned length, unsigned samples) {
    uint8_t prefix[2];
    uint8_t dropped[MAX_SBC_FRAME_SIZE];

    // drop oldest frames if necessary, so latency stays bounded
    while (btstack_ring_buffer_bytes_free(&_sbc_ring) < length + sizeof(prefix)) {
        if (!sbc_frame_read(dropped)) return;
        _overruns++;
    }

    little_endian_store_16(prefix, 0, (uint16_t)length);
    btstack_ring_buffer_write(&_sbc_ring, prefix, sizeof(prefix));
    btstack_ring_buffer_write(&_sbc_ring, (uint8_t *)frame, length);
    _sbc_samples += samples;
}


// resample decoded frames into i2s buffer and the rest into pcm ring buffer
static void handle_pcm_data(int16_t * data, int num_audio_frames, int num_channels, int sample_rate, void * context) {
    UNUSED(sample_rate);
    UNUSED(context);

    if (num_audio_frames <= 0 || num_audio_frames > SBC_MAX_AUDIO_FRAMES) return;

    int16_t stereo[SBC_MAX_AUDIO_FRAMES * NUM_CHANNELS];
    if (num_channels == 1) {
        for (int i = 0; i < num_audio_frames; i++) {
            stereo[2*i] = stereo[2*i+1] = data[i];
        }
        data = stereo;
    }
    else if (num_channels != NUM_CHANNELS) {
        return;
    }

    int16_t output_buffer[RESAMPLE_OUTPUT_FRAMES * NUM_CHANNELS];
    int resampled_frames = (int) btstack_resample_block(&_resample, data, (uint32_t) num_audio_frames, output_buffer);

    // store data in i2s buffer first
    int frames_to_copy = btstack_min(resampled_frames, _request_frames);
    memcpy(_request_buffer, output_buffer, (size_t) frames_to_copy * BYTES_PER_FRAME);
    _request_frames -= frames_to_copy;
    _request_buffer += frames_to_copy * NUM_CHANNELS;

    // and rest in ring buffer
    int frames_to_store = resampled_frames - frames_to_copy;
    if (frames_to_store > 0) {
        if (btstack_ring_buffer_write(&_pcm_ring, (uint8_t *)&output_buffer[frames_to_copy * NUM_CHANNELS], (uint32_t) frames_to_store * BYTES_PER_FRAME)) {
            _overruns++;
        }
    }
}


// provide pcm frames to i2s sink
static void playback_handler(int16_t * buffer, uint16_t num_audio_frames, const btstack_audio_context_t * context) {
    UNUSED(context);

    // called from lower-layer but guaranteed to be on main thread
    if (!_media_initialized || _buffering) {
        memset(buffer, 0, num_audio_frames * BYTES_PER_FRAME);
        return;
    }

    // first fill from resampled audio
    uint32_t bytes_read;
    btstack_ring_buffer_read(&_pcm_ring, (uint8_t *) buffer, num_audio_frames * BYTES_PER_FRAME, &bytes_read);
    buffer += bytes_read / sizeof(int16_t);
    num_audio_frames -= bytes_read / BYTES_PER_FRAME;

    // then decode sbc frames, handle_pcm_data() fills the request buffer
    _request_buffer = buffer;
    _request_frames = num_audio_frames;
    uint8_t sbc_frame[MAX_SBC_FRAME_SIZE];
    while (_request_frames > 0) {
        unsigned length = sbc_frame_read(sbc_frame);
        if (!length) break;
        _decoder->decode_signed_16(&_decoder_context, 0, sbc_frame, (uint16_t) length);
    }

    // out of data: play silence and buffer up again, better one gap than many small ones
    if (_request_frames > 0) {
        memset(_request_buffer, 0, (size_t) _request_frames * BYTES_PER_FRAME);
        _request_frames = 0;
        _buffering = true;
        _underruns++;
    }
}


static void media_processing_init(void) {
    if (_media_initialized || !_sampling_frequency) return;

    _decoder = btstack_sbc_decoder_bluedroid_init_instance(&_decoder_context);
    _decoder->configure(&_decoder_context, SBC_MODE_STANDARD, handle_pcm_data, NULL);

    btstack_ring_buffer_init(&_sbc_ring, _sbc_storage, sizeof(_sbc_storage));
    btstack_ring_buffer_init(&_pcm_ring, _pcm_storage, sizeof(_pcm_storage));
    btstack_resample_init(&_resample, NUM_CHANNELS);  // mono gets upmixed before resampling
    btstack_resample_set_factor(&_resample, RESAMPLE_NOMINAL);
    _compensation = 0;

    _sbc_samples = 0;
    _target_samples = (uint32_t)A2DP_LATENCY_MS * _sampling_frequency / 1000;
    _buffering = true;

    const btstack_audio_sink_t * audio = btstack_audio_sink_get_instance();
    audio->init(NUM_CHANNELS, _sampling_frequency, &playback_handler);

    _media_sampling_frequency = _sampling_frequency;
    _audio_started = false;
    _media_initialized = true;
}


// stop decoding but keep the i2s output running (silence), avoids pops and stale audio
static void media_processing_pause(void) {
    if (!_media_initialized) return;

    _buffering = true;
    btstack_ring_buffer_reset(&_pcm_ring);
    btstack_ring_buffer_reset(&_sbc_ring);
    _sbc_samples = 0;
}


static void media_processing_close(void) {
    if (!_media_initialized) return;

    _media_initialized = false;
    _buffering = true;

    if (_audio_started) {
        btstack_audio_sink_get_instance()->close();
        _audio_started = false;
    }
}


static void print_stats(void) {
#if PRINT_STATS
    static uint32_t last_ms = 0;
    uint32_t now = btstack_run_loop_get_time_ms();
    if (now - last_ms < STATS_INTERVAL_MS) return;
    last_ms = now;
    printf("A2DP: buffer %" PRIu32 "/%" PRIu32 " ms, comp %" PRId32 " ppm, underruns %" PRIu32 ", overruns %" PRIu32 ", bad frames %" PRIu32 "\n",
        _sbc_samples * 1000 / _sampling_frequency, _target_samples * 1000 / _sampling_frequency,
        _compensation * 1000000 / RESAMPLE_NOMINAL, _underruns, _overruns, _bad_frames);
#endif
}


// keep buffer level near target by resampling slightly faster or slower
static void regulate_buffer(void) {
    if (_buffering) {
        // start playing at target level, or earlier if the buffer can't hold that much
        if (_sbc_samples < _target_samples && btstack_ring_buffer_bytes_free(&_sbc_ring) > sizeof(_sbc_storage) / 4) return;

        _buffering = false;
        _level_avg = (int32_t)(_sbc_samples << 4);
        if (!_audio_started) {
            btstack_audio_sink_get_instance()->start_stream();
            _audio_started = true;
        }
        return;
    }

    // ~16 packets (300ms) low pass to smooth bursty arrival
    _level_avg += ((int32_t)(_sbc_samples << 4) - _level_avg) / 16;

    // proportional control: max compensation at +-50% off target
    int32_t error = (_level_avg >> 4) - (int32_t)_target_samples;
    int32_t comp = error * RESAMPLE_MAX_COMP / (int32_t)(_target_samples / 2 + 1);
    if (comp > RESAMPLE_MAX_COMP) comp = RESAMPLE_MAX_COMP;
    if (comp < -RESAMPLE_MAX_COMP) comp = -RESAMPLE_MAX_COMP;

    // factor > nominal consumes input faster
    _compensation = comp;
    btstack_resample_set_factor(&_resample, (uint32_t)(RESAMPLE_NOMINAL + comp));

    print_stats();
}


// tell the source how late we play, so it can sync video (AVDTP 1.3 delay reporting)
static void send_delay_report(void) {
    if (_delay_reported || !_cid) return;

    avdtp_stream_endpoint_t * endpoint = avdtp_get_stream_endpoint_for_seid(_seid);
    if (!endpoint || !(endpoint->remote_sep.configured_service_categories & (1 << AVDTP_DELAY_REPORTING))) return;

    uint16_t delay_100us = (A2DP_LATENCY_MS + I2S_LATENCY_MS) * 10;
    if (a2dp_sink_delay_report(_cid, _seid, delay_100us) == ERROR_CODE_SUCCESS) {
        _delay_reported = true;
    }
}


static void event_handler(uint8_t event, uint8_t *packet) {
    switch (event) {
        case A2DP_SUBEVENT_SIGNALING_CONNECTION_ESTABLISHED:
            if (a2dp_subevent_signaling_connection_established_get_status(packet) != ERROR_CODE_SUCCESS) break;
            _cid = a2dp_subevent_signaling_connection_established_get_a2dp_cid(packet);
            a2dp_subevent_signaling_connection_established_get_bd_addr(packet, _addr);
            // stable format: parsed by Remote_RxV1600 (receiver ESP) from the uart log
            printf("A2DP: connected to %s\n", bd_addr_to_str(_addr));
            if (_cb) (*_cb)(true, _addr);
            break;

        case A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_SBC_CONFIGURATION:
            _sampling_frequency = a2dp_subevent_signaling_media_codec_sbc_configuration_get_sampling_frequency(packet);
            _num_channels = a2dp_subevent_signaling_media_codec_sbc_configuration_get_num_channels(packet);
            printf("A2DP: SBC %u Hz, %u channels, bitpool %u-%u\n", _sampling_frequency, _num_channels,
                a2dp_subevent_signaling_media_codec_sbc_configuration_get_min_bitpool_value(packet),
                a2dp_subevent_signaling_media_codec_sbc_configuration_get_max_bitpool_value(packet));
            break;

        case A2DP_SUBEVENT_STREAM_ESTABLISHED:
            if (a2dp_subevent_stream_established_get_status(packet) != ERROR_CODE_SUCCESS) break;
            send_delay_report();
            break;

        case A2DP_SUBEVENT_STREAM_STARTED:
            // sample rate may have changed with a (re)configuration
            if (_media_initialized && _media_sampling_frequency != _sampling_frequency) {
                media_processing_close();
            }
            media_processing_init();
            _streaming = true;
            send_delay_report();  // in case it could not be sent before
            break;

        case A2DP_SUBEVENT_STREAM_SUSPENDED:
            _streaming = false;
            media_processing_pause();
            volume_save();
            break;

        case A2DP_SUBEVENT_STREAM_RELEASED:
            _streaming = false;
            _delay_reported = false;
            media_processing_close();
            volume_save();
            break;

        case A2DP_SUBEVENT_SIGNALING_CONNECTION_RELEASED:
            _streaming = false;
            _delay_reported = false;
            media_processing_close();
            _cid = 0;
            printf("A2DP: disconnected\n");
            if (_cb) (*_cb)(false, _addr);
            break;

        default:
            break;
    }
}


static void data_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    UNUSED(channel);
    UNUSED(size);

    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_A2DP_META) return;

    event_handler(packet[2], packet);
}


// media packet: rtp header, sbc header, sbc frames
static void media_handler(uint8_t seid, uint8_t *packet, uint16_t size) {
    UNUSED(seid);

    if (!_media_initialized) return;

    // rtp header: 12 bytes plus csrc list, optional extension and padding
    if (size < 13) return;
    unsigned pos = 12 + 4 * (packet[0] & 0x0F);
    unsigned end = size;
    if (packet[0] & 0x10) {
        if (pos + 4 > end) return;
        pos += 4 + 4 * big_endian_read_16(packet, pos + 2);
    }
    if (packet[0] & 0x20) {
        unsigned padding = packet[size - 1];
        if (padding > end) return;
        end -= padding;
    }
    if (pos >= end) return;

    // sbc header: fragmentation is only used if a frame does not fit into a packet (never seen)
    uint8_t sbc_header = packet[pos++];
    if (sbc_header & 0x80) {
        _bad_frames++;
        return;
    }

    unsigned num_frames = sbc_header & 0x0F;
    while (num_frames-- && pos < end) {
        unsigned samples;
        unsigned length = sbc_frame_length(packet + pos, end - pos, &samples);
        if (!length) {
            _bad_frames++;
            break;
        }
        sbc_frame_write(packet + pos, length, samples);
        pos += length;
    }

    regulate_buffer();
}


bool a2dp_is_streaming() {
    return _streaming;
}


void a2dp_sink_begin( a2dp_connection_cb_t cb ) {
    _cb = cb;

    // Init I2S interface
    btstack_audio_sink_set_instance(btstack_audio_pico_sink_get_instance());

    a2dp_sink_init();

    a2dp_sink_register_packet_handler(&data_handler);
    a2dp_sink_register_media_handler(&media_handler);

    avdtp_stream_endpoint_t * endpoint = a2dp_sink_create_stream_endpoint(AVDTP_AUDIO, AVDTP_CODEC_SBC,
        _sbc_capabilities, sizeof(_sbc_capabilities),
        _sbc_configuration, sizeof(_sbc_configuration));
    btstack_assert(endpoint != NULL);
    _seid = avdtp_local_seid(endpoint);
}
