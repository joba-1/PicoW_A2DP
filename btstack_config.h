#ifndef _PICO_BTSTACK_BTSTACK_CONFIG_H
#define _PICO_BTSTACK_BTSTACK_CONFIG_H

// Based on pico-examples a2dp_sink_demo config, reduced to classic A2DP/AVRCP.
// Few logs: printing via usb can cause audio hiccups

// BTstack features that can be enabled
#undef ENABLE_LOG_INFO
#define ENABLE_LOG_ERROR
#define ENABLE_PRINTF_HEXDUMP

#ifdef ENABLE_CLASSIC
#define ENABLE_L2CAP_ENHANCED_RETRANSMISSION_MODE
#endif

// BTstack configuration. buffers, sizes, ...
#define HCI_OUTGOING_PRE_BUFFER_SIZE 4
#define HCI_ACL_PAYLOAD_SIZE (1691 + 4)
#define HCI_ACL_CHUNK_SIZE_ALIGNMENT 4
#define MAX_NR_AVDTP_CONNECTIONS 1
#define MAX_NR_AVDTP_STREAM_ENDPOINTS 1
#define MAX_NR_AVRCP_CONNECTIONS 2
#define MAX_NR_BTSTACK_LINK_KEY_DB_MEMORY_ENTRIES  2
#define MAX_NR_HCI_CONNECTIONS 2
#define MAX_NR_L2CAP_CHANNELS  4
#define MAX_NR_L2CAP_SERVICES  3
#define MAX_NR_SERVICE_RECORD_ITEMS 4
#define MAX_NR_WHITELIST_ENTRIES 16

// Limit number of ACL Buffer to use by stack to avoid cyw43 shared bus overrun
#define MAX_NR_CONTROLLER_ACL_BUFFERS 3

// Enable and configure HCI Controller to Host Flow Control to avoid cyw43 shared bus overrun
#define ENABLE_HCI_CONTROLLER_TO_HOST_FLOW_CONTROL
#define HCI_HOST_ACL_PACKET_LEN 1024
#define HCI_HOST_ACL_PACKET_NUM 3
#define HCI_HOST_SCO_PACKET_LEN 120
#define HCI_HOST_SCO_PACKET_NUM 3

// Link Key DB using TLV on top of Flash Sector interface (also stores the volume)
#define NVM_NUM_DEVICE_DB_ENTRIES 16
#define NVM_NUM_LINK_KEYS 16

// We don't give btstack a malloc, so use a fixed-size ATT DB.
#define MAX_ATT_DB_SIZE 512

// BTstack HAL configuration
#define HAVE_EMBEDDED_TIME_MS

// map btstack_assert onto Pico SDK assert()
#define HAVE_ASSERT

// Some USB dongles take longer to respond to HCI reset (e.g. BCM20702A).
#define HCI_RESET_RESEND_TIMEOUT_MS 1000

#define ENABLE_SOFTWARE_AES128
#define ENABLE_MICRO_ECC_FOR_LE_SECURE_CONNECTIONS

#endif // _PICO_BTSTACK_BTSTACK_CONFIG_H
