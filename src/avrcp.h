#ifndef avrcp_h
#define avrcp_h

#include <btstack.h>

void avrcp_begin();

// connect avrcp to the a2dp source if it does not do it itself soon
void avrcp_connect_later( bd_addr_t addr );

bool avrcp_is_connected();

#endif
