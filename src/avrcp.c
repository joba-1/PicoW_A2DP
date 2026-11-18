#include "avrcp.h"
#include <stdio.h> // Remove this when done printing text to the console
#include <inttypes.h>
#include "volume.h"
#include <math.h>

#ifndef VOLUME_ABSOLUTE
#define VOLUME_ABSOLUTE 1
#endif

#define VOLUME_STEP 8                 // 16 steps from 0 to 127 like most phones
#define CONNECT_DELAY_MS 2000

static uint16_t _cid = 0;
static bd_addr_t _addr;
static btstack_timer_source_t _connect_timer;

// Wait for the handler to finish sending track info before getting the song length
static bool play_status_pending;

static void request_play_status(void) {
    if (_cid == 0 || play_status_pending) return;

    uint8_t play_status_response_code = avrcp_controller_get_play_status(_cid);

    if (play_status_response_code == ERROR_CODE_SUCCESS) {
        play_status_pending = true;
    }
}

// Convert milliseconds into minutes and seconds
struct mediaObject_milliseconds {
    unsigned int minutes, seconds;
};

typedef struct mediaObject_milliseconds Struct;
Struct convert_milliseconds(unsigned long song_length) {
    Struct value;

    unsigned int total_seconds = floor(song_length / 1000);
    value.seconds = total_seconds % 60;
    value.minutes = total_seconds / 60;

    return value;
}

static void connection_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    UNUSED(channel);
    UNUSED(size);

    uint16_t cid;

    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_AVRCP_META) return;

    switch (packet[2]) {
        case AVRCP_SUBEVENT_CONNECTION_ESTABLISHED:
            cid = avrcp_subevent_connection_established_get_avrcp_cid(packet);
            if (avrcp_subevent_connection_established_get_status(packet) != ERROR_CODE_SUCCESS){
                _cid = 0;
                return;
            }

            _cid = cid;
            btstack_run_loop_remove_timer(&_connect_timer);

            avrcp_controller_enable_notification(cid, AVRCP_NOTIFICATION_EVENT_PLAYBACK_STATUS_CHANGED);
            avrcp_controller_enable_notification(cid, AVRCP_NOTIFICATION_EVENT_NOW_PLAYING_CONTENT_CHANGED);
            avrcp_controller_enable_notification(cid, AVRCP_NOTIFICATION_EVENT_TRACK_CHANGED);
            avrcp_controller_enable_notification(cid, AVRCP_NOTIFICATION_EVENT_PLAYBACK_POS_CHANGED);

            #if VOLUME_ABSOLUTE
                // source reads our volume when it registers for volume notifications
                avrcp_target_support_event(cid, AVRCP_NOTIFICATION_EVENT_VOLUME_CHANGED);
                avrcp_target_adjust_absolute_volume(cid, volume_get());
            #endif

            avrcp_target_support_event(cid, AVRCP_NOTIFICATION_EVENT_BATT_STATUS_CHANGED);
            avrcp_target_battery_status_changed(cid, AVRCP_BATTERY_STATUS_EXTERNAL);
            return;

        case AVRCP_SUBEVENT_CONNECTION_RELEASED:
            _cid = 0;
            return;

        default:
            break;
    }
}

static void target_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    UNUSED(channel);
    UNUSED(size);

    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_AVRCP_META) return;

    switch (packet[2]){
        case AVRCP_SUBEVENT_NOTIFICATION_VOLUME_CHANGED:
            // source set absolute volume
            volume_set(avrcp_subevent_notification_volume_changed_get_absolute_volume(packet));
            printf("Volume is %d\n", volume_get());
            break;

        case AVRCP_SUBEVENT_OPERATION:
            // volume buttons of a remote control
            if (!avrcp_subevent_operation_get_button_pressed(packet)) break;
            switch (avrcp_subevent_operation_get_operation_id(packet)) {
                case AVRCP_OPERATION_ID_VOLUME_UP:
                    volume_step(VOLUME_STEP);
                    break;
                case AVRCP_OPERATION_ID_VOLUME_DOWN:
                    volume_step(-VOLUME_STEP);
                    break;
                default:
                    return;
            }
            if (_cid) avrcp_target_volume_changed(_cid, volume_get());
            break;

        default:
            break;
    }
}

// Controller Events
static void controller_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    UNUSED(channel);
    UNUSED(size);

    uint16_t cid;
    uint8_t avrcp_subevent_value[256];

    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_AVRCP_META) return;

    memset(avrcp_subevent_value, 0, sizeof(avrcp_subevent_value));

    switch (packet[2]) {
        case AVRCP_SUBEVENT_CONNECTION_ESTABLISHED:
            cid = avrcp_subevent_connection_established_get_avrcp_cid(packet);

            if (avrcp_subevent_connection_established_get_status(packet) != ERROR_CODE_SUCCESS){
                _cid = 0;
                return;
            }

            _cid = cid;
            btstack_run_loop_remove_timer(&_connect_timer);
            return;

        case AVRCP_SUBEVENT_NOTIFICATION_TRACK_CHANGED:
            request_play_status();
            break;

        case AVRCP_SUBEVENT_NOTIFICATION_PLAYBACK_STATUS_CHANGED:
            // Returns 1 for playing, 2 for paused
            if (avrcp_subevent_notification_playback_status_changed_get_play_status(packet) == 1) {
                printf("Playback status: Playing\n");
            }
            else if (avrcp_subevent_notification_playback_status_changed_get_play_status(packet) == 2) {
                printf("Playback status: Paused\n");
            }
            break;

        case AVRCP_SUBEVENT_NOW_PLAYING_TITLE_INFO:
            if (avrcp_subevent_now_playing_title_info_get_value_len(packet) > 0){
                memcpy(avrcp_subevent_value, avrcp_subevent_now_playing_title_info_get_value(packet), avrcp_subevent_now_playing_title_info_get_value_len(packet));
                printf("Track Title: %s\n", avrcp_subevent_value);
            }
            break;

        case AVRCP_SUBEVENT_NOW_PLAYING_ARTIST_INFO:
            if (avrcp_subevent_now_playing_artist_info_get_value_len(packet) > 0){
                memcpy(avrcp_subevent_value, avrcp_subevent_now_playing_artist_info_get_value(packet), avrcp_subevent_now_playing_artist_info_get_value_len(packet));
                printf("Artist: %s\n", avrcp_subevent_value);
            }
            break;

        case AVRCP_SUBEVENT_NOW_PLAYING_ALBUM_INFO:
            if (avrcp_subevent_now_playing_album_info_get_value_len(packet) > 0){
                memcpy(avrcp_subevent_value, avrcp_subevent_now_playing_album_info_get_value(packet), avrcp_subevent_now_playing_album_info_get_value_len(packet));
                printf("Album: %s\n", avrcp_subevent_value);
            }
            break;

        case AVRCP_SUBEVENT_NOTIFICATION_PLAYBACK_POS_CHANGED:
            Struct playback_result;
            playback_result = convert_milliseconds(avrcp_subevent_notification_playback_pos_changed_get_playback_position_ms(packet));
            printf("Playback position: %02d:%02d\n", playback_result.minutes, playback_result.seconds);
            break;

        case AVRCP_SUBEVENT_PLAY_STATUS:
            play_status_pending = false;

            Struct play_status_result;
            play_status_result = convert_milliseconds(avrcp_subevent_play_status_get_song_length(packet));
            printf("Song length: %02d:%02d\n", play_status_result.minutes, play_status_result.seconds);

            // Start the metadata request only after the play-status reply
            avrcp_controller_get_now_playing_info(_cid);
            break;

        case AVRCP_SUBEVENT_CONNECTION_RELEASED:
            _cid = 0;
            return;

        default:
            break;
    }
}

static void connect_timeout( btstack_timer_source_t *ts ) {
    UNUSED(ts);
    if (_cid) return;

    uint16_t cid;
    avrcp_connect(_addr, &cid);
}

void avrcp_connect_later( bd_addr_t addr ) {
    memcpy(_addr, addr, sizeof(bd_addr_t));
    btstack_run_loop_remove_timer(&_connect_timer);
    btstack_run_loop_set_timer_handler(&_connect_timer, connect_timeout);
    btstack_run_loop_set_timer(&_connect_timer, CONNECT_DELAY_MS);
    btstack_run_loop_add_timer(&_connect_timer);
}

void avrcp_begin() {
    avrcp_init();
    avrcp_controller_init();
    avrcp_target_init();

    avrcp_register_packet_handler(connection_handler);
    avrcp_controller_register_packet_handler(controller_handler);
    avrcp_target_register_packet_handler(target_handler);
}

bool avrcp_is_connected() {
    return _cid != 0;
}
