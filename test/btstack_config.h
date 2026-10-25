#ifndef BTSTACK_CONFIG_H
#define BTSTACK_CONFIG_H

// minimal config to build btstack's sbc codec and a2dp helpers on the host
#define ENABLE_CLASSIC
#define HAVE_ASSERT
#define MAX_NR_AVDTP_CONNECTIONS 1
#define MAX_NR_AVDTP_STREAM_ENDPOINTS 1
#define HCI_ACL_PAYLOAD_SIZE (1691 + 4)

#endif
