# Raspberry Pico W A2DP Sink to I2S/DAC

Use a Raspberry Pico W or Pico2 W to receive music from a smartphone 
or other bluetooth A2DB source and send it via I2S to a DAC like the PCM5102.

Based on the BTStack a2db-sink demo of the pico-examples repo.
Not being embedded in the build structure of all examples makes it much easier
to see what is actually required, to modularize and enhance.
Still uses pico-extras for i2s audio.

Feel free to ask if you need help to get this running. Comments or improvements welcome :)

## Programming
* Clone Pico-SDK (>= 1.5.0 for Pico W, >= 2.0.0 for Pico2 W), Pico-Extras from Github
```
# any directory name for your sdk and build environment
base=~/pico2
mkdir "$base"

# install sdk (maybe use branch or tag other than master, e.g. 2.1.0 or 1.5.1) 
cd "$base"
git clone git@github.com:raspberrypi/pico-sdk.git --branch master
cd pico-sdk
git submodule update --init
cd ..
export PICO_SDK_PATH="$base"/pico-sdk

git clone git@github.com:raspberrypi/pico-extras.git
export PICO_EXTRAS_PATH="$base"/pico-extras/

git clone https://github.com/joba-1/PicoW_A2DP.git
cd PicoW_A2DP

# optionally update files taken from sdk, extras or examples
cp -av $PICO_SDK_PATH/external/pico_sdk_import.cmake .
cp -av $PICO_EXTRAS_PATH/external/pico_extras_import.cmake .
cd ..
git clone git@github.com:raspberrypi/pico-examples.git
cd -
cp -av ../pico-examples/pico_w/bt/config/btstack_config.h .

# optionally update pins and bt name in CMakeLists.txt, then
mkdir build
cd build
cmake -DPICO_BOARD=pico2_w ..
# cmake -DPICO_BOARD=pico_w ..
make -j8

# bring pico in boot mode (start with bootsel pressed), then
cp -av picow-a2dp.uf2 /run/media/$USER/RP2350/
# cp -av picow-a2dp.uf2 /run/media/$USER/RP2040/
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

Status is: already works pretty well with Pico W. Pico2 W can be built but looses connection after a short time

## Added Features
* LED functions
    * on during boot
    * show a2dp connection status
    * blink on fatal errors
* configurable BT device name and BT connection pin
* configurable hard reset via pin
* Volume control
* Reboot on disconnect to work around buggy reconnect
* WIP: Support pico2_w 
