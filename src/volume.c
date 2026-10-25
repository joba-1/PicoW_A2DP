#include "volume.h"

#include <btstack.h>
#include <btstack_tlv.h>

#include "a2dp.h"

#ifndef VOLUME_DEFAULT
#define VOLUME_DEFAULT 100
#endif

#define VOLUME_MAX 127
#define VOLUME_TLV_TAG (((uint32_t)'V' << 24) | ((uint32_t)'O' << 16) | ((uint32_t)'L' << 8) | '1')
#define VOLUME_SAVE_DELAY_MS 3000


static uint8_t _volume = VOLUME_DEFAULT;
static uint8_t _stored = VOLUME_DEFAULT;
static btstack_timer_source_t _save_timer;


static void apply() {
    const btstack_audio_sink_t * audio = btstack_audio_sink_get_instance();
    if (audio) {
        audio->set_volume(_volume);
    }
}


// store a changed volume later, but only if no stream is playing (a2dp also saves when a stream stops)
static void save_later( btstack_timer_source_t *ts ) {
    UNUSED(ts);
    if (!a2dp_is_streaming()) {
        volume_save();
    }
}


void volume_begin() {
    const btstack_tlv_t * tlv_impl;
    void * tlv_context;
    btstack_tlv_get_instance(&tlv_impl, &tlv_context);

    uint8_t stored;
    if (tlv_impl && tlv_impl->get_tag(tlv_context, VOLUME_TLV_TAG, &stored, sizeof(stored)) == sizeof(stored) && stored <= VOLUME_MAX) {
        _volume = stored;
    }
    _stored = _volume;

    btstack_run_loop_set_timer_handler(&_save_timer, save_later);
    apply();
}


uint8_t volume_get() {
    return _volume;
}


void volume_set( uint8_t volume ) {
    if (volume > VOLUME_MAX) volume = VOLUME_MAX;
    if (volume == _volume) return;

    _volume = volume;
    apply();

    btstack_run_loop_remove_timer(&_save_timer);
    btstack_run_loop_set_timer(&_save_timer, VOLUME_SAVE_DELAY_MS);
    btstack_run_loop_add_timer(&_save_timer);
}


void volume_step( int delta ) {
    int volume = (int)_volume + delta;
    if (volume < 0) volume = 0;
    if (volume > VOLUME_MAX) volume = VOLUME_MAX;
    volume_set((uint8_t)volume);
}


void volume_save() {
    if (_volume == _stored) return;

    const btstack_tlv_t * tlv_impl;
    void * tlv_context;
    btstack_tlv_get_instance(&tlv_impl, &tlv_context);
    if (tlv_impl && tlv_impl->store_tag(tlv_context, VOLUME_TLV_TAG, &_volume, sizeof(_volume)) == 0) {
        _stored = _volume;
    }
}
