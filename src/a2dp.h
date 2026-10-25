#ifndef a2dp_h
#define a2dp_h

#include <stdbool.h>
#include <stdint.h>

// called when a source connects (connected=true) or disconnects
typedef void (*a2dp_connection_cb_t)( bool connected, uint8_t *addr );

void a2dp_sink_begin( a2dp_connection_cb_t cb );

bool a2dp_is_streaming();


#endif
