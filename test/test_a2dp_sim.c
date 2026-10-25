// Host simulation of the a2dp receive path: src/a2dp.c with btstack's real sbc codec, ring buffer
// and resampler. A simulated source sends sbc packets with jitter, stalls and a clock offset,
// a simulated i2s output pulls audio with its own clock offset.
// Checks: playback starts, no gaps, buffer level settles near A2DP_LATENCY_MS, reconfiguration works.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <btstack.h>
#include <classic/btstack_sbc_bluedroid.h>

#include "a2dp.h"

#define MS_PER_TICK 1

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)


// ---- stubs for btstack a2dp, avdtp, run loop and volume ----

static btstack_packet_handler_t _data_handler;
static void (*_media_handler)(uint8_t seid, uint8_t *packet, uint16_t size);
static avdtp_stream_endpoint_t _endpoint;
static uint32_t _now_ms = 0;
static int _delay_reports = 0;
static uint16_t _last_delay = 0;

void a2dp_sink_init(void) {}
void a2dp_sink_register_packet_handler(btstack_packet_handler_t callback) { _data_handler = callback; }
void a2dp_sink_register_media_handler(void (*callback)(uint8_t local_seid, uint8_t *packet, uint16_t size)) { _media_handler = callback; }
avdtp_stream_endpoint_t * a2dp_sink_create_stream_endpoint(avdtp_media_type_t media_type, avdtp_media_codec_type_t media_codec_type,
    const uint8_t *codec_capabilities, uint16_t codec_capabilities_len, uint8_t * codec_configuration, uint16_t codec_configuration_len) {
    return &_endpoint;
}
uint8_t avdtp_local_seid(const avdtp_stream_endpoint_t * stream_endpoint) { return 1; }
avdtp_stream_endpoint_t * avdtp_get_stream_endpoint_for_seid(uint16_t seid) { return seid == 1 ? &_endpoint : NULL; }
uint8_t a2dp_sink_delay_report(uint16_t a2dp_cid, uint8_t local_seid, uint16_t delay_100us) { _delay_reports++; _last_delay = delay_100us; return ERROR_CODE_SUCCESS; }
uint32_t btstack_run_loop_get_time_ms(void) { return _now_ms; }
void volume_save(void) {}


// ---- simulated i2s output ----

static void (*_playback)(int16_t * buffer, uint16_t num_samples, const btstack_audio_context_t * timeinfo);
static bool _sink_running = false;
static uint32_t _sink_rate = 0;
static int _sink_inits = 0;

static int sink_init(uint8_t channels, uint32_t samplerate, void (*playback)(int16_t *, uint16_t, const btstack_audio_context_t *)) {
    _playback = playback;
    _sink_rate = samplerate;
    _sink_inits++;
    return 0;
}
static uint32_t sink_get_samplerate(void) { return _sink_rate; }
static void sink_set_volume(uint8_t volume) {}
static void sink_start(void) { _sink_running = true; }
static void sink_stop(void) { _sink_running = false; }
static void sink_close(void) { _sink_running = false; }

static const btstack_audio_sink_t _sink = {
    sink_init, sink_get_samplerate, sink_set_volume, sink_start, sink_stop, sink_close
};
const btstack_audio_sink_t * btstack_audio_pico_sink_get_instance(void) { return &_sink; }


// ---- events ----

static void send_event(uint8_t subevent, const uint8_t *fields, unsigned len) {
    uint8_t event[64] = { HCI_EVENT_A2DP_META, (uint8_t)(len + 1), subevent };
    memcpy(&event[3], fields, len);
    (*_data_handler)(HCI_EVENT_PACKET, 0, event, (uint16_t)(len + 3));
}

static void connect(void) {
    uint8_t f[12] = {0};  // cid(2) addr(6) handle(2) incoming(1) status(1)
    little_endian_store_16(f, 0, 0x41);
    send_event(A2DP_SUBEVENT_SIGNALING_CONNECTION_ESTABLISHED, f, sizeof(f));
}

static void configure(uint16_t rate, uint8_t channels) {
    uint8_t f[16] = {0};  // cid(2) local seid(1) remote seid(1) reconfigure(1) media type(1) rate(2) channel mode(1) channels(1) ...
    little_endian_store_16(f, 0, 0x41);
    little_endian_store_16(f, 6, rate);
    f[9] = channels;
    f[13] = 2;   // min bitpool
    f[14] = 53;  // max bitpool
    send_event(A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_SBC_CONFIGURATION, f, sizeof(f));
}

static void stream_event(uint8_t subevent) {
    uint8_t f[12] = {0};  // cid, ..., status 0
    little_endian_store_16(f, 0, 0x41);
    send_event(subevent, f, sizeof(f));
}


// ---- simulated source ----

typedef struct {
    uint16_t rate;
    btstack_sbc_channel_mode_t mode;
    uint8_t bitpool;
    double source_ppm;      // source clock offset
    double sink_ppm;        // i2s clock offset
    int jitter_ms;          // random extra delay per packet
    int stall_every_ms;     // radio gap every n ms (0: none)
    int stall_ms;
    int seconds;
    bool rtp_extras;        // use csrc, extension and padding in rtp header
} scenario_t;

#define FRAMES_PER_PACKET 7
#define MAX_PACKETS 64

typedef struct {
    uint32_t due_ms;
    uint16_t size;
    uint8_t data[FRAMES_PER_PACKET * 256 + 64];
} packet_t;

static packet_t _queue[MAX_PACKETS];
static int _queue_len = 0;

static void run(const char *name, const scenario_t *sc) {
    btstack_sbc_encoder_bluedroid_t enc_ctx;
    const btstack_sbc_encoder_t *enc = btstack_sbc_encoder_bluedroid_init_instance(&enc_ctx);
    enc->configure(&enc_ctx, SBC_MODE_STANDARD, 16, 8, SBC_ALLOCATION_METHOD_LOUDNESS, sc->rate, sc->bitpool, sc->mode);
    unsigned frame_len = 0;  // known after the first encode
    uint8_t channels = sc->mode == SBC_CHANNEL_MODE_MONO ? 1 : 2;

    configure(sc->rate, channels);
    stream_event(A2DP_SUBEVENT_STREAM_ESTABLISHED);
    stream_event(A2DP_SUBEVENT_STREAM_STARTED);

    double src_acc = 0, sink_acc = 0;
    double phase = 0;
    uint64_t src_samples = 0, out_samples = 0;
    uint64_t sent_samples = 0;
    uint32_t start_ms = 0;
    int gaps = 0, run_zero = 0;
    double sum_sq = 0; uint64_t sum_n = 0;
    double level_min = 1e9, level_max = 0, level_sum = 0; int level_n = 0;
    uint16_t seq = 0;
    int16_t pcm[128 * 2];
    int pcm_fill = 0;
    uint8_t frames[FRAMES_PER_PACKET][256];
    int nframes = 0;
    uint32_t t0 = _now_ms;
    _queue_len = 0;

    for (uint32_t t = 0; t < (uint32_t)sc->seconds * 1000; t += MS_PER_TICK, _now_ms += MS_PER_TICK) {
        // source produces samples at its clock
        src_acc += sc->rate * (1 + sc->source_ppm * 1e-6) / 1000.0 * MS_PER_TICK;
        while (src_acc >= 1) {
            src_acc -= 1;
            int16_t v = (int16_t)(16000 * sin(phase));
            phase += 2 * M_PI * 997.0 / sc->rate;
            if (channels == 1) pcm[pcm_fill++] = v;
            else { pcm[2*pcm_fill] = v; pcm[2*pcm_fill+1] = (int16_t)-v; pcm_fill++; }
            src_samples++;
            if (pcm_fill == 128) {
                enc->encode_signed_16(&enc_ctx, pcm, frames[nframes++]);
                frame_len = enc->sbc_buffer_length(&enc_ctx);
                pcm_fill = 0;
                if (nframes == FRAMES_PER_PACKET) {
                    packet_t *p = &_queue[_queue_len++];
                    unsigned pos = 0;
                    uint8_t csrc = sc->rtp_extras ? 1 : 0;
                    p->data[pos++] = (uint8_t)(0x80 | csrc | (sc->rtp_extras ? 0x30 : 0));
                    p->data[pos++] = 0x60;
                    big_endian_store_16(p->data, pos, seq++); pos += 2;
                    big_endian_store_32(p->data, pos, (uint32_t)sent_samples); pos += 4;
                    big_endian_store_32(p->data, pos, 1); pos += 4;
                    if (csrc) { big_endian_store_32(p->data, pos, 0x12345678); pos += 4; }
                    if (sc->rtp_extras) { big_endian_store_16(p->data, pos, 0xBEDE); big_endian_store_16(p->data, pos + 2, 1); pos += 4; big_endian_store_32(p->data, pos, 0); pos += 4; }
                    p->data[pos++] = FRAMES_PER_PACKET;
                    for (int i = 0; i < FRAMES_PER_PACKET; i++) { memcpy(&p->data[pos], frames[i], frame_len); pos += frame_len; }
                    if (sc->rtp_extras) { p->data[pos++] = 0; p->data[pos++] = 0; p->data[pos++] = 3; }
                    p->size = (uint16_t)pos;
                    p->due_ms = _now_ms + (sc->jitter_ms ? (uint32_t)(rand() % sc->jitter_ms) : 0);
                    // radio gap: everything sent in a stall window arrives at its end
                    if (sc->stall_every_ms && (t % sc->stall_every_ms) < (uint32_t)sc->stall_ms) {
                        p->due_ms = _now_ms + (sc->stall_ms - t % sc->stall_every_ms);
                    }
                    if (_queue_len > 1 && p->due_ms < _queue[_queue_len - 2].due_ms) p->due_ms = _queue[_queue_len - 2].due_ms;  // in order
                    sent_samples += FRAMES_PER_PACKET * 128;
                    nframes = 0;
                }
            }
        }

        // deliver due packets in order
        while (_queue_len && _queue[0].due_ms <= _now_ms) {
            (*_media_handler)(1, _queue[0].data, _queue[0].size);
            memmove(&_queue[0], &_queue[1], (size_t)(--_queue_len) * sizeof(packet_t));
        }

        // i2s pulls blocks of 512 at its clock
        if (_sink_running) {
            sink_acc += _sink_rate * (1 + sc->sink_ppm * 1e-6) / 1000.0 * MS_PER_TICK;
            while (sink_acc >= 512) {
                sink_acc -= 512;
                int16_t out[512 * 2];
                (*_playback)(out, 512, NULL);
                for (int i = 0; i < 512; i++) {
                    // i2s keeps running while buffering, audio starts with the first non silent sample
                    if (!start_ms && (out[2*i] || out[2*i+1])) start_ms = _now_ms;
                    if (!start_ms) continue;
                    out_samples++;
                    if (out[2*i] == 0 && out[2*i+1] == 0) {
                        if (++run_zero == 32 && _now_ms - start_ms > 1000) gaps++;
                    } else {
                        run_zero = 0;
                    }
                    if (_now_ms - start_ms > 1000) { sum_sq += (double)out[2*i] * out[2*i]; sum_n++; }
                }
            }
            // level in ms between what arrived and what was played (after settling)
            if (start_ms && _now_ms - start_ms > (uint32_t)sc->seconds * 500) {
                double level_ms = ((double)(sent_samples - (uint64_t)_queue_len * FRAMES_PER_PACKET * 128) - out_samples) * 1000.0 / sc->rate;
                if (level_ms < level_min) level_min = level_ms;
                if (level_ms > level_max) level_max = level_ms;
                level_sum += level_ms; level_n++;
            }
        }
    }

    double rms = sum_n ? sqrt(sum_sq / sum_n) : 0;
    double level_avg = level_n ? level_sum / level_n : 0;
    printf("%-28s start %4u ms, gaps %d, rms %.0f, level avg %.0f ms (min %.0f, max %.0f)\n",
        name, start_ms - t0, gaps, rms, level_avg, level_min, level_max);

    CHECK(start_ms, "%s: playback never started", name);
    CHECK(gaps == 0, "%s: %d gaps", name, gaps);
    CHECK(fabs(rms - 16000 / sqrt(2)) < 1500, "%s: rms %.0f", name, rms);
    // level includes up to one packet in flight
    CHECK(level_avg > A2DP_LATENCY_MS * 0.7 && level_avg < A2DP_LATENCY_MS * 1.3 + 25, "%s: level %.0f ms", name, level_avg);

    stream_event(A2DP_SUBEVENT_STREAM_SUSPENDED);
}


int main(void) {
    srand(1);
    _endpoint.remote_sep.configured_service_categories = 1 << AVDTP_DELAY_REPORTING;

    a2dp_sink_begin(NULL);
    connect();

    scenario_t base = { 44100, SBC_CHANNEL_MODE_JOINT_STEREO, 53, 0, 0, 30, 0, 0, 60, false };

    run("44.1k joint stereo", &base);
    CHECK(_delay_reports == 1 && _last_delay == (A2DP_LATENCY_MS + 46) * 10, "delay report %d %u", _delay_reports, _last_delay);

    scenario_t drift = base; drift.source_ppm = 120; drift.sink_ppm = -80;
    stream_event(A2DP_SUBEVENT_STREAM_STARTED);
    run("drift +200 ppm", &drift);

    scenario_t drift2 = base; drift2.source_ppm = -150; drift2.sink_ppm = 100;
    stream_event(A2DP_SUBEVENT_STREAM_STARTED);
    run("drift -250 ppm", &drift2);

    scenario_t stall = base; stall.stall_every_ms = 5000; stall.stall_ms = 120;
    stream_event(A2DP_SUBEVENT_STREAM_STARTED);
    run("120 ms radio gaps", &stall);

    scenario_t extras = base; extras.rtp_extras = true;
    stream_event(A2DP_SUBEVENT_STREAM_STARTED);
    run("rtp csrc/ext/padding", &extras);

    // new configuration after release: 48k dual channel (pipewire sbc-xq like), max frame size
    stream_event(A2DP_SUBEVENT_STREAM_RELEASED);
    int inits = _sink_inits;
    scenario_t xq = base; xq.rate = 48000; xq.mode = SBC_CHANNEL_MODE_DUAL_CHANNEL; xq.source_ppm = 50;
    run("48k dual channel bitpool 53", &xq);
    CHECK(_sink_inits == inits + 1 && _sink_rate == 48000, "sink not re-initialized for 48k");

    stream_event(A2DP_SUBEVENT_STREAM_RELEASED);
    scenario_t mono = base; mono.rate = 32000; mono.mode = SBC_CHANNEL_MODE_MONO; mono.bitpool = 35;
    run("32k mono", &mono);

    if (failures) {
        printf("%d failures\n", failures);
        return 1;
    }
    printf("all simulations passed\n");
    return 0;
}
