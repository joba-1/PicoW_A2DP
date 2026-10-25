# Raspberry Pico W or Pico 2 W A2DP Sink to I2S/DAC

Use a Raspberry Pico W or Pico 2 W to receive music from a smartphone 
or other Bluetooth A2DP source and send it via I2S to a DAC like the PCM5102.

Based on the BTstack a2dp-sink demo of the pico-examples repo.
Not being embedded in the build structure of all examples makes it much easier
to see what is actually required, to modularize and enhance.
Still uses pico-extras for i2s audio.

Feel free to ask if you need help to get this running. Comments or improvements welcome :)

## Features
* SBC in every configuration a source may choose: 16/32/44.1/48 kHz, mono, dual channel, stereo, joint stereo, bitpool 2-53
* Volume controlled by the source (AVRCP absolute volume) with a dB curve, smooth ramps and stored in flash
* Volume buttons of AVRCP remote controls
* Adaptive jitter buffer with gentle clock drift compensation (max 0.2%, no audible pitch wobble)
* Delay reporting, so sources can keep video in sync with the delayed audio
* Secure simple pairing without PIN, bonding, reconnect of known sources
* Hidden for new sources while one is connected
* LED and a GPIO show the connection (I use the GPIO to switch my AV receiver input)
* Log line `A2DP: connected to <addr>` on stdio (with `STDIO_UART=ON`: GP0, 115200 baud) identifies the source, e.g. for per source settings of a receiver controller
* Watchdog, optional reboot on disconnect and hard reset via RUN pin

## Programming
* Needs Pico-SDK >= 2.3.0 (bundles BTstack 1.8, which changed the audio interfaces). Tested with 2.3.1.
* Clone Pico-SDK, Pico-Extras and PicoW_A2DP from Github, build and flash
```
base=~/pico
mkdir -p "$base"
cd "$base"
git clone https://github.com/raspberrypi/pico-sdk.git --branch 2.3.1
cd pico-sdk
git submodule update --init
cd ..
export PICO_SDK_PATH="$base"/pico-sdk
git clone https://github.com/raspberrypi/pico-extras.git --branch sdk-2.3.1
export PICO_EXTRAS_PATH="$base"/pico-extras/
git clone https://github.com/joba-1/PicoW_A2DP.git
cd PicoW_A2DP/
mkdir build && cd build

cmake -DPICO_BOARD=pico2_w ..   # or -DPICO_BOARD=pico_w

make -j
# boot pico into flash mode before this: 
cp picow-a2dp.uf2 /run/media/$USER/RP2??0/
```
* If no arm-none-eabi-gcc is installed, download the Arm GNU toolchain and point `PICO_TOOLCHAIN_PATH` to it
* For optional flashing/debugging with a picoprobe connect Pico-W power-, debug- and optionally uart-pins and clone OpenOCD Pico Branch from raspberrypi github

## Configuration
All settings are CMake cache variables, set them with `-D<NAME>=<value>` when running cmake:

| Name | Default | Meaning |
|---|---|---|
| `BT_NAME` | `RX-V1600` | Bluetooth name, `00:00:00:00:00:00` in it is replaced by the device address |
| `BT_PIN` | `0000` | PIN for legacy pairing, only very old sources use it |
| `I2S_DATA_PIN` | 9 | GPIO for I2S data (DIN) |
| `I2S_CLOCK_PIN_BASE` | 10 | GPIO for bit clock (BCK), LRCK is the next GPIO |
| `CONN_PIN` | 26 | GPIO that is high while a source is connected, -1: unused |
| `RUN_PIN` | -1 | GPIO wired to RUN, pulled low to reset on fatal errors, -1: unused |
| `A2DP_LATENCY_MS` | 200 | Jitter buffer target 20..1000. Lower: less delay, higher: fewer dropouts |
| `VOLUME_RANGE_DB` | 50 | Attenuation at the lowest volume step above mute |
| `VOLUME_DEFAULT` | 100 | Volume 0..127 until a source sets one |
| `VOLUME_ABSOLUTE` | ON | Let sources control the volume of the sink. OFF: sources scale the audio themselves |
| `REBOOT_ON_DISCONNECT` | ON | Reboot after a source disconnects, for a clean state |
| `WATCHDOG_MS` | 3000 | Reboot if the main loop stalls this long (max 8000), 0: off |
| `STDIO_UART` | OFF | stdio on uart (e.g. picoprobe) instead of usb |
| `PRINT_STATS` | OFF | Print jitter buffer statistics every 10s while streaming |

The used RAM grows with `A2DP_LATENCY_MS` (about 180 bytes per ms), that's why 1000 ms is the maximum.

## Volume
Sources send an absolute volume 0..127. The sink maps it to equal dB steps between `-VOLUME_RANGE_DB` and 0 dB (0 is mute),
so each of the usual 16 phone volume steps is about 3 dB. Changes are ramped over one I2S buffer (12 ms) to avoid clicks.
The current volume is reported to the source on connect, so the volume slider of the phone matches the sink.
It is stored in flash when a stream stops, not while playing, because flash writes stall the CPU.

With an AV receiver behind the DAC, set the receiver to a comfortable maximum and control the volume from the phone.
The DAC output is 16 bit, so very low phone volumes lose resolution. If that matters, lower `VOLUME_RANGE_DB` 
and use the receiver for coarse volume.

## Latency
Total delay is the jitter buffer (`A2DP_LATENCY_MS`, default 200 ms) plus about 45 ms in the I2S driver, 
plus whatever the source buffers (typically 50-150 ms on phones). Sources that support delay reporting 
(Android, iOS, PipeWire) delay video by the reported value, so lip sync is fine even with the default.

Lower values work if the radio link has no gaps: in the host simulation 60 ms played without dropouts with 30 ms
packet jitter, but every radio gap longer than the buffer causes a short dropout. 
Phones that share the antenna with WLAN do have such gaps, therefore the default is conservative.
Low latency in the sense of < 40 ms end to end is not possible with A2DP and SBC: the source side alone buffers more.
Codecs made for it are not an option here: aptX Low Latency is proprietary and not sent by iPhones, 
and LE Audio (LC3) needs isochronous channel support in the controller, which as far as I know the CYW43439 firmware does not have.

## Compatibility notes
* iPhone: uses SBC (AAC is not supported by the sink), absolute volume works, volume is synced on connect
* Android: absolute volume works. If the volume jumps, "Disable absolute volume" in the developer options lets the phone scale itself
* Linux (BlueZ with PipeWire or PulseAudio): absolute volume via AVRCP, SBC-XQ (dual channel, up to bitpool 53) is supported,
  stream reconfigurations (e.g. sample rate changes) are handled without reconnect

## Debugging / Flashing
Use commandline to cmake the firmware, then copy the UF2 to the USB filesystem or use picoprobe and openocd to flash the firmware and openocd/gdb to debug.
Alternatively use VS Code with CMake Tools and Cortex Debug extensions as a build/debug environment.
To help VS Code find your openocd binary, add it in ~/.config/Code/User/settings.json before the closing "}" similar to this:
```
    "cortex-debug.openocdPath": "/home/joachim/pico/openocd/src/openocd"
```
To flash/debug with VS Code, select "Run and Debug" or Ctrl+Shift+D, at the top select "Pico Debug" (Pico W) or "Pico2 Debug" (Pico 2 W). Now the green arrow or F5 flashes the new firmware and starts a debug session.

## Tests
The hardware independent parts run on the host: SBC frame parsing is checked against BTstack's SBC encoder 
for all configurations, the volume math, and a simulation of the whole receive path (decoder, jitter buffer, 
drift compensation) with clock offsets, packet jitter, radio gaps and reconfigurations.
```
cmake -S test -B build-test && cmake --build build-test && ctest --test-dir build-test --output-on-failure
```
They use the BTstack sources of `PICO_SDK_PATH`.

## History
* 2.0.0: Pico-SDK 2.3 support, fixes for reconnect, Linux and iPhone compatibility, new volume control, delay reporting, tests
* 1.x: Pico W and Pico 2 W support with Pico-SDK 2.1, volume control, reboot on disconnect
