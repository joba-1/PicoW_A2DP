#include "avrcp.h"


static uint16_t _cid = 0;
static bool _playing = false;
static uint8_t _volume = 0;


static void avrcp_volume_changed(uint8_t volume){
    const btstack_audio_sink_t * audio = btstack_audio_sink_get_instance();
    if (audio){
        audio->set_volume(volume);
    }
}


static void connection_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    UNUSED(channel);
    UNUSED(size);

    uint16_t cid;
    uint8_t  status;

    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_AVRCP_META) return;

    switch (packet[2]) {
        case AVRCP_SUBEVENT_CONNECTION_ESTABLISHED:
            cid = avrcp_subevent_connection_established_get_avrcp_cid(packet);
            status = avrcp_subevent_connection_established_get_status(packet);
            if (status != ERROR_CODE_SUCCESS){
                // printf("AVRCP: Connection failed, status 0x%02x\n", status);
                _cid = 0;
                return;
            }

            _cid = cid;
            // avrcp_subevent_connection_established_get_bd_addr(packet, address);
            // printf("AVRCP: Connected to %s, cid 0x%02x\n", bd_addr_to_str(address), cid);

            avrcp_target_support_event(cid, AVRCP_NOTIFICATION_EVENT_VOLUME_CHANGED);
            avrcp_target_support_event(cid, AVRCP_NOTIFICATION_EVENT_BATT_STATUS_CHANGED);
            avrcp_target_battery_status_changed(cid, AVRCP_BATTERY_STATUS_EXTERNAL);
        
            // query supported events:
            avrcp_controller_get_supported_events(cid);
            return;
        
        case AVRCP_SUBEVENT_CONNECTION_RELEASED:
            // printf("AVRCP: Channel released: cid 0x%02x\n", avrcp_subevent_connection_released_get_avrcp_cid(packet));
            _cid = 0;
            return;

        default:
            break;
    }
}


static void controller_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    UNUSED(channel);
    UNUSED(size);

    uint8_t play_status;

    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_AVRCP_META) return;
    if (_cid == 0) return;

    switch (packet[2]) {
        // case AVRCP_SUBEVENT_GET_CAPABILITY_EVENT_ID:
        //     Avrcp::_avrcp->_supported_notifications |= (1 << avrcp_subevent_get_capability_event_id_get_event_id(packet));
        //     break;

        case AVRCP_SUBEVENT_GET_CAPABILITY_EVENT_ID_DONE:
            
            // printf("AVRCP Controller: supported notifications by target:\n");
            // for (event_id = (uint8_t) AVRCP_NOTIFICATION_EVENT_FIRST_INDEX; event_id < (uint8_t) AVRCP_NOTIFICATION_EVENT_LAST_INDEX; event_id++){
            //     printf("   - [%s] %s\n", 
            //         (avrcp_connection->notifications_supported_by_target & (1 << event_id)) != 0 ? "X" : " ", 
            //         avrcp_notification2str((avrcp_notification_event_id_t)event_id));
            // }
            // printf("\n\n");

            // automatically enable notifications
            avrcp_controller_enable_notification(_cid, AVRCP_NOTIFICATION_EVENT_PLAYBACK_STATUS_CHANGED);
            avrcp_controller_enable_notification(_cid, AVRCP_NOTIFICATION_EVENT_NOW_PLAYING_CONTENT_CHANGED);
            avrcp_controller_enable_notification(_cid, AVRCP_NOTIFICATION_EVENT_TRACK_CHANGED);
            break;

        case AVRCP_SUBEVENT_NOTIFICATION_PLAYBACK_STATUS_CHANGED:
            // printf("AVRCP Controller: Playback status changed %s\n", avrcp_play_status2str(avrcp_subevent_notification_playback_status_changed_get_play_status(packet)));
            play_status = avrcp_subevent_notification_playback_status_changed_get_play_status(packet);
            switch (play_status){
                case AVRCP_PLAYBACK_STATUS_PLAYING:
                    _playing = true;
                    break;
                default:
                    _playing = false;
                    break;
            }
            break;

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
            _volume = avrcp_subevent_notification_volume_changed_get_absolute_volume(packet);
            // volume_percentage = volume * 100 / 127;
            // printf("AVRCP Target    : Volume set to %d%% (%d)\n", volume_percentage, volume);
            avrcp_volume_changed(_volume);
            break;
        
        // case AVRCP_SUBEVENT_OPERATION:
        //     auto operation_id = avrcp_subevent_operation_get_operation_id(packet);
        //     auto button_state = avrcp_subevent_operation_get_button_pressed(packet) > 0 ? "PRESS" : "RELEASE";
        //     switch (operation_id){
        //         case AVRCP_OPERATION_ID_VOLUME_UP:
        //             printf("AVRCP Target    : VOLUME UP (%s)\n", button_state);
        //             break;
        //         case AVRCP_OPERATION_ID_VOLUME_DOWN:
        //             printf("AVRCP Target    : VOLUME DOWN (%s)\n", button_state);
        //             break;
        //         default:
        //             return;
        //     }
        //     break;

        default:
            // printf("AVRCP Target    : Event 0x%02x is not parsed\n", packet[2]);
            break;
    }
}


void avrcp_begin() {
    avrcp_init();
    avrcp_controller_init();
    avrcp_target_init();

    avrcp_register_packet_handler(connection_handler);
    avrcp_controller_register_packet_handler(controller_handler);
    avrcp_target_register_packet_handler(target_handler);
}


uint8_t avrcp_get_volume() { 
    return _volume; 
};


bool avrcp_is_connected() { 
    return _cid != 0; 
};


bool avrcp_is_playing() { 
    return _playing; 
};
