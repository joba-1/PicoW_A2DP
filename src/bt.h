#ifndef bt_h
#define bt_h

#include <bluetooth.h>
#include <stdbool.h>

typedef void (*bt_on_up_cb_t)( void * );

void bt_begin( const char *name, const char *pin, bt_on_up_cb_t cb, void *data );
void bt_run();

bool bt_up();
void bt_addr( bd_addr_t local_addr );

#endif