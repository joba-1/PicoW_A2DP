#include "avrcp.h"

#include "volume.h"

#ifndef VOLUME_ABSOLUTE
#define VOLUME_ABSOLUTE 1
#endif

#define VOLUME_STEP 8                 // 16 steps from 0 to 127 like most phones
#define CONNECT_DELAY_MS 2000


static uint16_t _cid = 0;
static bd_addr_t _addr;
static btstack_timer_source_t _connect_timer;


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


// btstack needs a handler for controller events, we don't use any
static void controller_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    UNUSED(packet_type);
    UNUSED(channel);
    UNUSED(packet);
    UNUSED(size);
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
