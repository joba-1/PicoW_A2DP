#include "a2dp.h"

#include <btstack.h>
#include <btstack_resample.h>
#include <classic/a2dp_sink.h>
#include "hardware/watchdog.h"

// for connection led 
#include <pico/cyw43_arch.h>

#include "avrcp.h"

// from btstack_audio_pico.c
const btstack_audio_sink_t * btstack_audio_pico_sink_get_instance(void);


#define OPTIMAL_FRAMES_MIN 60
#define OPTIMAL_FRAMES_MAX 120
#define ADDITIONAL_FRAMES  30
#define NUM_CHANNELS       2
#define BYTES_PER_FRAME    (2*NUM_CHANNELS)
#define MAX_SBC_FRAME_SIZE 120


typedef struct {
    uint8_t  reconfigure;
    uint8_t  num_channels;
    uint16_t sampling_frequency;
    uint8_t  block_length;
    uint8_t  subbands;
    uint8_t  min_bitpool_value;
    uint8_t  max_bitpool_value;
    btstack_sbc_channel_mode_t      channel_mode;
    btstack_sbc_allocation_method_t allocation_method;
} sbc_configuration_t;


typedef enum {
    STREAM_STATE_CLOSED,
    STREAM_STATE_OPEN,
    STREAM_STATE_PLAYING,
    STREAM_STATE_PAUSED,
} stream_state_t;


// all configurations with bitpool 2-53 are supported
static const uint8_t _sbc_capabilities[] = {
    0xFF,  // (AVDTP_SBC_44100 << 4) | AVDTP_SBC_STEREO,
    0xFF,  // (AVDTP_SBC_BLOCK_LENGTH_16 << 4) | (AVDTP_SBC_SUBBANDS_8 << 2) | AVDTP_SBC_ALLOCATION_METHOD_LOUDNESS
    2, 53
};
uint8_t _seid = 0;
stream_state_t _stream_state = STREAM_STATE_CLOSED;
sbc_configuration_t _sbc_configuration = {0};
btstack_sbc_decoder_state_t _state = {0};
bool _media_initialized = false;
bool _audio_stream_started = false;
unsigned _sbc_frame_size = 0;
btstack_resample_t _resample_instance = {0};
btstack_ring_buffer_t _sbc_frame_ring_buffer = {0};
btstack_ring_buffer_t _decoded_audio_ring_buffer = {0};
uint8_t _sbc_frame_storage[(OPTIMAL_FRAMES_MAX + ADDITIONAL_FRAMES) * MAX_SBC_FRAME_SIZE] = {0};
uint8_t _decoded_audio_storage[(128+16) * BYTES_PER_FRAME] = {0};
int16_t * _request_buffer = 0;
int _request_frames = 0;


// process volume on decoded frames and send to i2s buffer or ringbuffer
static void handle_pcm_data(int16_t * data, int num_audio_frames, int num_channels, int sample_rate, void * context) {
    UNUSED(sample_rate);
    UNUSED(context);
    UNUSED(num_channels);   // must be stereo == 2

    const btstack_audio_sink_t * audio_sink = btstack_audio_sink_get_instance();
    if (!audio_sink){
        return;
    }

    // adjust volume
    int32_t volume = 1L + avrcp_get_volume();  // 1..128
    int32_t samples = num_audio_frames * NUM_CHANNELS;
    int32_t sample;
    for( size_t i=0; i<samples; ++i ) {
        sample = (volume * data[i]) >> 7;
        if( sample < INT16_MIN) {
            data[i] = INT16_MIN;
        } 
        else if( sample > INT16_MAX) {
            data[i] = INT16_MAX;
        } 
        else {
            data[i] = sample;
        } 
    }

    // resample into request buffer - add some additional space for resampling
    int16_t  output_buffer[(128+16) * NUM_CHANNELS]; // 16 * 8 * 2
    uint32_t resampled_frames = btstack_resample_block(&_resample_instance, data, num_audio_frames, output_buffer);

    // store data in btstack_audio buffer first
    int frames_to_copy = btstack_min(resampled_frames, _request_frames);
    memcpy(_request_buffer, output_buffer, frames_to_copy * BYTES_PER_FRAME);
    _request_frames -= frames_to_copy;
    _request_buffer += frames_to_copy * NUM_CHANNELS;

    // and rest in ring buffer
    int frames_to_store = resampled_frames - frames_to_copy;
    if (frames_to_store) {
        int status = btstack_ring_buffer_write(&_decoded_audio_ring_buffer, (uint8_t *)&output_buffer[frames_to_copy * NUM_CHANNELS], frames_to_store * BYTES_PER_FRAME);
        // if (status){
        //     printf("Error storing samples in PCM ring buffer!!!\n");
        // }
    }
}


/// provide pcm frames to i2s sink
static void playback_handler(int16_t * buffer, uint16_t num_audio_frames) {

    // called from lower-layer but guaranteed to be on main thread
    if (_sbc_frame_size == 0){
        memset(buffer, 0, num_audio_frames * BYTES_PER_FRAME);
        return;
    }

    // first fill from resampled audio
    uint32_t bytes_read;
    btstack_ring_buffer_read(&_decoded_audio_ring_buffer, (uint8_t *) buffer, num_audio_frames * BYTES_PER_FRAME, &bytes_read);
    buffer += bytes_read / NUM_CHANNELS;
    num_audio_frames -= bytes_read / BYTES_PER_FRAME;

    // then start decoding sbc frames using request_* globals
    _request_buffer = buffer;
    _request_frames = num_audio_frames;
    while (_request_frames && btstack_ring_buffer_bytes_available(&_sbc_frame_ring_buffer) >= _sbc_frame_size) {
        // decode frame
        uint8_t sbc_frame[MAX_SBC_FRAME_SIZE];
        btstack_ring_buffer_read(&_sbc_frame_ring_buffer, sbc_frame, _sbc_frame_size, &bytes_read);
        btstack_sbc_decoder_process_data(&_state, 0, sbc_frame, _sbc_frame_size);
    }
}


static void media_processing_init(sbc_configuration_t * configuration) {
    if (_media_initialized) return;

    btstack_sbc_decoder_init(&_state, SBC_MODE_STANDARD, handle_pcm_data, NULL);

    btstack_ring_buffer_init(&_sbc_frame_ring_buffer, _sbc_frame_storage, sizeof(_sbc_frame_storage));
    btstack_ring_buffer_init(&_decoded_audio_ring_buffer, _decoded_audio_storage, sizeof(_decoded_audio_storage));
    btstack_resample_init(&_resample_instance, configuration->num_channels);

    // setup audio playback
    const btstack_audio_sink_t * audio = btstack_audio_sink_get_instance();
    if (audio){
        audio->init(NUM_CHANNELS, configuration->sampling_frequency, &playback_handler);
    }

    _audio_stream_started = false;
    _media_initialized = true;
    return;
}


static void media_processing_start(void) {
    if (!_media_initialized) return;

    // setup audio playback
    const btstack_audio_sink_t * audio = btstack_audio_sink_get_instance();
    if (audio){
        audio->start_stream();
    }
    _audio_stream_started = true;
}


static void media_processing_pause(void) {
    if (!_media_initialized) return;

    // stop audio playback
    _audio_stream_started = false;

    const btstack_audio_sink_t * audio = btstack_audio_sink_get_instance();
    if (audio) {
        audio->stop_stream();
    }
    // discard pending data
    btstack_ring_buffer_reset(&_decoded_audio_ring_buffer);
    btstack_ring_buffer_reset(&_sbc_frame_ring_buffer);
}


static void media_processing_close(void) {
    if (!_media_initialized) return;

    _media_initialized = false;
    _audio_stream_started = false;
    _sbc_frame_size = 0;

    // stop audio playback
    const btstack_audio_sink_t * audio = btstack_audio_sink_get_instance();
    if (audio){
        // printf("close stream\n");
        audio->close();
    }
}


static void event_handler(uint8_t event, uint8_t *packet) {
    uint8_t status;
    uint8_t allocation_method;

    switch (event){
        // case A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_OTHER_CONFIGURATION:
        //     printf("A2DP  Sink      : Received non SBC codec - not implemented\n");
        //     break;

        case A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_SBC_CONFIGURATION:{
            // printf("A2DP  Sink      : Received SBC codec configuration\n");
            _sbc_configuration.reconfigure = a2dp_subevent_signaling_media_codec_sbc_configuration_get_reconfigure(packet);
            _sbc_configuration.num_channels = a2dp_subevent_signaling_media_codec_sbc_configuration_get_num_channels(packet);
            _sbc_configuration.sampling_frequency = a2dp_subevent_signaling_media_codec_sbc_configuration_get_sampling_frequency(packet);
            _sbc_configuration.block_length = a2dp_subevent_signaling_media_codec_sbc_configuration_get_block_length(packet);
            _sbc_configuration.subbands = a2dp_subevent_signaling_media_codec_sbc_configuration_get_subbands(packet);
            _sbc_configuration.min_bitpool_value = a2dp_subevent_signaling_media_codec_sbc_configuration_get_min_bitpool_value(packet);
            _sbc_configuration.max_bitpool_value = a2dp_subevent_signaling_media_codec_sbc_configuration_get_max_bitpool_value(packet);
            
            allocation_method = a2dp_subevent_signaling_media_codec_sbc_configuration_get_allocation_method(packet);
            
            // Adapt Bluetooth spec definition to SBC Encoder expected input
            _sbc_configuration.allocation_method = (btstack_sbc_allocation_method_t)(allocation_method - 1);
           
            switch (a2dp_subevent_signaling_media_codec_sbc_configuration_get_channel_mode(packet)) {
                case AVDTP_CHANNEL_MODE_JOINT_STEREO:
                    _sbc_configuration.channel_mode = SBC_CHANNEL_MODE_JOINT_STEREO;
                    break;
                case AVDTP_CHANNEL_MODE_STEREO:
                    _sbc_configuration.channel_mode = SBC_CHANNEL_MODE_STEREO;
                    break;
                case AVDTP_CHANNEL_MODE_DUAL_CHANNEL:
                    _sbc_configuration.channel_mode = SBC_CHANNEL_MODE_DUAL_CHANNEL;
                    break;
                case AVDTP_CHANNEL_MODE_MONO:
                    _sbc_configuration.channel_mode = SBC_CHANNEL_MODE_MONO;
                    break;
                default:
                    btstack_assert(false);
                    break;
            }
            // dump_sbc_configuration(&_a2dp->_sbc_configuration);
            break;
        }

        case A2DP_SUBEVENT_STREAM_ESTABLISHED:
            status = a2dp_subevent_stream_established_get_status(packet);
            if (status != ERROR_CODE_SUCCESS){
                // printf("A2DP  Sink      : Streaming connection failed, status 0x%02x\n", status);
                break;
            }

            // a2dp_subevent_stream_established_get_bd_addr(packet, _addr);
            // _cid = a2dp_subevent_stream_established_get_a2dp_cid(packet);
            _seid = a2dp_subevent_stream_established_get_local_seid(packet);
            _stream_state = STREAM_STATE_OPEN;
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, true);
            gpio_put(CONN_PIN, 1);

            // printf("A2DP  Sink      : Streaming connection is established, address %s, cid 0x%02x, local seid %d\n",
            //        bd_addr_to_str(_a2dp->addr), _a2dp->a2dp_cid, _a2dp->a2dp_local_seid);
            break;
        
        case A2DP_SUBEVENT_STREAM_STARTED:
            // printf("A2DP  Sink      : Stream started\n");
            _stream_state = STREAM_STATE_PLAYING;
            if (_sbc_configuration.reconfigure){
                media_processing_close();
            }
            // prepare media processing
            media_processing_init(&_sbc_configuration);
            // audio stream is started when buffer reaches minimal level
            break;
        
        case A2DP_SUBEVENT_STREAM_SUSPENDED:
            // printf("A2DP  Sink      : Stream paused\n");
            _stream_state = STREAM_STATE_PAUSED;
            media_processing_pause();
            break;
        
        case A2DP_SUBEVENT_STREAM_RELEASED:
            // printf("A2DP  Sink      : Stream released\n");
            _stream_state = STREAM_STATE_CLOSED;
            media_processing_close();
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, false);
            gpio_put(CONN_PIN, 0);
            watchdog_enable(100, true);  // reboot in 0.1s, since reconnect is buggy
            break;
        
        case A2DP_SUBEVENT_SIGNALING_CONNECTION_RELEASED:
            // printf("A2DP  Sink      : Signaling connection released\n");
            // _cid = 0;
            // _stream_state = STREAM_STATE_CLOSED;
            // media_processing_close();
            // cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, false);
            // gpio_put(CONN_PIN, 0);
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


int read_media_header(uint8_t *packet, int size, int *offset, avdtp_media_packet_header_t *media_header) {
    int media_header_len = 12; // without crc
    int pos = *offset;
    
    if (size - pos < media_header_len) {
        return 0;
    }

    media_header->version = packet[pos] & 0x03;
    media_header->padding = get_bit16(packet[pos],2);
    media_header->extension = get_bit16(packet[pos],3);
    media_header->csrc_count = (packet[pos] >> 4) & 0x0F;
    pos++;

    media_header->marker = get_bit16(packet[pos],0);
    media_header->payload_type  = (packet[pos] >> 1) & 0x7F;
    pos++;

    media_header->sequence_number = big_endian_read_16(packet, pos);
    pos+=2;

    media_header->timestamp = big_endian_read_32(packet, pos);
    pos+=4;

    media_header->synchronization_source = big_endian_read_32(packet, pos);
    pos+=4;
    *offset = pos;

    return 1;
}


int read_sbc_header(uint8_t * packet, int size, int * offset, avdtp_sbc_codec_header_t * sbc_header) {
    int sbc_header_len = 12; // without crc
    int pos = *offset;
    
    if (size - pos < sbc_header_len) {
        return 0;
    }

    sbc_header->fragmentation = get_bit16(packet[pos], 7);
    sbc_header->starting_packet = get_bit16(packet[pos], 6);
    sbc_header->last_packet = get_bit16(packet[pos], 5);
    sbc_header->num_frames = packet[pos] & 0x0f;
    pos++;
    *offset = pos;

    return 1;
}


static void media_handler(uint8_t seid, uint8_t *packet, uint16_t size) {
    UNUSED(seid);

    int pos = 0;
     
    avdtp_media_packet_header_t media_header;
    if (!read_media_header(packet, size, &pos, &media_header)) return;
    
    avdtp_sbc_codec_header_t sbc_header;
    if (!read_sbc_header(packet, size, &pos, &sbc_header)) return;

    int packet_length = size-pos;
    uint8_t *packet_begin = packet + pos;

    // store sbc frame size for buffer management
    _sbc_frame_size = packet_length / sbc_header.num_frames;
    int status = btstack_ring_buffer_write(&_sbc_frame_ring_buffer, packet_begin, packet_length);
    // if (status != ERROR_CODE_SUCCESS){
    //     printf("Error storing samples in SBC ring buffer!!!\n");
    // }

    // decide on audio sync drift based on number of sbc frames in queue
    int sbc_frames_in_buffer = btstack_ring_buffer_bytes_available(&_sbc_frame_ring_buffer) / _sbc_frame_size;

    uint32_t resampling_factor;

    // nominal factor (fixed-point 2^16) and compensation offset
    uint32_t nominal_factor = 0x10000;
    uint32_t compensation   = 0x00100;

    if (sbc_frames_in_buffer < OPTIMAL_FRAMES_MIN){
    	resampling_factor = nominal_factor - compensation;    // stretch samples
    } else if (sbc_frames_in_buffer <= OPTIMAL_FRAMES_MAX){
    	resampling_factor = nominal_factor;                   // nothing to do
    } else {
    	resampling_factor = nominal_factor + compensation;    // compress samples
    }

    btstack_resample_set_factor(&_resample_instance, resampling_factor);

    // start stream if enough frames buffered
    if (!_audio_stream_started && sbc_frames_in_buffer >= (OPTIMAL_FRAMES_MIN+OPTIMAL_FRAMES_MAX)/2){
        media_processing_start();
    }
}


void a2dp_sink_begin() {
    // Init I2S interface
    btstack_audio_sink_set_instance(btstack_audio_pico_sink_get_instance());

    // Init connection indicator pin (high if bt a2dp stream connected)
    gpio_init(CONN_PIN);
    gpio_set_dir(CONN_PIN, GPIO_OUT);
    gpio_put(CONN_PIN, 0);  // set to 1 while a bt connection is active

    a2dp_sink_init();

    a2dp_sink_register_packet_handler(&data_handler);
    a2dp_sink_register_media_handler(&media_handler);

    uint8_t sbc_configuration[4];
    avdtp_stream_endpoint_t * endpoint = a2dp_sink_create_stream_endpoint(AVDTP_AUDIO, AVDTP_CODEC_SBC, 
        _sbc_capabilities, sizeof(_sbc_capabilities),
        sbc_configuration, sizeof(sbc_configuration));
    _seid = avdtp_local_seid(endpoint);
}
