# Raspberry Pico W A2DP Sink to I2S/DAC

Use a Raspberry Pico W to receive music from a smartphone 
or other bluetooth A2DB source and send it via I2S to a DAC like the PCM5102.

Based on the BTStack a2db-sink demo of the pico-examples repo.
Not being embedded in the build structure of all examples makes it much easier
to see what is actually required, to modularize and enhance.
Still uses pico-extras for i2s audio.

## Programming
* Clone Pico-SDK >= 1.5.0, Pico-Extras and OpenOCD Pico Branch from Github
* For flashing/debugging with a picoprobe connect Pico-W power-, debug- and optionally uart-pins

## Configuration
* Used pins for connectiong the DAC are defined in CMakeLists.txt
* Bluetooth name and pin also defined in CMakeLists.txt
* If RUN_PIN is defined the pin will be used to pull down RUN pin to reset on fatal errors

## Debugging / Flashing
Use commandline to cmake the firmware, then copy the UF2 to the USB filesystem or use picoprobe and openocd to flash the firmware and openocd/gdb to debug.
Alternatively use VS Code with CMake Tools and Cortex Debug extensions as a build/debug environment.

## WIP
Currently done: 
* separate the demo from the pico-examples repo.
* enable debugging with picoprobe and VS Code
* modularize bt functionality: i2s, avrcp, a2dp, sdp and generic bt
Target is to have a modular design with stable functionality for everyday use.

## Added Features
* LED functions
    * on during boot
    * show a2dp connection status
    * blink on fatal errors
* configurable BT device name and BT connection pin
* configurable hard reset via pin
* Volume control
* Reboot on disconnect to work around buggy reconnect