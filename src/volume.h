#ifndef volume_h
#define volume_h

#include <stdint.h>

// Volume in AVRCP absolute volume units 0..127, applied by the audio sink and kept in flash

void volume_begin();  // load stored volume and apply it, needs the audio sink instance

uint8_t volume_get();
void volume_set( uint8_t volume );
void volume_step( int delta );

void volume_save();   // store in flash if changed (not while streaming: flash writes stall the cpu)

#endif
