#ifndef avrcp_h
#define avrcp_h

#include <btstack.h>

void avrcp_begin();

uint8_t avrcp_get_volume();  // 0..127
bool avrcp_is_connected();
bool avrcp_is_playing();

#endif