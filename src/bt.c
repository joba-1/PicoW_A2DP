#include "bt.h"

#include "sdp.h"
#include "a2dp.h"
#include "avrcp.h"

#include <hci.h>
#include "l2cap.h"
#include <btstack_event.h>
#include <btstack_run_loop.h>

#include <memory.h>


static bool _is_up = false;
static bd_addr_t _local_addr = {0};
static bt_on_up_cb_t _cb = 0;
static void *_data = 0;
static const char *_name = 0;
static const char *_pin = 0;
static btstack_packet_callback_registration_t _hci_registration;


static void packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    UNUSED(size);
    UNUSED(channel);

    bd_addr_t address;

    if (packet_type != HCI_EVENT_PACKET) return;

    switch(hci_event_packet_get_type(packet)) {

        case BTSTACK_EVENT_STATE:
            if (btstack_event_state_get_state(packet) != HCI_STATE_WORKING) return;
            gap_local_bd_addr(_local_addr);
            _is_up = true;
            if (_cb) (*_cb)(_data);
            break;

        case HCI_EVENT_PIN_CODE_REQUEST:
            hci_event_pin_code_request_get_bd_addr(packet, address);
            gap_pin_code_response(address, _pin);
            break;

        default:
            break;
    }
}


void bt_begin( const char *name, const char *pin, bt_on_up_cb_t cb, void *data ) {
    _name = name ? name : "Pico 00:00:00:00:00:00";
    _pin = pin ? pin : "0000";
    _data = data;
    _cb = cb;

    l2cap_init();
    sdp_begin();

    a2dp_sink_begin();
    avrcp_begin();

    gap_set_local_name(_name);
    gap_discoverable_control(1); 
    gap_set_class_of_device(0x200414);  // Service Class: Audio, Major Device Class: Audio, Minor: Loudspeaker
    gap_set_default_link_policy_settings( LM_LINK_POLICY_ENABLE_ROLE_SWITCH | LM_LINK_POLICY_ENABLE_SNIFF_MODE );
    gap_set_allow_role_switch(true);  // A2DP Source, e.g. smartphone, can become master after re-connect.

    _hci_registration.callback = &packet_handler;
    hci_add_event_handler(&_hci_registration);
}


void bt_run() {
    hci_power_control(HCI_POWER_ON);
    btstack_run_loop_execute();
}


bool bt_up() {
    return _is_up;
}


void bt_addr( bd_addr_t local_addr ) {
    memcpy(local_addr, _local_addr, sizeof(bd_addr_t));
}
