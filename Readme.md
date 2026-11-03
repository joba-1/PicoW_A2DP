# Raspberry Pico W or Pico2 W A2DP Sink to I2S/DAC

Use a Raspberry Pico W or Pico2 W to receive music from a smartphone 
or other bluetooth A2DB source and send it via I2S to a DAC like the PCM5102.

Based on the BTStack a2db-sink demo of the pico-examples repo.
Not being embedded in the build structure of all examples makes it much easier
to see what is actually required, to modularize and enhance.
Still uses pico-extras for i2s audio.

Feel free to ask if you need help to get this running. Comments or improvements welcome :)

## Feature branch
Branch [review-sdk-2.3](https://github.com/joba-1/PicoW_A2DP/tree/review-sdk-2.3) has more on top of the same SDK support:
reconnect and stream fixes, new volume control (dB curve, ramps, stored in flash), adaptive jitter buffer,
delay reporting, pairing without PIN and host tests. It works on hardware, but changes like changing the volume are only tested rudimentary.
To use it, run `git checkout review-sdk-2.3` after cloning and follow the Readme there.

## Programming
* Clone Pico-SDK, Pico-Extras and PicoW_A2DP from Github, build and flash
* needs Pico-SDK >= 2.3.0 (bundles BTstack 1.8, which changed the audio interface), builds with 2.3.1
* this version has not been used on hardware yet
* for older Pico-SDKs (tested >= 1.5.0 for Pico W, >= 2.1.0 for Pico2 W) use tag `sdk-2.1`: run `git checkout sdk-2.1` after cloning and follow the Readme there
```
base=~/pico-2.3.1
mkdir "$base"
cd "$base"
git clone git@github.com:raspberrypi/pico-sdk.git --branch 2.3.1
cd pico-sdk
git submodule update --init
cd ..
export PICO_SDK_PATH="$base"/pico-sdk
git clone git@github.com:raspberrypi/pico-extras.git --branch sdk-2.3.1
export PICO_EXTRAS_PATH="$base"/pico-extras/
git clone https://github.com/joba-1/PicoW_A2DP.git
cd PicoW_A2DP/
mkdir build && cd build

# cmake -DPICO_BOARD=pico_w -DCMAKE_BUILD_TYPE=Debug ..
cmake -DPICO_BOARD=pico2_w -DCMAKE_BUILD_TYPE=Debug ..

make -j
# boot pico into flash mode before this: 
cp pico2w-a2dp.uf2 /run/media/$USER/RP2??0/   # picow-a2dp.uf2 for pico_w
```
* For optional flashing/debugging with a picoprobe connect Pico-W power-, debug- and optionally uart-pins and clone OpenOCD Pico Branch from raspberrypi github

## Configuration
* Used pins for connectiong the DAC are defined in CMakeLists.txt
* Bluetooth name and pin also defined in CMakeLists.txt
* If RUN_PIN is defined the pin will be used to pull down RUN pin to reset on fatal errors
* CONN_PIN high indicates active bt connection (I use it to switch my AV receiver input)

## Debugging / Flashing
Use commandline to cmake the firmware, then copy the UF2 to the USB filesystem or use picoprobe and openocd to flash the firmware and openocd/gdb to debug.
Alternatively use VS Code with CMake Tools and Cortex Debug extensions as a build/debug environment.
To help VS Code find your openocd binary, add it in ~/.config/Code/User/settings.json before the closing "}" similar to this:
```
    "cortex-debug.openocdPath": "/home/joachim/pico/openocd/src/openocd"
```
To flash/debug with VS Code, select "Run and Debug" or Ctrl+Shift+D, at the top select "Pico Debug (your project)". Now the green arrow or F5 flashes the new firmware and starts a debug session.

## WIP
Currently done: 
* separate the demo from the pico-examples repo.
* enable debugging with picoprobe and VS Code
* modularize bt functionality: i2s, avrcp, a2dp, sdp and generic bt
Target is to have a modular design with stable functionality for everyday use.

Status is: works pretty well with Pico W and Pico2 W

## Added Features
* LED functions
    * on during boot
    * show a2dp connection status
    * blink on fatal errors
* configurable BT device name and BT connection pin
* configurable hard reset via pin
* Volume control
* Reboot on disconnect to work around buggy reconnect
* Support pico2_w (just change the board type for cmake)
